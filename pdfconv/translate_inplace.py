"""In-place translation: redact the source glyphs, redraw the translation.

``reflow`` re-paginates, which is right for papers and wrong for contracts,
invoices and forms: those carry page numbers, seals, signature blocks and
cross-references that must not move.  ``inplace`` therefore leaves the page
canvas exactly as it is and treats every text block as a box to be refilled:

1. read the block's bbox, dominant type size, colour, alignment and first
   baseline;
2. translate the block's text (sentence units, placeholders protected);
3. measure the translation with the *same* kernel the reverse renderers use
   (:mod:`pdfconv.measure`), at 100%, 96%, ... down to 75% of the original
   size, and take the first scale whose wrapped lines fit the box plus the
   whitespace gap below it;
4. if nothing fits, keep the source text for that block -- a partial
   translation is acceptable, an overflowing one is not;
5. redact the original glyphs (``fill=False`` so a tinted background
   survives, ``images=NONE`` so a logo inside the box survives) and draw the
   new lines with a ``TextWriter`` in the original colour.

Because step 5 only ever touches the block's own bbox, page count, figure
positions, page numbers and stamps are untouched by construction -- which is
the whole point of the mode.

Rotated text (``dir != (1, 0)``) is skipped: the box would need the
translation drawn along the rotation, and a wrong rotation is worse than an
untranslated label.
"""
import re

import fitz

from . import measure

CJK_FONT = "china-s"          # built-in CJK face MuPDF ships with
CJK_SCALE = 0.95              # han glyphs fill the em; 100% looks oversized
FLOOR = 0.75                  # smallest type scale we accept before giving up
LEAD = 1.24                   # baseline-to-baseline, in multiples of the size
_STEPS = (1.0, 0.96, 0.92, 0.88, 0.84, 0.80, 0.75)
_MAX_GROW = 3.0               # how many extra lines may spill into the gap


