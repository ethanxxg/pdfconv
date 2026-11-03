"""XLSX -> PDF: our own sheet layout engine.  No LibreOffice, no Aspose.

openpyxl reads the workbook (cached values, styles, merges, page setup); the
sheet is then laid out here and drawn with PyMuPDF primitives:

* geometry from the sheet itself -- column widths from ``<col>`` (character
  units converted to points), row heights from ``<row ht>`` or auto-derived
  from the wrapped cell text
* cell typography: font family (base-14 / built-in CJK face), size, bold,
  italic, colour, solid fill, and borders with per-side style weights
* number formats: percent, fixed decimals, thousands separators, scientific,
  dates and times -- so the print reads like Excel, not like raw floats
* merged ranges: text is laid out against the merged box, covered cells are
  skipped, and the box gets one border instead of interior lines
* alignment: horizontal (left / centre / right) and vertical (top / centre /
  bottom), wrapping on the box width, overflow clipped like Excel
* page setup: A4 (or Letter) portrait / landscape picked from the content
  width, fit-to-width scaling when even landscape is too narrow, frozen or
  ``print_title_rows`` rows repeated at the top of every page, optional
  column letters and row numbers
* automatic pagination down the sheet, with a sheet/page footer

All coordinates are PDF points (1/72 inch) -- also openpyxl's row-height unit
-- so the sheet geometry maps 1:1 onto the page.
"""
import datetime
import os
import re
from collections import namedtuple

import fitz
from openpyxl import load_workbook
from openpyxl.utils import column_index_from_string, get_column_letter, \
    range_boundaries

from . import measure
from .docx2pdf import _font, font_key, text_width

A4 = (595.28, 841.89)
LETTER = (612.0, 792.0)

PAD = 2.5                 # cell padding, points
LEAD = 1.22               # line advance as a multiple of the font size
DEFAULT_W = 8.43          # Excel's default column width, characters
DEFAULT_H = 15.0          # Excel's default row height, points
MIN_W = 6.0
MAX_W = 400.0

# border style -> stroke width in points
_BW = {
    "hair": 0.25, "thin": 0.5, "dotted": 0.5, "dashed": 0.5,
    "dashDot": 0.5, "slantDashDot": 0.5,
    "medium": 1.0, "mediumDashed": 1.0, "mediumDashDot": 1.0,
    "mediumDashDotDot": 1.0,
    "thick": 1.6, "double": 1.2,
}

_CJK = re.compile(r"[\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef\u3000-\u303f]")
_TOK = re.compile(r"[\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef\u3000-\u303f]"
                  r"|[^\s\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef\u3000-\u303f]+"
                  r"|\s+")

_CS = namedtuple("_CS", "text key size color ha va wrap fill border")


# --------------------------------------------------------------------------
# value / style translation
# --------------------------------------------------------------------------
def _rgb(color, default=(0.0, 0.0, 0.0)):
    """openpyxl Color -> RGB tuple.  Theme / indexed colours fall back."""
    try:
        s = color.rgb
    except Exception:
        return default
    if not isinstance(s, str) or len(s) < 6:
        return default
    s = s[-6:]
    try:
        return (int(s[0:2], 16) / 255.0, int(s[2:4], 16) / 255.0,
                int(s[4:6], 16) / 255.0)
    except ValueError:
        return default


_PCT = re.compile(r"0(?:\.(0+))?\s*%")
_DEC = re.compile(r"[0#]\.(0+)")


def _num(v, fmt):
    """Format one number the way the cell's number format would print it."""
    f = (fmt or "General").strip()
    low = f.lower()
    if low in ("general", "@", ""):
        if isinstance(v, int):
            return str(v)
        if v == int(v) and abs(v) < 1e15:
            return str(int(v))
        return ("%.6f" % v).rstrip("0").rstrip(".")
    if "%" in low:
        m = _PCT.search(low)
        d = len(m.group(1)) if (m and m.group(1)) else 0
        return ("{:,.%df}%%" % d).format(v * 100.0)
    if "e+" in low or "e-" in low:
        m = _DEC.search(low)
        d = len(m.group(1)) if m else 2
        return ("%.*E" % (d, v))
    m = _DEC.search(low)
    d = len(m.group(1)) if m else 0
    thou = "," in low
    if d:
        body = ("{:,.%df}" % d) if thou else ("{:.%df}" % d)
        return body.format(v)
    if thou:
        return "{:,.0f}".format(v)
    if isinstance(v, int):
        return str(v)
    return ("%g" % v)


