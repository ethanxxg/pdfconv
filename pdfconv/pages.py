"""Page-level PDF utilities (pure PyMuPDF).

Operations: merge, split, remove / extract pages, rotate, crop, watermark,
page numbers, rasterise to images, images to PDF, and a structural compress.
Each function takes explicit paths and returns simple counters so it can be
driven by the CLI (``python -m pdfconv.pages ...``) or the desktop GUI.
"""

from __future__ import annotations

import os
import re

import fitz

_CJK_RE = re.compile(r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af]")


def _font_for(text):
    """Pick a built-in font that can render *text* (CJK needs 'china-s')."""
    return "china-s" if _CJK_RE.search(text or "") else "helv"


def parse_pages(spec, total):
    """'1,3,5-7' -> sorted set of 1-based page numbers within 1..total.

    Empty / None -> empty set.  Open ranges like '5-' mean 5..total.
    """
    pages = set()
    if spec is None:
        return pages
    if isinstance(spec, (set, list, tuple)):
        return {int(p) for p in spec if 1 <= int(p) <= total}
    s = str(spec).strip()
    if not s:
        return pages
    for part in s.replace("，", ",").replace(" ", ",").split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            a = int(a)
            b = int(b) if b.strip() else total
            if a > b:
                a, b = b, a
            for i in range(a, b + 1):
                if 1 <= i <= total:
                    pages.add(i)
        else:
            i = int(part)
            if 1 <= i <= total:
                pages.add(i)
    return pages


def _save(doc, out_path):
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    doc.save(out_path, garbage=4, deflate=True, clean=True)


def merge_pdfs(inputs, out_path):
    """Concatenate *inputs* (in order) into one PDF.  Returns page count."""
    out = fitz.open()
    for p in inputs:
        with fitz.open(p) as src:
            out.insert_pdf(src)
    n = len(out)
    _save(out, out_path)
    out.close()
    return n


def split_pdf(in_path, out_dir, every=1, ranges=None):
    """Split *in_path* into pieces written under *out_dir*.

    ranges: optional list of (first, last) 1-based inclusive pairs.
    Otherwise split every *every* pages.  Returns list of written paths.
    """
    src = fitz.open(in_path)
    total = len(src)
    stem = os.path.splitext(os.path.basename(in_path))[0]
    os.makedirs(out_dir, exist_ok=True)
    if ranges:
        segs = ranges
    else:
        every = max(1, int(every))
        segs = [(s, min(s + every - 1, total))
                for s in range(1, total + 1, every)]
    outs = []
    for (a, b) in segs:
        new = fitz.open()
        new.insert_pdf(src, from_page=a - 1, to_page=b - 1)
        path = os.path.join(out_dir, "%s_%d-%d.pdf" % (stem, a, b))
        _save(new, path)
        new.close()
        outs.append(path)
    src.close()
    return outs


def remove_pages(in_path, out_path, pages):
    """Drop the given 1-based *pages*; keep everything else."""
    src = fitz.open(in_path)
    total = len(src)
    rm = parse_pages(pages, total)
    new = fitz.open()
    for i in range(total):
        if (i + 1) not in rm:
            new.insert_pdf(src, from_page=i, to_page=i)
    _save(new, out_path)
    n = len(new)
    new.close()
    src.close()
    return n


def extract_pages(in_path, out_path, pages):
    """Keep only the given 1-based *pages*, in ascending order."""
    src = fitz.open(in_path)
    total = len(src)
    keep = parse_pages(pages, total)
    new = fitz.open()
    for i in range(total):
        if (i + 1) in keep:
            new.insert_pdf(src, from_page=i, to_page=i)
    _save(new, out_path)
    n = len(new)
    new.close()
    src.close()
    return n


def rotate_pages(in_path, out_path, angle, pages=None):
    """Rotate *pages* (default all) clockwise by *angle* (90/180/270)."""
    src = fitz.open(in_path)
    total = len(src)
    sel = None if pages in (None, "", "all") else parse_pages(pages, total)
    for i in range(total):
        if sel is None or (i + 1) in sel:
            src[i].set_rotation((src[i].rotation + int(angle)) % 360)
    _save(src, out_path)
    src.close()
    return total


