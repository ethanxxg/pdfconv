"""Math reconstruction: geometry in, native OMML equations out.

Aspose renders PDF math as plain text (or images) -- this module goes one
level further and rebuilds displayed equations as Word *equation objects*
(``m:oMath``), which the user can actually edit in Word's equation editor.

Everything here is our own logic on top of data we already extract: per-token
boxes, baselines and font names (MuPDF's role ends at glyph extraction), plus
the thin horizontal rules the page's vector drawings contribute.  Scripts
(sub/superscripts) come from the baseline offsets our line model already
classifies; big operators (sums, integrals) take following script chains as
their limits; a rule sandwiched between two stacked rows becomes a fraction.

Blocks are admitted only when the *whole block* scores as math -- dense
operator symbols, real math fonts (CMMI/CMSY/MTSY...), a high script ratio --
so prose that merely contains "x <= 1" is never touched; its scripts already
render correctly as ordinary superscript/subscript runs.
"""
import re

from lxml import etree

M = "http://schemas.openxmlformats.org/officeDocument/2006/math"
W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"

_MATH_FONT_KEYS = ("cmmi", "cmsy", "cmex", "msam", "msbm", "mtsyn", "rmtmi",
                   "mtmi", "mtsy", "symbol")
# operators that mark math content; letters picked to survive font oddities
_OPS = set("∑∫∏∐√≤≥≠≈≡±×÷∈∉⊂⊃⊆⊇∪∩→←↔⇒⇐⇔∀∃¬∂∞∇"
           "αβγδεζηθικλμνξπρστυφχψωΓΔΘΛΞΠΣΦΨΩ"
           "⊗⊕⊙∘∘≲≳∼∝")
_NARY = set("∑∫∏∐")
_RAD = set("√√")


def _is_delim_pair(segs):
    """A line whose first and last tokens are a balanced bracket pair becomes
    one m:d (delimiter) object -- the common (a+b)(c+d) / [x] shell."""
    texts = [s[0].strip() for s in segs
             if not (len(s) == 8 and s[0] == " " and s[3] is None
                     and s[4] is None)]
    if len(texts) < 2:
        return None
    l, r = texts[0], texts[-1]
    pairs = {"(": ")", "[": "]", "{": "}"}
    if pairs.get(l) != r:
        return None
    depth = 0
    for t in texts:
        for ch in t:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
                if depth == 0 and t is not texts[-1]:
                    return None         # closes mid-line: not one shell
    if depth != 0:
        return None
    return l, r
_SPACE_SEG = (" ", False, False, None, None, 0, 0, None)


def _is_math_font(f):
    return bool(f) and any(k in f.lower() for k in _MATH_FONT_KEYS)


