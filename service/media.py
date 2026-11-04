"""PDF <-> image conversion for the job runner.

Two directions, both from ``pdfconv.pages``:

    pdf2img  -> rasterise every page to PNG/JPG, pack them into one .zip
    img2pdf  -> combine several JPG/PNG images into one PDF

Runs inside the process pool, so it must be importable with no side effects and
take only picklable arguments.  Two service-only concerns are handled here:

* uploads are stored as ``in/<file_id>`` with **no extension**, and MuPDF's
  image loader is happier with a real suffix -- so each image is staged to a
  temp file with the extension its magic bytes imply before the engine sees it;
* a failed rasterise must not leave a half-written zip behind.
"""
import os
import shutil
import tempfile
import zipfile

import pymupdf as fitz
from pdfconv import pages as P

MAX_PAGES = 5000
ALLOWED_DPI = (72, 150, 300)


class MediaError(Exception):
    """A refusal or failure with a stable code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; without this
        # the code is lost to a TypeError during unpickling (see convert.py).
        return (self.__class__, (self.code, self.message))


def _magic(src):
    """pdf / jpg / png, or None -- from the bytes, not the name."""
    try:
        with open(src, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    if head.startswith(b"%PDF-"):
        return "pdf"
    if head[:3] == b"\xff\xd8\xff":
        return "jpg"
    if head.startswith(b"\x89PNG"):
        return "png"
    return None


def _check_pdf(src, limit):
    """Validate the PDF input.  Returns its page count."""
    doc = fitz.open(src)
    try:
        if doc.needs_pass:
            raise MediaError("encrypted", "文件已加密，请先解除密码")
        pages = doc.page_count
        if pages == 0:
            raise MediaError("empty", "文件没有任何页面")
        if pages > limit:
            raise MediaError(
                "too_many_pages",
                "文件共 %d 页，最多支持 %d 页" % (pages, limit))
    finally:
        doc.close()
    return pages


def pdf_to_images_task(src, dst, fmt="png", dpi=150, max_pages=None):
    """Rasterise *src* to images and pack them into the zip at *dst*.

    Returns {op, pages, images, fmt, dpi}.  PNG/JPG are already compressed, so
    the zip is stored rather than deflated -- deflating them again burns CPU for
    nothing.
    """
    fmt = "jpg" if str(fmt).lower() in ("jpg", "jpeg") else "png"
    try:
        dpi = int(dpi)
    except (TypeError, ValueError):
        dpi = 150
    if dpi not in ALLOWED_DPI:
        dpi = 150
    limit = int(max_pages or MAX_PAGES)
    pages = _check_pdf(src, limit)

    out_dir = dst + ".pages"
    os.makedirs(out_dir, exist_ok=True)
    try:
        try:
            outs = P.pdf_to_images(src, out_dir, dpi=dpi, fmt=fmt)
        except Exception:
            raise MediaError("convert_failed", "导出图片失败，请重试")
        outs = [p for p in outs if p and os.path.exists(p)]
        if not outs:
            raise MediaError("convert_failed", "没有生成任何图片")
        try:
            with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as z:
                for p in outs:
                    z.write(p, os.path.basename(p))
        except Exception:
            try:
                os.remove(dst)
            except OSError:
                pass
            raise MediaError("convert_failed", "打包图片失败，请重试")
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)

    return {"op": "pdf2img", "pages": pages, "images": len(outs),
            "fmt": fmt, "dpi": dpi}


def images_to_pdf_task(paths, dst, max_pages=None):
    """Combine the image files *paths* (in order) into one PDF at *dst*.

    Returns {op, pages, images}.  Each input is staged to a temp path carrying
    the extension its magic bytes imply, because the upload has none and MuPDF
    decides how to decode from the suffix.
    """
    limit = int(max_pages or MAX_PAGES)
    if not paths:
        raise MediaError("empty", "没有图片")
    if len(paths) > limit:
        raise MediaError(
            "too_many_pages",
            "共 %d 张图片，最多支持 %d 张" % (len(paths), limit))

    staged = []
    try:
        for i, p in enumerate(paths):
            kind = _magic(p)
            if kind not in ("jpg", "png"):
                raise MediaError(
                    "wrong_type",
                    "第 %d 个文件不是 JPG 或 PNG 图片" % (i + 1))
            fd, tmp = tempfile.mkstemp(suffix="." + kind, prefix="pdfconv-img-")
            os.close(fd)
            shutil.copyfile(p, tmp)
            staged.append(tmp)
        try:
            pages = P.images_to_pdf(staged, dst)
        except Exception:
            raise MediaError("convert_failed", "图片转 PDF 失败，请检查图片是否损坏")
    finally:
        for t in staged:
            try:
                os.remove(t)
            except OSError:
                pass

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise MediaError("convert_failed", "转换没有产生文件，请重试")
    return {"op": "img2pdf", "pages": pages, "images": len(staged)}
