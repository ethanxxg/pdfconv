"""DOCX -> PDF: our own layout engine.  No LibreOffice, no Aspose.

The DOCX is read with python-docx, re-flowed against real font metrics, and
drawn with PyMuPDF primitives.  Scope is exactly what our own DOCX output
uses (and what a normal Word document uses in practice):

* sections: page size, margins
* paragraph flow: alignment (left/centre/right/justify), first-line indent,
  space before/after, line spacing
* run typography: font family (mapped to a base-14 or the built-in CJK face),
  size, bold, italic, colour
* headings, native bullet / numbered lists (markers read back from
  numbering.xml, so a <w:startOverride> group keeps counting past the prose
  that interrupts it)
* character formatting inherited through the style chain (run -> paragraph
  style -> basedOn -> document defaults), which is what makes a Heading 1 in
  a Word-authored file come out at 14pt bold blue instead of 10.5pt black
* tables: w:tcW column widths, merged cells, borders, cell shading, vertical
  alignment, repeated header rows, wrapped cell text
* footnotes: superscript markers in the body, note block + separator rule at
  the bottom of the page that references them, reserved height in pagination
* inline images (placed at their own size, scaled into the text column)
* headers / footers with live page numbers
* widow / orphan control and keepNext, then automatic pagination when the
  cursor runs past the bottom margin (minus the footnote reservation)

Coordinates are PDF points throughout (python-docx Length.pt == 1/72 inch),
so the output page geometry matches the DOCX section exactly.
"""
import os
import re

import fitz
from docx import Document
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.table import Table
from docx.text.paragraph import Paragraph

from . import measure

# the metrics / breaking kernel is shared with xlsx2pdf and pptx2pdf
_font = measure.font
_is_cjk = measure.is_cjk
_has_cjk = measure.has_cjk
font_key = measure.font_key
text_width = measure.text_width
_CELL_TOK = measure.TOK
_wrap = measure.wrap


def _iter_body(doc):
    """Body children in document order (paragraphs and tables interleaved)."""
    body = doc.element.body
    for child in body.iterchildren():
        if child.tag == qn("w:p"):
            yield Paragraph(child, doc)
        elif child.tag == qn("w:tbl"):
            yield Table(child, doc)


_STYLE_FONT = {}


def _style_font(par):
    """(size, bold, italic, colour) inherited through the style chain.

    Word resolves character formatting from the run, then the paragraph style,
    then its ``basedOn`` chain, then the document defaults.  Ignoring that
    chain renders a Heading 1 as 10.5pt upright black -- which is exactly what
    a Word file authored outside this project looks like.
    """
    try:
        st = par.style
    except Exception:
        return (None, None, None, None)
    if st is None:
        return (None, None, None, None)
    hit = _STYLE_FONT.get(st.element)
    if hit is not None:
        return hit
    size = bold = italic = col = None
    s = st
    seen = 0
    while s is not None and seen < 12:
        try:
            f = s.font
            if size is None and f.size is not None:
                size = f.size.pt
            if bold is None and f.bold is not None:
                bold = bool(f.bold)
            if italic is None and f.italic is not None:
                italic = bool(f.italic)
            if col is None:
                c = f.color
                if c is not None and c.rgb is not None:
                    v = c.rgb
                    col = (v[0] / 255.0, v[1] / 255.0, v[2] / 255.0)
        except Exception:
            pass
        try:
            s = s.base_style
        except Exception:
            s = None
        seen += 1
    out = (size, bold, italic, col)
    _STYLE_FONT[st.element] = out
    return out


def _run_color(run):
    """Explicit run colour, or None when the run inherits it."""
    try:
        c = run.font.color
        if c is not None and c.rgb is not None:
            v = c.rgb
            return ((v[0] / 255.0), (v[1] / 255.0), (v[2] / 255.0))
    except Exception:
        pass
    return None


def _run_size(run, default):
    try:
        if run.font.size is not None:
            return run.font.size.pt
    except Exception:
        pass
    return default


def _run_vert(run):
    """Baseline shift for superscript / subscript runs (fraction of size)."""
    try:
        if run.font.superscript:
            return -0.42
        if run.font.subscript:
            return 0.16
    except Exception:
        pass
    return 0.0