def _escape(t):
    return (t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


def score_block(b):
    """Math-ness of a block: 0..~3, calibrated so prose stays far below 0.35."""
    runs = [s for s in b["segs"] if s[0].strip()]
    if len(runs) < 3:
        return 0.0
    chars = sum(len(s[0]) for s in runs)
    if not chars:
        return 0.0
    sym = sum(1 for s in runs for ch in s[0] if ch in _OPS) / max(chars, 1)
    mfont = sum(1 for s in runs if _is_math_font(s[4])) / len(runs)
    vert = sum(1 for s in runs if s[3]) / len(runs)
    ital = sum(1 for s in runs
               if len(s[0].strip()) == 1 and s[0].strip().isalpha() and s[1]
               ) / len(runs)
    return 2.0 * mfont + 1.6 * sym + 0.8 * vert + 0.4 * ital


def is_formula(b, body, max_len=700):
    """True when the block is a displayed equation worth converting.

    Deliberately strict: false positives would turn prose into centered
    equation objects, which is far worse than leaving an equation as styled
    text (the old behaviour, and Aspose's).  Two gates:

    * the math score (math fonts + operators + scripts) must clear 0.35;
    * long alphabetic prose words must stay a minority of the text -- a real
      equation is symbols and single letters, prose with inline math is mostly
      ordinary words.
    """
    if len(b.get("text", "")) > max_len:
        return False
    if b.get("kind") in ("h1", "h2"):
        return False
    if score_block(b) < 0.35:
        return False
    runs = [s for s in b["segs"] if s[0].strip()]
    chars = sum(len(s[0]) for s in runs)
    if not chars:
        return False
    prose = sum(len(s[0]) for s in runs
                if not _is_math_font(s[4])
                and re.fullmatch(r"[A-Za-z]{4,}[.,;:!?]?", s[0].strip())
                and s[0].strip() not in ("over", "begin", "end"))
    return prose <= 0.30 * chars


# --------------------------------------------------------------------------
# Displayed-equation *regions* -- geometry only, no reconstruction
# --------------------------------------------------------------------------
# A displayed equation is a two-dimensional object: a summation's limits sit
# above and below its operator, a fraction's numerator and denominator sit on
# either side of a rule, and a multi-line equation steps through several
# baselines.  Linearising it into text rows therefore destroys it -- the sum
# limits become the stray words "2 N K", "N K", "N N" and the body of the
# formula turns into a column of fragments in no meaningful order.
#
# Rebuilding such a thing as OMML is only possible when the pieces happen to
# share a baseline, which is exactly the case build_omath() already covers.
# For everything taller than one row the honest answer is to keep the page's
# own rendering: crop the region and place it as a picture, which reproduces
# the equation exactly as the author set it.
_PROSE_WORD = re.compile(r"[A-Za-z]{3,}")
# words that are math operators rather than prose; a line built only from
# these still belongs to a formula
_MATH_WORDS = frozenset(("max", "min", "arg", "sin", "cos", "tan", "log",
                         "exp", "lim", "sup", "inf", "det", "diag", "rank",
                         "sqrt", "frac", "mod", "gcd", "sgn", "dim", "ker",
                         "vec", "cov", "var", "let", "s.t", "st", "argmax",
                         "argmin"))
_CAPTIONISH = re.compile(
    r"^\s*(?:fig(?:ure)?|table|algorithm|图|表)\s*\.?\s*[0-9ivxlc]", re.I)


def _line_text(ln):
    return "".join(s[0] for s in ln["segs"])


def is_math_line(ln):
    """True when a visual line reads as mathematics rather than prose.

    The discriminator is the *absence of words*: a formula row is symbols,
    single letters and digits, while even a short prose row carries at least
    one word of four or more letters.  A math indicator is then required as
    well, so a bare page number or a stray digit never qualifies.
    """
    segs = [s for s in ln["segs"] if s[0].strip()]
    if not segs:
        return False
    txt = _line_text(ln)
    if _CAPTIONISH.match(txt):
        return False
    if is_prose_line(ln):
        return False
    return (any(_is_math_font(s[4]) for s in segs)
            or any(s[3] for s in segs)                    # sub/superscript
            or any(ch in _OPS for ch in txt)              # operator
            or any(len(s[0].strip()) == 1 and s[0].strip().isalpha() and s[1]
                   for s in segs))                        # italic variable


def is_prose_line(ln):
    """True when the line carries a real word -- i.e. it is running text.

    Prose is what terminates an equation: a display formula's rows are never
    interrupted by running text, so a word between two symbol rows means the
    rows belong to two different things (typically inline math inside a
    sentence).  Digits and single letters are *not* prose -- they are the
    numerator and denominator of a fraction, and must not split the run.

    The test runs per *segment*, never over the concatenation: the row "M N K"
    is three variables set at one baseline, and gluing them together would
    invent the word "MNK" and misread a summation's limits as prose.
    """
    if _CAPTIONISH.match(_line_text(ln)):
        return True
    for s in ln["segs"]:
        for w in s[0].split():
            w = w.strip(".,;:!?()[]{}\"'`")
            if re.fullmatch(r"[A-Za-z]{3,}", w) and w.lower() not in _MATH_WORDS:
                return True
    return False


def _rows(lines, tol=2.0):
    """Merge line fragments that share a baseline into whole visual rows.

    ``build_lines`` splits a row wherever the glyph stream leaves a wide gap,
    so the tail of a sentence ("and x^p_i") can arrive as its own object even
    though it sits on the same baseline as the prose before it.  Judging those
    fragments in isolation makes a subscript look like a formula row; judging
    the reassembled row does not.
    """
    rows = []
    for ln in sorted(lines, key=lambda l: (l["y0"], l["x0"])):
        if rows and abs(ln["y0"] - rows[-1]["y0"]) <= tol:
            r = rows[-1]
            r["parts"].append(ln)
            r["y1"] = max(r["y1"], ln["y1"])
            r["x0"] = min(r["x0"], ln["x0"])
            r["x1"] = max(r["x1"], ln["x1"])
        else:
            rows.append(dict(y0=ln["y0"], y1=ln["y1"], x0=ln["x0"],
                             x1=ln["x1"], parts=[ln]))
    for r in rows:
        r["segs"] = [s for p in sorted(r["parts"], key=lambda l: l["x0"])
                     for s in p["segs"]]
    return rows


def math_regions(lines, lay, min_lines=2):
    """Group stacked math rows into regions that must be kept as pictures.

    Rows are clustered *per column* (a left-column formula must never merge
    with a right-column one that happens to share a baseline), only when they
    follow each other closely enough to belong to one equation, and only until
    running text interrupts them.  A single-row region is left alone:
    build_omath() handles those correctly and keeps them editable.
    """
    if not lines:
        return []
    lead = float(lay.get("leading") or 12.0)
    gap_max = max(1.5 * lead, 10.0)
    try:
        gut_l, gut_r = lay["gutter"]
    except Exception:
        gut_l = gut_r = 0.0

    def colof(ln):
        cx = (ln["x0"] + ln["x1"]) / 2.0
        if gut_l and gut_r:
            if cx <= gut_l:
                return "L"
            if cx >= gut_r:
                return "R"
            return "F"
        return "F"

    buckets = {}
    for ln in lines:
        buckets.setdefault(colof(ln), []).append(ln)

    regions = []
    for col, items in buckets.items():
        # reassemble rows *inside* a column: grouping across the gutter would
        # staple the left column's equation number to the right column's
        # heading, since both sit on the same baseline
        items = _rows(items)
        items.sort(key=lambda l: l["y0"])
        run = []
        for ln in items:
            if is_prose_line(ln):
                if len(run) >= min_lines:
                    regions.append((col, run))
                run = []
                continue
            if run and ln["y0"] - run[-1]["y1"] > gap_max:
                if len(run) >= min_lines:
                    regions.append((col, run))
                run = []
            run.append(ln)
        if len(run) >= min_lines:
            regions.append((col, run))

    out = []
    for col, grp in regions:
        scored = [l for l in grp if is_math_line(l)]
        if len(scored) < min_lines:
            continue
        # The box is measured over the rows that actually scored as math, not
        # over every row the run swallowed.  A page folio ("1152") is neither
        # prose nor math, so it does not break the run -- but it sits above the
        # equation in the top margin, and letting it set y0 would pull the
        # page number into the picture and delete it from the text.
        x0 = min(l["x0"] for l in scored)
        x1 = max(l["x1"] for l in scored)
        y0 = min(l["y0"] for l in scored)
        y1 = max(l["y1"] for l in scored)
        # Width is what separates a *displayed* equation from a wrapped piece
        # of inline math.  An equation set on its own line occupies most of the
        # measure; the tail of "D^p = {(x^p_i, y^p_i)|i = 1..N}" that spilled
        # onto a second line is only ~50pt wide, and turning that into a
        # picture would slice one sentence into text + image + text.
        try:
            colw = float(lay["right"].get(col, 0)) - float(lay["margin"].get(col, 0))
        except Exception:
            colw = 0.0
        min_w = max(60.0, 0.42 * colw) if colw > 0 else 60.0
        if x1 - x0 < min_w or y1 - y0 < 4:
            continue
        out.append(dict(col=col,
                        rows=grp,
                        lines=[p for r in grp for p in r["parts"]],
                        bbox=(x0 - 1.5, y0 - 1.5, x1 + 1.5, y1 + 1.5)))
    return out


# --------------------------------------------------------------------------
# OMML construction
# --------------------------------------------------------------------------
def _mr(text, italic=None):
    """A plain math run.  Word italicises single letters automatically; a
    multi-letter run (function name, word) is set upright like real math."""
    pr = ""
    if italic is False:
        pr = ('<m:rPr><m:sty m:val="p"/></m:rPr>')
    return "<m:r>%s<m:t xml:space=\"preserve\">%s</m:t></m:r>" % (
        pr, _escape(text))


def _wrap(tag, inner):
    return "<m:%s>%s</m:%s>" % (tag, inner, tag)


_SUP_RE = re.compile(r"<m:sSup><m:e>(.*)</m:e><m:sup>(.*)</m:sup></m:sSup>$",
                     re.S)
_SUB_RE = re.compile(r"<m:sSub><m:e>(.*)</m:e><m:sub>(.*)</m:sub></m:sSub>$",
                     re.S)


def _attach_script(nodes, vert, node):
    """Attach a script run to the previous node, upgrading sSub/sSup pairs
    into a single sSubSup (x_1^2) instead of nesting."""
    if not nodes:
        nodes.append(node)
        return
    prev = nodes[-1]
    m_sup, m_sub = _SUP_RE.match(prev), _SUB_RE.match(prev)
    if m_sup and vert == "sub":
        nodes[-1] = _wrap("sSubSup", "<m:e>%s</m:e><m:sub>%s</m:sub>"
                          "<m:sup>%s</m:sup>" % (m_sup.group(1), node,
                                                 m_sup.group(2)))
    elif m_sub and vert == "sup":
        nodes[-1] = _wrap("sSubSup", "<m:e>%s</m:e><m:sub>%s</m:sub>"
                          "<m:sup>%s</m:sup>" % (m_sub.group(1), m_sub.group(2),
                                                 node))
    elif vert == "sup":
        nodes.append(_wrap("sSup", "<m:e>%s</m:e><m:sup>%s</m:sup>"
                           % (prev, node)))
        nodes[-2:-1] = []
    else:
        nodes.append(_wrap("sSub", "<m:e>%s</m:e><m:sub>%s</m:sub>"
                           % (prev, node)))
        nodes[-2:-1] = []


def _line_nodes(segs):
    """One visual line -> list of OMML node strings.

    Big operators consume the script runs that follow them as their limits
    (m:nary), so ``\\sum_{i=1}^n`` becomes a real n-ary object rather than a
    weirdly-placed subscript.  Inter-word space segments are kept as space
    runs -- dropping them glued multi-word content together."""
    nodes = []
    i = 0
    while i < len(segs):
        s = segs[i]
        if len(s) == 8 and s[0] == " " and s[3] is None and s[4] is None:
            if nodes:
                nodes.append("<m:r><m:t xml:space=\"preserve\"> </m:t></m:r>")
            i += 1
            continue
        text, _it, _bo, vert, font, size, _col, _lnk = s[:8]
        if not text:
            i += 1
            continue
        if text.strip() in _NARY and vert is None:
            i += 1
            sub_parts, sup_parts = [], []
            cur = sub_parts
            while i < len(segs):
                t = segs[i]
                if len(t) == 8 and t[0] == " " and t[3] is None:
                    i += 1
                    continue
                if t[3] == "sub" and cur is sub_parts:
                    sub_parts.append(t)
                elif t[3] == "sup" and not sup_parts:
                    cur = sup_parts
                    sup_parts.append(t)
                else:
                    break
                i += 1
            chr_ = text.strip()
            inner = ('<m:naryPr><m:chr m:val="%s"/><m:limLoc m:val="subSup"/>'
                     '<m:subHide m:val="%d"/><m:supHide m:val="%d"/></m:naryPr>'
                     % (_escape(chr_), 1 if not sub_parts else 0,
                        1 if not sup_parts else 0))
            inner += "<m:sub>%s</m:sub>" % _line_xml(sub_parts)
            inner += "<m:sup>%s</m:sup>" % _line_xml(sup_parts)
            inner += "<m:e/>"
            nodes.append(_wrap("nary", inner))
            continue
        if text.strip() in _RAD and vert is None:
            i += 1
            rad = []
            while i < len(segs):
                t = segs[i]
                if len(t) == 8 and t[0] == " " and t[3] is None:
                    break                   # radicands are tight runs
                if t[3] is not None:
                    rad.append(t)           # script of the radicand
                else:
                    txt = t[0].strip()
                    if txt and any(ch in "+-=<>,;)" for ch in txt):
                        break               # back at base-level operator
                    rad.append(t)
                i += 1
            nodes.append(_wrap(
                "rad", '<m:radPr><m:degHide m:val="1"/></m:radPr><m:deg/>'
                       "<m:e>%s</m:e>" % _line_xml(rad)))
            continue
        node = _mr(text, italic=None if len(text.strip()) == 1 else False)
        if vert is None:
            nodes.append(node)
        else:
            _attach_script(nodes, vert, node)
        i += 1
    # a line that is one balanced bracket shell becomes a delimiter object
    pair = _is_delim_pair(segs)
    if pair and len(nodes) >= 2:
        nodes = [_wrap("d", '<m:dPr><m:begChr m:val="%s"/><m:endChr m:val="%s"/>'
                       '</m:dPr><m:e>%s</m:e>'
                       % (_escape(pair[0]), _escape(pair[1]),
                          "".join(nodes[1:-1])))]
    return nodes


def _line_xml(segs):
    return "".join(_line_nodes(segs))


def _parse_frag(xml):
    return etree.fromstring(
        '<w:p xmlns:m="%s" xmlns:w="%s">%s</w:p>' % (M, W, xml))[0]


def build_omath(b, rules=()):
    """Return a display-mode ``m:oMathPara`` element for a formula block
    (one ``m:oMath`` per visual line), or None when nothing usable could be
    built (caller falls back to styled runs)."""
    lines = b.get("lines") or []
    if not lines:
        return None
    lines = sorted(lines, key=lambda l: l["y0"])
    frags = []

    # fraction: a thin horizontal rule with a row directly above and below
    consumed = set()
    for i, ln in enumerate(lines):
        rule = None
        for r in rules:
            if (r.y0 >= ln["y1"] - 1 and r.y1 <= ln["y1"] + 0.45 * b["size"]
                    and min(r.x1, ln["x1"]) - max(r.x0, ln["x0"])
                    > 0.3 * (r.x1 - r.x0)):
                rule = r
                break
        if rule is None or i + 1 >= len(lines):
            continue
        below = lines[i + 1]
        if below["y0"] - rule.y1 > 0.5 * b["size"]:
            continue
        frags.append(_wrap("f", "<m:num>%s</m:num><m:den>%s</m:den>" % (
            _line_xml(ln["segs"]), _line_xml(below["segs"]))))
        consumed.add(id(ln))
        consumed.add(id(below))
    for ln in lines:
        if id(ln) in consumed:
            continue
        frags.append(_line_xml(ln["segs"]))
    frags = [f for f in frags if f]
    if not frags:
        return None
    return _parse_frag("<m:oMathPara>%s</m:oMathPara>"
                       % "".join("<m:oMath>%s</m:oMath>" % f for f in frags))
