"""PDF -> PPTX: one slide per PDF page, native editable shapes.

Two modes:

``editable`` (default)
    The page is rebuilt with real PowerPoint objects, absolutely positioned on
    the slide exactly where the PDF put them: figures become pictures, tables
    become native pptx tables (merged cells included), text becomes text
    boxes with run-level font / size / bold / italic / colour.  This is the
    Aspose-grade path -- the slide is editable, not a picture of a page.

``image``
    Each page is rendered at *dpi* and placed full-slide.  Pixel fidelity is
    total, editability is zero -- the fallback for pathological pages.

Slide size equals the PDF page size in points, so every PDF coordinate maps
1:1 onto the slide (both are 1/72 inch).
"""
import io
import os

import fitz
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.text import MSO_ANCHOR, MSO_AUTO_SIZE, PP_ALIGN
from pptx.util import Pt

from . import layout, media, ocr
from .convert import _absorb_labels, _has_art, _inside, _trim_figure
from .headers import detect_running_heads
from .structure import group_paragraphs
from .text import build_lines, page_words


def _set_run(run, text, seg, body):
    """One styled run: keep the PDF's font name / size / colour / emphasis."""
    _t, it, bo, vert, font, size, color, _link = seg
    run.text = text
    try:
        run.font.size = Pt(size or body)
        if font:
            run.font.name = font
        run.font.bold = bool(bo)
        run.font.italic = bool(it)
        if color:
            run.font.color.rgb = RGBColor((color >> 16) & 255,
                                          (color >> 8) & 255, color & 255)
    except Exception:
        pass


def _place_text(slide, b, body):
    x0, y0, x1, y1 = b["x0"], b["y0"], b["x1"], b["y1"]
    w, h = max(4.0, x1 - x0), max(6.0, y1 - y0)
    tb = slide.shapes.add_textbox(Pt(x0), Pt(y0), Pt(w), Pt(h))
    tf = tb.text_frame
    tf.word_wrap = True
    tf.auto_size = MSO_AUTO_SIZE.NONE
    tf.margin_left = tf.margin_right = 0
    tf.margin_top = tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.alignment = (PP_ALIGN.CENTER if b.get("halign") == "center"
                   else PP_ALIGN.LEFT)
    # exact single spacing: the PDF's line height is already baked into each
    # block's box, so PowerPoint's default 1.2x leading would overflow it
    p.line_spacing = 1.0
    p.space_before = Pt(0)
    p.space_after = Pt(0)
    for seg in b["segs"]:
        if not seg[0]:
            continue
        _set_run(p.add_run(), seg[0], seg, body)
    return tb


def _place_table(slide, t, body):
    bbox = t["bbox"]
    nrows, ncols = t["nrows"], t["ncols"]
    shp = slide.shapes.add_table(nrows, ncols, Pt(bbox.x0), Pt(bbox.y0),
                                 Pt(max(8.0, bbox.width)),
                                 Pt(max(8.0, bbox.height)))
    tbl = shp.table
    edges = t.get("edges")
    if edges and len(edges) == ncols + 1:
        for j in range(ncols):
            tbl.columns[j].width = Pt(max(6.0, float(edges[j + 1]) -
                                          float(edges[j])))
    fsize = max(5.0, min(body * 0.85, body))
    for i, row in enumerate(t["cells"]):
        for j in range(ncols):
            cell = tbl.cell(i, j)
            cell.margin_left = cell.margin_right = 0
            cell.margin_top = cell.margin_bottom = 0
            cell.vertical_anchor = MSO_ANCHOR.TOP
            txt = row[j] if j < len(row) else ""
            tf = cell.text_frame
            tf.word_wrap = True
            p = tf.paragraphs[0]
            r = p.add_run()
            r.text = txt
            r.font.size = Pt(fsize)
    for (r0, c0, r1, c1) in t.get("merges") or []:
        if r1 - r0 > 1 or c1 - c0 > 1:
            try:
                tbl.cell(r0, c0).merge(tbl.cell(r1 - 1, c1 - 1))
            except Exception:
                pass
    return shp


def _place_figure(slide, page, f, orig_bytes=None):
    bbox = f["bbox"]
    stream = None
    if orig_bytes:
        stream = io.BytesIO(orig_bytes)
    else:
        from .media import crop
        path, _w, _h = crop(page, bbox, 150, "ppt_")
        with open(path, "rb") as fh:
            stream = io.BytesIO(fh.read())
        try:
            os.remove(path)
        except Exception:
            pass
    try:
        slide.shapes.add_picture(stream, Pt(bbox.x0), Pt(bbox.y0),
                                 Pt(max(6.0, bbox.width)),
                                 Pt(max(6.0, bbox.height)))
    except Exception:
        pass


def _original_bytes(page, rect):
    """Embedded source bitmap for a figure region, when one dominates it."""
    from .writer import _original_image
    img = _original_image(page, rect)
    return img.get("image") if img else None


