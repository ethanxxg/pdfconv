"""Shared text-metrics and line-breaking kernel.

The three reverse renderers (DOCX->PDF, XLSX->PDF, PPT->PDF) all need the
same three primitives: a font key for a styled run, the width of a string in
points, and a greedy filler that knows where a line may break.  Each used to
carry its own copy, and the copies disagreed at the edges -- whether a space
could open a line, how much slack a word that is one glyph too wide gets,
whether a CJK closing bracket may start a line.

This module is the single implementation.  Everything else imports it, so the
three outputs cannot drift apart again.
"""
import functools
import re

import fitz

# DOCX font name -> base-14 family root
FAMILY = (
    ("times", "tiro"), ("serif", "tiro"), ("cambria", "tiro"),
    ("georgia", "tiro"), ("garamond", "tiro"), ("book", "tiro"),
    ("courier", "cour"), ("consol", "cour"), ("mono", "cour"),
)
VARIANTS = {
    ("helv", False, False): "helv", ("helv", True, False): "hebo",
    ("helv", False, True): "heit", ("helv", True, True): "hebi",
    ("tiro", False, False): "tiro", ("tiro", True, False): "tibo",
    ("tiro", False, True): "tiit", ("tiro", True, True): "tibi",
    ("cour", False, False): "cour", ("cour", True, False): "cobo",
    ("cour", False, True): "coit", ("cour", True, True): "cobi",
}

_CJK_RANGES = (
    (0x2E80, 0x9FFF), (0xF900, 0xFAFF), (0xFF00, 0xFFEF), (0x3000, 0x303F),
)
# a CJK glyph, a run of non-space Latin, or a space run
TOK = re.compile(r"[\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef\u3000-\u303f]"
                 r"|[^\s\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef"
                 r"\u3000-\u303f]+|\s+")

# kinsoku: punctuation that may not open a line / may not close one
NO_START = set("，。、；：？！）〕］｝〉》」』】”’%‰℃…·,.;:?!)]}>")
NO_END = set("（〔［｛〈《「『【“‘([{<")

_FONTS = {}


def font(key):
    f = _FONTS.get(key)
    if f is None:
        f = fitz.Font(key)
        _FONTS[key] = f
    return f


def is_cjk(ch):
    o = ord(ch)
    for lo, hi in _CJK_RANGES:
        if lo <= o <= hi:
            return True
    return False


def has_cjk(text):
    return any(is_cjk(c) for c in text)


def font_key(name, bold, italic, serif=False):
    """Base-14 key for a styled run.

    ``serif`` is the PDF span flag hint (bit 4): when the embedded font name
    is one we do not recognise -- Computer Modern, STIX, a subset tag -- the
    name alone maps to Helvetica, and a serif paper would come back sans.
    The flag lets the caller override that.
    """
    n = (name or "").lower()
    fam = "helv"
    for token, root in FAMILY:
        if token in n:
            fam = root
            break
    else:
        if serif:
            fam = "tiro"
    return VARIANTS[(fam, bool(bold), bool(italic))]


@functools.lru_cache(maxsize=131072)
def _width_cached(text, key, size):
    f = font(key)
    if not has_cjk(text):
        try:
            return f.text_length(text, size)
        except Exception:
            pass
    w = 0.0
    for ch in text:
        if is_cjk(ch):
            w += size
        else:
            try:
                w += f.text_length(ch, size)
            except Exception:
                w += 0.5 * size
    return w


def text_width(text, key, size):
    """Width of *text* in points.  Latin strings are measured in one call;
    CJK falls back to per-character metrics (han glyphs are one em wide)."""
    return _width_cached(text, key, round(size, 2))


def tokenize(text):
    """Split *text* into break-eligible tokens (words, CJK glyphs, spaces)."""
    return TOK.findall(text)


def _is_space(tok):
    return tok.isspace() if isinstance(tok, str) else tok[0].isspace()


def _w(tok, width_of):
    return width_of(tok[0], tok[1], tok[2])


def _split_wide(tokens, width, width_of, slack=0.0):
    """Hard-split any token wider than the line box on glyph boundaries.

    *slack* lets a word that overshoots by a few percent stay in one piece --
    chopping "Datasets" into "Dataset/s" is worse than a 2% overhang."""
    limit = width * (1.0 + slack)
    out = []
    for tok in tokens:
        t = tok[0]
        if len(t) < 2 or _w(tok, width_of) <= limit:
            out.append(tok)
            continue
        piece = ""
        for ch in t:
            cand = piece + ch
            if piece and width_of(cand, tok[1], tok[2]) > width:
                out.append((piece,) + tuple(tok[1:]))
                piece = ch
            else:
                piece = cand
        if piece:
            out.append((piece,) + tuple(tok[1:]))
    return out


def wrap(tokens, width, indent=0.0, width_of=text_width, kinsoku=True,
         slack=0.0):
    """Greedy line fill over styled tokens.

    ``tokens`` are ``(text, key, size, *payload)`` tuples.  Returns a list of
    lines; each line is a list of ``(token, x_offset, token_width)`` where the
    offset is measured from the line start plus *indent* on the first line.
    """
    if width <= 1.0 or not tokens:
        return [[(t, 0.0, width_of(t[0], t[1], t[2])) for t in tokens]] \
            if tokens else []
    toks = _split_wide(tokens, width, width_of, slack)
    n = len(toks)
    starts = [0]
    x = indent
    i = 0
    while i < n:
        w = _w(toks[i], width_of)
        if x + w > width + 0.05 and i > starts[-1]:
            b = i
            if kinsoku:
                guard = 0
                while b > starts[-1] + 1 and guard < 4:
                    head = toks[b][0][:1]
                    tail = toks[b - 1][0][-1:]
                    if head in NO_START or tail in NO_END:
                        b -= 1
                        guard += 1
                    else:
                        break
            starts.append(b)
            x = 0.0
            i = b
            continue
        x += w
        i += 1
    lines = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else n
        seg = toks[s:e]
        while seg and _is_space(seg[0]):
            seg = seg[1:]
        while seg and _is_space(seg[-1]):
            seg = seg[:-1]
        if not seg:
            continue
        x = indent if k == 0 else 0.0
        out = []
        for tok in seg:
            w = _w(tok, width_of)
            out.append((tok, x, w))
            x += w
        lines.append(out)
    return lines


def wrap_text(text, key, size, width, slack=0.0):
    """Plain-text wrapper: returns a list of strings (no styling payload)."""
    out = []
    for para in str(text).split("\n"):
        toks = [(t, key, size) for t in tokenize(para)]
        lines = wrap(toks, width, slack=slack)
        if not lines:
            out.append("")
            continue
        for ln in lines:
            out.append("".join(t[0] for t, _x, _w in ln))
    return out


def justify_extra(line, width, line_size, cap=0.33):
    """Extra points per gap for a justified line, or 0 when stretching would
    create a river of white space."""
    gaps = sum(1 for t, _x, _w in line if t[0].isspace())
    if not gaps:
        return 0.0
    used = line[-1][1] + line[-1][2] if line else 0.0
    extra = max(0.0, (width - used) / gaps)
    return extra if extra <= cap * line_size else 0.0
