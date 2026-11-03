"""DOCX emission: turn our reconstructed blocks into python-docx content.

python-docx writes OOXML directly (no C++ conversion engine is involved).
Runs carry the original per-span typography (font / size / colour / bold /
italic / super-subscript) for Aspose-level fidelity.
"""
import os
import re
import tempfile

import fitz
from docx.shared import Pt, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.oxml import OxmlElement
from docx.opc.constants import RELATIONSHIP_TYPE as RT
from docx.image.image import Image as DocxImage

from .media import crop
from . import svgs
from . import math as math_mod
from .structure import _NUMITEM

# marker without a required trailing space: '2)' may be its own run
_NUM_MARK = re.compile(r"^(?:\(\d{1,2}\)|\d{1,2}[.)])")

# front-matter lead-ins that are bold-italic inline labels, not banners
_LEAD_INS = ("abstract", "index terms", "key words", "keywords", "summary")

# bookmark ids are document-scoped and must be unique
_BM = {"id": 0, "n": 0}

# numbering instance ids handed out per list group (document-scoped too)
_NUM = {"next": 200}

# PDF font name -> the name Word should actually be asked for.  Set once per
# conversion by the orchestrator (see fonts.resolve); a missing CJK font
# otherwise makes Word substitute a serif and the document stops looking like
# its source.
_FONT = {"map": {}}


def reset_fonts(mapping=None):
    _FONT["map"] = dict(mapping or {})


def _font(name):
    return _FONT["map"].get(name, name) if name else name


def reset_bookmarks():
    _BM["id"] = 0
    _BM["n"] = 0
    _NUM["next"] = 200


def _set_outline(p, level):
    """Give a heading a Word outline level *and* the built-in Heading style.

    The outline level is what makes the navigation pane and a generated TOC
    work; the pStyle is what makes the Styles pane, cross-references and
    "insert TOC" behave the way Word users expect (Aspose does the same).
    Every run still carries its own direct formatting measured from the PDF
    (font / size / colour / bold), and direct formatting beats style
    formatting in OOXML -- so the style never repaints our typography.  The
    one thing a style *can* leak through is colour on runs without an
    explicit w:color, so those get pinned to black (Word's own default).
    """
    pPr = p._p.get_or_add_pPr()
    el = OxmlElement("w:outlineLvl")
    el.set(qn("w:val"), str(level))
    pPr.append(el)
    try:
        p.style = p.part.document.styles["Heading %d" % (level + 1)]
    except Exception:
        pass
    for r in p.runs:
        try:
            if r.font.color.rgb is None:
                r.font.color.rgb = RGBColor(0, 0, 0)
        except Exception:
            pass


def _add_bookmark(p):
    """Wrap a heading in a bookmark so it can be cross-referenced / TOC-linked."""
    _BM["id"] += 1
    _BM["n"] += 1
    bid = str(_BM["id"])
    name = "_Toc%08d" % _BM["n"]
    start = OxmlElement("w:bookmarkStart")
    start.set(qn("w:id"), bid)
    start.set(qn("w:name"), name)
    end = OxmlElement("w:bookmarkEnd")
    end.set(qn("w:id"), bid)
    # w:pPr must stay the first child of w:p, so the bookmark opens after it
    pPr = p._p.find(qn("w:pPr"))
    idx = list(p._p).index(pPr) + 1 if pPr is not None else 0
    p._p.insert(idx, start)
    p._p.append(end)
    return name


def _is_lead_in(tl):
    """True when a block is 'Abstract--text...' style body text rather than a
    standalone banner heading."""
    if len(tl) <= 40:
        return False
    return any(tl.startswith(w) for w in _LEAD_INS)


def _strip_num(segs):
    """Remove the literal list marker ('(1) ', '2. ') from the first run so a
    native Word numbering can supply it.  The marker may be its own run (no
    trailing space) -- only the marker itself plus a whitespace-or-end
    boundary is required.  Returns None when the marker cannot be isolated
    cleanly -- the caller then keeps the plain-text fallback (a paragraph with
    its literal number) instead of risking a doubled marker."""
    if not segs:
        return None
    lead = segs[0][0].lstrip()
    m = _NUMITEM.match(lead)
    if m:
        cut = m.end()
    else:
        # marker alone in this run: '(2)' / '2)' with the text in later runs
        m2 = _NUM_MARK.match(lead)
        if not m2:
            return None
        rest = lead[m2.end():]
        if rest and not rest[0] in " \t":
            return None
        cut = m2.end()
    first = list(segs[0])
    first[0] = lead[cut:]
    out = []
    if first[0]:
        out.append(tuple(first))
    out.extend(tuple(segs[1:]))
    return tuple(out)


