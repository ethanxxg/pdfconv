"""OCR fallback for pages whose text cannot be recovered from the PDF itself.

Two situations need it, and they are different problems:

1. **Scanned pages** -- no text layer at all.
2. **Undecodable CID text** -- there *is* a text layer, but the font is
   Identity-encoded with no /ToUnicode, so the code carries no meaning and
   extraction yields garbage.  (`安全牛-自主智能体Agentic.pdf`: 118 pages of
   plausible-looking but wrong CJK.)

Where the boundary sits
-----------------------
The engine is a *perception primitive*: pixels in, boxes and strings out --
the same role MuPDF plays turning a PDF into glyphs.  Everything above it is
this project's own code: deciding which pages need OCR, discarding the
watermark, re-flowing boxes into our line/paragraph model, inferring font size
from box height and ink colour from the pixels, reading order, tables,
figures, DOCX emission.

The engine is optional.  Without rapidocr installed the converter still runs
and simply leaves such pages exactly as they were.
"""
import math
import os
import re

import fitz

from . import cache

_DETECT_DEG = 5.0          # OCR boxes rotated more than this are watermark
_DPI = 200

# OCR box height is a noisy proxy for type size: the *same* body line measures
# anywhere between ~0.85x and ~1.18x of the page reference, depending on
# whether it happens to contain digits, ascenders/descenders or full-width
# punctuation.  Feeding that noise into the downstream `size >= body + 0.6`
# heading test promotes ordinary paragraphs to Heading 1 -- and, because the
# paragraph grouper also breaks on a >1.5pt type-size jump, shreds sentences
# into one-line paragraphs.  Snapping this band onto the document body size
# fixes both symptoms at once; genuinely larger type (headings, ~1.25x and up)
# is left untouched.
#
# This is only the *fallback*: whenever the page still has a usable text layer
# (an undecodable-CID page does -- see _pdf_line_sizes) the exact font size is
# read from it and this estimate is not used at all.
_BODY_BAND = (0.85, 1.18)

_ENGINE = {"obj": None, "tried": False}

# Recognition model shipped with the project: PP-OCRv5 mobile rec reads the
# same text as the bundled PP-OCRv3 rec with better-calibrated confidence and
# ~40% less time per page (measured on the CJK corpus).  Detection stays on
# the bundled model -- server det/rec variants were evaluated and rejected:
# identical text output at 10-25x the cost, and server det is unusable on CPU.
_REC_MODEL = None


def _rec_model_path():
    global _REC_MODEL
    if _REC_MODEL is None:
        import os
        import sys
        name = "ch_PP-OCRv5_rec_mobile.onnx"
        # frozen (PyInstaller) bundles the model under _MEIPASS/pdfconv/models;
        # from source it sits next to this file.
        meipass = getattr(sys, "_MEIPASS", None)
        cands = []
        if meipass:
            cands.append(os.path.join(meipass, "pdfconv", "models", name))
        cands.append(os.path.join(os.path.dirname(__file__), "models", name))
        p = next((c for c in cands if os.path.exists(c)), "")
        _REC_MODEL = p
    return _REC_MODEL or None


# --------------------------------------------------------------------------
# engine
# --------------------------------------------------------------------------
def available():
    """True when an OCR engine can be imported."""
    try:
        import rapidocr_onnxruntime  # noqa: F401
        return True
    except Exception:
        return False


def _engine():
    if _ENGINE["tried"]:
        return _ENGINE["obj"]
    _ENGINE["tried"] = True
    try:
        from rapidocr_onnxruntime import RapidOCR
        kw = {}
        rp = _rec_model_path()
        if rp:
            kw["rec_model_path"] = rp
        _ENGINE["obj"] = RapidOCR(**kw)
    except Exception:
        _ENGINE["obj"] = None
    return _ENGINE["obj"]


