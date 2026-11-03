"""Top-level conversion orchestrator: PDF -> DOCX.

Pipeline
--------
    page_words -> build_lines -> group_paragraphs -> reading_order
    -> python-docx emission
    tables  : extract_tables (MuPDF find_tables)
    figures : extract_figures (embedded rasters + vector clusters -> PNG crop)
"""
import math
import re

import fitz
from docx import Document
from docx.shared import Pt
from docx.oxml.ns import qn

from . import layout
from .config import DOC_MARGIN_PT
from .text import page_words, build_lines, group_rows
from .structure import (is_furniture, group_paragraphs, reading_order,
                        _NUMITEM)
from .media import extract_tables, extract_figures, colof_rect
from .writer import (emit_block, emit_table, emit_figure, emit_math,
                     pair_captions, reset_bookmarks)
from . import writer as writer_mod
from . import math as math_mod
from .headers import (detect_running_heads, detect_running_logo,
                      apply_header_footer, _strip_folio)
from . import annots as annots_mod
from . import fonts as fonts_mod
from . import ocr
from . import svgs as svgs_mod


def _inside(w, excl):
    """True when a token's centre sits inside a table / figure region."""
    cx = (w["x0"] + w["x1"]) / 2.0
    cy = (w["y0"] + w["y1"]) / 2.0
    for e in excl:
        if e.x0 - 1 <= cx <= e.x1 + 1 and e.y0 - 1 <= cy <= e.y1 + 1:
            return True
    return False


def _mark_num_groups(flow):
    """Mark runs of consecutive numbered list items whose literal numbers
    increase monotonically ("(1)... (2)... (3)...").  Only these qualify as
    native Word numbered lists -- an isolated '1.' or a broken sequence is far
    more likely a heading or a scattered enumeration that must keep its
    literal number.  Members get b['numgroup'] / b['numval']."""
    gid = 0
    i = 0
    while i < len(flow):
        it = flow[i]
        if it.get("t") != "block" or it["b"].get("kind") != "numitem":
            i += 1
            continue
        group = []
        last = None
        j = i
        while j < len(flow):
            jt = flow[j]
            if jt.get("t") != "block" or jt["b"].get("kind") != "numitem":
                # prose / figures may sit between list items (IEEE enumerates
                # contributions with commentary between them); the strict +1
                # number rule below is what guards correctness
                j += 1
                continue
            m = _NUMITEM.match(jt["b"]["text"].strip())
            if not m:
                break
            n = int(m.group(1) or m.group(2))
            # strictly +1: real lists count 1,2,3...; equation-number lookalikes
            # like (2) ... (19) jump and must not form a list
            if last is not None and n != last + 1:
                break
            group.append((jt["b"], n))
            last = n
            j += 1
        if len(group) >= 2:
            gid += 1
            for b, n in group:
                b["numgroup"] = gid
                b["numval"] = n
        i = j if j > i else i + 1


def _has_art(rect, graphics):
    """True when real artwork (a raster or a vector cluster) sits in *rect*.

    A region that find_tables refused is only a figure if something was drawn
    there.  A refused table with no artwork is plain text -- a data listing the
    detector mis-segmented -- and must stay in the text flow, otherwise its
    numbers survive only as pixels.
    """
    area = rect.get_area()
    if area <= 0:
        return False
    for g in graphics:
        inter = fitz.Rect(g["bbox"]) & rect
        if inter.is_empty:
            continue
        if inter.get_area() >= 0.05 * area:
            return True
    return False


_CAPTION_RE = re.compile(r"^(fig(ure)?|tab(le)?|scheme|algorithm)\b", re.I)