def _num_override(doc, start):
    """Create a <w:num> that restarts the List Number sequence at *start*, so
    each detected list group numbers itself exactly like the PDF shows
    (Word would otherwise continue numbering across groups).  Returns the new
    numId, or None when the numbering part is unavailable -- the caller then
    falls back to literal numbers in the text."""
    try:
        st = doc.styles["List Number"]
        vals = st.element.xpath('./w:pPr/w:numPr/w:numId/@w:val')
        if not vals:
            return None
        base = int(vals[0])
        root = doc.part.numbering_part.element
        abs_id = None
        for num in root.findall(qn("w:num")):
            if num.get(qn("w:numId")) == str(base):
                a = num.find(qn("w:abstractNumId"))
                if a is not None:
                    abs_id = a.get(qn("w:val"))
                break
        if abs_id is None:
            return None
        _NUM["next"] += 1
        new_id = _NUM["next"]
        el = OxmlElement("w:num")
        el.set(qn("w:numId"), str(new_id))
        a = OxmlElement("w:abstractNumId")
        a.set(qn("w:val"), str(abs_id))
        el.append(a)
        ov = OxmlElement("w:lvlOverride")
        ov.set(qn("w:ilvl"), "0")
        so = OxmlElement("w:startOverride")
        so.set(qn("w:val"), str(start))
        ov.append(so)
        el.append(ov)
        root.append(el)
        return new_id
    except Exception:
        return None


def _wrap_in_hyperlink(paragraph, run, url):
    """Move an already-styled run under a <w:hyperlink> with an external rel."""
    try:
        r_id = paragraph.part.relate_to(url, RT.HYPERLINK, is_external=True)
    except Exception:
        return
    h = OxmlElement("w:hyperlink")
    h.set(qn("r:id"), r_id)
    r_el = run._r
    parent = r_el.getparent()
    if parent is not None:
        idx = list(parent).index(r_el)   # keep the run's original position
        parent.remove(r_el)
        h.append(r_el)
        parent.insert(idx, h)
    else:
        h.append(r_el)
        paragraph._p.append(h)
    run.font.underline = True


def _pt(value):
    """Points snapped to Word's own resolution.

    OOXML stores font size in half-points (``w:sz``) and python-docx truncates
    when converting, so ``Pt(9.9)`` is written as 9.5pt and ``Pt(21.9)`` as
    21.5pt -- every run ends up 0.4pt smaller than the source.  Rounding to the
    nearest half-point keeps the error at 0.05pt, which is the best Word can
    represent anyway.
    """
    return Pt(round(float(value) * 2.0) / 2.0)


def add_runs(p, segs, scale, body):
    """Emit runs preserving the original per-span typography: font name, size
    (scaled relative to the paragraph role), RGB colour and bold/italic from
    the PDF.  `scale` applies the relative shrink used by captions / abstracts
    while keeping each run's own size ratio (Aspose-depth fidelity)."""
    for text, it, bo, vert, font, size, color, link in segs:
        if text == " ":
            p.add_run(" ")
            continue
        r = p.add_run(text)
        if size:
            r.font.size = _pt(size * scale)
        else:
            r.font.size = _pt(body * scale)
        font = _font(font)
        if font:
            r.font.name = font
            try:
                r._element.rPr.rFonts.set(qn("w:eastAsia"), font)
            except Exception:
                pass
        if it:
            r.italic = True
        if bo:
            r.bold = True
        if color:
            r.font.color.rgb = RGBColor((color >> 16) & 255,
                                       (color >> 8) & 255, color & 255)
        if vert == "sup":
            r.font.superscript = True
        elif vert == "sub":
            r.font.subscript = True
        if link:
            _wrap_in_hyperlink(p, r, link)


