"""Detect a PDF's layout so the converter carries no document-specific
constants.

Everything the emitter needs -- page size, margins, column geometry, body
size, leading, font roles, framed algorithm boxes -- is measured from the
file itself.
"""
import collections
import json
import re

import fitz

MATH_NAME = re.compile(
    r"(CMSY|CMEX|CMMI|CMSL|MSBM|MTSY|MTMI|MTEX|MT Extra|RBLMI|RMTMIB|RMTMI|"
    r"BLEX|Symbol|MathematicalPi|STIXMath|LatinModernMath|XITSMath)", re.I)

GREEK_MATH = re.compile(
    "[\u0370-\u03ff\u2190-\u22ff\u2300-\u23ff\u27c0-\u27ef\u2a00-\u2aff"
    "\u1d400-\u1d7ff]")


def _page_spans(page):
    out = []
    d = page.get_text("rawdict")
    for b in d["blocks"]:
        if b["type"] != 0:
            continue
        for l in b["lines"]:
            for s in l["spans"]:
                txt = "".join(c["c"] for c in s["chars"])
                if not txt.strip():
                    continue
                out.append(dict(font=s["font"], size=round(s["size"], 2),
                                bbox=s["bbox"], text=txt,
                                x0=s["bbox"][0], x1=s["bbox"][2],
                                y0=s["bbox"][1], y1=s["bbox"][3],
                                baseline=s["chars"][0]["origin"][1],
                                n=len(txt)))
    return out


def _sample(doc, limit=40):
    """Evenly spaced pages, skipping a likely cover page."""
    n = len(doc)
    idx = list(range(n))
    if n > limit:
        step = n / float(limit)
        idx = sorted({int(i * step) for i in range(limit)})
    return [doc[i] for i in idx if i < n]


def _weighted_mode(pairs, ndigits=1):
    c = collections.Counter()
    for value, weight in pairs:
        c[round(value, ndigits)] += weight
    return c.most_common(1)[0][0] if c else None


def _pct(values, q):
    """q-th percentile of an unsorted list (nearest-rank)."""
    if not values:
        return None
    a = sorted(values)
    i = min(len(a) - 1, max(0, int(len(a) * q)))
    return a[i]


def _text_frame(body_spans, per_page=None, body=None, page_h=None):
    """The rectangle the body text actually occupies: [x0, y0, x1, y1].

    Percentiles, not extremes: one full-bleed caption or a page with an
    oversized table would otherwise drag the frame out to the paper edge and
    the DOCX would be laid out with a text area the source never uses.  The
    2nd/98th percentile is where a normal page's text starts and stops.

    The bottom edge needs its own treatment.  A page's last line is only
    about 1/45 of the spans on that page, so a flat 98th percentile over the
    whole sample cuts it off and the frame ends exactly one line above the
    source's text area.  Every page that fills to the bottom then asks for
    one line more than it is given and spills it; where a forced page
    boundary follows each source page (see convert.py) that stray line
    becomes a nearly blank page of its own -- 102 went from 80 to 99 pages
    on that single line.  Measuring each page's own bottom and taking a high
    percentile across pages still ignores one stray caption, but lands on
    the real bottom edge.
    """
    if not body_spans:
        return None
    x0 = _pct([s["x0"] for s in body_spans], 0.02)
    x1 = _pct([s["x1"] for s in body_spans], 0.98)
    y0 = _pct([s["y0"] for s in body_spans], 0.02)
    y1 = _pct([s["y1"] for s in body_spans], 0.98)
    if per_page and body is not None:
        bots = []
        for spans in per_page:
            b = [s["y1"] for s in spans if abs(s["size"] - body) < 0.35]
            if b:
                bots.append(max(b))
        if bots:
            ext = _pct(bots, 0.9)
            # never let the extension eat the strip a Word footer lives in
            if page_h:
                ext = min(ext, float(page_h) - 40.0)
            y1 = max(y1, ext)
    if x1 - x0 < 80 or y1 - y0 < 120:
        return None                      # nonsense measurement, caller defaults
    return [round(x0, 1), round(y0, 1), round(x1, 1), round(y1, 1)]


