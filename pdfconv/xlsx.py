"""PDF -> XLSX: every detected table becomes a worksheet.

The table geometry is the *same* reconstruction the DOCX path uses
(``media.extract_tables``), so the two outputs cannot disagree about what a
table is: find_tables grid + coordinate mapping + merged regions + the PDF's
own column edges.

Beyond plain text transfer:

* numeric-looking cells become real numbers (Excel can compute on them);
  percentages keep a percent number format, zero-padded ids stay text
* merged PDF cells become merged ranges
* column widths come from the PDF's own grid, not Excel's defaults
* the header row is bolded, filled and frozen when it looks like one
* every sheet records its page and the nearest "TABLE ..." caption, and an
  Overview sheet indexes them
* documents without any table fall back to one line-per-row text sheet, so
  the converter never produces an empty workbook

Only MuPDF reads the PDF; the workbook is written by openpyxl.
"""
import os
import re

import fitz
from openpyxl import Workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter

from . import layout, media, ocr
from .text import build_lines, page_words

_THIN = Side(style="thin", color="D0D7E2")
_BORDER = Border(left=_THIN, right=_THIN, top=_THIN, bottom=_THIN)
_HEAD_FILL = PatternFill("solid", fgColor="EEF2F7")
_HEAD_FONT = Font(bold=True)

# plain decimal / thousands-separated number, optional sign (U+2212 handled)
_NUM = re.compile(r"^[-+\u2212]?\d{1,3}(?:,\d{3})+(?:\.\d+)?$"
                  r"|^[-+\u2212]?(?:\d+\.\d+|\d+)$")
_INT = re.compile(r"^[-+\u2212]?\d+$")
_PCT = re.compile(r"^[-+\u2212]?(?:\d+\.\d+|\d+)%$")
_CAP = re.compile(r"^table$", re.I)
_CAP2 = re.compile(r"^table\s+[ivxlcdm\d]", re.I)


def _value(text):
    """(value, number_format) for one cell.  Returns text when the string is
    not unambiguously numeric -- ids like '007' or '1.2.3' must stay text."""
    t = (text or "").strip().replace("\u2212", "-").replace("\u00a0", " ")
    if not t:
        return None, None
    if _PCT.match(t):
        try:
            return float(t[:-1]) / 100.0, "0.0%"
        except ValueError:
            return t, None
    if _NUM.match(t):
        raw = t.replace(",", "")
        if _INT.match(raw):
            digits = raw.lstrip("+-")
            if len(digits) > 1 and digits.startswith("0"):
                return t, None          # keep zero-padded ids as text
            try:
                return int(raw), "0"
            except ValueError:
                return t, None
        try:
            return float(raw), "General"
        except ValueError:
            return t, None
    return t, None


def _caption(page, bbox):
    """Nearest 'TABLE ...' caption line above (or just below) the table."""
    try:
        words = page.get_text("words")
    except Exception:
        return ""
    lines = {}
    for w in words:
        r = fitz.Rect(w[:4])
        key = round(r.y0, 1)
        lines.setdefault(key, []).append((r.x0, r.x1, w[4]))
    best = None
    for y, ws in lines.items():
        txt = " ".join(t for _x0, _x1, t in sorted(ws))
        if not _CAP2.match(txt.strip()):
            continue
        dy = bbox.y0 - y
        if -30 < dy < 90:               # caption sits above the table
            d = abs(dy)
        elif -70 < dy <= -30:           # or a line or two below it
            d = abs(dy) + 40
        else:
            continue
        if best is None or d < best[0]:
            best = (d, txt.strip())
    return best[1] if best else ""


def _page_sheet(pno, used):
    base = "p%d" % pno
    name, k = base, 2
    while name in used:
        name = "%s-%d" % (base, k)
        k += 1
    used.add(name)
    return name


def _sheet_name(i, pno, used):
    base = "T%02d p%d" % (i, pno)
    name, k = base, 2
    while name in used:                 # sheet names must be unique
        name = "%s-%d" % (base, k)
        k += 1
    used.add(name)
    return name


_TOT_EN = re.compile(r"^\s*(total|totals|subtotal|sum|overall|average|mean"
                     r"|avg)\b", re.I)
_TOT_CJK = ("合计", "总计", "总和", "总数", "小计", "汇总", "平均")


def _total_kind(text):
    """'sum' / 'average' when *text* is a total-row label, else None."""
    s = (text or "").strip()
    if not s:
        return None
    m = _TOT_EN.match(s)
    if m:
        return "average" if m.group(1).lower().startswith(("avg", "mean",
                                                           "average")) \
            else "sum"
    for k in _TOT_CJK:
        if k in s:
            return "average" if k == "平均" else "sum"
    return None