def emit_block(doc, b, body, lead=None):
    kind = b["kind"]
    segs = b["segs"]
    tl = b["text"].strip().lower()
    # heading alignment is taken from the PDF geometry (see structure.heading_align)
    align = (WD_ALIGN_PARAGRAPH.CENTER if b.get("halign") == "center"
             else WD_ALIGN_PARAGRAPH.LEFT)

    # a block that scores as a displayed equation becomes a native Word
    # equation object (editable in the equation editor) -- strictly beyond
    # Aspose, which keeps math as plain text.  Anything the builder cannot
    # structure falls through to the styled-run path below.
    if math_mod.is_formula(b, body):
        try:
            omath = math_mod.build_omath(b, b.get("rules") or ())
        except Exception:
            omath = None
        if omath is not None:
            p = doc.add_paragraph()
            p.alignment = WD_ALIGN_PARAGRAPH.CENTER
            p.paragraph_format.space_before = Pt(3)
            p.paragraph_format.space_after = Pt(3)
            p._p.append(omath)
            return p

    # a numbered list item that belongs to a detected consecutive group becomes
    # a native Word numbered list (Aspose-level behaviour); isolated or
    # non-consecutive numbers keep their literal text as ordinary paragraphs
    if kind == "numitem" and b.get("numgroup") is not None:
        stripped = _strip_num(segs)
        if stripped:
            p = doc.add_paragraph(style="List Number")
            add_runs(p, stripped, 1.0, body)
            nid = _num_override(doc, b.get("numval", 1))
            if nid is not None:
                pPr = p._p.get_or_add_pPr()
                numPr = OxmlElement("w:numPr")
                ilvl = OxmlElement("w:ilvl")
                ilvl.set(qn("w:val"), "0")
                numIdEl = OxmlElement("w:numId")
                numIdEl.set(qn("w:val"), str(nid))
                numPr.append(ilvl)
                numPr.append(numIdEl)
                ps = pPr.find(qn("w:pStyle"))
                pPr.insert(list(pPr).index(ps) + 1 if ps is not None else 0,
                           numPr)
            return p

    if kind == "h1":
        p = doc.add_paragraph()
        p.alignment = align
        p.paragraph_format.space_before = Pt(14)
        p.paragraph_format.space_after = Pt(7)
        p.paragraph_format.keep_with_next = True
        add_runs(p, segs, 1.0, body)
        # keep the source's own emphasis; only synthesise bold when the PDF
        # carries no emphasis at all (otherwise we would over-bold headings)
        if not any(r.bold for r in p.runs):
            for r in p.runs:
                r.bold = True
        _set_outline(p, 0)
        _add_bookmark(p)
        return p
    if kind == "p" and _is_lead_in(tl):
        # inline "Abstract--..." / "Index Terms--..." is body text, not a banner
        p = doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY
        p.paragraph_format.space_after = Pt(6)
        add_runs(p, segs, 1.0, body)
        if p.runs:
            lead = p.runs[0].text.strip().rstrip(":-\u2014\u2013").lower()
            if lead in _LEAD_INS:
                p.runs[0].bold = True
                p.runs[0].italic = True
        return p
    if kind == "bullet":
        p = doc.add_paragraph(style="List Bullet")
        add_runs(p, segs, 1.0, body)
        return p
    if kind == "h2":
        p = doc.add_paragraph()
        p.alignment = align
        p.paragraph_format.space_before = Pt(9)
        p.paragraph_format.space_after = Pt(4)
        p.paragraph_format.keep_with_next = True
        add_runs(p, segs, 1.0, body)
        # subsection headings are italic in most templates; respect the source
        if not any(r.italic for r in p.runs):
            for r in p.runs:
                r.italic = True
        _set_outline(p, 1)
        _add_bookmark(p)
        return p
    if kind in ("tcaption", "fcaption", "acaption"):
        p = doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        add_runs(p, segs, 0.8, body)
        return p
    if kind == "ref":
        p = doc.add_paragraph()
        p.paragraph_format.left_indent = Pt(18)
        p.paragraph_format.first_line_indent = Pt(-18)
        add_runs(p, segs, 0.8, body)
        return p
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.JUSTIFY
    p.paragraph_format.first_line_indent = Pt(body * 1.2)
    p.paragraph_format.space_after = Pt(0)
    # reproduce the source's line grid exactly.  A *multiple* would be
    # interpreted against Word's own single-line height (about 1.18 x the font
    # size), so "2.02" would come out at 23.8pt instead of the source's 20pt;
    # an exact point value means Word, LibreOffice and our own renderer all
    # lay the page out the same way.
    if lead:
        p.paragraph_format.line_spacing = Pt(round(float(lead), 2))
    add_runs(p, segs, 1.0, body)
    return p