def _lifted(ptxt, band_txt):
    """True when *ptxt* is the running head / foot already lifted to the
    Word header or footer, so it must not also appear in the body.

    Cases covered:

    * exact match;
    * the paragraph (with any glued folio removed) is a substring of the band
      text -- needed when the band is the *join* of several recurring lines
      ("Hu et al." + "Meteorological data ...") while a given page shows only
      one of them;
    * the band text dominates the paragraph (OCR'd logos, joined);
    * a folio got glued onto the head ("Meteorological data ... 9").

    Both substring tests are length-guarded.  Detection runs on raw top-band
    lines, and a page whose top band holds a stray glyph yields a one-character
    "running head" -- on 122 the footer came back as "i", and testing
    ``"i" in paragraph`` discarded 495 of 610 paragraphs, i.e. the whole
    article.  A band string that short is noise, never furniture.
    """
    if not band_txt or len(band_txt) < 4:
        return False
    p = _strip_folio(ptxt)
    if p == band_txt:
        return True
    if len(p) < 3:
        return False
    if p in band_txt:
        return True
    # reverse direction: only when the band text is most of the paragraph,
    # otherwise any paragraph merely *containing* it would be dropped
    if band_txt in p and len(band_txt) >= 0.5 * len(p):
        return True
    return False


def _band_furniture(block, band, body):
    """True when a block is a running head rather than document text.

    The string-based detector can only lift text that *recurs* across pages,
    which misses a per-chapter banner ("第一章技术发展与应用现状"): it changes
    from chapter to chapter, so no single string is common enough, yet it sits
    at the top of every page in its chapter and would otherwise be emitted as
    a Heading 1 once per page.

    Geometry settles it.  A running head lives *inside* the top margin, so its
    whole box sits above the text margin, while a real body or section heading
    starts *at* the margin and therefore always extends past it.  The extra
    guards keep a cover title (larger type) and a wide first line (long text)
    out of the rule.
    """
    if len(block.get("lines") or ()) != 1:
        return False
    txt = " ".join(block["text"].split())
    if not txt or len(txt) > 60:
        return False
    if block["size"] > body * 2.6:
        return False
    return block["y1"] < band


def _has_line_neighbor(ln, words, body):
    """True when another line of text sits directly above/below *ln* in the
    same horizontal band -- i.e. *ln* is part of a flowing paragraph, not a
    standalone label.

    Axis labels / tick labels on a plot are spaced a full line-height or more
    apart, while a paragraph's lines are packed tightly, so this cleanly tells
    a plot caption-less figure's labels from an author bio sitting next to a
    headshot photo.
    """
    lx0, lx1 = ln["x0"], ln["x1"]
    lc = (ln["y0"] + ln["y1"]) / 2.0
    lh = max(ln["y1"] - ln["y0"], body * 0.6)
    band = 1.8 * lh
    for w in words:
        ov = min(lx1, w["x1"]) - max(lx0, w["x0"])
        if ov <= 0.35 * (lx1 - lx0):        # no real horizontal overlap
            continue
        wc = (w["y0"] + w["y1"]) / 2.0
        d = abs(wc - lc)
        if 0 < d < band:                    # a different line, close vertically
            return True
    return False


def _absorb_labels(bbox, words, body, col_w):
    """Grow a figure box over the axis labels printed just outside the frame.

    A plot's frame is what the graphics extractor sees; its tick labels and
    axis titles are *text* drawn a few points outside that frame, so they used
    to survive as stray body paragraphs ("1 Probability 0.5", "phase").

    Two guards keep this from eating real text:

    * only large figures (plots, not small headshot photos) are eligible;
    * a candidate line is absorbed only when it is vertically *isolated* -- a
      line with text above or below it is a paragraph (e.g. an author bio
      beside a portrait), and must stay as real text.
    """
    if col_w <= 1 or bbox.width < 130:
        return bbox
    grown = fitz.Rect(bbox)
    for _ in range(2):                      # tick labels, then axis titles
        near = [w for w in words
                if w["size"] <= body * 1.30
                and not (grown.x0 - 1 <= (w["x0"] + w["x1"]) / 2.0 <= grown.x1 + 1
                         and grown.y0 - 1 <= (w["y0"] + w["y1"]) / 2.0 <= grown.y1 + 1)
                and grown.x0 - 22 <= w["x0"] and w["x1"] <= grown.x1 + 22
                and grown.y0 - 22 <= w["y0"] and w["y1"] <= grown.y1 + 22]
        if not near:
            break
        added = False
        for ln in group_rows(near):
            toks = ln.get("segs") or []
            text = "".join(s[0] for s in toks).strip()
            if not text or _CAPTION_RE.match(text):
                continue
            if len(toks) > 3 or (ln["x1"] - ln["x0"]) > 0.35 * col_w:
                continue                    # a sentence, not a label
            if _has_line_neighbor(ln, words, body):
                continue                    # part of a paragraph, leave it
            grown |= fitz.Rect(ln["x0"], ln["y0"], ln["x1"], ln["y1"])
            added = True
        if not added:
            break
    return grown