def _atoms(par, default):
    """Flatten a paragraph into drawable atoms:
    (text, fontkey, size, colour, dy) where *dy* is a baseline shift in points.

    Walks the paragraph XML in document order so both ordinary runs and the
    text of native OMML equations are carried over (python-docx's ``par.runs``
    only sees ``w:r`` -- math lives in ``m:r``).  Spaces are kept as their own
    atoms so justification can stretch them.
    """
    from docx.text.run import Run
    out = []
    prose = False
    ssize, sbold, sital, scol = _style_font(par)
    sname = None
    try:
        sname = par.style.font.name
    except Exception:
        sname = None
    base = ssize or default
    m_ns = "http://schemas.openxmlformats.org/officeDocument/2006/math"
    m_omath = "{%s}oMath" % m_ns
    m_t = "{%s}t" % m_ns
    w_r = qn("w:r")
    for el in par._p.iter():
        if el.tag == w_r:
            try:
                r = Run(el, par)
            except Exception:
                continue
            if not r.text:
                continue
            prose = True
            size = _run_size(r, base)
            bold = r.bold if r.bold is not None else bool(sbold)
            ital = r.italic if r.italic is not None else bool(sital)
            key = font_key(r.font.name or sname, bold, ital)
            col = _run_color(r) or scol or (0, 0, 0)
            shift = _run_vert(r)
            dy = shift * size
            if shift:
                size = size * 0.68
            buf = ""
            for ch in r.text:
                if ch in " \t\u00a0":
                    if buf:
                        out.append((buf, key, size, col, dy))
                        buf = ""
                    out.append((" ", key, size, col, dy))
                else:
                    buf += ch
            if buf:
                out.append((buf, key, size, col, dy))
        elif el.tag == qn("w:footnoteReference"):
            fid = el.get(qn("w:id"))
            num = _FN_NUM.get(fid, 0)
            if num:
                # the marker is a superscript run in Word, so draw it as one
                out.append((str(num), "tiro", default * 0.70, (0, 0, 0),
                            -0.42 * default))
                prose = True
        elif el.tag == m_omath:
            txt = "".join(t.text or "" for t in el.iter(m_t))
            if txt:
                out.append((txt, "tiit", default, (0, 0, 0), 0.0))
    return out, prose


# footnote id -> display number, assigned in order of appearance in the body
_FN_NUM = {}
_FN_TEXT = {}
_ROMAN = ((1000, "M"), (900, "CM"), (500, "D"), (400, "CD"), (100, "C"),
          (90, "XC"), (50, "L"), (40, "XL"), (10, "X"), (9, "IX"), (5, "V"),
          (4, "IV"), (1, "I"))


def _roman(n):
    out = ""
    for v, s in _ROMAN:
        while n >= v:
            out += s
            n -= v
    return out


def _alpha(n):
    """1 -> a, 26 -> z, 27 -> aa (Word's lowerLetter)."""
    out = ""
    while n > 0:
        n, r = divmod(n - 1, 26)
        out = chr(97 + r) + out
    return out


def _marker(n, fmt, lvl_text):
    """Render a list marker the way Word renders it for this numbering level."""
    fmt = (fmt or "decimal").lower()
    if fmt == "bullet":
        ch = (lvl_text or "\u2022")
        # symbol-font bullets arrive as private-use code points we cannot draw
        if not ch or 0xF000 <= ord(ch[0]) <= 0xF0FF:
            ch = "\u2022"
        return ch + " "
    if fmt == "lowerletter":
        s = _alpha(n)
    elif fmt == "upperletter":
        s = _alpha(n).upper()
    elif fmt == "lowerroman":
        s = _roman(n).lower()
    elif fmt == "upperroman":
        s = _roman(n)
    elif fmt == "decimalzero":
        s = "%02d" % n
    elif fmt in ("chinesecounting", "chinesecountingthousand",
                 "taiwanesecounting", "ideographdigital"):
        s = str(n)
    else:
        s = str(n)
    t = lvl_text or "%1."
    if "%1" in t:
        return t.replace("%1", s) + " "
    return t + " "


