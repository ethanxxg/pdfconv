"""Photo -> clean scan job for the process pool.

Thin service layer over ``pdfconv.scan``: validate uploads, stage them with
real extensions, run the enhancer, then hand the cleaned images to the same
``images_to_pdf`` engine the plain image-to-PDF tool uses.  Output can also be
a zip of the cleaned images when the caller asks for ``fmt: "images"``.

Runs in a worker, so arguments and exceptions must be picklable.
"""
import os
import shutil
import tempfile
import zipfile

from pdfconv import pages as P
from pdfconv import scan as S

MAX_IMAGES = 200
DPI = (150, 200, 300)
MODES = ("color", "gray", "bw")


class ScanError(Exception):
    """A refusal or failure with a code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        return (self.__class__, (self.code, self.message))


def _magic(src):
    try:
        with open(src, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    if head[:3] == b"\xff\xd8\xff":
        return "jpg"
    if head.startswith(b"\x89PNG"):
        return "png"
    return None


def _options(opts):
    o = opts or {}
    mode = str(o.get("mode") or "color").lower()
    if mode not in MODES:
        raise ScanError("bad_mode", "未知的扫描模式")
    try:
        dpi = int(o.get("dpi") or 200)
    except (TypeError, ValueError):
        dpi = 200
    if dpi not in DPI:
        raise ScanError("bad_dpi", "分辨率只能是 150 / 200 / 300")
    fmt = str(o.get("fmt") or "pdf").lower()
    if fmt not in ("pdf", "images"):
        raise ScanError("bad_fmt", "未知的输出格式")
    op = str(o.get("op") or "scan").lower()
    if op not in ("scan", "idcard"):
        raise ScanError("bad_op", "未知的扫描操作")
    clean = {}
    for key in ("crop", "deskew", "flatten", "denoise", "sharpen"):
        v = o.get(key, True)
        if v is None:
            v = True
        clean[key] = v if isinstance(v, bool) else str(v).lower() not in (
            "false", "0", "no", "off")
    clean["mode"] = mode
    clean["op"] = op
    return clean, dpi, fmt


def _idcard_task(paths, dst, options, dpi, fmt):
    """One or two card photos -> one A4 sheet (PDF by default)."""
    if len(paths) > 2:
        raise ScanError("too_many_images", "证件扫描最多 2 张（正面和背面）")
    if fmt == "images":
        raise ScanError("bad_fmt", "证件扫描只输出 PDF")
    tmpdir = tempfile.mkdtemp(prefix="pdfconv-idcard-")
    staged = []
    try:
        for i, src in enumerate(paths):
            kind = _magic(src)
            if kind not in ("jpg", "png"):
                raise ScanError("wrong_type",
                                "第 %d 个文件不是 JPG 或 PNG 图片" % (i + 1))
            fd, inp = tempfile.mkstemp(suffix="." + kind, dir=tmpdir)
            os.close(fd)
            shutil.copyfile(src, inp)
            staged.append(inp)
        try:
            meta = S.idcard_sheet(staged, dst, options, dpi=dpi, fmt="pdf")
        except RuntimeError as e:
            if str(e) == "bad_image":
                raise ScanError("bad_image", "证件照片无法读取，可能已损坏")
            raise
        except Exception:
            raise ScanError("enhance_failed", "证件扫描失败，请重试")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise ScanError("convert_failed", "处理没有产生文件，请重试")
    return {"op": "idcard", "pages": 1, "images": meta["cards"],
            "fmt": "pdf", "dpi": dpi, "mode": options["mode"]}


def scan_task(paths, dst, options=None, max_pages=None):
    """Enhance each image in *paths*, write the result to *dst*.

    Returns {op, pages, images, fmt, dpi, mode}.  ``pages`` is the output PDF
    page count, or the number of cleaned images for a zip.
    """
    if not S.available():
        raise ScanError("unavailable", "扫描增强不可用：缺少 OpenCV")
    clean, dpi, fmt = _options(options)
    limit = int(max_pages or MAX_IMAGES)
    if not paths:
        raise ScanError("empty", "没有图片")
    if len(paths) > limit:
        raise ScanError("too_many_images",
                        "共 %d 张，最多支持 %d 张" % (len(paths), limit))

    if clean["op"] == "idcard":
        return _idcard_task(paths, dst, clean, dpi, fmt)

    tmpdir = tempfile.mkdtemp(prefix="pdfconv-scan-")
    staged, cleaned = [], []
    try:
        for i, src in enumerate(paths):
            kind = _magic(src)
            if kind not in ("jpg", "png"):
                raise ScanError("wrong_type",
                                "第 %d 个文件不是 JPG 或 PNG 图片" % (i + 1))
            fd, inp = tempfile.mkstemp(suffix="." + kind, dir=tmpdir)
            os.close(fd)
            shutil.copyfile(src, inp)
            staged.append(inp)
            out = os.path.join(tmpdir, "c%03d.%s" % (i, kind))
            try:
                S.enhance_file(inp, out, clean, fmt=kind, dpi=dpi)
            except RuntimeError as e:
                if str(e) == "bad_image":
                    raise ScanError("bad_image",
                                    "第 %d 张图片无法读取，可能已损坏" % (i + 1))
                raise
            except Exception:
                raise ScanError("enhance_failed",
                                "第 %d 张图片处理失败" % (i + 1))
            cleaned.append(out)

        if fmt == "pdf":
            try:
                pages = P.images_to_pdf(cleaned, dst)
            except Exception:
                raise ScanError("convert_failed", "生成扫描 PDF 失败，请重试")
        else:
            try:
                with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as z:
                    for i, p in enumerate(cleaned):
                        ext = os.path.splitext(p)[1]
                        z.write(p, "scan-%03d%s" % (i + 1, ext))
                pages = len(cleaned)
            except Exception:
                try:
                    os.remove(dst)
                except OSError:
                    pass
                raise ScanError("convert_failed", "打包图片失败，请重试")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise ScanError("convert_failed", "处理没有产生文件，请重试")
    return {"op": "scan", "pages": pages, "images": len(cleaned),
            "fmt": fmt, "dpi": dpi, "mode": clean["mode"]}
