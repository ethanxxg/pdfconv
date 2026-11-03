"""PPTX -> PDF: our own slide layout engine.  No LibreOffice, no Aspose.

python-pptx reads the deck (shape tree, geometry, styles, tables, pictures);
each slide is then laid out here and drawn with PyMuPDF primitives.  Slide
size and PDF page size are both expressed in 1/72 inch, so a shape's EMU
coordinates divide by 12700 and land exactly where PowerPoint puts them.

Supported:

* slide size, solid slide background, z-order (the shape tree order)
* text boxes and autoshapes: insets, word wrap, horizontal alignment,
  vertical anchor (top / middle / bottom), paragraph line spacing, space
  before/after, run-level font family, size, bold, italic, colour, and
  bullet markers (``a:buChar`` / ``a:buAutoNum``)
* shape geometry: rectangle / rounded rectangle / ellipse / line, with solid
  fill and stroke (width + colour)
* tables: column widths, row heights, merged cells (origin + spanned),
  cell fills, per-side borders from ``a:lnL/lnR/lnT/lnB`` (a hairline when the
  table style is implicit), cell text with the same flow engine
* pictures: the embedded image blob, placed at the shape rectangle
* groups: children are mapped through the group's ``chOff``/``chExt``
  transform, so nested coordinates come out right

Text that needs slightly more room than its box is shrunk to fit (down to
80%, then clipped) instead of spilling over the neighbouring shape.
"""
import os

import fitz
from pptx import Presentation
from pptx.enum.dml import MSO_FILL
from pptx.enum.shapes import MSO_SHAPE_TYPE
from pptx.oxml.ns import qn

from . import measure
from .docx2pdf import _font, font_key, text_width

EMU_PT = 12700.0
DEF_SIZE = 18.0            # PowerPoint's default body size
DEF_BORDER = ((0.55, 0.55, 0.55), 0.5)


# --------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------
def _pt(v):
    try:
        return float(v) / EMU_PT
    except (TypeError, ValueError):
        return 0.0


def _rgb(color, default=None):
    try:
        s = str(color).strip().lstrip("#")
    except Exception:
        return default
    if len(s) != 6:
        return default
    try:
        return (int(s[0:2], 16) / 255.0, int(s[2:4], 16) / 255.0,
                int(s[4:6], 16) / 255.0)
    except ValueError:
        return default


def _fill(shape):
    try:
        f = shape.fill
        if f.type == MSO_FILL.SOLID:
            return _rgb(f.fore_color.rgb)
    except Exception:
        pass
    return None


def _stroke(shape):
    try:
        ln = shape.line
        if ln.fill.type != MSO_FILL.SOLID:
            return None
        col = _rgb(ln.color.rgb)
        if col is None:
            return None
        w = _pt(ln.width) if ln.width else 0.75
        return (col, max(0.25, min(12.0, w)))
    except Exception:
        return None


def _has_cjk(text):
    return any(0x2E80 <= ord(c) <= 0xFFEF for c in text)


# --------------------------------------------------------------------------
# text flow
# --------------------------------------------------------------------------
def _atoms(par, default):
    """[(text, key, size, colour, bold)] for one paragraph."""
    out = []
    for run in par.runs:
        t = run.text
        if not t:
            continue
        f = run.font
        size = _pt(f.size) if f.size else default
        key = font_key(f.name, f.bold, f.italic)
        col = _rgb(f.color.rgb, (0, 0, 0)) if f.color and \
            f.color.type is not None else (0, 0, 0)
        out.append((t, key, size, col, bool(f.bold)))
    return out


def _tokens(atoms):
    """Split styled atoms into word / CJK-glyph tokens that keep their style."""
    out = []
    for text, key, size, col, bold in atoms:
        buf = ""
        for ch in text:
            cjk = 0x2E80 <= ord(ch) <= 0xFFEF
            if cjk or ch.isspace():
                if buf:
                    out.append((buf, key, size, col, bold))
                    buf = ""
                out.append((ch, key, size, col, bold))
            else:
                buf += ch
        if buf:
            out.append((buf, key, size, col, bold))
    return out


def _lines(tokens, avail, indent=0.0):
    """Greedy line breaking over styled tokens -> [[(tok, x_offset), ...]].

    Delegates to :mod:`pdfconv.measure` so PowerPoint slides, Excel sheets and
    Word pages all break lines with the same rules."""
    out = []
    for ln in measure.wrap(tokens, avail, indent):
        out.append([(tok, x) for tok, x, _w in ln])
    return out