def _text_of(v, fmt):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, datetime.datetime):
        if v.hour or v.minute or v.second:
            return v.strftime("%Y-%m-%d %H:%M")
        return v.strftime("%Y-%m-%d")
    if isinstance(v, datetime.date):
        return v.strftime("%Y-%m-%d")
    if isinstance(v, datetime.time):
        return v.strftime("%H:%M:%S")
    if isinstance(v, (int, float)):
        return _num(v, fmt)
    return str(v)


def _fill_rgb(cell):
    """Solid fill colour, or None."""
    try:
        f = cell.fill
        if f is None or f.patternType != "solid":
            return None
        c = f.fgColor
        if c is None:
            return None
        if getattr(c, "type", "rgb") != "rgb":
            return None
        return _rgb(c, None)
    except Exception:
        return None


def _border_sides(cell):
    """(left, right, top, bottom) as (colour, width) or None."""
    out = []
    try:
        b = cell.border
    except Exception:
        return (None, None, None, None)
    for name in ("left", "right", "top", "bottom"):
        s = getattr(b, name, None)
        if s is None or not s.style or s.style not in _BW:
            out.append(None)
            continue
        out.append((_rgb(getattr(s, "color", None)), _BW[s.style]))
    return tuple(out)


# the aggregate formulas our own PDF->XLSX pass writes back
_AGG = re.compile(r"^=\s*(SUM|AVERAGE|MIN|MAX|PRODUCT|COUNT)\s*\(\s*"
                  r"([A-Z]{1,3})(\d+)\s*:\s*([A-Z]{1,3})(\d+)\s*\)\s*$",
                  re.I)


def eval_cell(ws, cell, depth=0, memo=None):
    """Value of *cell*, evaluating the simple aggregate formulas we emit.

    The workbook is loaded with ``data_only=False`` so a cell that carries
    ``=SUM(B2:B4)`` still has its formula.  Excel would have cached the result
    next to it; openpyxl does not, so the handful of aggregates this project
    writes (SUM / AVERAGE / MIN / MAX / PRODUCT / COUNT over one rectangular
    range) are evaluated here instead of rendering as blanks.
    """
    v = cell.value
    if not isinstance(v, str) or not v.startswith("="):
        return v
    if memo is None:
        memo = {}
    key = cell.coordinate
    if key in memo:
        return memo[key]
    m = _AGG.match(v)
    if not m or depth > 8:
        memo[key] = None
        return None
    fn = m.group(1).upper()
    c1, r1 = column_index_from_string(m.group(2)), int(m.group(3))
    c2, r2 = column_index_from_string(m.group(4)), int(m.group(5))
    nums = []
    memo[key] = None                    # guard against a circular reference
    for r in range(min(r1, r2), max(r1, r2) + 1):
        for c in range(min(c1, c2), max(c1, c2) + 1):
            sub = eval_cell(ws, ws.cell(row=r, column=c), depth + 1, memo)
            if isinstance(sub, bool) or not isinstance(sub, (int, float)):
                continue
            nums.append(sub)
    out = None
    if nums:
        if fn == "SUM":
            out = sum(nums)
        elif fn == "AVERAGE":
            out = sum(nums) / len(nums)
        elif fn == "MIN":
            out = min(nums)
        elif fn == "MAX":
            out = max(nums)
        elif fn == "PRODUCT":
            out = 1.0
            for x in nums:
                out *= x
        elif fn == "COUNT":
            out = len(nums)
    memo[key] = out
    return out


def _wrap(text, key, size, width):
    """Greedy line breaking via the shared kernel (CJK glyphs, Latin words,
    kinsoku punctuation, 6% slack before a word is chopped)."""
    if width <= 1.0:
        return [str(text)]
    return measure.wrap_text(text, key, size, width, slack=0.06)