def crop_pages(in_path, out_path, top=0, bottom=0, left=0, right=0):
    """Trim *top/bottom/left/right* points off every page (crop box)."""
    src = fitz.open(in_path)
    for page in src:
        r = page.rect
        box = fitz.Rect(r.x0 + left, r.y0 + top, r.x1 - right, r.y1 - bottom)
        if box.is_empty or box.width <= 1 or box.height <= 1:
            continue
        page.set_cropbox(box)
    _save(src, out_path)
    n = len(src)
    src.close()
    return n


def add_watermark(in_path, out_path, text, opacity=0.15, angle=0, fontsize=48):
    """Stamp *text* across every page (grey, semi-transparent)."""
    src = fitz.open(in_path)
    font = _font_for(text)
    for page in src:
        r = page.rect
        rect = fitz.Rect(r.x0 + 40, r.y0 + 40, r.x1 - 40, r.y1 - 40)
        try:
            page.insert_textbox(
                rect, text, fontsize=fontsize, fontname=font,
                color=(0.5, 0.5, 0.5), align=fitz.TEXT_ALIGN_CENTER,
                rotate=int(angle) % 360, fill_opacity=opacity)
        except TypeError:
            # older PyMuPDF without fill_opacity
            page.insert_textbox(
                rect, text, fontsize=fontsize, fontname=font,
                color=(0.7, 0.7, 0.7), align=fitz.TEXT_ALIGN_CENTER,
                rotate=int(angle) % 360)
    _save(src, out_path)
    n = len(src)
    src.close()
    return n


def add_page_numbers(in_path, out_path, position="bottom-center",
                     start=1, fontsize=10, fmt="{n}"):
    """Write a page number on every page.  position: bottom/top + center/left/right."""
    src = fitz.open(in_path)
    for i, page in enumerate(src):
        r = page.rect
        txt = fmt.format(n=start + i)
        if position.startswith("bottom"):
            rect = fitz.Rect(r.x0, r.y1 - 30, r.x1, r.y1 - 6)
        else:
            rect = fitz.Rect(r.x0, r.y0 + 6, r.x1, r.y0 + 30)
        if position.endswith("left"):
            align = fitz.TEXT_ALIGN_LEFT
        elif position.endswith("right"):
            align = fitz.TEXT_ALIGN_RIGHT
        else:
            align = fitz.TEXT_ALIGN_CENTER
        page.insert_textbox(rect, txt, fontsize=fontsize, fontname="helv",
                            color=(0, 0, 0), align=align)
    _save(src, out_path)
    n = len(src)
    src.close()
    return n


def pdf_to_images(in_path, out_dir, dpi=150, fmt="png", pages=None):
    """Rasterise pages to *out_dir* as png/jpg.  Returns list of paths."""
    src = fitz.open(in_path)
    total = len(src)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(in_path))[0]
    if pages in (None, "", "all"):
        sel = range(total)
    else:
        sel = [p - 1 for p in sorted(parse_pages(pages, total))]
    outs = []
    for i in sel:
        pix = src[i].get_pixmap(dpi=int(dpi))
        path = os.path.join(out_dir, "%s_p%d.%s" % (stem, i + 1, fmt))
        pix.save(path)
        outs.append(path)
    src.close()
    return outs


def images_to_pdf(images, out_path):
    """Combine image files (jpg/png/...) into a single PDF.  Returns pages."""
    out = fitz.open()
    for p in images:
        img = fitz.open(p)
        try:
            pdf_bytes = img.convert_to_pdf()
        finally:
            img.close()
        part = fitz.open("pdf", pdf_bytes)
        out.insert_pdf(part)
        part.close()
    n = len(out)
    _save(out, out_path)
    out.close()
    return n


def compress_pdf(in_path, out_path):
    """Structural compression: drop unused objects, deflate streams, clean."""
    src = fitz.open(in_path)
    before = os.path.getsize(in_path)
    _save(src, out_path)
    src.close()
    after = os.path.getsize(out_path) if os.path.exists(out_path) else before
    return before, after