# --------------------------------------------------------------------------
# deciding what needs OCR
# --------------------------------------------------------------------------
class FontStatus:
    """Per-document cache: is text drawn with this font recoverable at all?

    Only CID fonts are genuinely unrecoverable.  A simple font without
    /ToUnicode still decodes, because MuPDF falls back to its built-in
    encoding -- treating that as undecodable would send half the corpus to OCR
    for no reason.
    """

    def __init__(self, doc):
        self.doc = doc
        self._cache = {}

    def _ok(self, xref, subtype, encoding):
        if subtype != "Type0":
            return True
        if not encoding.startswith("Identity"):
            return True
        try:
            kind, val = self.doc.xref_get_key(xref, "ToUnicode")
        except Exception:
            return False
        return kind not in ("null", "none") and bool(val)

    def page_map(self, pno):
        m = {}
        for xref, _ext, sub, base, _res, enc in self.doc.get_page_fonts(pno):
            ok = self._ok(xref, sub, enc)
            m[base] = ok
            m[base.split("+")[-1]] = ok
        return m

    def dominant(self, pno):
        """(font name, most common size) of the page's real text."""
        names, sizes = {}, {}
        try:
            trace = self.doc[pno].get_texttrace()
        except Exception:
            return None, None
        for span in trace:
            n = len(span.get("chars", ()))
            f = span.get("font", "")
            names[f] = names.get(f, 0) + n
            s = round(float(span.get("size", 0)), 1)
            if s > 0:
                sizes[s] = sizes.get(s, 0) + n
        font = max(names, key=names.get) if names else None
        size = max(sizes, key=sizes.get) if sizes else None
        return font, size


def undecodable_ratio(doc, pno, status=None):
    """Fraction of characters on the page drawn with an unrecoverable font."""
    status = status or FontStatus(doc)
    ok = status.page_map(pno)
    total = bad = 0
    try:
        trace = doc[pno].get_texttrace()
    except Exception:
        return 0.0
    for span in trace:
        n = len(span.get("chars", ()))
        total += n
        f = span.get("font", "")
        if not ok.get(f, ok.get(f.split("+")[-1], True)):
            bad += n
    return (bad / total) if total else 0.0


def needs_ocr(doc, pno, words, status=None, threshold=0.5):
    """True when the page's own text is absent or not trustworthy."""
    if not words:
        return True
    return undecodable_ratio(doc, pno, status) >= threshold


# --------------------------------------------------------------------------
# geometry: exact type size from the page's own text layer
# --------------------------------------------------------------------------
def _pdf_line_sizes(page):
    """[(x0, y0, x1, y1, size)] for the page's own text lines.

    A page can need OCR and still have a perfectly good text layer.  That is
    exactly the undecodable-CID case: the font is Identity-encoded with no
    /ToUnicode, so the *characters* are meaningless garbage -- but the glyph
    positions and the font size are stored verbatim and are exact.

    So the text layer is the right source for type size, and OCR box height is
    only a fallback for pages that have no text layer at all (real scans).  The
    distinction matters: box height varies with ascenders, digits and
    full-width punctuation, so on a page containing a table of small labels it
    under-estimates the reference and inflates every body line by ~50%, which
    the structure pass then reads as Heading 1.
    """
    out = []
    try:
        blocks = page.get_text("dict")["blocks"]
    except Exception:
        return out
    for b in blocks:
        if b.get("type", 0) != 0:
            continue
        for ln in b.get("lines") or ():
            spans = ln.get("spans") or ()
            if not spans:
                continue
            # the span carrying the most characters sets the line's size
            s = max(spans, key=lambda sp: len(sp.get("text") or ""))
            try:
                size = float(s.get("size") or 0.0)
            except (TypeError, ValueError):
                size = 0.0
            if size <= 0:
                continue
            bb = ln.get("bbox") or b.get("bbox")
            if not bb:
                continue
            out.append((float(bb[0]), float(bb[1]), float(bb[2]), float(bb[3]),
                        size))
    return out


