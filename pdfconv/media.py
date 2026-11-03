"""Table and figure extraction (tables via MuPDF, figures via crops)."""
import os
import tempfile

import fitz

from .text import _clean_word, group_rows


def colof_rect(xc, gut_l, gut_r):
    if gut_r <= gut_l + 1:
        return "F"
    if xc < gut_l:
        return "L"
    if xc > gut_r:
        return "R"
    return "F"


def _rotated(page, bbox):
    """True when the text inside *bbox* is set on its side.

    Journals print a wide catalogue table rotated 90 degrees on a portrait
    page.  find_tables happily returns a grid for it, but the grid is built
    from the page's axes, so the rows and columns come out transposed and
    merged -- a 35-row, 13-column table arrives as 12 rows of 29 columns whose
    header is a mash of parameter names and status codes.  There is no
    orientation to recover from the cell rectangles, so the region is refused
    and its words stay in the text flow (a picture of the region would at
    least be faithful, but it would be printed sideways).
    """
    vertical = total = 0
    try:
        blocks = page.get_text("dict", clip=bbox).get("blocks", ())
    except Exception:
        return False
    for b in blocks:
        if b.get("type") != 0:
            continue
        for ln in b.get("lines", ()):
            d = ln.get("dir") or (1.0, 0.0)
            total += 1
            if abs(d[0]) < 0.7:            # more vertical than horizontal
                vertical += 1
    return total > 0 and vertical > 0.6 * total


def _plausible_table(rows):
    """Reject find_tables hits that cannot be turned into a usable Word table.

    MuPDF returns *something* for almost any ruled region, and on figure-heavy
    pages it returns grids whose "cells" are whole text blocks: a 1x4 grid
    holding four 40-line columns, a 35x20 grid that is 95% empty, or a 5x2 grid
    where nine cells in ten contain a paragraph.  Emitting those gives a table
    nobody can read.

    A refused region is **not** automatically a figure.  convert.py only crops
    it when real artwork sits inside; otherwise its words fall back into the
    normal text flow, which keeps the content instead of hiding it in a picture.
    """
    if not rows:
        return False
    cells = [c for row in rows for c in row]
    if not cells:
        return False
    filled = [c for c in cells if c.strip()]
    if len(filled) < 4 or len(filled) < 0.25 * len(cells):
        return False                       # mostly-empty grid
    # A single-row grid is legitimate -- bit arrays and block diagrams are
    # printed as one row of cells -- so row count is not a criterion.  What
    # separates a real table from a mis-segmented text listing is whether the
    # cells hold values or paragraphs.
    multiline = sum(1 for c in filled if "\n" in c)
    if multiline > 0.50 * len(filled):
        return False                       # cells hold paragraphs, not values
    longc = sum(1 for c in filled if len(c) > 80)
    if longc > 0.15 * len(filled):
        return False
    return True


def _cell_paragraphs(words, rect):
    """Per-cell content: list of lines, each a seg tuple list with styling."""
    inner = [w for w in words
             if rect.x0 - 1 <= (w["x0"] + w["x1"]) / 2.0 <= rect.x1 + 1
             and rect.y0 - 1 <= (w["y0"] + w["y1"]) / 2.0 <= rect.y1 + 1]
    if not inner:
        return []
    return [ln["segs"] for ln in group_rows(inner)]


def _col_edges(cells, ncols):
    """Distinct vertical grid lines of the table, left to right."""
    xs = sorted({round(float(c[0]), 1) for c in cells} |
                {round(float(c[2]), 1) for c in cells})
    if len(xs) - 1 != ncols:
        return None
    return xs


def _grid_spans(cells, nrows, ncols):
    """Derive (r0,c0,r1,c1) grid spans from cell rectangles.

    Conservative: returns None unless the cells map cleanly onto a regular
    nrows x ncols grid (every grid cell covered exactly once).  That guarantees
    we only ever *add* correct merges and never corrupt an irregular table.
    """
    if not cells or nrows <= 0 or ncols <= 0:
        return None
    try:
        xs = sorted({round(float(c[0]), 1) for c in cells} |
                    {round(float(c[2]), 1) for c in cells})
        ys = sorted({round(float(c[1]), 1) for c in cells} |
                    {round(float(c[3]), 1) for c in cells})
    except Exception:
        return None
    if len(xs) - 1 != ncols or len(ys) - 1 != nrows:
        return None

    def ei(v, edges):
        best, bd = 0, 1e18
        for i, e in enumerate(edges):
            d = abs(e - v)
            if d < bd:
                bd, best = d, i
        return best

    occ = [[0] * ncols for _ in range(nrows)]
    spans = []
    for c in cells:
        try:
            c0 = ei(float(c[0]), xs)
            c1 = ei(float(c[2]), xs)
            r0 = ei(float(c[1]), ys)
            r1 = ei(float(c[3]), ys)
        except Exception:
            return None
        if c1 <= c0 or r1 <= r0 or c1 > ncols or r1 > nrows:
            return None
        spans.append((r0, c0, r1, c1))
        for r in range(r0, r1):
            for cc in range(c0, c1):
                occ[r][cc] += 1
    for r in range(nrows):
        for cc in range(ncols):
            if occ[r][cc] != 1:
                return None
    return spans