# --------------------------------------------------------------------- CLI
def main(argv=None):
    import argparse
    import sys

    argv = list(sys.argv[1:] if argv is None else argv)
    p = argparse.ArgumentParser(
        prog="pdfconv.pages",
        description="Page-level PDF tools (merge / split / rotate / crop / ...).")
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("merge", help="merge PDFs in order")
    m.add_argument("out")
    m.add_argument("inputs", nargs="+")

    s = sub.add_parser("split", help="split a PDF")
    s.add_argument("input")
    s.add_argument("--outdir", required=True)
    s.add_argument("--every", type=int, default=1)

    r = sub.add_parser("rotate", help="rotate pages")
    r.add_argument("input")
    r.add_argument("out")
    r.add_argument("--angle", type=int, default=90, choices=(90, 180, 270))
    r.add_argument("--pages", default=None)

    c = sub.add_parser("crop", help="crop margins (points)")
    c.add_argument("input")
    c.add_argument("out")
    for side in ("top", "bottom", "left", "right"):
        c.add_argument("--" + side, type=float, default=0)

    rm = sub.add_parser("remove", help="remove pages")
    rm.add_argument("input")
    rm.add_argument("out")
    rm.add_argument("--pages", required=True)

    ex = sub.add_parser("extract", help="extract pages")
    ex.add_argument("input")
    ex.add_argument("out")
    ex.add_argument("--pages", required=True)

    w = sub.add_parser("watermark", help="add a text watermark")
    w.add_argument("input")
    w.add_argument("out")
    w.add_argument("--text", required=True)
    w.add_argument("--opacity", type=float, default=0.15)
    w.add_argument("--angle", type=int, default=0)
    w.add_argument("--fontsize", type=float, default=48)

    pn = sub.add_parser("pagenum", help="add page numbers")
    pn.add_argument("input")
    pn.add_argument("out")
    pn.add_argument("--pos", default="bottom-center")
    pn.add_argument("--start", type=int, default=1)
    pn.add_argument("--fontsize", type=float, default=10)

    pi = sub.add_parser("pdf2img", help="rasterise PDF to images")
    pi.add_argument("input")
    pi.add_argument("--outdir", required=True)
    pi.add_argument("--dpi", type=int, default=150)
    pi.add_argument("--fmt", default="png", choices=("png", "jpg"))
    pi.add_argument("--pages", default=None)

    ii = sub.add_parser("img2pdf", help="images -> one PDF")
    ii.add_argument("out")
    ii.add_argument("images", nargs="+")

    cp = sub.add_parser("compress", help="structural compress")
    cp.add_argument("input")
    cp.add_argument("out")

    a = p.parse_args(argv)
    if a.cmd == "merge":
        print("pages=%d" % merge_pdfs(a.inputs, a.out))
    elif a.cmd == "split":
        outs = split_pdf(a.input, a.outdir, every=a.every)
        print("files=%d -> %s" % (len(outs), a.outdir))
    elif a.cmd == "rotate":
        print("pages=%d" % rotate_pages(a.input, a.out, a.angle, a.pages))
    elif a.cmd == "crop":
        print("pages=%d" % crop_pages(a.input, a.out, a.top, a.bottom,
                                      a.left, a.right))
    elif a.cmd == "remove":
        print("pages=%d" % remove_pages(a.input, a.out, a.pages))
    elif a.cmd == "extract":
        print("pages=%d" % extract_pages(a.input, a.out, a.pages))
    elif a.cmd == "watermark":
        print("pages=%d" % add_watermark(a.input, a.out, a.text, a.opacity,
                                         a.angle, a.fontsize))
    elif a.cmd == "pagenum":
        print("pages=%d" % add_page_numbers(a.input, a.out, a.pos, a.start,
                                            a.fontsize))
    elif a.cmd == "pdf2img":
        outs = pdf_to_images(a.input, a.outdir, a.dpi, a.fmt, a.pages)
        print("images=%d -> %s" % (len(outs), a.outdir))
    elif a.cmd == "img2pdf":
        print("pages=%d" % images_to_pdf(a.images, a.out))
    elif a.cmd == "compress":
        b, af = compress_pdf(a.input, a.out)
        print("bytes %d -> %d" % (b, af))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