def _trim_figure(bbox, words, col_w, body, leading=None, col_x0=None):
    """Cut a figure box that has swallowed full-measure body paragraphs.

    Chart bars are routinely drawn taller than the plot area and clipped by a
    clipping path, so the *unclipped* geometry reaches far below the visible
    figure.  Clustering then grows the figure box over the body text
    underneath, and every word in that box is excluded from the flow -- on 039
    that silently deleted the whole CONCLUSION section.

    A real figure never contains a full-measure body paragraph, so the box is
    cut just above the first such *paragraph* -- a run of consecutive rows, not
    a single row.  A diagram is full of wide rows of labels ("安全Agent  浏览器
    操作Agent  具身智能Agent"), and cutting there saws the diagram in half and
    spills its labels into the text flow as one-line paragraphs.

    Alignment is the decisive signal.  Body prose starts flush at the column
    margin and ends at the measure; a diagram's label rows are inset and
    irregular.  So a candidate run must also be flush with the margin, and the
    cut is then extended upwards over any indented first line of the same
    paragraph.
    """
    if col_w <= 1 or bbox.height <= 0:
        return bbox
    inside = [w for w in words
              if bbox.x0 - 1 <= (w["x0"] + w["x1"]) / 2.0 <= bbox.x1 + 1
              and bbox.y0 - 1 <= (w["y0"] + w["y1"]) / 2.0 <= bbox.y1 + 1]
    if not inside:
        return bbox
    lead = float(leading or body * 1.4)
    rows = group_rows(inside)

    def _is_body(ln):
        size = ln.get("bsize") or body
        return (abs(size - body) <= 0.12 * body
                and (ln["x1"] - ln["x0"]) >= 0.75 * col_w)

    def _cont(a, b):                      # b follows a with normal leading
        return b["y0"] - a["y1"] <= lead * 1.5

    for i in range(1, len(rows)):
        a, b = rows[i - 1], rows[i]
        if not (_is_body(a) and _is_body(b) and _cont(a, b)):
            continue
        if col_x0 is not None and min(a["x0"], b["x0"]) - col_x0 > 4:
            continue                      # inset: a diagram's label row
        j = i - 1
        while j > 0 and _is_body(rows[j - 1]) and _cont(rows[j - 1], rows[j]):
            j -= 1                        # keep an indented first line too
        cut = rows[j]["y0"] - 2
        # only trim when the figure keeps most of its height, otherwise the
        # "body rows" are really part of the figure (e.g. a wide legend)
        if cut - bbox.y0 >= 0.35 * bbox.height:
            return fitz.Rect(bbox.x0, bbox.y0, bbox.x1, cut)
        return bbox
    return bbox


def _page_h(items, lead_pt, text_w, body):
    """How tall, in points, this page's items will be in the *output*.

    The source geometry is the wrong yardstick here: a block that the source
    set in 3 lines can need 4 once our re-flow has no hyphenation and a
    slightly different measure, and that single extra line is what decides
    whether a page still fits.  Text is therefore estimated from its length
    against an average character width, everything else from its own box.
    """
    cpl = max(20.0, float(text_w) / max(1.0, float(body) * 0.5))
    h = 0.0
    for it in items:
        t = it["t"]
        if t == "block":
            b = it["b"]
            n = max(1, int(math.ceil(len(b.get("text") or "") / cpl)))
            h += n * (lead_pt or float(body) * 1.2)
        elif t == "math":
            m = it["m"]
            h += (float(m["y1"]) - float(m["y0"])) + 6.0
        elif t == "table":
            h += float(it["tbl"]["bbox"].height)
        else:
            h += float(it["fig"]["bbox"].height)
    return h