class _Writer:
    """Text writer bound to one page.

    ``TextWriter.color`` is a property of the writer rather than of each
    append -- ``write_text`` paints everything with the colour current at that
    moment -- so a new writer is started whenever the colour changes.
    """

    def __init__(self, page):
        self.page = page
        self.tw = None
        self.color = None

    def put(self, x, baseline, text, key, size, color):
        if not text.strip():
            return
        if self.tw is None or self.color != color:
            self.flush()
            self.tw = fitz.TextWriter(self.page.rect, color=color)
            self.color = color
        fkey = "china-s" if _has_cjk(text) else key
        try:
            self.tw.append((x, baseline), text, font=_font(fkey),
                           fontsize=size)
        except Exception:
            try:
                self.page.insert_text((x, baseline), text, fontname=fkey,
                                      fontsize=size, color=color)
            except Exception:
                pass

    def flush(self):
        if self.tw is None:
            return
        try:
            self.tw.write_text(self.page, color=self.color)
        except Exception:
            pass
        self.tw = None
        self.color = None


def _par_marker(par):
    """Bullet / number marker for a paragraph, or ''."""
    pPr = par._p.find(qn("a:pPr"))
    if pPr is None:
        return ""
    if pPr.find(qn("a:buNone")) is not None:
        return ""
    ch = pPr.find(qn("a:buChar"))
    if ch is not None:
        return ch.get("char") or ""
    if pPr.find(qn("a:buAutoNum")) is not None:
        return "\u2022"
    return ""


def _flow(tf, default, avail, scale=1.0, wrap=True):
    """Lay the frame's paragraphs out at *scale* -> (blocks, total height)."""
    blocks = []
    for par in tf.paragraphs:
        atoms = _atoms(par, default)
        marker = _par_marker(par)
        if marker:
            atoms = [(marker + " ", "helv", default, (0, 0, 0), False)] + atoms
        if not atoms:
            blocks.append(([], 0.0, None, 0.0, 0.0))
            continue
        if scale != 1.0:
            atoms = [(t, k, s * scale, c, b) for t, k, s, c, b in atoms]
        toks = _tokens(atoms)
        size0 = max(a[2] for a in atoms)
        ls = par.line_spacing
        lead = size0 * 1.2
        if isinstance(ls, float):
            lead = size0 * 1.2 * ls
        elif ls is not None:
            lead = _pt(ls) or lead
        before = (_pt(par.space_before) if par.space_before else 0.0) * scale
        after = (_pt(par.space_after) if par.space_after else 0.0) * scale
        lines = _lines(toks, avail) if wrap else [[(t, 0.0) for t in toks]]
        blocks.append((lines, lead, par.alignment, before, after))
    total = 0.0
    for lines, lead, _al, before, after in blocks:
        total += before + max(1, len(lines)) * lead + after
    return blocks, total


def _draw_frame(w, tf, x0, y0, wd, ht, default, wrap=True, fit=True):
    """Lay a text frame into the box (x0, y0, wd, ht); returns the used height.

    With *fit*, a frame whose text needs a little more room than the box is
    shrunk (PowerPoint's "shrink text on overflow").  The floor is 0.80: past
    that the type stops being readable, so the frame keeps 80% and the rest is
    clipped at the box bottom instead of spilling over the neighbouring shape.
    """
    ml = _pt(tf.margin_left or 0)
    mr = _pt(tf.margin_right or 0)
    mt = _pt(tf.margin_top or 0)
    mb = _pt(tf.margin_bottom or 0)
    avail = max(2.0, wd - ml - mr)
    room = max(0.0, ht - mt - mb)
    blocks, total = _flow(tf, default, avail, 1.0, wrap)
    if fit and room > 1.0 and total > room:
        k = max(0.80, room / total)
        blocks, total = _flow(tf, default, avail, k, wrap)
    anchor = None
    try:
        anchor = tf.vertical_anchor
    except Exception:
        anchor = None
    y = y0 + mt
    if anchor is not None and int(getattr(anchor, "value", 0)) == 3:      # middle
        y = y0 + (ht - total) / 2.0
    elif anchor is not None and int(getattr(anchor, "value", 0)) == 4:    # bottom
        y = y0 + ht - mb - total
    bottom = y0 + ht + 0.5
    for lines, lead, al, before, after in blocks:
        y += before
        if not lines:
            y += lead
            continue
        for ln in lines:
            if y > bottom:
                return y - y0
            size0 = max(t[0][2] for t in ln) if ln else default
            width = sum(text_width(t[0][0], t[0][1], t[0][2]) for t in ln)
            al_v = int(getattr(al, "value", 1)) if al is not None else 1
            dx = 0.0
            if al_v == 2:                     # centre
                dx = (avail - width) / 2.0
            elif al_v == 3:                   # right
                dx = avail - width
            base = y + size0 * 0.86
            for (text, key, size, col, _b), ox in ln:
                w.put(x0 + ml + dx + ox, base, text, key, size, col)
            y += lead
        y += after
    return y - y0