def _numbering(doc):
    """numId -> (start, numFmt, lvlText) for level 0 of numbering.xml.

    Reading the real definitions matters because our own writer gives every
    numbered item its own ``<w:num>`` with a ``<w:startOverride>``: the marker
    must be the override, not a counter that restarts whenever a comment
    paragraph interrupts the list.
    """
    out = {}
    try:
        root = doc.part.numbering_part.element
    except Exception:
        return out
    absmap = {}
    for an in root.findall(qn("w:abstractNum")):
        aid = an.get(qn("w:abstractNumId"))
        fmt, start, text = "decimal", 1, "%1."
        lvl = an.find(qn("w:lvl"))
        if lvl is not None:
            f = lvl.find(qn("w:numFmt"))
            s = lvl.find(qn("w:start"))
            t = lvl.find(qn("w:lvlText"))
            if f is not None:
                fmt = f.get(qn("w:val")) or fmt
            if s is not None:
                try:
                    start = int(s.get(qn("w:val")))
                except Exception:
                    pass
            if t is not None:
                text = t.get(qn("w:val")) or text
        absmap[aid] = (fmt, start, text)
    for num in root.findall(qn("w:num")):
        nid = num.get(qn("w:numId"))
        a = num.find(qn("w:abstractNumId"))
        fmt, start, text = absmap.get(
            a.get(qn("w:val")) if a is not None else "",
            ("decimal", 1, "%1."))
        ov = num.find(qn("w:lvlOverride"))
        if ov is not None:
            so = ov.find(qn("w:startOverride"))
            if so is not None:
                try:
                    start = int(so.get(qn("w:val")))
                except Exception:
                    pass
        out[nid] = (start, fmt, text)
    return out


def _num_id(par):
    """(numId, ilvl) of a paragraph, or (None, 0)."""
    try:
        pPr = par._p.pPr
        if pPr is None:
            return None, 0
        numPr = pPr.find(qn("w:numPr"))
        if numPr is None:
            # the numbering may come from the style
            st = par.style.element.find(qn("w:pPr"))
            if st is None:
                return None, 0
            numPr = st.find(qn("w:numPr"))
            if numPr is None:
                return None, 0
        nid = numPr.find(qn("w:numId"))
        ilvl = numPr.find(qn("w:ilvl"))
        return (nid.get(qn("w:val")) if nid is not None else None,
                int(ilvl.get(qn("w:val"))) if ilvl is not None else 0)
    except Exception:
        return None, 0


def _footnotes(doc):
    """id -> plain text of every real footnote in word/footnotes.xml."""
    out = {}
    try:
        from docx.oxml import parse_xml
        part = None
        for p in doc.part.package.iter_parts():
            if str(p.partname).endswith("/footnotes.xml"):
                part = p
                break
        if part is None:
            return out
        root = parse_xml(part.blob)
        for fn in root.findall(qn("w:footnote")):
            fid = fn.get(qn("w:id"))
            try:
                if int(fid) <= 0:
                    continue          # separator / continuation separator
            except Exception:
                continue
            txt = "".join(t.text or "" for t in fn.iter(qn("w:t")))
            txt = " ".join(txt.split())
            if txt:
                out[fid] = txt
    except Exception:
        pass
    return out


def _footnote_refs(par):
    """Footnote ids referenced by this paragraph, in document order."""
    out = []
    for el in par._p.iter(qn("w:footnoteReference")):
        fid = el.get(qn("w:id"))
        if fid is not None and fid not in out:
            out.append(fid)
    return out


def _shd(cell):
    """Cell shading (w:shd) as an RGB tuple, or None."""
    try:
        tcPr = cell._tc.tcPr
        if tcPr is None:
            return None
        el = tcPr.find(qn("w:shd"))
        if el is None:
            return None
        fill = (el.get(qn("w:fill")) or "").strip()
        if len(fill) != 6 or fill.lower() == "auto":
            return None
        return (int(fill[0:2], 16) / 255.0, int(fill[2:4], 16) / 255.0,
                int(fill[4:6], 16) / 255.0)
    except Exception:
        return None


def _v_align(cell):
    """Vertical alignment of a table cell: top / center / bottom."""
    try:
        tcPr = cell._tc.tcPr
        if tcPr is None:
            return "top"
        el = tcPr.find(qn("w:vAlign"))
        if el is None:
            return "top"
        v = (el.get(qn("w:val")) or "top").lower()
        return v if v in ("top", "center", "bottom") else "top"
    except Exception:
        return "top"


def _expand(atoms):
    """Split run-granular atoms into word / CJK-glyph tokens.

    A whole sentence is usually a single run, so filling lines at run
    granularity leaves the first line half empty (a long run cannot be broken).
    Splitting first lets the filler break anywhere a reader would expect.
    """
    out = []
    for (t, key, size, col, dy) in atoms:
        if not t:
            continue
        for tok in _CELL_TOK.findall(t):
            out.append((tok, key, size, col, dy))
    return out