def _geom_size(box, lines, min_cover=0.30):
    """Type size of the text-layer line that *box* sits on, or None.

    Matching is by overlap area, which is robust to the two sides disagreeing
    about a line's extent: MuPDF reports a line box tall enough to hold
    ascender and descender, while the OCR engine reports the ink box, and
    either side may split one visual line into several pieces.
    """
    if not lines:
        return None
    x0, y0, x1, y1 = box
    area = max((x1 - x0) * (y1 - y0), 1e-6)
    best, best_a = None, 0.0
    for lx0, ly0, lx1, ly1, size in lines:
        ox = min(x1, lx1) - max(x0, lx0)
        oy = min(y1, ly1) - max(y0, ly0)
        if ox <= 0 or oy <= 0:
            continue
        a = ox * oy
        if a > best_a:
            best, best_a = size, a
    if best is None or best_a < min_cover * area:
        return None
    return best


def _ref_height(res):
    """Reference line height: the height carrying the most *characters*.

    The median is the obvious choice and the wrong one -- a single figure or
    table packed with small labels outnumbers the body lines on a text-light
    page and drags the median down, so every body line then looks oversized.
    Weighting by character count instead picks the height that actually holds
    the page's reading text.
    """
    bucket = {}
    for _x0, y0, _x1, y1, text, _c in res:
        h = round((y1 - y0) * 2.0) / 2.0      # half-pixel buckets
        bucket[h] = bucket.get(h, 0) + len(text or "")
    if not bucket:
        return 1.0
    return float(max(bucket, key=bucket.get)) or 1.0


def _row_blocks(res):
    """Block id per OCR box, shared by all boxes on one visual row.

    `build_lines` clusters tokens into lines *within a block*, and a block is
    what the PDF engine provides for free -- one block per text object.  OCR
    provides no such grouping: the obvious per-box id makes every box its own
    block, so a printed line the engine happens to return as two boxes
    ("…（自主智能范式）" + "系统组件") can never be rejoined and the two halves
    become two paragraphs, the second of which is short and centred and so gets
    classified as a heading.

    Rows are rebuilt here instead: same vertical centre, and no gap wider than
    a few ems.  The gap test keeps a column gutter (and a diagram's scattered
    labels) from being glued into one line.
    """
    n = len(res)
    blk = [0] * n
    order = sorted(range(n),
                   key=lambda i: ((res[i][1] + res[i][3]) / 2.0, res[i][0]))
    cur = -1
    row_cy = None
    row_h = 0.0
    row_x1 = None
    for i in order:
        x0, y0, x1, y1 = res[i][:4]
        cy = (y0 + y1) / 2.0
        h = max(y1 - y0, 1.0)
        ref = max(row_h, h)
        same = (row_cy is not None
                and abs(cy - row_cy) <= 0.5 * ref
                and (row_x1 is None or x0 - row_x1 <= 3.0 * ref))
        if not same:
            cur += 1
            row_cy, row_h, row_x1 = cy, h, x1
        else:
            row_h = ref
            row_x1 = max(row_x1, x1)
        blk[i] = 10000 + cur
    return blk


# --------------------------------------------------------------------------
# running OCR
# --------------------------------------------------------------------------
def _angle(box):
    b = box
    return math.degrees(math.atan2(b[1][1] - b[0][1], b[1][0] - b[0][0]))


def _ink_color(img, box):
    """Mean colour of the dark pixels inside a box (the glyph ink)."""
    import numpy as np
    b = np.array(box, dtype=float)
    x0, y0 = int(max(0, b[:, 0].min())), int(max(0, b[:, 1].min()))
    x1, y1 = int(b[:, 0].max()) + 1, int(b[:, 1].max()) + 1
    crop = img[y0:y1, x0:x1]
    if crop.size == 0:
        return 0
    grey = crop.mean(axis=2)
    mask = grey < 128
    if not mask.any():
        return 0
    r, g, bl = (int(crop[:, :, i][mask].mean()) for i in range(3))
    return (r << 16) | (g << 8) | bl


