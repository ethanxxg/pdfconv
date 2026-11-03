"""Layout-independent structure reconstruction.

Turns visual lines into paragraphs, assigns columns, classifies block roles
(heading / body / caption / reference / bullet) and computes reading order.
All of this is our own logic -- no external converter involved.
"""
import re

from .config import SHORT_LINE_EM, SHORT_LINE_RATIO

# "I. INTRODUCTION" / "IV. RESULTS" -- roman-numeral section headings.
_ROMAN_HEAD = re.compile(r"^(i{1,3}|iv|v|vi{0,3}|ix|x)\.\s+[a-z]")

# numbered list item lead-in: "(1) ", "1) ", "1. " -- captures the literal number
_NUMITEM = re.compile(r"^(?:\((\d{1,2})\)|(\d{1,2})[.)])[ \t]+\S")
# "1.1 Concept" / "1.1.1 Sub-section" -- dotted section numbers (at least one
# dot, so a plain "1. Item" list is not caught).  These are usually
# left-aligned, so the centred test below misses the short ones.
_SECTION_HEAD = re.compile(r"^\d{1,2}(?:\.\d{1,2}){1,3}\.?\s+\S")
# Chinese figure / table captions: "图 1 ..." / "表 2 ...".  Length-guarded so a
# body sentence that merely starts with "图1" is not taken for a caption.
_CN_FCAP = re.compile(r"^图\s*\d+")
_CN_TCAP = re.compile(r"^表\s*\d+")
# "A. PRELIMINARIES" -- letter-prefixed subsection headings.  Strictly
# uppercase so ordinary prose such as "patterns. For example" is not caught.
_ALPHA_HEAD = re.compile(r"^[A-Z]\.\s+[A-Z0-9]")
# banner words that are their own heading regardless of numbering
_BANNERS = ("references", "acknowledgment", "acknowledgement",
            "appendix", "index terms", "abstract")


def is_furniture(text):
    """Detect journal running-heads / page furniture that should not become
    body text (IEEE/Elsevier banners, copyright lines, author running heads)."""
    t = text.strip()
    if not t:
        return False
    tl = t.lower()
    if "\u00a9" in t and "ieee" in tl:
        return True
    if re.match(r"^ieee transactions", tl):
        return True
    if re.match(r"^vol\w*\.?\s+\d+,\s*\d{4}$", tl):
        return True
    if "et al.:" in tl:
        return True
    if "translations and content mining are permitted" in tl:
        return True
    if "personal use is permitted" in tl:
        return True
    # The same licence notice, when a page footer lands in the middle of it
    # ("…Translations and content VOLUME 7, 2019 Personal use is als…").  Once
    # a row is reassembled across blocks the full sentence is no longer
    # contiguous, so the distinctive fragments are matched on their own.
    if "translations and content" in tl:
        return True
    if "mining are permitted" in tl:
        return True
    if "personal use is" in tl:
        return True
    if "republication/redistribution requires" in tl:
        return True
    if "authorized licensed use limited to" in tl:
        return True
    if "digital object identifier" in tl:
        return True
    if "ieee xplore" in tl and "downloaded on" in tl:
        return True
    # ISSN + "IEEE." licence strip ("2169-3536 2018 IEEE. …")
    if re.match(r"^\d{4}-\d{4}\s+(19|20)\d{2}\s+ieee", tl):
        return True
    # copyright line "2016 IEEE." -- must END the string, otherwise a reference
    # citation such as "(2018). ... IEEE/ACM Trans. ..." is wrongly discarded
    if len(t) < 120 and re.search(r"\b(19|20)\d{2}\s+ieee\.?$", tl):
        return True
    if re.match(r"^\d{1,4}$", t):
        return True
    return False


def _bsize(line):
    """Dominant type size of a visual line (ignores inline math / scripts)."""
    return float(line.get("bsize") or line.get("size") or 0.0)


def assign_column(line, gut_l, gut_r):
    if gut_r <= gut_l + 1:
        return "F"
    if line["x0"] < gut_l and line["x1"] > gut_r:
        return "F"
    return "L" if line["cx"] < gut_l else "R"