def _rgb(c):
    return (((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
            (c & 255) / 255.0)


def _join_lines(parts):
    """Stitch a block's lines back into one string.

    Hyphenated line breaks are undone (``transla-`` / ``tion``) because the
    translation must not carry the source's break positions; CJK joins
    without a space, Latin joins with one.
    """
    text = ""
    for p in parts:
        p = p.strip()
        if not p:
            continue
        if not text:
            text = p
            continue
        if (text.endswith("-") and not text.endswith("--")
                and p[:1].islower()):
            text = text[:-1] + p
        elif measure.is_cjk(text[-1]) or measure.is_cjk(p[0]):
            text += p
        else:
            text += " " + p
    return text


def _align_of(rects, bbox):
    """Guess left / right / center from the line boxes of one block.

    The *first* line is deliberately ignored: it is the one line that may be
    indented (a justified paragraph) or that may fill the whole measure (the
    first line of a centred title).  Deciding on the remaining lines is what
    separates ``left`` from ``center`` in both cases.
    """
    if not rects or len(rects) < 2:
        return "left"
    body = rects[1:]
    lefts = [r.x0 for r in body]

    def spread(v):
        return max(v) - min(v)

    if spread(lefts) < 2.0 and abs(min(lefts) - bbox.x0) < 2.0:
        return "left"
    centres = [(r.x0 + r.x1) / 2.0 for r in rects]
    if spread(centres) < 2.0:
        return "center"
    rights = [r.x1 for r in rects]
    if spread(rights) < 2.0 and abs(max(rights) - bbox.x1) < 2.0:
        return "right"
    return "left"


def _clean(text):
    """Repair the two extraction artefacts that survive a redraw badly.

    TeX sets accents as spacing glyphs, so ``Méaux`` extracts as ``M´eaux``
    (U+00B4) and would be re-drawn with a visible gap; soft hyphens are
    invisible but count as characters when the box is measured.
    """
    if not text:
        return text
    text = text.replace("\u00ad", "")
    # TeX emits the accent as a spacing glyph *before* the letter it belongs
    # to ("M´eaux"), so the mark has to move past that letter before NFC can
    # compose it -- otherwise NFC happily builds "Ḿeaux".
    text = re.sub(r"\u00b4([A-Za-z])", lambda m: m.group(1) + "\u0301", text)
    try:
        import unicodedata
        text = unicodedata.normalize("NFC", text)
    except Exception:
        pass
    return text


def _read(block):
    """Extract (text, size, colour, font key, first baseline, align)."""
    lines = block.get("lines") or []
    if not lines:
        return None
    for ln in lines:
        d = ln.get("dir", (1.0, 0.0))
        if abs(d[0] - 1.0) > 1e-3 or abs(d[1]) > 1e-3:
            return None                       # rotated: leave it alone
    parts, rects, spans = [], [], []
    for ln in lines:
        ls = ln.get("spans") or []
        if not ls:
            continue
        parts.append("".join(s.get("text", "") for s in ls))
        rects.append(fitz.Rect(ln["bbox"]))
        spans.extend(ls)
    if not spans:
        return None
    text = _clean(_join_lines(parts))
    if len(text.strip()) < 2:
        return None
    dom = max(spans, key=lambda s: len(s.get("text", "")))
    flags = int(dom.get("flags", 0))
    size = float(dom.get("size", 10.0)) or 10.0
    color = _rgb(int(dom.get("color", 0)))
    key = measure.font_key(dom.get("font", ""), flags & 16, flags & 2,
                           serif=bool(flags & 4))
    base0 = None
    try:
        base0 = float(spans[0]["origin"][1])
    except Exception:
        pass
    if base0 is None:
        base0 = float(lines[0]["bbox"][3]) - 0.2 * size
    return (text, size, color, key, base0,
            _align_of(rects, fitz.Rect(block["bbox"])))


def _fit(text, key, size, rect, gap, floor):
    """Largest scale whose wrapped lines fit the box.  ``None`` if none does."""
    if rect.width <= 1.0 or rect.height <= 1.0:
        return None
    for sc in _STEPS:
        if sc < floor - 1e-6:
            break
        s = size * sc
        lines = measure.wrap_text(text, key, s, rect.width, slack=0.02)
        lead = s * LEAD
        need = len(lines) * lead
        room = rect.height + min(gap, _MAX_GROW * lead)
        if need <= room + 0.6:
            return sc, s, lines, lead
    return None


def _flush(page, items):
    """Draw prepared lines, one TextWriter per colour.

    ``TextWriter.color`` belongs to the writer, not to each ``append``, so a
    page with black body text and a red heading needs two writers.
    """
    by = {}
    for x, y, text, key, size, color in items:
        by.setdefault(color, []).append((x, y, text, key, size))
    for color, group in by.items():
        tw = fitz.TextWriter(page.rect, color=color)
        for x, y, text, key, size in group:
            try:
                tw.append((x, y), text, font=measure.font(key), fontsize=size)
            except Exception:
                continue
        try:
            tw.write_text(page, color=color)
        except Exception:
            pass


def _rewrite_doc(doc, out_pdf, transform, want, floor, st=None):
    """Shared in-place loop: refill each text box with ``transform(text)``.

    ``transform`` returns the replacement, or a falsy / equal value to leave a
    box alone.  Translation and literal find/replace are just two transforms;
    keeping the redact + measure + redraw machinery here is what guarantees the
    page canvas, page count, figures and stamps never move.
    """
    report = {"boxes": 0, "rewritten": 0, "shrunk": 0, "overflow": 0}
    for pno in want:
        if pno >= len(doc):
            break
        page = doc[pno]
        try:
            blocks = [b for b in page.get_text("dict").get("blocks", [])
                      if b.get("type") == 0]
        except Exception:
            continue
        if not blocks:
            continue
        # every block's vertical extent, for the gap-below computation
        bands = sorted((float(b["bbox"][1]), float(b["bbox"][3]),
                        float(b["bbox"][0]), float(b["bbox"][2]))
                       for b in blocks)

        def gap_below(r):
            g = 1e9
            for top, _bot, x0, x1 in bands:
                if top < r.y1 - 0.5:
                    continue
                if x1 <= r.x0 + 0.5 or x0 >= r.x1 - 0.5:
                    continue                      # different column
                g = min(g, top - r.y1)
            return 1e9 if g == 1e9 else max(0.0, g)

        plans = []
        for b in blocks:
            info = _read(b)
            if info is None:
                continue
            text, size, color, key, base0, align = info
            report["boxes"] += 1
            if st is not None:
                st.boxes += 1
            new = transform(text)
            if not new or new == text:
                continue                          # nothing changed: no redaction
            if measure.has_cjk(new):
                # one face for the whole box: Droid Sans Fallback carries both
                # han and Latin, so a mixed line never switches mid-sentence
                key, size = CJK_FONT, size * CJK_SCALE
            r = fitz.Rect(b["bbox"])
            got = _fit(new, key, size, r, gap_below(r), floor)
            if got is None:
                report["overflow"] += 1
                if st is not None:
                    st.overflow += 1
                continue
            sc, s, lines, lead = got
            if sc < 1.0:
                report["shrunk"] += 1
                if st is not None:
                    st.shrunk += 1
            report["rewritten"] += 1
            plans.append((r, lines, s, lead, color, key, base0, align))

        if not plans:
            continue
        # 1) drop the source glyphs, keeping backgrounds and images
        for r, _l, _s, _ld, _c, _k, _b0, _a in plans:
            try:
                page.add_redact_annot(r, fill=False, cross_out=False)
            except Exception:
                try:
                    page.add_redact_annot(r, fill=False)
                except Exception:
                    pass
        try:
            page.apply_redactions(images=fitz.PDF_REDACT_IMAGE_NONE,
                                  graphics=fitz.PDF_REDACT_LINE_ART_NONE)
        except Exception:
            pass
        # 2) draw the replacement inside the same boxes
        items = []
        for r, lines, s, lead, color, key, base0, align in plans:
            asc = 0.80
            try:
                asc = float(measure.font(key).ascender) or 0.80
            except Exception:
                pass
            y = base0 if len(lines) == 1 else r.y0 + asc * s
            for ln in lines:
                w = measure.text_width(ln, key, s)
                if align == "right":
                    x = r.x1 - w
                elif align == "center":
                    x = r.x0 + (r.width - w) / 2.0
                else:
                    x = r.x0
                items.append((x, y, ln, key, s, color))
                y += lead
        _flush(page, items)
    doc.save(out_pdf, garbage=3, deflate=True)
    doc.close()
    return report


def translate_pdf_inplace(pdf_path, out_pdf, translator, src="en", tgt="zh",
                          glossary=None, stats=None, floor=FLOOR,
                          pages=None):
    """Translate *pdf_path* without moving anything.  Returns ``(stats, n)``."""
    from .translate import Stats, translate_text

    doc = fitz.open(pdf_path)
    st = stats if stats is not None else Stats()
    want = range(len(doc)) if pages is None else pages

    def transform(text):
        return translate_text(text, translator, src, tgt, glossary, stats=st)

    _rewrite_doc(doc, out_pdf, transform, want, floor, st)
    return st, len(want)


def rewrite_pdf_inplace(pdf_path, out_pdf, pairs, floor=FLOOR, pages=None):
    """Literal find/replace inside the existing text boxes.

    ``pairs`` is a list of ``(find, replace)``; all are applied to each block in
    order.  Returns ``(report, n_pages)`` where report counts boxes,
    rewritten boxes, shrunk boxes and boxes kept because the new text would not
    fit.  Deliberately no regex: this is a document edit, not a search tool, and
    an accidental catastrophic backtracking inside a worker process would take
    the whole pool down.
    """
    doc = fitz.open(pdf_path)
    want = range(len(doc)) if pages is None else pages

    def transform(text):
        new = text
        for find, repl in pairs:
            new = new.replace(find, repl)
        return new

    report = _rewrite_doc(doc, out_pdf, transform, want, floor)
    return report, len(want)