def convert(pdf_path, docx_path, progress_cb=None):
    """Convert a PDF to a DOCX.

    progress_cb, if given, is called as progress_cb(page_no, page_total) once
    per page so a caller (e.g. a GUI progress bar) can report real progress.
    """
    L = layout.detect(pdf_path)
    gut_l, gut_r = L["gutter"]
    body = float(L["body_size"])
    # line grid of the source, as a Word "multiple" (20/9.9 = 2.02 for a
    # double-spaced report, 12/10 = 1.20 for a normal paper).  Emitting it is
    # what makes the DOCX paginate like the PDF instead of fitting ~1.8x more
    # text on every page.
    lead_pt = float(L.get("leading") or 0)
    if not (body * 1.02 < lead_pt < body * 3.0):
        lead_pt = 0.0
    pw, ph = float(L["page"][0]), float(L["page"][1])
    doc_margin = DOC_MARGIN_PT
    # page margins come from the source's own text frame, not a constant: the
    # frame is what decides how much fits on a page, so matching it is what
    # makes the DOCX paginate like the PDF.  Falling back to the constant keeps
    # documents whose frame could not be measured working.
    fr = L.get("frame")
    if fr and (pw - fr[2]) > 8 and (ph - fr[3]) > 8:
        m_l, m_t = float(fr[0]), float(fr[1])
        m_r, m_b = pw - float(fr[2]), ph - float(fr[3])
    else:
        m_l = m_r = m_t = m_b = doc_margin
    # a Word header has to live in the top margin; give it room when the
    # source's own top margin is too tight for one
    if m_t < 30:
        m_t = 30.0
    text_w = pw - m_l - m_r
    # the source's full text measure (both columns on a two-column paper) --
    # figures are sized against it so they keep their physical size instead of
    # being stretched to whatever measure our re-flow happens to use
    src_w = (float(L["right"]["R"]) - float(L["margin"]["L"])) or text_w

    pdf = fitz.open(pdf_path)
    # Which fonts can travel inside the DOCX?  Only sfnt ones; a CID-keyed CFF
    # (what most Chinese PDFs carry) cannot be represented, and Word then
    # substitutes a serif for the missing CJK family -- a report set in
    # 思源黑体 would come out in 宋体.  Resolving each name to an installed
    # equivalent of the same class is what keeps the document looking right.
    all_fonts = set()
    for pno in range(min(len(pdf), 40)):
        try:
            for _x, _e, _s, base, _r, _enc in pdf.get_page_fonts(pno):
                all_fonts.add(base.split("+")[-1])
        except Exception:
            continue
    embeddable = fonts_mod.collect(pdf, all_fonts)
    font_map = fonts_mod.resolve_map(all_fonts, embeddable)
    writer_mod.reset_fonts(font_map)
    body_font = font_map.get(L["body_fonts"][0], L["body_fonts"][0]) \
        if L.get("body_fonts") else "Times New Roman"

    doc = Document()
    s = doc.sections[0]
    # match the source's paper, not python-docx's US Letter default: an A4
    # report re-flowed onto Letter comes out with a different page count and a
    # visibly different measure, whatever the typography does
    s.page_width, s.page_height = Pt(pw), Pt(ph)
    s.top_margin, s.bottom_margin = Pt(m_t), Pt(m_b)
    s.left_margin, s.right_margin = Pt(m_l), Pt(m_r)
    s.header_distance = Pt(max(12.0, m_t * 0.45))
    s.footer_distance = Pt(max(12.0, m_b * 0.45))
    st = doc.styles["Normal"]
    st.font.name = body_font
    st.font.size = Pt(round(body * 2.0) / 2.0)
    try:
        st.element.rPr.rFonts.set(qn("w:eastAsia"), body_font)
    except Exception:
        pass

    reset_bookmarks()
    svgs_mod.reset()
    # private handle for SVG figure export: its pages get their crop box moved,
    # which must never touch the working document
    side_doc = fitz.open(pdf_path)
    status = ocr.FontStatus(pdf) if ocr.available() else None
    ocr_cache = {}

    def page_tokens(pno0):
        """Tokens for one page, OCR'd when the PDF's own text is unusable."""
        if pno0 in ocr_cache:
            return ocr_cache[pno0]
        page = pdf[pno0]
        w = page_words(page)
        if status is not None and ocr.needs_ocr(pdf, pno0, w, status):
            f_hint, _sz = status.dominant(pno0)
            got = ocr.page_words(page, body, f_hint)
            if got:
                w = got
        ocr_cache[pno0] = w
        return w

    # the running head has to be read from the same source as the body: on a
    # scanned / undecodable-CID document the PDF's own top-band text is garbage
    scan = {}
    if status is not None:
        for i in range(min(len(pdf), 6)):
            if ocr.needs_ocr(pdf, i, page_words(pdf[i]), status):
                scan[i] = [(w["x0"], w["y0"], w["x1"], w["y1"], w["word"])
                           for w in page_tokens(i)]
    # top margin band: anything lying wholly inside it is page furniture.
    # Capped by the measured text margin so a document with a narrow top
    # margin cannot have its first body line swept up by the rule.
    band = min(ph * 0.09, float((L.get("margin") or {}).get("F") or ph * 0.09))
    # lift running heads / page furniture into real Word header & footer
    head_txt, foot_txt = detect_running_heads(pdf, ph, page_lines=scan or None)
    # a logo printed in the corner of every page belongs in the Word header,
    # not in the body -- and once the top band is treated as furniture its
    # OCR'd text is dropped too, so without this the logo vanishes entirely
    logo = detect_running_logo(pdf, ph, band=band)
    apply_header_footer(s, head_txt, foot_txt, body, logo=logo)
    pdf_annots = annots_mod.collect(pdf)

    flow = []
    # Height of the output text frame and how much of it the flow has used
    # since the last page boundary.  Used only to decide whether a source page
    # boundary is also a real boundary in the re-flow (see the break below).
    frame_h = (float(L["frame"][3]) - float(L["frame"][1])
               if L.get("frame") else float(ph) - m_t - m_b)
    used_h = 0.0
    for pno in range(1, len(pdf) + 1):
        page = pdf[pno - 1]
        # a scanned page, or a page whose CID text cannot be decoded, is read
        # from pixels instead: the PDF's own text there is either absent or
        # wrong, and wrong text is worse than no text
        words = page_tokens(pno - 1)
        # locate tables / figures first so their text can be excluded from the
        # body flow *per token*.  Filtering whole paragraphs was wrong once a
        # line spans the gutter: one figure in one column would delete the
        # other column's text too.
        tabs, rejected, text_only = extract_tables(page, gut_l, gut_r, words)
        gfx = [g for g in L["graphics"] if g["page"] == pno]
        # only refused tables that actually contain artwork become pictures;
        # the rest are mis-segmented data listings and keep their words as text.
        # A grid refused because it cannot be set at all (rotated catalogue,
        # sub-character column) keeps its words too -- cropping it would leave
        # its numbers readable only as pixels.
        rejected = [r for r in rejected
                    if _has_art(r, gfx) and not any(r == n for n in text_only)]
        figs = extract_figures(page, gfx, tabs, gut_l, gut_r, rejected,
                               suppress=text_only)
        for f in figs:
            col = f.get("col") or colof_rect((f["bbox"].x0 + f["bbox"].x1) / 2.0,
                                             gut_l, gut_r)
            col_w = float(L["right"].get(col, 0)) - float(L["margin"].get(col, 0))
            # first cut away body prose the box swallowed, then take in the
            # small-type labels printed around the frame (order matters: a
            # label absorbed before the trim could bridge into the prose)
            bb = _trim_figure(f["bbox"], words, col_w, body, L.get("leading"),
                              float(L["margin"].get(col, 0) or 0.0))
            f["bbox"] = _absorb_labels(bb, words, body, col_w)
        excl = [t["bbox"] for t in tabs] + [f["bbox"] for f in figs]
        if excl:
            words = [w for w in words if not _inside(w, excl)]
        lines = build_lines(words, L["gutter"] if L["columns"] == 2 else None)
        # A displayed equation taller than one row cannot survive being
        # linearised: its summation limits, fraction bars and aligned rows are
        # two-dimensional, so the pieces come out as the fragments "2 N K",
        # "N K", "N N" in no meaningful order.  Those regions are lifted out of
        # the text flow here and re-emitted as pictures of the source area,
        # which reproduces the equation exactly.  Single-row formulas stay in
        # the flow, where build_omath() turns them into editable equations.
        math_boxes = []
        try:
            math_boxes = math_mod.math_regions(lines, L)
        except Exception:
            math_boxes = []
        if math_boxes:
            taken = set()
            for r in math_boxes:
                for ln in r["lines"]:
                    taken.add(id(ln))
            lines = [ln for ln in lines if id(ln) not in taken]
        paras = group_paragraphs(lines, L)
        # the lifted-out equations join the page flow as pseudo-paragraphs, so
        # reading_order() places each one where it sat on the page (inside its
        # own column) instead of stacking them all at the end
        for r in math_boxes:
            mx0, my0, mx1, my1 = r["bbox"]
            paras.append(dict(col=r["col"], x0=mx0, y0=my0, x1=mx1, y1=my1,
                              kind="math", text="", size=body, segs=[],
                              lines=[], bbox=r["bbox"]))
        # thin horizontal rules are fraction bars for the math builder
        page_rules = [fitz.Rect(d["rect"]) for d in page.get_drawings()
                      if d["rect"].height < 1.6 and d["rect"].width >= 4]
        page_items = []
        for p in reading_order(paras, L):
            if p.get("kind") == "math":
                page_items.append(dict(t="math", pno=pno, m=p))
                continue
            p["rules"] = [r for r in page_rules
                          if p["x0"] - 6 <= (r.x0 + r.x1) / 2.0 <= p["x1"] + 6
                          and p["y0"] - 4 <= r.y0 <= p["y1"] + 4]
            ptxt = " ".join(p["text"].split())
            if is_furniture(p["text"]):
                continue
            # a per-chapter banner / logo living inside the top margin: it
            # changes per chapter so it never "recurs", but it must not become
            # a heading on every page
            if _band_furniture(p, band, body):
                continue
            # already emitted into the Word header/footer.  Substring matching
            # matters for OCR'd pages: the logo comes back as two separate
            # lines ("安全牛", "AQNIU.NET") while the detected head is the
            # combined string, so equality alone would leave 100+ duplicates.
            # The head is also matched when a page number got glued onto it
            # ("Meteorological data ... 9"): the head is a prefix and the tail
            # is just the folio, so it must be skipped too -- otherwise the
            # running head is duplicated once per page in the body.
            #
            # The size guard is what saves a document's *title*: journals set
            # the running head to "J. Wu et al. : <title>", so the 22pt title on
            # page 1 is a substring of the 8pt head and used to be discarded
            # along with it.  A running head is never set larger than the body,
            # so anything meaningfully bigger cannot be one.
            if (p["size"] <= body * 1.35
                    and (_lifted(ptxt, head_txt) or _lifted(ptxt, foot_txt))):
                continue
            p["pno"] = pno
            page_items.append(dict(t="block", pno=pno, b=p))
        page_items += [dict(t="table", pno=pno, tbl=t) for t in tabs]
        for f in figs:
            f["pno"] = pno
        page_items += [dict(t="figure", pno=pno, fig=f) for f in figs]
        # Single-column pages re-flow top to bottom, so a figure that sits at
        # the top of the source page must stay at the top.  Appending every
        # figure after the page's text pushed top-of-page charts to the bottom
        # and stranded their captions above the prose.  Sorting the page's
        # items by vertical position restores the source order.  Two-column
        # papers keep the old order: a global y-sort there would interleave the
        # two columns' independent flows.
        if L["columns"] == 1 and len(page_items) > 1:
            def _item_y(it):
                if it["t"] == "block":
                    return it["b"]["y0"]
                if it["t"] == "table":
                    return it["tbl"]["bbox"].y0
                if it["t"] == "math":
                    return it["m"]["y0"]
                return it["fig"]["bbox"].y0
            page_items.sort(key=_item_y)
        flow.extend(page_items)
        # Honour the source's page boundary -- but only where it is also a
        # boundary of the re-flow.  A re-flow that ignores it packs two source
        # pages onto one and the document ends up a fifth shorter than the
        # original, which is exactly what "页数对不上" means to a reader
        # comparing the two side by side.  The frame, the line grid and the
        # figure sizes are all measured from the source, so where our re-flow
        # of a page is no taller than the source's, the break pins the two
        # page-for-page.
        #
        # Where it *is* taller, forcing the boundary is what breaks the
        # document: the overrun line goes to a fresh page and the break then
        # closes that page, so every overrunning page costs two.  102 (a maths
        # paper -- our re-flow of its display rows wraps where the original did
        # not) came out at 97 pages against the source's 67 with the boundary
        # forced unconditionally, 72 with it forced only where the page fits,
        # and 69 with no boundaries at all.  Dropping the boundary is the safe
        # direction: the document falls back to a natural re-flow instead of
        # gaining a near-empty page.
        #
        # Two-column papers are excluded on the same grounds.  Their figures
        # are sized for a half-width column, so in a single-column re-flow
        # every figure costs twice the page area it did in the original (039:
        # 8 pages -> 12 naturally, 15 with breaks); they always re-flow longer,
        # so they always re-flow cleanly.
        if pno < len(pdf) and L["columns"] == 1:
            h = _page_h(page_items, lead_pt, text_w, body)
            # two lines of slack: the estimate is a character-count average,
            # and breaking a line early costs a line of white space while
            # breaking a line late costs a whole page
            slack = 2.0 * (lead_pt or float(body) * 1.2)
            if used_h + h + slack <= frame_h:
                flow.append(dict(t="break", pno=pno))
                used_h = 0.0
            else:
                # the page spills: let the renderer place the boundary, and
                # carry the remainder as the fill the next page starts with
                used_h = max(0.0, used_h + h - frame_h)
        if progress_cb is not None:
            progress_cb(pno, len(pdf))

    all_figs = [it for it in flow if it["t"] == "figure"]
    mapping, _used = pair_captions(
        [it["b"] for it in flow if it["t"] == "block"],
        [f["fig"] for f in all_figs])
    # captions that got attached to a figure are re-emitted under the image,
    # so suppress their standalone paragraph to avoid duplication
    paired_cap_ids = set(id(c) for (c, _k) in mapping.values())
    emitted_ids = set()      # figure identities already written
    emitted = []             # (page, bbox, paragraph) for comment anchoring
    _mark_num_groups(flow)
    for it in flow:
        if it["t"] == "block":
            if id(it["b"]) in paired_cap_ids:
                continue
            p = emit_block(doc, it["b"], body, lead_pt)
            if p is not None:
                b = it["b"]
                emitted.append((it["pno"],
                                fitz.Rect(b["x0"], b["y0"], b["x1"], b["y1"]), p))
        elif it["t"] == "break":
            doc.add_page_break()
        elif it["t"] == "table":
            emit_table(doc, it["tbl"], body, text_w)
        elif it["t"] == "math":
            m = it["m"]
            emit_math(doc, pdf[it["pno"] - 1], m["bbox"], text_w, src_w=src_w)
        else:
            f = it["fig"]
            fid = id(f)
            if fid in emitted_ids:
                continue
            emitted_ids.add(fid)
            cap, cap_kind = mapping.get(fid, (None, None))
            emit_figure(doc, pdf[it["pno"] - 1], f, cap, cap_kind, text_w, body,
                        side_doc=side_doc, src_w=src_w)
    annots_mod.attach(doc, pdf_annots, emitted)
    doc.save(docx_path)
    # embed only the fonts the runs really reference -- and only the ones that
    # are sfnt, which is the same set whose names resolve() kept verbatim
    used_fonts = set()
    for it in flow:
        if it["t"] == "block":
            for s in it["b"]["segs"]:
                if s[4]:
                    used_fonts.add(s[4])
    fonts_mod.embed(docx_path, {k: v for k, v in embeddable.items()
                                if k in used_fonts})
    svgs_mod.apply(docx_path)
    side_doc.close()
    pdf.close()
    nblk = sum(1 for it in flow if it["t"] == "block")
    return nblk, len([it for it in flow if it["t"] == "table"]), len(emitted_ids)
