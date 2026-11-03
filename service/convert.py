"""Office <-> PDF conversion, wrapping the pdfconv engine for the job runner.

Six directions, all served by the same engine:

    pdf  -> docx | xlsx | pptx      (extract)
    docx | xlsx | pptx -> pdf       (render)

This module adds only what a *service* needs and a library does not:

  * refuse work before spending the user's only daily allowance
  * report progress across a process boundary (the worker is a child process)
  * normalise six different engine return shapes into one report dict

OCR is a paid hook, mirroring how smallpdf sells it.  The engine turns OCR on by
itself whenever rapidocr is importable, so "off" is enforced here by patching
`pdfconv.ocr.available` for the duration of the call -- and reset on every call,
because pool worker processes are reused across jobs.
"""
import os
import shutil
import tempfile

from .tools import EXT, get as get_tool

MAX_PAGES = 5000

# A page with fewer than this many characters is treated as having no text
# layer.  Real text pages carry hundreds; the threshold only has to separate
# "scanned image" from "sparse title page".
_SCANNED_CHARS_PER_PAGE = 20
_SCANNED_SAMPLE_PAGES = 3

_orig_available = None


class ConvertError(Exception):
    """A refusal or failure with a stable code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # This class is raised inside a pool *worker process* and re-raised in
        # the web process.  The default Exception pickle is cls(*self.args),
        # which here is cls(message) -- one argument short -- so unpickling
        # raised TypeError and every worker-side failure surfaced as a generic
        # "internal" instead of its real code.
        return (self.__class__, (self.code, self.message))


def _looks_scanned(doc):
    """True when the sampled pages carry essentially no extractable text."""
    sample = min(_SCANNED_SAMPLE_PAGES, doc.page_count)
    chars = 0
    for i in range(sample):
        try:
            chars += len((doc[i].get_text() or "").strip())
        except Exception:
            continue
    return sample > 0 and chars < _SCANNED_CHARS_PER_PAGE * sample


def _set_ocr(enabled):
    """Force the engine's OCR detection on or off for this worker process."""
    global _orig_available
    from pdfconv import ocr as _ocr
    if _orig_available is None:
        _orig_available = _ocr.available
    _ocr.available = (lambda: bool(enabled) and bool(_orig_available()))


def _check_pdf_source(src, limit, use_ocr):
    """Validate a PDF input.  Returns its page count."""
    import fitz

    doc = fitz.open(src)
    try:
        if doc.needs_pass:
            raise ConvertError("encrypted",
                               "这个 PDF 有密码保护，请先解除密码再转换")
        pages = doc.page_count
        if pages == 0:
            raise ConvertError("empty", "这个 PDF 没有任何页面")
        if pages > limit:
            raise ConvertError(
                "too_many_pages",
                "文件共 %d 页，最多支持 %d 页" % (pages, limit))
        # Checking here rather than letting the engine emit an image-only result:
        # a Word/Excel file with no editable text looks like a bug to the user.
        if not use_ocr and _looks_scanned(doc):
            raise ConvertError(
                "needs_ocr",
                "这是扫描版 PDF（没有文字层），需要 OCR 才能转成可编辑的文件")
    finally:
        doc.close()
    return pages


def _check_office_source(src, kind):
    """Cheaply prove the upload really is the format the tool promises.

    The upload endpoint only checks the zip magic, which all three OOXML kinds
    share -- so a .pptx posted to the Word tool would otherwise reach the
    engine and fail with a stack trace.  Opening it with the right library is
    the honest check.
    """
    try:
        if kind == "docx":
            from docx import Document
            Document(src)
        elif kind == "xlsx":
            from openpyxl import load_workbook
            load_workbook(src, read_only=True).close()
        elif kind == "pptx":
            from pptx import Presentation
            Presentation(src)
    except Exception:
        raise ConvertError(
            "bad_file",
            "这个文件不是有效的 %s，或者已经损坏" % kind.upper())


# direction -> (module, function).  Imported lazily so the web process never
# pays for python-docx / openpyxl / python-pptx.
_ENGINE = {
    ("pdf", "docx"): ("pdfconv.convert", "convert"),
    ("pdf", "xlsx"): ("pdfconv.xlsx", "convert_xlsx"),
    ("pdf", "pptx"): ("pdfconv.pptx", "convert_pptx"),
    ("docx", "pdf"): ("pdfconv.docx2pdf", "convert_docx_pdf"),
    ("xlsx", "pdf"): ("pdfconv.xlsx2pdf", "convert_xlsx_pdf"),
    ("pptx", "pdf"): ("pdfconv.pptx2pdf", "convert_pptx_pdf"),
}


def _load_engine(src_kind, dst_kind):
    import importlib
    spec = _ENGINE.get((src_kind, dst_kind))
    if spec is None:
        raise ConvertError("unsupported",
                           "不支持 %s → %s 的转换" % (src_kind, dst_kind))
    mod = importlib.import_module(spec[0])
    return getattr(mod, spec[1])


def _stage(src, kind):
    """Copy the upload to a temp path that carries the real extension.

    Storage keys have no extension (``in/<file_id>``), and openpyxl decides the
    workbook format from the *filename* -- so an .xlsx upload reached it as
    "does not support file format".  python-docx and python-pptx sniff the zip
    instead, but staging all three keeps one code path.
    """
    fd, tmp = tempfile.mkstemp(suffix=EXT[kind], prefix="pdfconv-")
    os.close(fd)
    shutil.copyfile(src, tmp)
    return tmp