def detect(path, verbose=False):
    doc = fitz.open(path)
    pages = _sample(doc)

    size = collections.Counter(
        (round(p.rect.width), round(p.rect.height)) for p in doc)
    pw, ph = size.most_common(1)[0][0]

    per_page = [_page_spans(p) for p in pages]
    spans = [s for pg in per_page for s in pg]
    if not spans:
        raise SystemExit("no extractable text in " + path)

    # ---- body size: character-weighted mode -------------------------------
    body = _weighted_mode([(s["size"], s["n"]) for s in spans])

    # ---- horizontal extent of the text block ----------------------------
    body_spans = [s for s in spans if abs(s["size"] - body) < 0.35]
    left = _weighted_mode([(s["x0"], 1) for s in body_spans], 0)
    right = _weighted_mode([(s["x1"], 1) for s in body_spans], 0)
    # a hanging indent or a short last line can bias the mode; widen to the
    # 2nd/98th percentile of the actual extremes
    xs0 = sorted(s["x0"] for s in body_spans)
    xs1 = sorted(s["x1"] for s in body_spans)
    left = min(left, xs0[int(len(xs0) * 0.02)])
    right = max(right, xs1[int(len(xs1) * 0.98)])

    # ---- columns --------------------------------------------------------
    mid = (left + right) / 2.0
    left_only = [s for s in body_spans if s["x1"] <= mid + 6]
    right_only = [s for s in body_spans if s["x0"] >= mid - 6]
    lf = len(left_only) / float(len(body_spans))
    rf = len(right_only) / float(len(body_spans))
    columns = 2 if (lf > 0.18 and rf > 0.18) else 1

    if columns == 2:
        gl = max(s["x1"] for s in left_only)
        gr = min(s["x0"] for s in right_only)
        if gr - gl < 4:                    # no clean gap: split at the middle
            gl, gr = mid - 4, mid + 4
        # Spans are the wrong unit for this: on a justified single-column page
        # every line's tail ends on one side of the middle and its head starts
        # on the other, so counting spans on both sides "finds" a gutter in a
        # one-column document.  Body-size *lines* settle it: in a genuine
        # two-column layout no line of running text reaches across the whole
        # measure, so their widths stop at about half of it; a one-column page
        # sets them at full width.  Only body-size lines count -- a maths paper
        # is full of short display rows, and letting those into the sample
        # drags the percentile down until a one-column page looks like two.
        widths = []
        for p in pages:
            try:
                blocks = p.get_text("dict")["blocks"]
            except Exception:
                continue
            for b in blocks:
                if b.get("type") != 0:
                    continue
                for ln in b.get("lines", ()):
                    sp = ln.get("spans") or ()
                    if not sp:
                        continue
                    if abs(max(x["size"] for x in sp) - body) > 0.6:
                        continue
                    widths.append(ln["bbox"][2] - ln["bbox"][0])
        full = float(right - left)
        if full > 0 and widths:
            widths.sort()
            wide = widths[min(len(widths) - 1, int(len(widths) * 0.9))]
            if wide > 0.75 * full:
                columns = 1
    if columns == 2:
        margin = {"L": round(left), "R": round(gr), "F": round(left)}
        rightm = {"L": round(gl), "R": round(right), "F": round(right)}
    else:
        gl = gr = mid
        margin = {"L": round(left), "R": round(left), "F": round(left)}
        rightm = {"L": round(right), "R": round(right), "F": round(right)}

    # ---- leading: modal baseline delta at body size ---------------------
    # The window has to reach past 1.8x body: a report typeset with double
    # spacing (安全牛: 9.9pt body on a 20pt grid, i.e. 2.0x) would otherwise
    # have its real pitch filtered out, and the modal delta would fall back to
    # whatever tighter gap happens to survive -- 11.9 instead of 20.0.  That
    # number drives the DOCX line spacing, so getting it wrong re-flows a
    # 118-page document onto 67 pages.
    deltas = []
    for p in pages:
        rows = collections.defaultdict(list)
        for s in _page_spans(p):
            if abs(s["size"] - body) < 0.35:
                rows[round(s["baseline"], 1)].append(s)
        ys = sorted(rows)
        for a, b in zip(ys, ys[1:]):
            d = b - a
            if body * 0.95 <= d <= body * 2.6:
                deltas.append(round(d, 1))
    leading = collections.Counter(deltas).most_common(1)[0][0] if deltas else round(body * 1.2, 1)

    # ---- font roles ------------------------------------------------------
    counts = collections.Counter()
    for s in spans:
        counts[s["font"]] += s["n"]
    top = [f for f, _ in counts.most_common(6)]

    symbol, italic, bold = set(), set(), set()
    for s in spans:
        f = s["font"]
        if MATH_NAME.search(f):
            symbol.add(f)
            if "italic" in f.lower() or f in ("RBLMI", "RMTMIB", "CMMI10"):
                italic.add(f)
            if "bold" in f.lower() or f in ("RBLMI", "RMTMIB", "Times-BoldItalic"):
                bold.add(f)
            continue
        if GREEK_MATH.search(s["text"]) and f not in top[:3]:
            symbol.add(f)
        if re.search(r"(italic|oblique)", f, re.I):
            italic.add(f)
        if re.search(r"(bold|black|semibold|heavy)", f, re.I):
            bold.add(f)

    # ---- framed graphics (figures, tables, listings) ---------------------
    graphics = _graphics(doc)
    algo = _algo_boxes(doc, graphics)

    # ---- the rectangle the body text occupies ---------------------------
    # Reproducing it as the DOCX page margin is what makes the output paginate
    # like the source.  A fixed 54pt margin gives a page ~12% more text area
    # than the original and the document comes out visibly shorter; on a
    # 118-page report that is tens of pages of drift.
    frame = _text_frame(body_spans, per_page, body, ph)

    layout = dict(
        source=path, pages=len(doc), page=[pw, ph],
        body_size=body, leading=leading, columns=columns,
        frame=frame,
        gutter=[round(gl), round(gr)],
        margin=margin, right=rightm,
        math_fonts=sorted(symbol), italic_fonts=sorted(italic),
        bold_fonts=sorted(bold), body_fonts=top,
        graphics=graphics, algo_boxes=algo,
    )
    if verbose:
        print(json.dumps(layout, ensure_ascii=False, indent=1))
    return layout