# --------------------------------------------------------------------------
# sheet geometry
# --------------------------------------------------------------------------
def _used_range(ws):
    """(r1, c1, r2, c2) of the cells that carry content, honouring print_area."""
    try:
        pa = ws.print_area
    except Exception:
        pa = None
    if isinstance(pa, str) and pa and ":" in pa:
        try:
            a, b = pa.replace("$", "").split(":")[:2]
            c1, r1 = range_boundaries(a)[:2]
            c2, r2 = range_boundaries(b)[2:]
            if r2 >= r1 and c2 >= c1:
                return r1, c1, r2, c2
        except Exception:
            pass
    r1 = c1 = 10 ** 9
    r2 = c2 = 0
    try:
        items = list(ws._cells.items())
    except Exception:
        items = []
    for (r, c), cell in items:
        if r < 1 or c < 1:
            continue
        try:
            v = cell.value
        except Exception:
            v = None
        if v is None:
            continue
        r1 = min(r1, r); r2 = max(r2, r)
        c1 = min(c1, c); c2 = max(c2, c)
    for mr in getattr(ws.merged_cells, "ranges", ()):
        r1 = min(r1, mr.min_row); r2 = max(r2, mr.max_row)
        c1 = min(c1, mr.min_col); c2 = max(c2, mr.max_col)
    if r2 == 0:
        return 1, 1, 1, 1
    return r1, c1, r2, c2


def _col_widths(ws, first, last, sc):
    """Column widths in points, honouring grouped <col min/max> definitions."""
    n = last - first + 1
    w = [None] * n
    try:
        dims = list(ws.column_dimensions.items())
    except Exception:
        dims = []
    for letter, d in dims:
        if getattr(d, "width", None) is None:
            continue
        try:
            idx = column_index_from_string(letter)
        except Exception:
            continue
        lo = getattr(d, "min", None) or idx
        hi = getattr(d, "max", None) or idx
        for c in range(max(lo, first), min(hi, last) + 1):
            w[c - first] = d.width
    try:
        default = ws.sheet_format.defaultColWidth or DEFAULT_W
    except Exception:
        default = DEFAULT_W
    out = []
    for v in w:
        v = default if v is None else v
        v = max(MIN_W, min(MAX_W, float(v)))
        out.append((v * 7.0 + 5.0) * 0.75 * sc)   # chars -> px -> pt
    return out


def _merges(ws):
    spans, skip = {}, set()
    for mr in getattr(ws.merged_cells, "ranges", ()):
        spans[(mr.min_row, mr.min_col)] = (mr.max_row, mr.max_col)
        for r in range(mr.min_row, mr.max_row + 1):
            for c in range(mr.min_col, mr.max_col + 1):
                if (r, c) != (mr.min_row, mr.min_col):
                    skip.add((r, c))
    return spans, skip


def _repeat_rows(ws, first):
    """How many leading rows to repeat at the top of every printed page."""
    n = 0
    try:
        tr = ws.print_title_rows
    except Exception:
        tr = None
    if isinstance(tr, str) and ":" in tr:
        try:
            b = tr.split(":")[1].replace("$", "")
            n = max(n, int(re.sub(r"\D", "", b)) - first + 1)
        except Exception:
            pass
    fp = getattr(ws, "freeze_panes", None)
    if fp:
        try:
            r1 = range_boundaries(fp)[1]
            n = max(n, r1 - first)
        except Exception:
            pass
    return max(0, min(n, 8))


def _page_setup(ws, paper, first, last):
    """(pw, ph, scale) for one sheet, from its own content width."""
    pw0, ph0 = A4 if str(paper).lower() != "letter" else LETTER
    ml = mr = 36.0
    widths = _col_widths(ws, first, last, 1.0)
    total = sum(widths)
    port = pw0 - ml - mr
    land = ph0 - ml - mr
    if total <= port:
        return pw0, ph0, 1.0
    if total <= port / 0.85:
        return pw0, ph0, port / total
    if total <= land:
        return ph0, pw0, 1.0
    if total <= land / 0.85:
        return ph0, pw0, land / total
    return ph0, pw0, max(0.35, land / total)


