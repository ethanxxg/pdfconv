"""PDF text layer: glyph-accurate tokenisation, styling, and line building.

MuPDF is used only to *read* the PDF (glyph boxes + per-span style).  Every
decision below -- where a word starts and ends, how tokens are grouped into
visual rows, and whether a token is super-/subscript -- is our own logic.

Why span-level tokens instead of MuPDF's word extractor
------------------------------------------------------
A printed token such as ``LIU2,3`` straddles two font sizes.  MuPDF reports it
as one "word" with one style, so the superscript ``2,3`` would be lost.  By
splitting *span* text ourselves every token inherits exactly the style of the
span it came from, which is what makes superscript / subscript placement match
the original at Aspose-level fidelity.

Why visual-row clustering instead of MuPDF's (block, line) ids
-------------------------------------------------------------
A raised baseline (superscript) makes MuPDF start a *new* line in the middle of
a visual row.  Author blocks therefore arrived as fragments like
``"ZHONGMEI ZHANG"`` + ``"1,2,3, CHEN LIU2,3, XIAOHONG LI"`` + ``"1, YANBO HAN"``
and got interleaved in the wrong order.  We re-cluster tokens inside each block
by vertical overlap and then order them left-to-right.
"""
from collections import defaultdict

from .config import ITALIC_RE, BOLD_RE, SYMBOL_RE, XML_DROP


def is_italic(font):
    return bool(ITALIC_RE.search(font or ""))


def is_bold(font):
    return bool(BOLD_RE.search(font or ""))


def is_symbol(font):
    return bool(SYMBOL_RE.search(font or ""))


def _c1_to_unicode(ch):
    """Decode a 0x80-0x9F C1 byte as CP1252 (where many PDFs stash smart
    quotes / dashes / bullets), so we keep the punctuation instead of
    throwing it away."""
    try:
        return bytes([ord(ch)]).decode("cp1252")
    except Exception:
        return ""


def _clean_word(word):
    """Map CP1252 C1 controls (0x80-0x9F) to real Unicode, drop the
    remaining XML-illegal control chars and soft hyphens."""
    out = []
    for ch in word:
        o = ord(ch)
        if 0x80 <= o <= 0x9F:
            ch = _c1_to_unicode(ch)
            if not ch:
                continue
        if ch == "\u00ad":  # soft hyphen
            continue
        out.append(ch)
    return XML_DROP.sub("", "".join(out))


def _link_lookup(page):
    links = []
    for l in page.get_links():
        uri = l.get("uri")
        if uri:
            links.append((tuple(l["from"]), uri))

    def link_for(x0, y0, x1, y1):
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        for (lx0, ly0, lx1, ly1), uri in links:
            if lx0 - 1 <= cx <= lx1 + 1 and ly0 - 1 <= cy <= ly1 + 1:
                return uri
        return None

    return link_for


def page_words(page):
    """Tokenise the page at *span* granularity.

    Returns dicts with: word, x0/y0/x1/y1, blk, font, size, color, bold,
    italic, link, sp (an explicit space character preceded the token).
    """
    link_for = _link_lookup(page)
    out = []
    d = page.get_text("rawdict")
    for b in d["blocks"]:
        if b["type"] != 0:
            continue
        bno = b.get("number", 0)
        for line in b["lines"]:
            for s in line["spans"]:
                if s.get("type", 0) != 0:
                    continue
                font = s.get("font") or ""
                size = float(s.get("size") or 0) or 10.0
                color = s.get("color", 0)
                flags = s.get("flags", 0)
                bo = bool(flags & 16) or is_bold(font)
                it = bool(flags & 2) or (is_italic(font) and not is_symbol(font))
                chars = s.get("chars") or []
                if not chars:
                    _tokens_from_text(s, bno, font, size, color, bo, it,
                                      link_for, out)
                    continue
                pending = []
                had_space = False
                for c in chars:
                    ch = c.get("c", "")
                    if not ch:
                        continue
                    if ch.isspace() or ch == "\u00a0":
                        if pending:
                            _emit(pending, had_space, bno, font, size, color,
                                  bo, it, link_for, out)
                            pending = []
                        had_space = True
                        continue
                    pending.append(c)
                if pending:
                    _emit(pending, had_space, bno, font, size, color,
                          bo, it, link_for, out)
    return out


def _emit(pending, had_space, bno, font, size, color, bo, it, link_for, out):
    txt = _clean_word("".join(c.get("c", "") for c in pending))
    if not txt:
        return
    x0 = min(c["bbox"][0] for c in pending)
    y0 = min(c["bbox"][1] for c in pending)
    x1 = max(c["bbox"][2] for c in pending)
    y1 = max(c["bbox"][3] for c in pending)
    out.append(dict(word=txt, x0=x0, y0=y0, x1=x1, y1=y1, blk=bno,
                    font=font, size=size, color=color, bold=bo, italic=it,
                    link=link_for(x0, y0, x1, y1), sp=had_space))


def _tokens_from_text(s, bno, font, size, color, bo, it, link_for, out):
    """Fallback for spans without per-character boxes: split the span text and
    distribute the span box proportionally to character counts."""
    text = s.get("text") or ""
    bx0, by0, bx1, by1 = s["bbox"]
    total = len(text) or 1
    step = (bx1 - bx0) / float(total)
    pos = 0
    for raw in text.split():
        idx = text.find(raw, pos)
        if idx < 0:
            continue
        pos = idx + len(raw)
        txt = _clean_word(raw)
        if not txt:
            continue
        x0 = bx0 + step * idx
        x1 = bx0 + step * (idx + len(raw))
        out.append(dict(word=txt, x0=x0, y0=by0, x1=x1, y1=by1, blk=bno,
                        font=font, size=size, color=color, bold=bo, italic=it,
                        link=link_for(x0, by0, x1, by1), sp=True))