def _graphics(doc):
    """Cluster vector drawings and large images into figure regions.

    A PDF whose fonts MuPDF cannot map (a bare CFF, or a Type0 with no
    ToUnicode) draws every glyph as a vector outline.  Those outlines are
    drawings like any other, so clustering happily turns a paragraph or a
    heading into a "figure" -- and the emitter then rasterises real text into a
    picture.  A glyph outline is recognisable: it is a small drawing that sits
    almost entirely inside one text-layer span.  Dropping those *before*
    clustering means a text region produces no cluster at all, while a genuine
    figure's axes, bars and lines (which are not contained in a span) still
    cluster normally.
    """
    out = []
    for pno in range(len(doc)):
        page = doc[pno]
        blocks = page.get_text("dict")["blocks"]
        span_rects = []
        for b in blocks:
            if b["type"] == 0:
                for ln in b.get("lines", ()):
                    for s in ln.get("spans", ()):
                        if s.get("text", "").strip():
                            span_rects.append(fitz.Rect(s["bbox"]))

        def _glyph(dr):
            r = dr["rect"]
            if r.width > 80 or r.height > 80:
                return False            # too big to be a single glyph
            a = max(1e-6, r.get_area())
            for sr in span_rects:
                inter = r & sr
                if not inter.is_empty and inter.get_area() >= 0.6 * a:
                    return True         # drawn text, not a figure element
            return False

        rects = [d["rect"] for d in page.get_drawings()
                 if len(d["items"]) >= 8
                 and not (d["rect"].height < 1.4 and d["rect"].width > 60)
                 and not _glyph(d)]
        for b in blocks:
            if b["type"] == 1:
                r = fitz.Rect(b["bbox"])
                if r.height > 25 and r.width > 40:
                    rects.append(r)
        if not rects:
            continue
        rects.sort(key=lambda r: (r.y0, r.x0))
        clusters = []
        for r in rects:
            hit = None
            for i, c in enumerate(clusters):
                if (r.y0 <= c.y1 + 26 and r.y1 >= c.y0 - 26
                        and not (r.x1 < c.x0 - 25 or r.x0 > c.x1 + 25)):
                    hit = i
                    break
            if hit is None:
                clusters.append(fitz.Rect(r))
            else:
                clusters[hit] = clusters[hit] | r
        for c in clusters:
            if c.height > 35 and c.width > 90:
                out.append(dict(page=pno + 1,
                                bbox=[round(v, 1) for v in c]))
    return out


def _algo_boxes(doc, graphics):
    """A graphic region headed by an 'Algorithm N' caption is a listing.

    The caption often sits just above the frame (the frame is drawn around the
    body only), so the search reaches upward and the box is grown to include it.
    """
    out = []
    for g in graphics:
        page = doc[g["page"] - 1]
        x0, y0, x1, y1 = g["bbox"]
        for s in _page_spans(page):
            if not re.match(r"^Algorithm\s*\d?", s["text"].strip()):
                continue
            if not (x0 - 8 <= s["x0"] <= x1 + 8):
                continue
            if not (y0 - 42 <= s["y0"] <= y1 + 4):
                continue
            box = [min(x0, s["x0"] - 2), min(y0, s["y0"] - 3),
                   max(x1, s["x1"] + 2), y1]
            out.append(dict(page=g["page"], bbox=[round(v, 1) for v in box]))
            break
    return out