def _mark_header_row(row):
    """Repeat this row at the top of every page the table spans."""
    tr = row._tr
    trPr = tr.get_or_add_trPr()
    el = OxmlElement("w:tblHeader")
    el.set(qn("w:val"), "true")
    trPr.append(el)


def emit_table(doc, t, body=10.0, text_w=None):
    tbl = doc.add_table(rows=t["nrows"], cols=t["ncols"])
    tbl.style = "Table Grid"
    content = t.get("content") or []
    merges = t.get("merges", [])
    # cells swallowed by a merge must stay empty, otherwise python-docx appends
    # their text into the merged cell and the content appears duplicated
    covered = set()
    for (r0, c0, r1, c1) in merges:
        for r in range(r0, r1):
            for c in range(c0, c1):
                if (r, c) != (r0, c0):
                    covered.add((r, c))
    for i, row in enumerate(t["cells"]):
        for j in range(t["ncols"]):
            if (i, j) in covered:
                continue
            cell = tbl.cell(i, j)
            segs_list = content[i][j] if content and i < len(content) \
                and j < len(content[i]) else None
            if segs_list:
                # keep the cell's original typography instead of flattening it
                for k, segs in enumerate(segs_list):
                    p = cell.paragraphs[0] if k == 0 else cell.add_paragraph()
                    p.paragraph_format.space_after = Pt(0)
                    add_runs(p, segs, 1.0, body)
            else:
                cell.text = row[j] if j < len(row) else ""
    # apply detected merged regions (horizontal gridSpan / vertical vMerge)
    for (r0, c0, r1, c1) in merges:
        try:
            tbl.cell(r0, c0).merge(tbl.cell(r1 - 1, c1 - 1))
        except Exception:
            pass
    # column widths from the PDF's own grid, scaled into the DOCX text width
    edges = t.get("edges")
    if edges and len(edges) == t["ncols"] + 1 and text_w:
        total = edges[-1] - edges[0]
        scale = min(1.0, float(text_w) / total) if total > 0 else 1.0
        try:
            tbl.autofit = False
            for j in range(t["ncols"]):
                w = Pt((edges[j + 1] - edges[j]) * scale)
                tbl.columns[j].width = w
                for row in tbl.rows:
                    row.cells[j].width = w
        except Exception:
            pass
    if t["nrows"] > 1:
        try:
            _mark_header_row(tbl.rows[0])
        except Exception:
            pass
    doc.add_paragraph()


def _original_image(page, rect):
    """Bytes of the *original* embedded image when one bitmap dominates the
    figure region -- extracting the source beats re-rendering it at 200 dpi
    (no double compression, original resolution, exactly what Aspose does)."""
    area = rect.get_area()
    if area <= 0:
        return None
    try:
        infos = page.get_image_info(xrefs=True)
    except Exception:
        return None
    best = None
    for info in infos:
        r = fitz.Rect(info["bbox"])
        if not (info.get("xref", 0) > 0 and r.get_area() > 0):
            continue
        inter = (r & rect).get_area()
        if inter >= 0.92 * area and (best is None or inter > best[1]):
            best = (info["xref"], inter)
    if best is None:
        return None
    try:
        img = page.parent.extract_image(best[0])
        if img and img.get("image"):
            blob = img["image"]
            # A figure's transparency lives in a separate /SMask, not in the
            # base bitmap: extract_image() hands back the base alone, whose
            # "transparent" pixels are stored as solid black -- so a chart on a
            # white page arrived in Word as a black rectangle with the plot
            # invisible.  Compositing the mask restores the real appearance.
            smask = img.get("smask") or 0
            if smask:
                try:
                    base = fitz.Pixmap(page.parent, best[0])
                    mask = fitz.Pixmap(page.parent, smask)
                    if mask.n != 1:
                        mask = fitz.Pixmap(mask, 1)
                    blob = fitz.Pixmap(base, mask).tobytes("png")
                    img = dict(img, image=blob, ext="png")
                except Exception:
                    pass
            return img
    except Exception:
        pass
    return None