def col_bounds(col, L):
    """Left/right edges of the text column a block belongs to."""
    margin = L.get("margin") or {}
    right = L.get("right") or {}
    page_w = float(L["page"][0])
    le = float(margin.get(col, margin.get("F", 0.0)) or 0.0)
    re_ = float(right.get(col, right.get("F", page_w)) or page_w)
    if re_ <= le + 1:
        le, re_ = 0.0, page_w
    return le, re_


def heading_align(x0, x1, col, L):
    """Reproduce the heading alignment measured in the PDF.

    Aspose keeps whatever alignment the source uses; the old code forced every
    heading to centre, which is wrong for left-aligned styles (IEEE Access,
    most Elsevier journals).  A line is centred only when its left and right
    slack inside the column are equal."""
    le, re_ = col_bounds(col, L)
    width = re_ - le
    tol = max(6.0, width * 0.05)
    lg, rg = x0 - le, re_ - x1
    return "center" if abs(lg - rg) <= tol else "left"


def classify(text, size, body, cx, page_w):
    t = text.strip()
    if not t:
        return "p"
    tl = t.lower()
    centered = cx > page_w * 0.3 and cx < page_w * 0.7
    if re.match(r"^fig(ure)?\.?\s*\d+\.?", tl):
        return "fcaption"
    if re.match(r"^table\s+[ivxlcd]+\.?", tl):
        return "tcaption"
    if _CN_FCAP.match(t) and len(t) < 90:
        return "fcaption"
    if _CN_TCAP.match(t) and len(t) < 90:
        return "tcaption"
    if re.match(r"^algorithm\s+\d", tl):
        return "acaption"
    if re.match(r"^\[\d+\]", t):
        return "ref"
    if t[:1] in "\u2022\u00b7\u25cf-" and len(t) < 200:
        return "bullet"
    # numbered / lettered list item: "(1) ...", "1) ...", "1. ..." -- long
    # enough not to be a section heading ("1. Introduction" stays a heading);
    # the consecutive-group check in convert.py keeps this honest
    if _NUMITEM.match(t) and len(t) > 30:
        return "numitem"
    if (_SECTION_HEAD.match(t) and len(t) < 60
            and not t.endswith(("。", ".")) and size >= body + 0.6):
        return "h1"
    if _ROMAN_HEAD.match(tl) and size < body + 0.6:
        return "h1"
    if tl in _BANNERS:
        return "h1"
    if (_ALPHA_HEAD.match(t) and len(t) < 80 and not t.endswith(".")
            and size <= body + 0.6):
        return "h2"
    if centered and size >= body + 0.6:
        return "h1"
    return "p"


def _ends_short(line, col, L, base):
    """A line that stops well before its neighbours' right edge ends a
    paragraph.

    Blank-line spacing is not always available (reference lists and tight
    journal settings are single-spaced), so the classic typographic rule is
    used too.  The reference edge is taken from a local window rather than the
    page margin: abstracts and insets are often set on a narrower measure than
    the body, and comparing against the margin would split every one of their
    lines into its own paragraph.
    """
    le, re_ = col_bounds(col, L)
    width = re_ - le
    if width <= 1:
        return False
    ref = line.get("ref_x1") or re_
    return (ref - line["x1"]) > max(SHORT_LINE_EM * base, SHORT_LINE_RATIO * width)