def page_words(page, body, font_hint=None, dpi=_DPI):
    """OCR one page into the same token dicts the rest of the pipeline uses.

    Returns [] when the engine is unavailable or finds nothing, which makes the
    caller fall back to the PDF's own (possibly garbage) text rather than to an
    empty page.

    Accuracy first: a page whose mean box confidence comes back low is retried
    once at a higher DPI, and the attempt that reads *more* text wins (recall
    is what matters -- the watermark filter and downstream structure code work
    on whatever boxes survive).
    """
    import numpy as np

    src = ""
    try:
        src = page.parent.name or ""
    except Exception:
        src = ""
    pno = page.number
    if src and os.path.exists(src):
        hit = cache.get(src, "ocr", pno)
        if hit:
            return hit
    eng = _engine()
    if eng is None:
        return []
    res, img = _run(page, eng, dpi)
    if not res:
        return []
    confs = [c for *_x, c in res]
    eff = dpi
    if confs and sum(confs) / len(confs) < 0.72 and dpi < 400:
        alt, alt_img = _run(page, eng, dpi + 100)
        if alt and sum(len(t) for *_x, t, _c in alt) > sum(
                len(t) for *_x, t, _c in res):
            res, img, eff = alt, alt_img, dpi + 100

    # scale pixels -> points, and derive each box's type size.  Preference
    # order matters: the page's own text layer is exact (it survives even when
    # the characters are garbage), box height is a noisy last resort.
    k = 72.0 / eff
    ref = _ref_height(res)
    geom = _pdf_line_sizes(page)
    blocks = _row_blocks(res)
    if font_hint:
        font_hint = re.sub(r"^[A-Z]{6}\+", "", font_hint)

    out = []
    for i, (x0, y0, x1, y1, text, conf) in enumerate(res):
        pt = (x0 * k, y0 * k, x1 * k, y1 * k)
        size = _geom_size(pt, geom)
        if size is None:
            # no text layer to read it from: normalise the box height so the
            # reference line equals the document's body size, and collapse
            # measurement noise onto the body size (see _BODY_BAND)
            ratio = ((y1 - y0) / ref) if ref else 1.0
            size = (body if _BODY_BAND[0] <= ratio <= _BODY_BAND[1]
                    else body * ratio)
        out.append(dict(word=text, x0=pt[0], y0=pt[1], x1=pt[2], y1=pt[3],
                        blk=blocks[i], font=font_hint, size=round(size, 1),
                        color=_ink_color(img, [(x0, y0), (x1, y0), (x1, y1),
                                               (x0, y1)]),
                        bold=False, italic=False, link=None, sp=True))
    if src and out:
        cache.put(src, "ocr", pno, out)
    return out


def _run(page, eng, dpi):
    """Render + OCR one page -> ([(x0, y0, x1, y1, text, conf), ...], img)
    with the rotated watermark boxes already dropped; the rendered image
    comes back too so ink colours can be sampled without a re-render."""
    import numpy as np
    pix = page.get_pixmap(dpi=dpi)
    img = np.frombuffer(pix.samples, dtype=np.uint8)
    img = img.reshape(pix.height, pix.width, pix.n)
    if pix.n >= 3:
        img = img[:, :, :3]
    else:
        img = np.repeat(img[:, :, :1], 3, axis=2)
    try:
        res, _elapse = eng(img)
    except Exception:
        return None
    if not res:
        return None
    # drop rotated boxes: the watermark runs diagonally across every page,
    # while real text is horizontal.  Ink darkness cannot separate them --
    # part of the watermark is darker than a light heading.
    kept = []
    for box, text, score in res:
        text = (text or "").strip()
        if not text:
            continue
        if abs(_angle(box)) > _DETECT_DEG:
            continue
        xs = [p[0] for p in box]
        ys = [p[1] for p in box]
        try:
            score = float(score)
        except Exception:
            score = 0.0
        kept.append((min(xs), min(ys), max(xs), max(ys), text, score))
    return (kept or None), img