class _State:
    """Cursor + page factory for one document."""

    def __init__(self, pdf, pw, ph, ml, mr, mt, mb, head, foot, foot_page,
                 head_img=None):
        self.pdf = pdf
        self.pw, self.ph = pw, ph
        self.ml, self.mr, self.mt, self.mb = ml, mr, mt, mb
        self.text_w = pw - ml - mr
        self.head = head
        self.foot = foot
        self.foot_page = foot_page
        self.head_img = head_img
        self.page = None
        self.y = 0.0
        self.pages = 0
        self.tw = None
        self.tw_color = None
        # footnotes land at the bottom of the page that references them, so the
        # body has to stop above the reserved block
        self.note_lead = 9.6
        self.note_size = 8.0
        self.notes = []                  # [(display number, wrapped lines)]
        self.note_h = 0.0
        self.new_page()

    # -- footnotes -----------------------------------------------------
    def _note_lines(self, fid):
        """Wrapped lines a footnote occupies at the bottom of the page."""
        txt = _FN_TEXT.get(fid)
        if not txt:
            return []
        return measure.wrap_text(txt, "tiro", self.note_size,
                                 self.text_w - 12.0) or [""]

    def reserve_notes(self, refs):
        """Height that *refs* (not yet placed) will need at the page bottom."""
        h = 0.0
        for fid in refs:
            if any(n == _FN_NUM.get(fid) for n, _l in self.notes):
                continue
            lines = self._note_lines(fid)
            if lines:
                h += len(lines) * self.note_lead
        return h + (5.0 if h else 0.0)

    def add_notes(self, refs):
        for fid in refs:
            num = _FN_NUM.get(fid)
            if not num or any(n == num for n, _l in self.notes):
                continue
            lines = self._note_lines(fid)
            if not lines:
                continue
            self.notes.append((num, lines))
        self.note_h = sum(len(l) for _n, l in self.notes) * self.note_lead + \
            (5.0 if self.notes else 0.0)

    def _draw_notes(self):
        if not self.notes:
            return
        top = self.ph - self.mb - self.note_h
        try:
            self.page.draw_line((self.ml, top), (self.ml + self.text_w * 0.33, top),
                                color=(0.35, 0.35, 0.35), width=0.5)
        except Exception:
            pass
        y = top + 5.0
        for num, lines in self.notes:
            for i, ln in enumerate(lines):
                if i == 0:
                    self._draw(str(num), self.ml, y + self.note_size * 0.80,
                               "tiro", self.note_size * 0.70, (0, 0, 0))
                self._draw(ln, self.ml + 10.0,
                           y + self.note_size * 0.80, "tiro",
                           self.note_size, (0, 0, 0))
                y += self.note_lead

    def new_page(self):
        self._draw_notes()
        self._flush()
        self.page = self.pdf.new_page(width=self.pw, height=self.ph)
        self.pages += 1
        self.y = self.mt
        self.tw = None
        self.tw_color = None
        self.notes = []
        self.note_h = 0.0
        if self.head_img:
            blob, iw, ih, align = self.head_img
            y0 = max(4.0, self.mt * 0.45 - 4.0)
            # never let the logo reach into the text area
            ih = min(ih, max(4.0, self.mt - y0 - 2.0))
            if align == "right":
                x0 = self.pw - self.mr - iw
            elif align == "center":
                x0 = (self.pw - iw) / 2.0
            else:
                x0 = self.ml
            try:
                self.page.insert_image(
                    fitz.Rect(x0, y0, x0 + iw, y0 + ih), stream=blob)
            except Exception:
                pass
        elif self.head:
            self._text_at(self.head, self.ml, max(10.0, self.mt * 0.45), 8.5,
                          (0.45, 0.45, 0.45), align="left")
        if self.foot or self.foot_page:
            txt = self.foot or ""
            if self.foot_page:
                txt = (txt + " " + str(self.pages)).strip()
            if txt:
                self._text_at(txt, self.ml, self.ph - max(12.0, self.mb * 0.45),
                              8.5, (0.45, 0.45, 0.45), align="left")

    def _flush(self):
        """Commit the accumulated text of the current page.

        ``TextWriter.color`` is a property of the writer, not of each append:
        ``write_text`` paints *everything* in the writer with the colour that
        is current when it runs.  So one writer per colour is the only way to
        keep run colours -- see :meth:`_draw`.
        """
        if getattr(self, "tw", None) is not None:
            try:
                self.tw.write_text(self.page, color=self.tw_color)
            except Exception:
                pass
            self.tw = None
            self.tw_color = None

    def ensure(self, h):
        if self.y + h > self.ph - self.mb - self.note_h:
            self.new_page()

    def _draw(self, text, x, baseline, key, size, color):
        if not text.strip():
            return
        cjk = _has_cjk(text)
        fkey = "china-s" if cjk else key
        if self.tw is None or self.tw_color != color:
            self._flush()
            self.tw = fitz.TextWriter(self.page.rect, color=color)
            self.tw_color = color
        try:
            self.tw.append((x, baseline), text, font=_font(fkey),
                           fontsize=size)
        except Exception:
            try:
                self.page.insert_text((x, baseline), text,
                                      fontname="china-s" if cjk else key,
                                      fontsize=size, color=color)
            except Exception:
                pass

    def _text_at(self, text, x, baseline, size, color, align="left"):
        key = "helv"
        w = text_width(text, key, size)
        if align == "center":
            x = self.ml + (self.text_w - w) / 2.0
        self._draw(text, x, baseline, key, size, color)

    # ------------------------------------------------------------------
    def para(self, par, size_default=10.5, indent=0.0, width=None,
             prefix=None):
        """Flow one paragraph, wrapping at the text column width."""
        avail = width if width is not None else self.text_w - indent
        atoms, prose = _atoms(par, size_default)
        if prefix:
            atoms = [(prefix, "tiro", size_default, (0, 0, 0), 0.0)] + atoms
        if not atoms:
            self.y += size_default * 0.6
            return
        # line advance must follow the *largest* type on the line: a paragraph
        # that starts with a superscript would otherwise get a 6pt lead and
        # collide with the next paragraph
        size0 = max(a[2] for a in atoms)
        pf = par.paragraph_format
        before = pf.space_before.pt if pf.space_before is not None else 0.0
        after = pf.space_after.pt if pf.space_after is not None else 0.0
        self.y += before
        ls = pf.line_spacing
        lead = size0 * 1.18
        if isinstance(ls, float):
            lead = size0 * 1.18 * ls
        elif ls is not None:
            try:
                lead = ls.pt
            except Exception:
                pass
        first_indent = 0.0
        try:
            if pf.first_line_indent is not None:
                first_indent = pf.first_line_indent.pt
        except Exception:
            pass
        align = par.alignment
        if align is None:
            try:
                align = par.style.paragraph_format.alignment
            except Exception:
                align = None
        if atoms and not prose:
            # a displayed equation (OMML only): centre it like Word does
            align = WD_ALIGN_PARAGRAPH.CENTER

        # greedy line fill (shared kernel: word / CJK tokens + kinsoku)
        atoms = _expand(atoms)
        limit = avail
        lines = [(ln, ln[-1][1] + ln[-1][2] if ln else 0.0)
                 for ln in _wrap(atoms, limit)]
        if not lines:
            return
        try:
            keep_next = bool(pf.keep_with_next)
        except Exception:
            keep_next = False

        # footnotes referenced here need room at the bottom of this page, so
        # reserve it before anything is drawn (Word does the same)
        refs = _footnote_refs(par)
        if refs:
            self.ensure(self.reserve_notes(refs))

        # Word's widow / orphan control: never leave a single line of a
        # multi-line paragraph alone at the bottom of a page, and keep a
        # keepNext paragraph (every heading) with the line that follows it
        if len(lines) > 1:
            self.ensure(min(2, len(lines)) * lead)
        if keep_next:
            self.ensure((len(lines) + 1) * lead)
        for li, (line, lw) in enumerate(lines):
            # per-line lead from the largest type actually on that line
            line_size = max((a[0][2] for a in line), default=size0)
            line_lead = lead if isinstance(ls, float) or ls is not None \
                else line_size * 1.18
            if line_size > size0:
                line_lead = max(line_lead, line_size * 1.18)
            self.ensure(line_lead)
            x0 = self.ml + indent + (first_indent if li == 0 else 0.0)
            space = 0.0
            if align == WD_ALIGN_PARAGRAPH.CENTER:
                x0 += max(0.0, (avail - lw) / 2.0)
            elif align == WD_ALIGN_PARAGRAPH.RIGHT:
                x0 += max(0.0, avail - lw)
            elif align == WD_ALIGN_PARAGRAPH.JUSTIFY and li < len(lines) - 1:
                space = measure.justify_extra(line, limit, line_size)
            baseline = self.y + line_lead * 0.78
            shift = 0.0
            for atom, ox, _w in line:
                t, key, size, col, dy = atom
                if t == " ":
                    shift += space
                    continue
                self._draw(t, x0 + ox + shift, baseline + dy, key, size, col)
            self.y += line_lead
        if refs:
            self.add_notes(refs)
        self.y += after

    # ------------------------------------------------------------------
    def table(self, tbl, size_default=9.0):
        """Draw a table: grid widths, merged cells, thin borders, wrapped text."""
        ncols = len(tbl.columns)
        if not ncols:
            return
        widths = []
        for col in tbl.columns:
            w = None
            try:
                w = col.width.pt if col.width is not None else None
            except Exception:
                w = None
            widths.append(w)
        rows = tbl.rows
        # Word lays a fixed table out by w:tcW (the cell width), not by the
        # grid: python-docx writes per-cell widths and leaves <w:gridCol>
        # evenly split, so reading the grid alone would flatten every column
        tc_widths = []
        try:
            for tc in rows[0]._tr.findall(qn("w:tc")):
                span = 1
                w = None
                tcPr = tc.tcPr
                if tcPr is not None:
                    gs = tcPr.find(qn("w:gridSpan"))
                    if gs is not None:
                        span = int(gs.get(qn("w:val")) or 1)
                    tcw = tcPr.find(qn("w:tcW"))
                    if tcw is not None and (tcw.get(qn("w:type")) in (None, "dxa")):
                        try:
                            w = float(tcw.get(qn("w:w"))) / 20.0
                        except Exception:
                            w = None
                tc_widths.append((span, w))
        except Exception:
            tc_widths = []
        if tc_widths and all(w for _s, w in tc_widths) and \
                sum(sp for sp, _w in tc_widths) == ncols:
            widths = [w for _s, w in tc_widths]
        if not all(widths) or sum(widths) <= 1:
            widths = [self.text_w / ncols] * ncols
        else:
            scale = min(1.0, self.text_w / sum(widths))
            widths = [w * scale for w in widths]
        pad = 2.0

        occupied = set()
        header = None                    # (plans, height) of a repeat row
        line_h = size_default * 1.22

        def draw_row(plans, top, row_h):
            for (c0, span, cell, vmerge, lines, h) in plans:
                x = self.ml + sum(widths[:c0])
                w = sum(widths[c0:c0 + span])
                rect = fitz.Rect(x, top, x + w, top + row_h)
                shd = _shd(cell)
                if shd:
                    try:
                        self.page.draw_rect(rect, color=None, fill=shd)
                    except Exception:
                        pass
                try:
                    self.page.draw_rect(rect, color=(0.72, 0.75, 0.80),
                                        width=0.5)
                except Exception:
                    pass
                va = _v_align(cell)
                block_h = len(lines) * line_h
                if va == "center":
                    yy = top + max(pad, (row_h - block_h) / 2.0) \
                        + size_default * 0.92
                elif va == "bottom":
                    yy = top + max(pad, row_h - block_h - pad) \
                        + size_default * 0.92
                else:
                    yy = top + pad + size_default * 0.92
                for atoms, lw in lines:
                    xx = x + pad
                    for atom, aw in atoms:
                        t, key, size, col, dy = atom
                        if t != " ":
                            self._draw(t, xx, yy + dy, key, size, col)
                        xx += aw
                    yy += line_h

        for ri, row in enumerate(rows):
            # cell rectangles for this row, deduped by underlying tc
            cells = []
            ci = 0
            seen = set()
            for cell in row.cells:
                tc = cell._tc
                if id(tc) in seen:
                    ci += 1
                    continue
                seen.add(id(tc))
                span = 1
                try:
                    gs = tc.tcPr.find(qn("w:gridSpan"))
                    if gs is not None:
                        span = int(gs.get(qn("w:val")) or 1)
                except Exception:
                    span = 1
                vmerge = None
                try:
                    vm = tc.tcPr.find(qn("w:vMerge"))
                    if vm is not None:
                        vmerge = vm.get(qn("w:val")) or "continue"
                except Exception:
                    vmerge = None
                cells.append((ci, span, cell, vmerge))
                ci += span
            if not cells:
                continue

            # measure row height from cell text
            row_h = line_h
            plans = []
            for (c0, span, cell, vmerge) in cells:
                if (ri, c0) in occupied:
                    continue
                w = sum(widths[c0:c0 + span]) - 2 * pad
                lines = self._cell_lines(cell, max(6.0, w), size_default)
                h = max(line_h, len(lines) * line_h) + 2 * pad
                row_h = max(row_h, h)
                plans.append((c0, span, cell, vmerge, lines, h))
            # a header row flagged w:tblHeader repeats at the top of every
            # continuation page, exactly like Word
            try:
                trPr = row._tr.trPr
                is_head = trPr is not None and \
                    trPr.find(qn("w:tblHeader")) is not None
            except Exception:
                is_head = False
            pages_before = self.pages
            self.ensure(row_h)
            if self.pages != pages_before and header is not None:
                draw_row(header[0], self.y, header[1])
                self.y += header[1]
            draw_row(plans, self.y, row_h)
            if is_head and header is None:
                header = (plans, row_h)
            self.y += row_h
        self.y += 4.0

    def _cell_lines(self, cell, width, size_default):
        """Wrap one cell's paragraphs into lines of atoms.

        Atoms arrive at run granularity, which is far too coarse to break on:
        a whole sentence would overflow the cell.  Tokenise to words / CJK
        glyphs first, then greedy-fill, and hard-split a token that is wider
        than the cell on its own.
        """
        out = []
        for par in cell.paragraphs:
            atoms, _prose = _atoms(par, size_default)
            cur, curw = [], 0.0
            for (t, key, size, col, dy) in atoms:
                for tok in _CELL_TOK.findall(t):
                    if tok.isspace():
                        if cur:
                            w = text_width(tok, key, size)
                            cur.append(((tok, key, size, col, dy), w))
                            curw += w
                        continue
                    w = text_width(tok, key, size)
                    if cur and curw + w > width:
                        out.append((cur, curw))
                        cur, curw = [], 0.0
                    if w > width and len(tok) > 1:
                        piece = ""
                        for ch in tok:
                            if piece and \
                                    text_width(piece + ch, key, size) > width:
                                pw = text_width(piece, key, size)
                                out.append(([((piece, key, size, col, dy),
                                              pw)], pw))
                                piece = ch
                            else:
                                piece += ch
                        if piece:
                            pw = text_width(piece, key, size)
                            cur, curw = [((piece, key, size, col, dy), pw)], pw
                        continue
                    cur.append(((tok, key, size, col, dy), w))
                    curw += w
            if cur:
                out.append((cur, curw))
        return out or [([], 0.0)]

    # ------------------------------------------------------------------
    def image(self, blob, w_pt, h_pt):
        """Place an inline image, scaled into the text column."""
        scale = min(1.0, self.text_w / w_pt) if w_pt > 0 else 1.0
        w, h = w_pt * scale, h_pt * scale
        self.ensure(h + 4)
        rect = fitz.Rect(self.ml + (self.text_w - w) / 2.0, self.y,
                         self.ml + (self.text_w + w) / 2.0, self.y + h)
        try:
            self.page.insert_image(rect, stream=blob)
        except Exception:
            pass
        self.y += h + 4

    def finish(self):
        self._draw_notes()
        self._flush()