def _restore_formulas(ws, top, nrows, ncols):
    """Re-derive the arithmetic a table was showing.

    A PDF only carries the *result* of a spreadsheet formula.  When a column
    adds up to the value in its "Total" row -- or a row adds up to its last
    cell -- write the real Excel formula instead of a frozen number, so the
    workbook stays live when an editor changes an input.  Only exact matches
    within 0.2% qualify; anything else keeps the number it had.
    """
    if nrows < 3 or ncols < 2:
        return 0

    def num(r, c):
        v = ws.cell(row=r, column=c).value
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            return None
        return v

    got = 0
    # vertical: a labelled total row under a column of numbers
    for r in range(top + 1, top + nrows):
        kind = _total_kind(ws.cell(row=r, column=1).value)
        if not kind:
            continue
        for c in range(2, ncols + 1):
            v = num(r, c)
            if v is None:
                continue
            rows_above = [(rr, num(rr, c)) for rr in range(top, r)]
            rows_above = [(rr, x) for rr, x in rows_above if x is not None]
            above = [x for _rr, x in rows_above]
            if len(above) < 2:
                continue
            first = rows_above[0][0]
            target = sum(above) if kind == "sum" else sum(above) / len(above)
            if abs(v - target) > max(0.01, 0.002 * abs(target)):
                continue
            col = get_column_letter(c)
            fn = "SUM" if kind == "sum" else "AVERAGE"
            cell = ws.cell(row=r, column=c)
            cell.value = "=%s(%s%d:%s%d)" % (fn, col, first, col, r - 1)
            cell.number_format = ws.cell(row=r - 1, column=c).number_format
            got += 1
    # horizontal: a row whose last cell is the sum of the numbers before it
    for r in range(top, top + nrows):
        if _total_kind(ws.cell(row=r, column=1).value):
            continue
        last = num(r, ncols)
        if last is None:
            continue
        body = [num(r, c) for c in range(2, ncols)]
        body = [x for x in body if x is not None]
        if len(body) < 2 or len(body) != ncols - 2:
            continue
        if abs(last - sum(body)) > max(0.01, 0.002 * abs(sum(body))):
            continue
        cell = ws.cell(row=r, column=ncols)
        cell.value = "=SUM(B%d:%s%d)" % (r, get_column_letter(ncols - 1), r)
        cell.number_format = ws.cell(row=r, column=ncols - 1).number_format
        got += 1
    return got


def _write_table(ws, t, top=1):
    """Lay one reconstructed table onto a worksheet at row *top*.

    Returns the first free row below it, so several tables from the same PDF
    page can share one sheet the way a person would stack them.
    """
    rows = t["cells"]
    ncols = t["ncols"]
    off = top - 1
    for i, row in enumerate(rows):
        for j in range(ncols):
            txt = row[j] if j < len(row) else ""
            v, fmt = _value(txt)
            cell = ws.cell(row=i + 1 + off, column=j + 1)
            cell.value = v
            cell.alignment = Alignment(vertical="top", wrap_text=True)
            cell.border = _BORDER
            if fmt and fmt != "General":
                cell.number_format = fmt
    # merged PDF cells -> merged ranges (merges are 0-based, end-exclusive)
    for (r0, c0, r1, c1) in t.get("merges") or []:
        if r1 - r0 > 1 or c1 - c0 > 1:
            try:
                ws.merge_cells(start_row=r0 + 1 + off, start_column=c0 + 1,
                               end_row=r1 + off, end_column=c1)
            except Exception:
                pass
    # column widths from the PDF's own grid (widest table on the sheet wins)
    edges = t.get("edges")
    if edges and len(edges) == ncols + 1:
        for j in range(ncols):
            wpt = float(edges[j + 1]) - float(edges[j])
            w = max(8.0, min(60.0, wpt / 6.0))
            letter = get_column_letter(j + 1)
            cur = ws.column_dimensions[letter].width
            if cur is None or w > cur:
                ws.column_dimensions[letter].width = w
    # header row: bold + fill when the first row carries no numbers
    if rows:
        head_numeric = sum(1 for j in range(ncols)
                           if isinstance(ws.cell(row=top, column=j + 1).value,
                                         (int, float)))
        if head_numeric == 0 and any(
                (row[j] if j < len(row) else "").strip()
                for row in rows[:1] for j in range(ncols)):
            for j in range(ncols):
                c = ws.cell(row=top, column=j + 1)
                c.font = _HEAD_FONT
                c.fill = _HEAD_FILL
                c.alignment = Alignment(horizontal="center", vertical="center",
                                        wrap_text=True)
    got = _restore_formulas(ws, top, len(rows), ncols)
    return top + len(rows), got