def _report(src_kind, dst_kind, result, pages):
    """Normalise the six engine return shapes into one dict.

    They are all tuples of counts, but the meaning differs per direction, and
    the UI wants to say "24 张幻灯片 · 33 张图" rather than print a raw tuple.
    """
    detail = []
    if src_kind == "pdf" and dst_kind == "docx":
        blocks, tables, figures = result
        detail = [("%d 段文字" % blocks)]
        if tables:
            detail.append("%d 个表格" % tables)
        if figures:
            detail.append("%d 张图片" % figures)
        extra = {"blocks": blocks, "tables": tables, "figures": figures}
    elif src_kind == "pdf" and dst_kind == "xlsx":
        sheets, tables = result
        detail = ["%d 个工作表" % sheets, "%d 个表格" % tables]
        extra = {"sheets": sheets, "tables": tables}
    elif src_kind == "pdf" and dst_kind == "pptx":
        slides, pictures, tables = result
        detail = ["%d 张幻灯片" % slides]
        if pictures:
            detail.append("%d 张图" % pictures)
        if tables:
            detail.append("%d 个表格" % tables)
        extra = {"slides": slides, "pictures": pictures, "tables": tables}
    elif dst_kind == "pdf" and src_kind == "docx":
        _, paras, tables = result
        detail = ["%d 段文字" % paras]
        if tables:
            detail.append("%d 个表格" % tables)
        extra = {"paragraphs": paras, "tables": tables}
    elif dst_kind == "pdf" and src_kind == "xlsx":
        _, sheets, cells = result
        detail = ["%d 个工作表" % sheets, "%d 个单元格" % cells]
        extra = {"sheets": sheets, "cells": cells}
    elif dst_kind == "pdf" and src_kind == "pptx":
        _, shapes, tables = result
        detail = ["%d 个形状" % shapes]
        if tables:
            detail.append("%d 个表格" % tables)
        extra = {"shapes": shapes, "tables": tables}
    else:
        extra = {}
    return {"pages": pages, "detail": detail, "stats": extra}


def convert_task(src, dst, tool_id, max_pages=None, use_ocr=False,
                 progress_path=None):
    """Convert one file.  Runs inside a pool worker process.

    Raises ConvertError for anything the caller should surface as a 4xx.
    """
    tool = get_tool(tool_id)
    if tool is None:
        raise ConvertError("bad_tool", "未知的转换类型")
    src_kind, dst_kind = tool["src"], tool["dst"]
    limit = int(max_pages or MAX_PAGES)
    before = os.path.getsize(src)

    staged = _stage(src, src_kind) if src_kind != "pdf" else None
    work_src = staged or src

    try:
        if src_kind == "pdf":
            pages = _check_pdf_source(work_src, limit, use_ocr)
        else:
            _check_office_source(work_src, src_kind)
            pages = 0        # only known after rendering

        _set_ocr(use_ocr)

        engine = _load_engine(src_kind, dst_kind)

        def progress(page_no, page_total):
            if not progress_path:
                return
            try:
                # Atomic enough for a counter the UI polls every 250 ms: a torn
                # read would show a stale page number, never a wrong one.
                with open(progress_path, "w") as f:
                    f.write("%d/%d" % (page_no, page_total))
            except OSError:
                pass

        try:
            if (src_kind, dst_kind) == ("pdf", "docx"):
                result = engine(work_src, dst, progress_cb=progress)
            else:
                # Only the PDF->DOCX path reports progress; the rest are single
                # blocking calls, so the UI shows an indeterminate bar instead
                # of a percentage invented here.
                result = engine(work_src, dst)
        except ConvertError:
            raise
        except Exception as e:
            import traceback
            traceback.print_exc()
            raise ConvertError("convert_failed",
                               "转换失败，这个文件可能已损坏（%s）"
                               % type(e).__name__)
    finally:
        if staged:
            try:
                os.remove(staged)
            except OSError:
                pass
        if progress_path:
            try:
                os.remove(progress_path)
            except OSError:
                pass

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise ConvertError("convert_failed", "转换没有产生文件，请重试")

    if dst_kind == "pdf":
        # Office -> PDF has no page count to check before the work, so the
        # ceiling is applied to the result.  The user's allowance is already
        # spent by now, which is why the limit is a hard ceiling rather than
        # the free tier's small number -- see README.
        import fitz
        with fitz.open(dst) as d:
            pages = d.page_count
        if pages > limit:
            try:
                os.remove(dst)
            except OSError:
                pass
            raise ConvertError(
                "too_many_pages",
                "转换结果是 %d 页，超过 %d 页上限" % (pages, limit))
    elif dst_kind == "xlsx":
        # The engine's first value is the number of table sheets it wrote, not
        # the workbook's sheet count -- it also adds an "Overview"/"Text" sheet.
        # Reporting the engine's number next to a two-sheet file reads as a bug,
        # so the real count is taken from the file that was just produced.
        try:
            from openpyxl import load_workbook
            wb = load_workbook(dst, read_only=True)
            result = (len(wb.sheetnames), result[1])
            wb.close()
        except Exception:
            pass

    rep = _report(src_kind, dst_kind, result, pages)
    rep.update({"before": before, "after": os.path.getsize(dst),
                "ocr": bool(use_ocr), "tool": tool_id,
                "src": src_kind, "dst": dst_kind})
    return rep