def _body_size(doc):
    try:
        sz = doc.styles["Normal"].font.size
        if sz is not None:
            return sz.pt
    except Exception:
        pass
    return 10.5


def _header_footer(sec):
    """(header text, footer text, footer carries a PAGE field, header picture).

    A running head is often a *bitmap* -- corporate reports print a logo in the
    corner of every page -- and reading only ``p.text`` dropped it, so a DOCX
    whose header correctly carried the logo came back out of this renderer
    without one.  The picture is returned as (blob, w_pt, h_pt, align).
    """
    head = ""
    foot = ""
    page_field = False
    head_img = None
    try:
        if not sec.header.is_linked_to_previous:
            hp = [p for p in sec.header.paragraphs]
            head = " ".join(p.text for p in hp if p.text)
            for p in hp:
                got = _images_in(p)
                if got:
                    align = "left"
                    try:
                        from docx.enum.text import WD_ALIGN_PARAGRAPH as _AL
                        if p.alignment == _AL.RIGHT:
                            align = "right"
                        elif p.alignment == _AL.CENTER:
                            align = "center"
                    except Exception:
                        pass
                    head_img = (got[0][0], got[0][1], got[0][2], align)
                    break
    except Exception:
        pass
    try:
        if not sec.footer.is_linked_to_previous:
            foot = " ".join(p.text for p in sec.footer.paragraphs if p.text)
            xml = sec.footer._element.xml
            page_field = "PAGE" in xml
    except Exception:
        pass
    return head, foot, page_field, head_img