# --------------------------------------------------------------------------
# renderer
# --------------------------------------------------------------------------
class _Renderer:
    """Lay out and draw one worksheet, paginating down the sheet."""

    def __init__(self, pdf, ws, paper="a4", headings=None, footer=True):
        self.pdf = pdf
        self.ws = ws
        self.footer = footer
        self.r1, self.c1, self.r2, self.c2 = _used_range(ws)
        self.pw, self.ph, self.sc = _page_setup(ws, paper, self.c1, self.c2)
        self.ml = self.mr = self.mt = self.mb = 36.0
        if headings is None:
            try:
                headings = bool(ws.print_options.headings)
            except Exception:
                headings = False
        self.headings = bool(headings)
        self.gut = 22.0 if self.headings else 0.0
        self.widths = _col_widths(ws, self.c1, self.c2, self.sc)
        self.spans, self.skip = _merges(ws)
        self.repeat = _repeat_rows(ws, self.r1)
        self._cs = {}
        self._rh = {}
        self._xs = [0.0]
        for w in self.widths:
            self._xs.append(self._xs[-1] + w)
        self.pages = 0
        self.cells = 0
        self.clipped = 0
        self.page = None
        self.tw = None
        self.borders = {}
        self.rowpos = []

    # -- geometry -----------------------------------------------------
    def x_of(self, c):
        return self.ml + self.gut + self._xs[c - self.c1]

    def _span_w(self, c0, c1):
        return self._xs[c1 - self.c1 + 1] - self._xs[c0 - self.c1]

    def row_h(self, r):
        h = self._rh.get(r)
        if h is not None:
            return h
        try:
            d = self.ws.row_dimensions.get(r)
            explicit = d.height if d is not None else None
        except Exception:
            explicit = None
        if explicit:
            h = float(explicit) * self.sc
        else:
            need = 0.0
            for c in range(self.c1, self.c2 + 1):
                if (r, c) in self.skip:
                    continue
                cs = self.style(r, c)
                if not cs.text:
                    continue
                sp = self.spans.get((r, c))
                w = self._span_w(c, sp[1] if sp else c) - 2 * PAD
                n = len(_wrap(cs.text, cs.key, cs.size, w))
                need = max(need, n * cs.size * LEAD)
            h = max(DEFAULT_H * self.sc, need + 2 * PAD)
        self._rh[r] = h
        return h

    def span_h(self, r, r2):
        if r2 <= r:
            return self.row_h(r)
        return sum(self.row_h(x) for x in range(r, r2 + 1))

    # -- styles -------------------------------------------------------
    def style(self, r, c):
        k = (r, c)
        cs = self._cs.get(k)
        if cs is not None:
            return cs
        try:
            cell = self.ws.cell(row=r, column=c)
        except Exception:
            cs = _CS("", "helv", 11.0, (0, 0, 0), "left", "bottom",
                     True, None, (None, None, None, None))
            self._cs[k] = cs
            return cs
        try:
            raw = cell.value
            if isinstance(raw, str) and raw.startswith("="):
                raw = eval_cell(self.ws, cell)
        except Exception:
            raw = None
        fmt = "General"
        try:
            fmt = cell.number_format or "General"
        except Exception:
            pass
        text = _text_of(raw, fmt)
        font = getattr(cell, "font", None)
        name = getattr(font, "name", None)
        bold = bool(getattr(font, "b", False))
        italic = bool(getattr(font, "i", False))
        size = float(getattr(font, "sz", None) or 11.0) * self.sc
        color = _rgb(getattr(font, "color", None))
        al = getattr(cell, "alignment", None)
        ha = getattr(al, "horizontal", None)
        if not ha:
            ha = "right" if isinstance(raw, (int, float)) and \
                not isinstance(raw, bool) else "left"
        if isinstance(raw, bool):
            ha = "center"
        va = getattr(al, "vertical", None) or "bottom"
        wrap = bool(getattr(al, "wrap_text", False))
        key = font_key(name, bold, italic)
        if not wrap and text:
            sp = self.spans.get((r, c))
            w = self._span_w(c, sp[1] if sp else c) - 2 * PAD
            if text_width(text, key, size) > w:
                wrap = True
        cs = _CS(text, key, size, color, ha, va, wrap,
                 _fill_rgb(cell), _border_sides(cell))
        self._cs[k] = cs
        return cs

    # -- pages --------------------------------------------------------
    def new_page(self):
        self.page = self.pdf.new_page(width=self.pw, height=self.ph)
        self.pages += 1
        self.tw = None
        self.tw_color = None
        self.borders = {}
        self.rowpos = []

    def _flush(self):
        # TextWriter.color belongs to the writer, not to each append: whatever
        # colour is current when write_text() runs paints the whole batch, so
        # one writer per colour is the only way to keep cell colours.
        if self.tw is not None:
            try:
                self.tw.write_text(self.page, color=self.tw_color)
            except Exception:
                pass
            self.tw = None
            self.tw_color = None

    def finish_page(self):
        if self.gut and self.rowpos:
            self._gutters(self.mt, self.rowpos)
        for (p1, p2), (col, wd) in self.borders.items():
            try:
                self.page.draw_line(p1, p2, color=col, width=wd)
            except Exception:
                pass
        self.borders = {}
        self._flush()

    def _put(self, x, baseline, text, key, size, color):
        if not text:
            return
        fkey = "china-s" if _CJK.search(text) else key
        if self.tw is None or self.tw_color != color:
            self._flush()
            self.tw = fitz.TextWriter(self.page.rect, color=color)
            self.tw_color = color
        try:
            self.tw.append((x, baseline), text, font=_font(fkey),
                           fontsize=size)
        except Exception:
            try:
                self.page.insert_text((x, baseline), text, fontname=fkey,
                                      fontsize=size, color=color)
            except Exception:
                pass

    def _line(self, p1, p2, spec):
        if not spec:
            return
        col, wd = spec
        self.borders[(p1, p2)] = (col, wd)

    def draw_row(self, r, y):
        h = self.row_h(r)
        self.rowpos.append((r, y))
        for c in range(self.c1, self.c2 + 1):
            if (r, c) in self.skip:
                continue
            cs = self.style(r, c)
            sp = self.spans.get((r, c))
            r2, c2 = (sp if sp else (r, c))
            x = self.x_of(c)
            w = self._span_w(c, c2)
            hh = self.span_h(r, r2)
            box = fitz.Rect(x, y, x + w, y + hh)
            if cs.fill:
                try:
                    self.page.draw_rect(box, color=None, fill=cs.fill)
                except Exception:
                    pass
            if cs.text:
                self.cells += 1
                self._cell_text(cs, box)
            lf, rt, tp, bt = cs.border
            self._line((box.x0, box.y0), (box.x1, box.y0), tp)
            self._line((box.x0, box.y1), (box.x1, box.y1), bt)
            self._line((box.x0, box.y0), (box.x0, box.y1), lf)
            self._line((box.x1, box.y0), (box.x1, box.y1), rt)
        return y + h

    def _cell_text(self, cs, box):
        avail = box.width - 2 * PAD
        lines = _wrap(cs.text, cs.key, cs.size, avail) if cs.wrap \
            else [cs.text]
        lead = cs.size * LEAD
        f = _font("china-s" if _CJK.search(cs.text) else cs.key)
        try:
            asc = f.ascender * cs.size
            desc = -f.descender * cs.size
        except Exception:
            asc, desc = cs.size * 0.75, cs.size * 0.25
        room = box.height - 2 * PAD
        if room > 0:
            # the epsilon matters: room is derived from n*size*LEAD, so the
            # quotient is an exact integer in real arithmetic but can land at
            # 3.9999999 in floating point and drop a whole line
            fit = max(1, int(room / lead + 1e-9))
            if len(lines) > fit:
                lines = lines[:fit]
                self.clipped += 1
        block = len(lines) * lead
        va = cs.va
        if va == "top":
            base = box.y0 + PAD + asc
        elif va == "center":
            base = box.y0 + (box.height - block) / 2.0 + asc
        else:
            base = box.y1 - PAD - block + asc
        for i, ln in enumerate(lines):
            if not ln:
                continue
            lw = text_width(ln, cs.key, cs.size)
            if cs.ha == "center":
                x = box.x0 + (box.width - lw) / 2.0
            elif cs.ha == "right":
                x = box.x1 - PAD - lw
            else:
                x = box.x0 + PAD
            self._put(x, base + i * lead, ln, cs.key, cs.size, cs.color)

    def _gutters(self, y_top, rows):
        """Column letters and row numbers (only when the sheet asks for them)."""
        if not self.gut:
            return
        size = min(7.5, 6.0 * self.sc + 2.0)
        col = (0.35, 0.35, 0.35)
        for c in range(self.c1, self.c2 + 1):
            x = self.x_of(c)
            label = get_column_letter(c)
            w = text_width(label, "helv", size)
            self._put(x + (self.widths[c - self.c1] - w) / 2.0,
                      y_top - 4.0, label, "helv", size, col)
        for r, y in rows:
            label = str(r)
            w = text_width(label, "helv", size)
            self._put(self.ml + (self.gut - w) / 2.0, y + self.row_h(r) / 2.0
                      + size * 0.35, label, "helv", size, col)

    # -- main loop ----------------------------------------------------
    def run(self):
        body = self.r1 + self.repeat
        bottom = self.ph - self.mb
        r = body
        while True:
            self.new_page()
            y = self.mt
            if self.repeat:
                for rr in range(self.r1, body):
                    y = self.draw_row(rr, y)
                y += 1.5
            started = False
            while r <= self.r2:
                h = self.row_h(r)
                if y + h > bottom + 0.01:
                    if started:
                        break
                    self.draw_row(r, y)     # taller than the page: clip it
                    r += 1
                    break
                y = self.draw_row(r, y)
                r += 1
                started = True
            self.finish_page()
            if r > self.r2:
                break
        return self.pages