def _base_size(items):
    """Dominant font size of a visual row, weighted by character count."""
    weight = defaultdict(int)
    for w in items:
        weight[round(w["size"], 1)] += len(w["word"])
    return max(weight.items(), key=lambda kv: kv[1])[0]


def _make_line(items):
    items = sorted(items, key=lambda w: (w["x0"], w["y0"]))
    base = _base_size(items)
    base_tokens = [w for w in items if abs(w["size"] - base) <= 0.35]
    base_y0 = sorted(w["y0"] for w in base_tokens or items)
    base_y0 = base_y0[len(base_y0) // 2]
    rise = 0.10 * base

    segs = []
    text_parts = []
    prev = None
    for w in items:
        off = base_y0 - w["y0"]
        smaller = w["size"] <= base * 0.92
        vert = None
        if smaller and off > rise:
            vert = "sup"
        elif smaller and off < -rise:
            vert = "sub"
        if prev is not None:
            gap = w["x0"] - prev["x1"]
            need = w["sp"] or gap > 0.16 * max(w["size"], prev["size"])
            if vert is not None:
                need = False          # scripts attach to the preceding token
            if need:
                segs.append((" ", False, False, None, None, 0, 0, None))
                text_parts.append(" ")
        segs.append((w["word"], w["italic"], w["bold"], vert, w["font"],
                     w["size"], w["color"], w["link"]))
        text_parts.append(w["word"])
        prev = w

    x0 = min(w["x0"] for w in items)
    x1 = max(w["x1"] for w in items)
    y0 = min(w["y0"] for w in items)
    y1 = max(w["y1"] for w in items)
    return dict(text="".join(text_parts), segs=segs, x0=x0, x1=x1,
                y0=y0, y1=y1, size=max(w["size"] for w in items),
                bsize=base, cx=(x0 + x1) / 2.0)


def _merge_script_rows(rows):
    """Fold a row made only of sub/superscripts back into the row above it.

    A script set far enough below its baseline falls outside group_rows' band
    and becomes a row of its own -- which then becomes its own paragraph, so a
    sentence comes out as "…space, D^p = {(x^p, y^p)|i =" / "i" / "1, 2, …".
    Such a row is recognised by being entirely smaller type, horizontally
    inside the row above, and touching it vertically.
    """
    out = []
    for r in rows:
        if out:
            prev = out[-1]
            big = max((w["size"] for w in r), default=0)
            psz = max((w["size"] for w in prev), default=0)
            if big and psz and big <= 0.85 * psz:
                px0 = min(w["x0"] for w in prev)
                px1 = max(w["x1"] for w in prev)
                rx0 = min(w["x0"] for w in r)
                rx1 = max(w["x1"] for w in r)
                gap = min(w["y0"] for w in r) - max(w["y1"] for w in prev)
                if (rx0 >= px0 - 3 and rx1 <= px1 + 3
                        and gap <= 0.35 * psz):
                    prev.extend(r)
                    continue
        out.append(r)
    return out


def group_rows(words):
    """Cluster arbitrary tokens into visual rows, ignoring block ids.

    Used both for page text (per block, see build_lines) and for table cells,
    where the tokens of one cell can come from several PDF blocks.
    """
    if not words:
        return []
    ws = sorted(words, key=lambda w: ((w["y0"] + w["y1"]) / 2.0, w["x0"]))
    rows = []
    cur = [ws[0]]
    cur_cy = (ws[0]["y0"] + ws[0]["y1"]) / 2.0
    cur_sz = ws[0]["size"]
    for w in ws[1:]:
        cy = (w["y0"] + w["y1"]) / 2.0
        base = cur_sz if cur_sz > w["size"] else w["size"]
        if abs(cy - cur_cy) <= 0.5 * base:
            cur.append(w)
            if w["size"] > cur_sz:
                cur_sz = w["size"]
        else:
            rows.append(cur)
            cur = [w]
            cur_cy = cy
            cur_sz = w["size"]
    rows.append(cur)
    return [_make_line(r) for r in _merge_script_rows(rows)]


def build_lines(words, gut=None):
    """Cluster tokens into visual rows, then order each row left-to-right.

    Rows are grouped per *column*, not per PDF block.  A single visual row is
    routinely spread over several blocks -- a subscript, an italic variable and
    the surrounding prose each arrive as their own block -- so grouping by
    block shatters one line into fragments that later become separate
    paragraphs ("i", ". y ,", "1, 2, . . . , N, f = i}").  Column is the right
    granularity: it keeps a row whole without ever stapling the left column's
    text to the right column's, which is what grouping across every block
    would do on a two-column page.
    """
    if not words:
        return []
    lines = []
    if gut:
        gl, gr = gut
        buckets = defaultdict(list)
        for w in words:
            cx = (w["x0"] + w["x1"]) / 2.0
            key = "L" if cx <= gl else ("R" if cx >= gr else "F")
            buckets[key].append(w)
        for _k, ws in buckets.items():
            lines.extend(group_rows(ws))
    else:
        byblk = defaultdict(list)
        for w in words:
            byblk[w["blk"]].append(w)
        for _bno, ws in byblk.items():
            lines.extend(group_rows(ws))
    lines.sort(key=lambda l: (l["y0"], l["x0"]))
    return lines