def emit_figure(doc, page, f, caption, cap_kind, text_w, body, side_doc=None,
                src_w=None):
    orig = _original_image(page, f["bbox"])
    if orig:
        fd, path = tempfile.mkstemp(suffix="." + orig.get("ext", "png"),
                                    prefix="fig_")
        with os.fdopen(fd, "wb") as fh:
            fh.write(orig["image"])
        try:
            # header probe -- python-docx rejects CMYK-JPEG / JPX etc. that
            # Word itself also often mishandles; fall back to a clean render
            DocxImage.from_file(path)
            w, h = f["bbox"].width, f["bbox"].height
        except Exception:
            try:
                os.remove(path)
            except Exception:
                pass
            path, w, h = crop(page, f["bbox"], 200, "fig_")
    else:
        path, w, h = crop(page, f["bbox"], 200, "fig_")
    if cap_kind == "table" and caption:
        p = doc.add_paragraph()
        p.alignment = WD_ALIGN_PARAGRAPH.CENTER
        add_runs(p, caption["segs"], 0.8, body)
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    # Size the picture the way the source did: relative to the *source's* text
    # measure, not to ours.  Stretching every figure to fill our measure
    # (`min(text_w / w, 1.6)`) blew a two-column paper apart -- a half-column
    # figure 250pt wide became 487pt, so its height doubled and an 8-page paper
    # re-flowed onto 16 pages.  Scaling by the measure ratio keeps the picture
    # the same physical size on the page whatever the column arrangement, and
    # the clamp keeps a full-bleed figure inside the margin.
    scale = (float(text_w) / float(src_w)) if src_w else 1.0
    width = min(w * scale, float(text_w))
    r = p.add_run()
    r.add_picture(path, width=Pt(width))
    # every figure additionally rides along as SVG (Word 2016+ draws the SVG,
    # everything else falls back to the PNG).  Regions containing bitmaps are
    # fine too -- MuPDF embeds the pixels inside the SVG -- the size cap in
    # export_svg is what keeps pathological drawings out.
    if side_doc is not None:
        svg = svgs.export_svg(side_doc, page.number, f["bbox"])
        if svg:
            svgs.register(r, svg)
    try:
        os.remove(path)
    except Exception:
        pass
    if cap_kind != "table" and caption:
        p = doc.add_paragraph()
        add_runs(p, caption["segs"], 0.8, body)


def emit_math(doc, page, bbox, text_w, src_w=None, dpi=300):
    """Place a displayed equation as a picture of the source region.

    A tall equation -- stacked summation limits, a fraction bar, several
    aligned rows -- cannot be rebuilt from glyph runs without destroying it,
    so the page's own rendering is kept.  Cropping at 300 dpi keeps the
    strokes crisp at body size, and the width is scaled by the measure ratio
    exactly like a figure so the equation keeps its physical size and its
    type stays matched to the surrounding text.
    """
    rect = fitz.Rect(*bbox)
    if rect.is_empty:
        return None
    path, w, h = crop(page, rect, dpi, "math_")
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    p.paragraph_format.space_before = Pt(3)
    p.paragraph_format.space_after = Pt(3)
    scale = (float(text_w) / float(src_w)) if src_w else 1.0
    width = min(w * scale, float(text_w))
    r = p.add_run()
    try:
        r.add_picture(path, width=Pt(width))
    except Exception:
        pass
    finally:
        try:
            os.remove(path)
        except Exception:
            pass
    return p


def pair_captions(paras, figures):
    caps = [p for p in paras if p["kind"] in ("tcaption", "fcaption")]
    used, mapping = set(), {}
    for f in figures:
        b = f["bbox"]
        fp = f.get("pno")
        best = None
        for c in caps:
            if id(c) in used:
                continue
            # A caption belongs to a figure on its own page.  Without this
            # check the bbox test below pairs across pages (page 9's chart
            # grabbed page 11's "图3" caption because the coordinates lined up).
            if (fp is not None and c.get("pno") is not None
                    and c["pno"] != fp):
                continue
            if c["kind"] == "tcaption":
                d = b.y0 - c["y1"]
                if not (-10 <= d <= 95 and abs(c["x0"] - b.x0) < 260):
                    continue
                kind = "table"
            else:
                d = c["y0"] - b.y1
                if not (-12 <= d <= 95 and abs(c["x0"] - b.x0) < 320):
                    continue
                kind = "figure"
            if best is None or d < best[0]:
                best = (d, kind, c)
        if best:
            used.add(id(best[2]))
            mapping[id(f)] = (best[2], best[1])
    return mapping, used
