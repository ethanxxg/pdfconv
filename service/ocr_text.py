"""Standalone image-to-text job (the OCR primitive exposed as a tool).

Unlike the convert path, which uses OCR only as a fallback for pages with no
text layer, this tool runs OCR on every uploaded image and returns the plain
text.  Output is a .txt for one image or a .zip of .txt files for several;
``fmt: "docx"`` bundles the lines into a Word document instead.
"""
import io
import os
import shutil
import tempfile
import zipfile

from pdfconv import ocr
from pdfconv import scan as S

MAX_IMAGES = 100


class OcrTextError(Exception):
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


def ocr_text_task(paths, dst, options=None, max_pages=None):
    """OCR each image in *paths* and write text to *dst*."""
    o = options or {}
    fmt = str(o.get("fmt") or "txt").lower()
    if fmt not in ("txt", "docx"):
        raise OcrTextError("bad_fmt", "输出格式只能是 TXT 或 Word")
    if not ocr.available():
        raise OcrTextError("unavailable", "文字识别不可用：缺少 OCR 引擎")
    limit = int(max_pages or MAX_IMAGES)
    if not paths:
        raise OcrTextError("empty", "没有图片")
    if len(paths) > limit:
        raise OcrTextError("too_many_images",
                           "共 %d 张，最多支持 %d 张" % (len(paths), limit))

    tmpdir = tempfile.mkdtemp(prefix="pdfconv-ocrtext-")
    results = []
    try:
        for i, src in enumerate(paths):
            kind = _magic(src)
            if kind not in ("jpg", "png"):
                raise OcrTextError("wrong_type",
                                   "第 %d 个文件不是 JPG 或 PNG 图片" % (i + 1))
            fd, inp = tempfile.mkstemp(suffix="." + kind, dir=tmpdir)
            os.close(fd)
            shutil.copyfile(src, inp)
            try:
                r = ocr.image_text(inp)
            except RuntimeError as e:
                if str(e) == "bad_image":
                    raise OcrTextError("bad_image",
                                       "第 %d 张图片无法读取" % (i + 1))
                raise
            except Exception:
                raise OcrTextError("ocr_failed",
                                   "第 %d 张图片识别失败" % (i + 1))
            results.append((os.path.basename(src) or ("image-%d" % (i + 1)), r))

        if all(not r["text"] for _n, r in results):
            raise OcrTextError("no_text", "没有从图片中识别出文字")

        if fmt == "docx":
            import docx as _docx
            d = _docx.Document()
            for idx, (name, r) in enumerate(results):
                if len(results) > 1:
                    d.add_heading(name, level=2)
                for line in r["text"].splitlines():
                    if line.strip():
                        d.add_paragraph(line)
            if len(results) > 1:
                d.save(dst)
            else:
                d.save(dst)
        elif len(results) == 1:
            with open(dst, "w", encoding="utf-8") as f:
                f.write(results[0][1]["text"])
        else:
            with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
                used = set()
                for name, r in results:
                    stem = os.path.splitext(name)[0] or "image"
                    fn = stem + ".txt"
                    n = 2
                    while fn in used:
                        fn = "%s-%d.txt" % (stem, n)
                        n += 1
                    used.add(fn)
                    z.writestr(fn, r["text"])
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise OcrTextError("convert_failed", "识别没有产生文件，请重试")
    return {"op": "image-to-text", "fmt": fmt, "images": len(results),
            "lines": sum(r["lines"] for _n, r in results),
            "boxes": sum(r["boxes"] for _n, r in results)}