def _flatten(blob):
    """Composite a transparent image onto white.

    ``insert_image(stream=...)`` drops a PNG's alpha channel, so a logo whose
    background is transparent came out as a solid black rectangle in the
    rendered PDF while the DOCX itself was correct.  Flattening here keeps the
    renderer faithful to what Word shows.
    """
    try:
        from PIL import Image
        import io as _io
        im = Image.open(_io.BytesIO(blob))
        if im.mode not in ("RGBA", "LA", "P"):
            return blob
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        bg.alpha_composite(im)
        buf = _io.BytesIO()
        bg.convert("RGB").save(buf, format="PNG")
        return buf.getvalue()
    except Exception:
        return blob


def _images_in(par):
    """[(blob, w_pt, h_pt)] for the inline pictures anchored in this paragraph."""
    out = []
    for blip in par._p.iter(qn("a:blip")):
        rid = blip.get(qn("r:embed"))
        if not rid:
            continue
        try:
            part = par.part.related_parts[rid]
            blob = part.blob
        except Exception:
            continue
        w = h = 0.0
        ext = par._p.find(".//" + qn("wp:extent"))
        if ext is not None:
            try:
                w = int(ext.get("cx")) / 12700.0
                h = int(ext.get("cy")) / 12700.0
            except Exception:
                w = h = 0.0
        if w <= 0 or h <= 0:
            try:
                from PIL import Image
                import io as _io
                im = Image.open(_io.BytesIO(blob))
                w, h = im.size[0] * 0.75, im.size[1] * 0.75
            except Exception:
                w, h = 200.0, 150.0
        out.append((_flatten(blob), w, h))
    return out