def group_paragraphs(lines, L):
    gut_l, gut_r = L["gutter"]
    leading = float(L["leading"])
    body = float(L["body_size"])
    page_w = float(L["page"][0])
    bycol = {"F": [], "L": [], "R": []}
    fw_l, fw_r = col_bounds("F", L)
    full_w = max(fw_r - fw_l, 1.0)
    ordered = sorted(lines, key=lambda r: r["y0"])
    prev = None
    for ln in ordered:
        ln["col"] = assign_column(ln, gut_l, gut_r)
        # A full-width block's *last* line is short, so it no longer crosses
        # the gutter and would be filed under the left column -- splitting the
        # abstract / funding note into two paragraphs.  Inherit "full width"
        # from the preceding line when it clearly continues that block.
        if (prev is not None and prev["col"] == "F"
                and ln["col"] == "L"
                and prev["x1"] > gut_r
                and ln["y0"] - prev["y1"] <= leading * 1.5
                and abs(ln["x0"] - prev["x0"]) <= 2.5
                and (ln["x1"] - ln["x0"]) > 0.20 * full_w):
            ln["col"] = "F"
        bycol[ln["col"]].append(ln)
        prev = ln

    paras = []
    for col in ("F", "L", "R"):
        ls = sorted(bycol[col], key=lambda r: r["y0"])
        # local right edge: the furthest a *comparable* line (+/-3 rows, same
        # type size, same left edge) reaches.  Restricting to comparable lines
        # keeps a wider title or a full-width banner from making a body line
        # look short.
        for idx, ln in enumerate(ls):
            lo = max(0, idx - 3)
            hi = min(len(ls), idx + 4)
            nb = [x for x in ls[lo:hi]
                  if abs(_bsize(x) - _bsize(ln)) <= 0.6
                  and abs(x["x0"] - ln["x0"]) <= 3.0]
            ln["ref_x1"] = max((x["x1"] for x in nb), default=ln["x1"])
        i = 0
        while i < len(ls):
            cur = [ls[i]]
            first_kind = classify(ls[i]["text"], ls[i]["size"], body,
                                  ls[i]["cx"], page_w)
            j = i + 1
            while j < len(ls):
                gap = ls[j]["y0"] - cur[-1]["y1"]
                jk = classify(ls[j]["text"], ls[j]["size"], body,
                              ls[j]["cx"], page_w)
                # a caption is its own block: never absorb body text into it,
                # and never absorb a caption into a body paragraph.  Only a
                # wrapped caption line of the same kind may join it.
                if first_kind in ("fcaption", "tcaption"):
                    if jk == first_kind and gap <= leading * 1.5:
                        cur.append(ls[j])
                        j += 1
                        continue
                    break
                if jk in ("fcaption", "tcaption"):
                    break
                if first_kind in ("h1", "h2"):
                    # A heading may wrap onto further lines ("III. OVERVIEW OF
                    # THE DECLARATIVE" / "SERVICE-BASED METHOD").  Only absorb
                    # a line that keeps the heading's own type size and sits on
                    # the same baseline grid and left edge -- body text is
                    # either larger, indented, or ends in punctuation.
                    nxt = ls[j]["text"].rstrip()
                    if (jk in ("h1", "h2") or len(cur) >= 3
                            or gap > leading * 1.35
                            or abs(ls[j]["size"] - cur[0]["size"]) > 0.7
                            or ls[j]["col"] != cur[0]["col"]
                            or nxt[-1:] in ".,-;:"
                            or abs(ls[j]["x0"] - cur[0]["x0"]) > 3.0):
                        break
                    cur.append(ls[j])
                    j += 1
                    continue
                if gap > leading * 1.7 or jk in ("h1", "h2"):
                    break
                # a visible type-size change starts a new block: author lines
                # (9.9pt) must not absorb the affiliation block (6.6pt) below
                if abs(_bsize(ls[j]) - _bsize(cur[0])) > 1.5:
                    break
                # previous line stopped short -> paragraph boundary
                prev_txt = cur[-1]["text"].rstrip()
                if (not prev_txt.endswith("-")
                        and _ends_short(cur[-1], col, L, _bsize(cur[0]))):
                    break
                cur.append(ls[j])
                j += 1
            line_seg_lists = [ln["segs"] for ln in cur]
            # join words that were split across a line break by a hyphen
            no_space = set()
            for k in range(1, len(line_seg_lists)):
                prev, nxt = line_seg_lists[k - 1], line_seg_lists[k]
                if (prev and nxt and prev[-1][0].endswith("-")
                        and nxt[0][0] and nxt[0][0][0].islower()):
                    lw = prev[-1]
                    prev[-1] = (lw[0][:-1], lw[1], lw[2], lw[3],
                                lw[4], lw[5], lw[6], lw[7])
                    no_space.add(k)
            segs = []
            for k, seglist in enumerate(line_seg_lists):
                if k and k not in no_space:
                    segs.append((" ", False, False, None, None, 0, 0, None))
                segs.extend(seglist)
            text = "".join(s[0] for s in segs)
            while segs and segs[0][0] == " ":
                segs.pop(0)
            while segs and segs[-1][0] == " ":
                segs.pop()
            if not text.strip():
                i = j
                continue
            y0 = min(l["y0"] for l in cur)
            y1 = max(l["y1"] for l in cur)
            x0 = min(l["x0"] for l in cur)
            x1 = max(l["x1"] for l in cur)
            cx = (x0 + x1) / 2.0
            sz = max(l["size"] for l in cur)
            # keep the heading role established by the first line; re-running
            # classify on merged text can demote a wrapped heading to body
            if len(cur) > 1 and first_kind in ("h1", "h2"):
                kind = first_kind
            else:
                kind = classify(text, sz, body, cx, page_w)
            paras.append(dict(text=text, segs=segs, col=col, y0=y0, y1=y1,
                              x0=x0, x1=x1, size=sz, kind=kind, cx=cx,
                              lines=list(cur),
                              halign=(heading_align(x0, x1, col, L)
                                      if kind in ("h1", "h2") else None)))
            i = j
    return paras