def _text_sheet(ws, pdf, tokens=None):
    """Fallback for PDFs without a single detected table: one line per row."""
    tokens = tokens or (lambda pno: page_words(pdf[pno]))
    ws.cell(row=1, column=1, value="Page").font = _HEAD_FONT
    ws.cell(row=1, column=2, value="Text").font = _HEAD_FONT
    r = 2
    for pno in range(len(pdf)):
        try:
            words = tokens(pno)
            lines = build_lines(words)
        except Exception:
            continue
        for ln in lines:
            txt = (ln.get("text") or "").strip()
            if not txt:
                continue
            ws.cell(row=r, column=1, value=pno + 1)
            ws.cell(row=r, column=2, value=txt)
            r += 1
    ws.column_dimensions["A"].width = 8
    ws.column_dimensions["B"].width = 120


def _overview(ws, items):
    head = ["#", "Worksheet", "Page", "Table", "Rows", "Cols", "Merges",
            "Formulas", "Caption"]
    for j, h in enumerate(head, 1):
        c = ws.cell(row=1, column=j, value=h)
        c.font = _HEAD_FONT
        c.fill = _HEAD_FILL
        c.border = _BORDER
        c.alignment = Alignment(horizontal="center")
    for i, (name, pno, tno, t, cap, nf) in enumerate(items, 1):
        vals = [i, name, pno, tno, t["nrows"], t["ncols"],
                len(t.get("merges") or []), nf, cap]
        for j, v in enumerate(vals, 1):
            c = ws.cell(row=i + 1, column=j, value=v)
            c.border = _BORDER
            c.alignment = Alignment(vertical="top", wrap_text=(j == 9))
    for col, w in zip("ABCDEFGHI", (5, 13, 7, 6, 7, 7, 8, 9, 70)):
        ws.column_dimensions[col].width = w
    ws.freeze_panes = "A2"


def convert_xlsx(pdf_path, xlsx_path, progress_cb=None):
    """Convert one PDF into a workbook of native tables.  Returns
    (sheets, tables).

    progress_cb(page_no, page_total) is called once per page when supplied.
    """
    L = layout.detect(pdf_path)
    gut_l, gut_r = L["gutter"]
    pdf = fitz.open(pdf_path)
    wb = Workbook()
    wb.remove(wb.active)                 # no empty default sheet
    used = set()
    items = []
    n = 0
    ntab = 0
    # pages whose CID text cannot be decoded are read from pixels, exactly like
    # the DOCX and PPT paths do -- otherwise a scanned PDF lands garbage text
    status = ocr.FontStatus(pdf) if ocr.available() else None
    body = float(L.get("body_size") or 10.0)

    def page_tokens(pno0):
        page = pdf[pno0]
        w = page_words(page)
        if status is not None and ocr.needs_ocr(pdf, pno0, w, status):
            f_hint, _sz = status.dominant(pno0)
            got = ocr.page_words(page, body, f_hint)
            if got:
                w = got
        return w

    for pno in range(len(pdf)):
        page = pdf[pno]
        if progress_cb is not None:
            progress_cb(pno + 1, len(pdf))
        try:
            words = page_tokens(pno)
            tabs, _rejected, _text_only = media.extract_tables(
                page, gut_l, gut_r, words)
        except Exception:
            tabs = []
        if not tabs:
            continue
        caps = [_caption(page, t["bbox"]) for t in tabs]
        if len(tabs) == 1:
            # one table on the page: it gets a sheet of its own
            n += 1
            name = _sheet_name(n, pno + 1, used)
            ws = wb.create_sheet(name)
            row, nf = _write_table(ws, tabs[0])
            if len(tabs[0]["cells"]) > 6:
                ws.freeze_panes = "A2"
            items.append((name, pno + 1, 1, tabs[0], caps[0], nf))
            ntab += 1
            continue
        # several tables on one page: they belong together, so they share a
        # sheet, stacked in reading order with their captions -- one sheet per
        # table would scatter a page's contents across the workbook
        name = _page_sheet(pno + 1, used)
        ws = wb.create_sheet(name)
        row = 1
        for k, (t, cap) in enumerate(zip(tabs, caps), 1):
            if cap:
                c = ws.cell(row=row, column=1, value=cap)
                c.font = Font(italic=True, bold=True, size=9)
                row += 1
            row, nf = _write_table(ws, t, row)
            items.append((name, pno + 1, k, t, cap, nf))
            ntab += 1
            row += 2
    if not items:
        ws = wb.create_sheet("Text")
        _text_sheet(ws, pdf, page_tokens)
        items.append(("Text", "-", 1,
                      dict(nrows=0, ncols=0, merges=[]), "", 0))
    ov = wb.create_sheet("Overview", 0)
    _overview(ov, items)
    pdf.close()
    d = os.path.dirname(os.path.abspath(xlsx_path))
    if d:
        os.makedirs(d, exist_ok=True)
    wb.save(xlsx_path)
    return len(wb.sheetnames) - 1, ntab