def extract_tables(page, gut_l, gut_r, words=None):
    """Return (tables, rejected_regions, text_only_regions).

    rejected_regions are find_tables hits that are really plot regions; the
    caller turns them into figures so their labels stay out of the body text.
    text_only_regions are hits refused because the grid itself is unusable (a
    rotated catalogue, a sub-character column): their words must stay in the
    text flow and they must not be cropped as pictures either.
    """
    good, rejected, text_only = [], [], []
    if not hasattr(page, "find_tables"):
        return good, rejected, text_only
    if words is None:
        from .text import page_words
        words = page_words(page)
    for t in page.find_tables().tables:
        try:
            rows = t.extract()
        except Exception:
            continue
        rows = [[_clean_word("" if c is None else str(c)).strip() for c in row]
                for row in rows]
        if not any(r.strip() for row in rows for r in row):
            continue
        bbox = fitz.Rect(t.bbox)
        if not _plausible_table(rows):
            rejected.append(bbox)
            continue
        if _rotated(page, bbox):
            rejected.append(bbox)
            text_only.append(bbox)
            continue
        ncols = max((len(row) for row in rows), default=0)
        cells = getattr(t, "cells", None)
        # A grid with a column narrower than a character cannot be set: every
        # glyph wraps to a line of its own and one table row then fills a
        # whole page.  050's framed algorithm box arrived as a 17x4 grid whose
        # second column was one character wide, and the table alone cost seven
        # near-empty pages.
        if cells:
            xs = sorted({round(float(c[0]), 1) for c in cells} |
                        {round(float(c[2]), 1) for c in cells})
            if len(xs) > 1 and min(b - a for a, b in zip(xs, xs[1:])) < 8.0:
                rejected.append(bbox)
                text_only.append(bbox)
                continue
        spans = _grid_spans(cells, len(rows), ncols)
        merges = []
        if spans:
            merges = [s for s in spans
                      if (s[2] - s[0] > 1 or s[3] - s[1] > 1)]
        # per-cell styled content, so table text keeps its original typography.
        # find_tables returns `cells` in *column*-major order, so each rect is
        # mapped onto the grid by coordinates instead of by position -- that is
        # also what makes merged cells land on their top-left corner.
        content = [[None] * ncols for _ in range(len(rows))]
        edges = _col_edges(cells, ncols) if cells else None
        if cells and edges:
            ys = sorted({round(float(c[1]), 1) for c in cells} |
                        {round(float(c[3]), 1) for c in cells})

            def _idx(v, seq):
                best, bd = 0, 1e18
                for i, e in enumerate(seq):
                    dd = abs(e - v)
                    if dd < bd:
                        bd, best = dd, i
                return best

            for c in cells:
                r0 = _idx(float(c[1]), ys)
                c0 = _idx(float(c[0]), edges)
                if 0 <= r0 < len(rows) and 0 <= c0 < ncols:
                    content[r0][c0] = _cell_paragraphs(words, fitz.Rect(c))
        good.append(dict(bbox=bbox, nrows=len(rows), ncols=ncols, cells=rows,
                         content=content, edges=edges, merges=merges,
                         col=colof_rect((bbox.x0 + bbox.x1) / 2, gut_l, gut_r),
                         y=bbox.y0))
    return good, rejected, text_only


def extract_figures(page, graphics, tables, gut_l, gut_r, extra_rects=(),
                    suppress=()):
    out = []
    # regions that must never be cropped: accepted tables, and refused ones
    # whose words are meant to stay in the text flow
    blocked = [t["bbox"] for t in tables] + [fitz.Rect(r) for r in suppress]

    def _covered(bbox):
        return any(bbox.intersects(b) and b.width > bbox.width * 0.6
                   for b in blocked)

    for im in page.get_image_info():
        bbox = fitz.Rect(im["bbox"])
        if bbox.width < 40 or bbox.height < 40:
            continue
        if _covered(bbox):
            continue
        out.append(dict(bbox=bbox,
                        col=colof_rect((bbox.x0 + bbox.x1) / 2, gut_l, gut_r),
                        y=bbox.y0))
    for g in graphics:
        bbox = fitz.Rect(g["bbox"])
        if bbox.width < 90 or bbox.height < 35:
            continue
        if _covered(bbox):
            continue
        if any(o["bbox"].intersects(bbox) and o["bbox"].width > bbox.width * 0.6
               for o in out):
            continue
        out.append(dict(bbox=bbox,
                        col=colof_rect((bbox.x0 + bbox.x1) / 2, gut_l, gut_r),
                        y=bbox.y0))
    # regions find_tables mistook for tables: keep them as pictures instead
    for r in extra_rects:
        bbox = fitz.Rect(r)
        if bbox.width < 40 or bbox.height < 25:
            continue
        if any(o["bbox"].intersects(bbox) and o["bbox"].width > bbox.width * 0.6
               for o in out):
            continue
        out.append(dict(bbox=bbox,
                        col=colof_rect((bbox.x0 + bbox.x1) / 2, gut_l, gut_r),
                        y=bbox.y0))
    return out


def crop(page, rect, dpi, prefix):
    r = fitz.Rect(rect.x0 - 2, rect.y0 - 2, rect.x1 + 2, rect.y1 + 2)
    pix = page.get_pixmap(clip=r, dpi=dpi)
    fd, path = tempfile.mkstemp(suffix=".png", prefix=prefix)
    os.close(fd)
    pix.save(path)
    return path, r.width, r.height