def convert_docx_pdf(docx_path, pdf_path):
    """Render one DOCX into a PDF.  Returns (pages, paragraphs, tables)."""
    doc = Document(docx_path)
    sec = doc.sections[0]
    pw, ph = sec.page_width.pt, sec.page_height.pt
    ml, mr = sec.left_margin.pt, sec.right_margin.pt
    mt, mb = sec.top_margin.pt, sec.bottom_margin.pt
    body = _body_size(doc)
    head, foot, foot_page, head_img = _header_footer(sec)

    # numbering definitions + footnote text, read once for the whole document
    numbering = _numbering(doc)
    used = {}
    _FN_NUM.clear()
    _FN_TEXT.clear()
    _FN_TEXT.update(_footnotes(doc))
    seq = 0
    for el in doc.element.body.iter(qn("w:footnoteReference")):
        fid = el.get(qn("w:id"))
        if fid is not None and fid in _FN_TEXT and fid not in _FN_NUM:
            seq += 1
            _FN_NUM[fid] = seq

    pdf = fitz.open()
    st = _State(pdf, pw, ph, ml, mr, mt, mb, head, foot, foot_page,
                head_img=head_img)
    npar = ntab = 0
    num = 0
    for block in _iter_body(doc):
        if isinstance(block, Table):
            st.table(block, size_default=max(6.0, body - 1.5))
            ntab += 1
            num = 0
            continue
        # an explicit page break: our own writer emits these to reproduce the
        # source PDF's pagination, and Word-authored files use them too.  The
        # renderer used to ignore them entirely, so a document that paginated
        # to 118 pages in Word came out at 94 here.
        try:
            if block.paragraph_format.page_break_before:
                st.new_page()
        except Exception:
            pass
        _brs = block._p.findall(".//" + qn("w:br"))
        if any((b.get(qn("w:type")) or "") == "page" for b in _brs):
            st.new_page()
            npar += 1
            continue
        style = ""
        try:
            style = (block.style.name or "").lower()
        except Exception:
            pass
        prefix = None
        nid, _ilvl = _num_id(block)
        nfo = numbering.get(nid) if nid else None
        if style.startswith("list") and nfo is not None:
            # the real <w:startOverride> wins: our own writer emits one
            # <w:num> per item, and a plain counter would restart at 1
            # whenever a comment paragraph interrupts the list
            start, fmt, lvl_text = nfo
            k = used.get(nid, 0)
            prefix = _marker(start + k, fmt, lvl_text)
            used[nid] = k + 1
        elif style.startswith("list number"):
            num += 1
            prefix = "%d. " % num
        elif style.startswith("list bullet"):
            prefix = "\u2022  "
        else:
            num = 0
        imgs = _images_in(block)
        for blob, w, h in imgs:
            st.image(blob, w, h)
        # a displayed equation carries no w:t at all -- python-docx sees an
        # empty paragraph, but the m:t text must still be rendered
        has_math = bool(block._p.findall(".//" + qn("m:oMath")))
        if block.text.strip() or prefix or has_math:
            st.para(block, size_default=body, prefix=prefix)
        elif not imgs:
            st.y += body * 0.5
        npar += 1

    d = os.path.dirname(os.path.abspath(pdf_path))
    if d:
        os.makedirs(d, exist_ok=True)
    st.finish()
    pdf.save(pdf_path, deflate=True, garbage=3)
    pdf.close()
    return st.pages, npar, ntab