def _column_major(items):
    """Order two-column content the way a reader actually moves: all the way
    down the left column, then all the way down the right.

    Sorting by ``(y0, column)`` -- the obvious thing, and what this used to do
    -- interleaves the columns line by line.  On a journal page whose left
    column holds a formula and whose right column holds prose, that produces
    "formula fragment, prose fragment, formula fragment", which reads as
    gibberish.  A full-width element (col ``F``) genuinely interrupts both
    columns at its own y, so it splits the page into bands; inside each band
    the columns are still read one after the other.
    """
    items = sorted(items, key=lambda p: p["y0"])
    out, band = [], []
    for p in items:
        if p["col"] == "F":
            if band:
                out += _emit_band(band)
                band = []
            out.append(p)
        else:
            band.append(p)
    if band:
        out += _emit_band(band)
    return out


def _emit_band(band):
    return (sorted([p for p in band if p["col"] == "L"], key=lambda p: p["y0"])
            + sorted([p for p in band if p["col"] == "R"], key=lambda p: p["y0"]))


def reading_order(paras, L):
    columns = L["columns"]
    rank = {"F": 0, "L": 1, "R": 2}
    if columns == 1 or all(p["col"] == "F" for p in paras):
        return sorted(paras, key=lambda p: (p["y0"], rank[p["col"]]))
    Lp = [p for p in paras if p["col"] == "L"]
    Rp = [p for p in paras if p["col"] == "R"]
    # a page that only uses one of the two columns has no flow to interleave
    if not Lp or not Rp:
        return sorted(paras, key=lambda p: (p["y0"], rank[p["col"]]))
    # find the contiguous two-column body region
    paired = []
    for lp in Lp:
        if any(abs(rp["y0"] - lp["y0"]) <= 14 for rp in Rp):
            paired.append(lp["y0"])
    for rp in Rp:
        if any(abs(lp["y0"] - rp["y0"]) <= 14 for lp in Lp):
            paired.append(rp["y0"])
    if not paired:
        return _column_major(paras)
    paired.sort()
    clusters, cur = [], [paired[0]]
    for y in paired[1:]:
        if y - cur[-1] <= 40:
            cur.append(y)
        else:
            clusters.append(cur)
            cur = [y]
    clusters.append(cur)
    body = max(clusters, key=lambda c: c[-1] - c[0])
    body_start, body_end = body[0], body[-1]
    header = _column_major([p for p in paras if p["y0"] < body_start])
    body_items = [p for p in paras if body_start <= p["y0"] <= body_end]
    body_sorted = _column_major(body_items)
    footer = _column_major([p for p in paras if p["y0"] > body_end])
    return header + body_sorted + footer