# --------------------------------------------------------------------------
# tables
# --------------------------------------------------------------------------
def _cell_border(cell, side):
    """(colour, width) for one side of a table cell."""
    tcPr = cell._tc.find(qn("a:tcPr"))
    if tcPr is None:
        return DEF_BORDER
    ln = tcPr.find(qn("a:" + side))
    if ln is None:
        return DEF_BORDER
    fill = ln.find(qn("a:solidFill"))
    if fill is None:
        return None
    col = None
    for tag in ("a:srgbClr", "a:schemeClr", "a:sysClr"):
        el = fill.find(qn(tag))
        if el is not None:
            if tag == "a:srgbClr":
                col = _rgb(el.get("val"))
            break
    w = _pt(ln.get("w")) if ln.get("w") else 0.75
    return (col or (0.4, 0.4, 0.4), max(0.25, min(8.0, w)))


def _row_heights(tbl, cols, given, default):
    """Grow rows so no cell has to clip its text (PowerPoint does the same)."""
    out = list(given)
    for i, row in enumerate(tbl.rows):
        need = 0.0
        for j, cell in enumerate(row.cells):
            try:
                if cell.is_spanned:
                    continue
                if cell.span_height > 1:
                    continue
            except Exception:
                pass
            tf = cell.text_frame
            if not tf.text.strip():
                continue
            ml = _pt(tf.margin_left or 0)
            mr = _pt(tf.margin_right or 0)
            mt = _pt(tf.margin_top or 0)
            mb = _pt(tf.margin_bottom or 0)
            avail = max(2.0, cols[j] - ml - mr)
            _blocks, total = _flow(tf, default, avail)
            need = max(need, total + mt + mb)
        if need > out[i]:
            out[i] = need
    return out


def _draw_table(page, w, shape, default):
    tbl = shape.table
    x0, y0 = _pt(shape.left), _pt(shape.top)
    cols = [_pt(c.width) for c in tbl.columns]
    rows = _row_heights(tbl, cols, [_pt(r.height) for r in tbl.rows], default)
    xs = [x0]
    for c in cols:
        xs.append(xs[-1] + c)
    ys = [y0]
    for r in rows:
        ys.append(ys[-1] + r)
    spans, skip = {}, set()
    for i, row in enumerate(tbl.rows):
        for j, cell in enumerate(row.cells):
            try:
                if cell.is_merge_origin:
                    spans[(i, j)] = (i + cell.span_height - 1,
                                     j + cell.span_width - 1)
                if cell.is_spanned:
                    skip.add((i, j))
            except Exception:
                pass
    edges = {}
    for i in range(len(rows)):
        for j in range(len(cols)):
            if (i, j) in skip:
                continue
            cell = tbl.cell(i, j)
            r2, c2 = spans.get((i, j), (i, j))
            box = fitz.Rect(xs[j], ys[i], xs[min(c2 + 1, len(cols))],
                            ys[min(r2 + 1, len(rows))])
            col = None
            try:
                if cell.fill.type == MSO_FILL.SOLID:
                    col = _rgb(cell.fill.fore_color.rgb)
            except Exception:
                col = None
            if col:
                try:
                    page.draw_rect(box, color=None, fill=col)
                except Exception:
                    pass
            _draw_frame(w, cell.text_frame, box.x0, box.y0, box.width,
                        box.height, default)
            for side, seg in (("lnT", (box.x0, box.y0, box.x1, box.y0)),
                              ("lnB", (box.x0, box.y1, box.x1, box.y1)),
                              ("lnL", (box.x0, box.y0, box.x0, box.y1)),
                              ("lnR", (box.x1, box.y0, box.x1, box.y1))):
                spec = _cell_border(cell, side)
                if spec:
                    edges[(round(seg[0], 2), round(seg[1], 2),
                           round(seg[2], 2), round(seg[3], 2))] = spec
    for (xa, ya, xb, yb), (col, wd) in edges.items():
        try:
            page.draw_line((xa, ya), (xb, yb), color=col, width=wd)
        except Exception:
            pass
    return len(rows)


