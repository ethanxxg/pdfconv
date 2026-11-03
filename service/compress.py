"""Compression worker body.

Runs inside the process pool, so it must be importable with no side effects and
take only picklable arguments.

Two levers, and they apply to different documents:

* ``rewrite_images`` resamples and re-encodes image XObjects.  This is the whole
  story for scans and photo-heavy files -- measured 5.77 MB -> 0.39 MB (7%) on a
  94%-image paper -- and completely inert on a born-digital paper whose 3% is
  already a compressed logo.
* ``subset_fonts`` + ``garbage=4, clean=True`` strip unused glyphs and dead
  objects.  That is the only thing that helps a text-only file, and it is worth
  a few percent at most.

Measured on this corpus, image share predicts the result far better than file
size does: 3% image -> 98% of original, 94% image -> 7%.  So the honest outcome
for an already-optimised file is "no gain", and the API says so instead of
handing back a file that is a hair larger and calling it a win.
"""
import os
import shutil

import pymupdf as fitz

try:
    from PIL import Image
    _HAVE_PIL = True
except Exception:  # pragma: no cover - optional dependency
    _HAVE_PIL = False

MAX_PAGES = 5000


def _peek_magic(src):
    """pdf / jpg / png, or None -- read from the bytes, not the name.

    Uploads are stored under ``in/<file_id>`` with no extension, so dispatching
    on the filename would send a JPEG down the PyMuPDF path.  The first bytes
    are unambiguous.
    """
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

# name -> (dpi target, jpeg quality).  This MuPDF build requires
# dpi_threshold > dpi_target, hence the +1 at the call site.
LEVELS = {
    "extreme":     (72, 45),
    "recommended": (110, 70),
    "less":        (200, 85),
}
DEFAULT_LEVEL = "recommended"


class CompressError(Exception):
    """A failure we can explain to the user, as opposed to a crash."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; see the note in
        # convert.py -- without this the code is lost to a TypeError.
        return (self.__class__, (self.code, self.message))


def compress_task(src, dst, level=DEFAULT_LEVEL, max_pages=None):
    """Compress *src* into *dst*.  Returns {pages, before, after, level}.

    If the result is not smaller, the original is copied to *dst* instead.  A
    "compressor" that returns a bigger file is worse than useless: the user
    loses quality and gains bytes, and either way it ends in a support ticket.

    Source may be a PDF or a JPEG/PNG image; the right engine is chosen by the
    file extension.
    """
    if level not in LEVELS:
        level = DEFAULT_LEVEL

    magic = _peek_magic(src)
    if magic in ("jpg", "png"):
        if not _HAVE_PIL:
            raise CompressError("unsupported", "图片压缩依赖未安装")
        return _compress_image(src, dst, level, os.path.getsize(src))

    dpi, quality = LEVELS[level]
    limit = int(max_pages or MAX_PAGES)

    before = os.path.getsize(src)
    try:
        doc = fitz.open(src)
    except Exception:
        raise CompressError("bad_pdf", "不是有效的 PDF 文件")

    try:
        if doc.needs_pass:
            raise CompressError("encrypted", "文件已加密，请先解除密码")
        if doc.page_count == 0:
            raise CompressError("empty", "文件没有页面")
        if doc.page_count > limit:
            raise CompressError(
                "too_many_pages",
                "文件共 %d 页，最多支持 %d 页" % (doc.page_count, limit))
        pages = doc.page_count
        try:
            doc.rewrite_images(dpi_threshold=dpi + 1, dpi_target=dpi,
                               quality=quality, lossy=True)
            try:
                # an optimisation, not a requirement: a PDF with an exotic font
                # program must still compress through the image path
                doc.subset_fonts()
            except Exception:
                pass
            doc.save(dst, garbage=4, clean=True, deflate=True,
                     deflate_images=True, deflate_fonts=True)
        except CompressError:
            raise
        except Exception:
            raise CompressError("compress_failed",
                                "这个 PDF 的结构无法压缩，请换一档再试")
    finally:
        doc.close()

    after = os.path.getsize(dst)
    if after >= before:
        shutil.copyfile(src, dst)
        after = before
    return {"pages": pages, "before": before, "after": after, "level": level}


# JPEG quality per level.  PNG is lossless, so its only lever is optimisation;
# the "extreme" tier additionally quantises to 256 colours, which collapses a
# screenshot's millions of near-identical pixels into something tiny.
_IMAGE_QUALITY = {"extreme": 42, "recommended": 72, "less": 88}


def _compress_image(src, dst, level, before):
    """Re-encode a JPEG/PNG so it is smaller, or hand back the original.

    Mirrors the PDF rule: a "compressor" that returns a *larger* file is worse
    than useless, so when re-encoding does not win we copy the source through
    and report it unchanged.
    """
    quality = _IMAGE_QUALITY.get(level, 72)
    with Image.open(src) as im:
        fmt = (im.format or "").upper()
        exif = None
        try:
            exif = im.info.get("exif")
        except Exception:
            exif = None

        if fmt == "JPEG":
            # Pillow keeps a JPEG as "L"/"CMYK" sometimes; RGB is the safe,
            # universally-openable target.
            rgb = im.convert("RGB")
            kwargs = {"format": "JPEG", "quality": quality,
                      "optimize": True, "progressive": True}
            if exif:
                kwargs["exif"] = exif
            rgb.save(dst, **kwargs)
        elif fmt == "PNG":
            im.save(dst, "PNG", optimize=True)
            if level == "extreme":
                # Keep the palette version only when it actually beats the
                # truecolour one; otherwise the screenshot stays crisp.
                try:
                    pal = im.convert("P", palette=Image.ADAPTIVE, colors=256)
                    tmp = dst + ".p"
                    pal.save(tmp, "PNG", optimize=True)
                    if os.path.getsize(tmp) < os.path.getsize(dst):
                        os.replace(tmp, dst)
                    else:
                        os.remove(tmp)
                except Exception:
                    pass
        else:
            # Any other flavour (BMP, TIFF, ...): don't corrupt it, just keep.
            shutil.copyfile(src, dst)

    after = os.path.getsize(dst)
    if after >= before:
        shutil.copyfile(src, dst)
        after = before
    return {"pages": 0, "before": before, "after": after,
            "level": level, "media": "image"}