# --------------------------------------------------------------------------
# public entry point
# --------------------------------------------------------------------------
def convert_xlsx_pdf(xlsx_path, pdf_path, footer=True, paper="a4",
                     headings=None, stats=None):
    """Render every visible worksheet of *xlsx_path* into *pdf_path*.

    Returns ``(pages, sheets, cells)``.  When *stats* is a dict it is filled
    with the finer counters (``clipped`` cells, per-sheet scaling).
    """
    wb = load_workbook(xlsx_path, data_only=False)
    pdf = fitz.open()
    total_pages = 0
    total_cells = 0
    clipped = 0
    scaled = []
    sheets = 0
    marks = []                      # (page, title, index, count)
    try:
        for ws in wb.worksheets:
            if getattr(ws, "sheet_state", "visible") != "visible":
                continue
            ren = _Renderer(pdf, ws, paper=paper, headings=headings,
                            footer=footer)
            first_page = len(pdf)
            n = ren.run()
            for i in range(n):
                marks.append((first_page + i, ws.title, i + 1, n))
            total_pages += n
            total_cells += ren.cells
            clipped += ren.clipped
            if ren.sc < 0.999:
                scaled.append((ws.title, round(ren.sc, 3)))
            sheets += 1
        if footer and marks:
            size = 7.5
            grey = (0.45, 0.45, 0.45)
            for pno, title, i, n in marks:
                page = pdf[pno]
                txt = "%s  ·  %d / %d" % (title, i, n)
                try:
                    w = text_width(txt, "helv", size)
                    page.insert_text(((page.rect.width - w) / 2.0,
                                      page.rect.height - 20.0), txt,
                                     fontname="helv", fontsize=size,
                                     color=grey)
                except Exception:
                    pass
        if len(pdf) == 0:
            pdf.new_page(width=A4[0], height=A4[1])
        out_dir = os.path.dirname(os.path.abspath(pdf_path))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        pdf.save(pdf_path, garbage=3, deflate=True)
    finally:
        pdf.close()
    if isinstance(stats, dict):
        stats.update(pages=total_pages, sheets=sheets, cells=total_cells,
                     clipped=clipped, scaled=scaled)
    return total_pages, sheets, total_cells