# --------------------------------------------------------------------------
# shape tree
# --------------------------------------------------------------------------
def _group_xfrm(shape):
    """(dx, dy, sx, sy) mapping child coordinates into the parent space."""
    try:
        grp = shape._element.find(qn("p:grpSpPr"))
        xfrm = grp.find(qn("a:xfrm"))
        off = xfrm.find(qn("a:off"))
        ext = xfrm.find(qn("a:ext"))
        cho = xfrm.find(qn("a:chOff"))
        che = xfrm.find(qn("a:chExt"))
        if che is None or cho is None:
            return (0.0, 0.0, 1.0, 1.0)
        sx = float(ext.get("cx")) / float(che.get("cx") or 1)
        sy = float(ext.get("cy")) / float(che.get("cy") or 1)
        return (float(off.get("x")) - float(cho.get("x")) * sx,
                float(off.get("y")) - float(cho.get("y")) * sy, sx, sy)
    except Exception:
        return (0.0, 0.0, 1.0, 1.0)


def _draw_shape(page, w, shape, default, xf=(0.0, 0.0, 1.0, 1.0), stats=None):
    dx, dy, sx, sy = xf
    left = (float(shape.left) * sx + dx) / EMU_PT
    top = (float(shape.top) * sy + dy) / EMU_PT
    wd = float(shape.width) * sx / EMU_PT
    ht = float(shape.height) * sy / EMU_PT
    kind = shape.shape_type
    if kind == MSO_SHAPE_TYPE.GROUP:
        sub = _group_xfrm(shape)
        for child in shape.shapes:
            _draw_shape(page, w, child, default, sub, stats)
        return
    if stats is not None:
        stats["shapes"] += 1
    box = fitz.Rect(left, top, left + max(0.0, wd), top + max(0.0, ht))
    if kind == MSO_SHAPE_TYPE.PICTURE:
        try:
            blob = shape.image.blob
            page.insert_image(box, stream=blob, keep_proportion=True)
            if stats is not None:
                stats["pics"] += 1
        except Exception:
            pass
        return
    if getattr(shape, "has_table", False):
        n = _draw_table(page, w, shape, default)
        if stats is not None:
            stats["tables"] += 1
            stats["rows"] += n
        return
    fill = _fill(shape)
    stroke = _stroke(shape)
    if kind == MSO_SHAPE_TYPE.LINE or (wd <= 0.5 and ht <= 0.5 and stroke):
        try:
            page.draw_line((box.x0, box.y0), (box.x1, box.y1),
                           color=stroke[0] if stroke else (0, 0, 0),
                           width=stroke[1] if stroke else 0.75)
        except Exception:
            pass
        return
    if fill or stroke:
        try:
            page.draw_rect(box, color=stroke[0] if stroke else None,
                           fill=fill, width=stroke[1] if stroke else 0)
        except Exception:
            pass
    if getattr(shape, "has_text_frame", False) and shape.text_frame.text.strip():
        _draw_frame(w, shape.text_frame, box.x0, box.y0, box.width,
                    box.height, default)


# --------------------------------------------------------------------------
# public entry point
# --------------------------------------------------------------------------
def convert_pptx_pdf(pptx_path, pdf_path, stats=None):
    """Render every slide of *pptx_path* into *pdf_path*.

    Returns ``(pages, shapes, tables)``.
    """
    prs = Presentation(pptx_path)
    pw = _pt(prs.slide_width) or 720.0
    ph = _pt(prs.slide_height) or 540.0
    pdf = fitz.open()
    st = {"shapes": 0, "pics": 0, "tables": 0, "rows": 0, "texts": 0}
    try:
        for slide in prs.slides:
            page = pdf.new_page(width=pw, height=ph)
            bg = None
            try:
                if slide.background.fill.type == MSO_FILL.SOLID:
                    bg = _rgb(slide.background.fill.fore_color.rgb)
            except Exception:
                bg = None
            if bg:
                try:
                    page.draw_rect(page.rect, color=None, fill=bg)
                except Exception:
                    pass
            w = _Writer(page)
            default = DEF_SIZE
            for shape in slide.shapes:
                _draw_shape(page, w, shape, default, stats=st)
            w.flush()
        if len(pdf) == 0:
            pdf.new_page(width=pw, height=ph)
        out_dir = os.path.dirname(os.path.abspath(pdf_path))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        pdf.save(pdf_path, garbage=3, deflate=True)
        pages = len(pdf)
    finally:
        pdf.close()
    if isinstance(stats, dict):
        stats.update(st)
        stats["pages"] = pages
    return pages, st["shapes"], st["tables"]