def _norm(s):
    return " ".join((s or "").split()).lower()


def _strip_furniture(lines, head, foot):
    """Drop the lines that make up the running head / foot: they are placed
    once per slide as grey furniture text, so keeping them in the body would
    duplicate them."""
    hs, fs = _norm(head), _norm(foot)
    if not hs and not fs:
        return lines
    out = []
    for ln in lines:
        t = _norm(ln.get("text"))
        if not t:
            continue
        if (hs and (t in hs or hs in t)) or (fs and (t in fs or fs in t)):
            continue
        out.append(ln)
    return out


def convert_pptx(pdf_path, pptx_path, mode="editable", dpi=150,
                 progress_cb=None):
    """Convert one PDF into a slide deck.  Returns (slides, pictures, tables).

    progress_cb(page_no, page_total) is called once per page when supplied.
    """
    L = layout.detect(pdf_path)
    gut_l, gut_r = L["gutter"]
    body = float(L["body_size"])
    pw, ph = float(L["page"][0]), float(L["page"][1])
    pdf = fitz.open(pdf_path)
    prs = Presentation()
    prs.slide_width = Pt(pw)
    prs.slide_height = Pt(ph)
    blank = prs.slide_layouts[6]
    # pages whose CID text cannot be decoded must be read from pixels, exactly
    # like the DOCX path does -- otherwise a scanned deck carries garbage text
    status = ocr.FontStatus(pdf) if ocr.available() else None

    def page_tokens(pno0):
        page = pdf[pno0]
        w = page_words(page)
        if status is not None and ocr.needs_ocr(pdf, pno0, w, status):
            f_hint, _sz = status.dominant(pno0)
            got = ocr.page_words(page, body, f_hint)
            if got:
                w = got
        return w

    scan = {}
    if status is not None:
        for i in range(min(len(pdf), 6)):
            if ocr.needs_ocr(pdf, i, page_words(pdf[i]), status):
                scan[i] = [(w["x0"], w["y0"], w["x1"], w["y1"], w["word"])
                           for w in page_tokens(i)]
    head_txt, foot_txt = detect_running_heads(pdf, ph, page_lines=scan or None)

    npics = ntabs = 0
    for pno in range(len(pdf)):
        page = pdf[pno]
        if progress_cb is not None:
            progress_cb(pno + 1, len(pdf))
        slide = prs.slides.add_slide(blank)
        if mode == "image":
            pix = page.get_pixmap(dpi=dpi)
            slide.shapes.add_picture(io.BytesIO(pix.tobytes("png")), 0, 0,
                                     Pt(pw), Pt(ph))
            continue
        words = page_tokens(pno)
        tabs, rejected, text_only = media.extract_tables(page, gut_l, gut_r,
                                                         words)
        gfx = [g for g in L["graphics"] if g["page"] == pno + 1]
        rejected = [r for r in rejected
                    if _has_art(r, gfx) and not any(r == n for n in text_only)]
        figs = media.extract_figures(page, gfx, tabs, gut_l, gut_r, rejected,
                                     suppress=text_only)
        for f in figs:
            col = f.get("col") or "F"
            col_w = float(L["right"].get(col, pw)) - \
                float(L["margin"].get(col, 0))
            bb = _trim_figure(f["bbox"], words, col_w, body)
            f["bbox"] = _absorb_labels(bb, words, body, col_w)
        excl = [t["bbox"] for t in tabs] + [f["bbox"] for f in figs]
        if excl:
            words = [w for w in words if not _inside(w, excl)]
        for t in tabs:
            _place_table(slide, t, body)
            ntabs += 1
        for f in figs:
            _place_figure(slide, page, f, _original_bytes(page, f["bbox"]))
            npics += 1
        lines = _strip_furniture(build_lines(words), head_txt, foot_txt)
        for b in group_paragraphs(lines, L):
            _place_text(slide, b, body)
        # running head / foot as small grey text, once per slide
        if head_txt:
            _furniture(slide, head_txt, ph * 0.035, 10.0, pw * 0.86, pw)
        if foot_txt:
            _furniture(slide, foot_txt, ph * 0.955, 10.0, pw * 0.86, pw)

    pdf.close()
    d = os.path.dirname(os.path.abspath(pptx_path))
    if d:
        os.makedirs(d, exist_ok=True)
    prs.save(pptx_path)
    return len(prs.slides), npics, ntabs


def _furniture(slide, text, top, size, width, page_w):
    """Small grey running head / foot line, centred on the slide."""
    try:
        tb = slide.shapes.add_textbox(Pt((page_w - width) / 2.0), Pt(top),
                                      Pt(width), Pt(size * 1.6))
    except Exception:
        return
    tf = tb.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = 0
    tf.margin_top = tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    r = p.add_run()
    r.text = text
    r.font.size = Pt(size * 0.72)
    r.font.color.rgb = RGBColor(0x80, 0x80, 0x80)
