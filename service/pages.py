"""Page-level PDF operations for the job runner.

Four operations, one engine (``pdfconv.pages``):

    split    -> split into several PDFs, returned as a single .zip
    extract  -> keep only the named pages, one PDF out
    delete   -> drop the named pages, one PDF out
    rotate   -> rotate the named (or all) pages, one PDF out

Like the other workers this runs inside a process pool, so it must be
importable with no side effects and take only picklable arguments.  The
service-specific concerns it adds are: refuse work before spending the
user's daily allowance, and turn the engine's varied return values (a page
count, or a list of written files) into one report the UI can render.
"""
import os
import shutil
import zipfile

import pymupdf as fitz
from pdfconv import pages as P

MAX_PAGES = 5000


class PagesError(Exception):
    """A refusal or failure with a stable code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; the default
        # Exception pickle is cls(*self.args), which here is cls(message) --
        # one argument short -- so without this the code is lost to TypeError.
        return (self.__class__, (self.code, self.message))


def _check_pdf(src, limit):
    """Validate the single PDF input.  Returns its page count."""
    doc = fitz.open(src)
    try:
        if doc.needs_pass:
            raise PagesError("encrypted", "文件已加密，请先解除密码")
        pages = doc.page_count
        if pages == 0:
            raise PagesError("empty", "文件没有任何页面")
        if pages > limit:
            raise PagesError(
                "too_many_pages",
                "文件共 %d 页，最多支持 %d 页" % (pages, limit))
    finally:
        doc.close()
    return pages


def _parse(spec, total):
    """Resolve a '1,3,5-7' spec to a set of valid pages, or bad_pages.

    Reuses the engine's parser so the worker and the CLI agree on what a spec
    means, but wraps its ValueError so malformed input is a clean 4xx.
    """
    try:
        return P.parse_pages(spec, total)
    except Exception:
        raise PagesError("bad_pages", "页码格式不正确，例如 1,3,5-7")


def _zip(paths, dst):
    """Pack *paths* into a zip at *dst* (arcname = basename)."""
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        for p in paths:
            z.write(p, os.path.basename(p))


def pages_task(src, dst, op, max_pages=None, pages=None, angle=None,
               ranges=None, every=None):
    """Run one page operation.  Returns a report dict.

    ``op`` must be one of split / extract / delete / rotate.  The remaining
    arguments are per-op and may be None when not applicable.  Raises
    PagesError for anything the caller should surface as a 4xx.
    """
    if op not in ("split", "extract", "delete", "rotate"):
        raise PagesError("bad_op", "未知操作")
    limit = int(max_pages or MAX_PAGES)
    total = _check_pdf(src, limit)

    if op == "split":
        return _split(src, dst, total, ranges, every)

    # The three single-output operations all take a page spec (rotate may
    # omit it, meaning "all pages").  The spec is resolved against the real
    # page count *before* calling the engine: pdfconv's extract/remove save a
    # document even when the selection is empty, and MuPDF refuses to write a
    # zero-page PDF -- so an out-of-range spec would surface as an opaque
    # "internal" instead of a clean bad_pages.
    if op == "rotate":
        try:
            ang = int(angle or 0)
        except (TypeError, ValueError):
            ang = 0
        if ang not in (90, 180, 270):
            raise PagesError("bad_angle", "旋转角度只能是 90 / 180 / 270")
        spec = (pages or "").strip()
        if spec and not _parse(spec, total):
            raise PagesError("bad_pages", "没有有效页码（1–%d）" % total)
        kept = P.rotate_pages(src, dst, ang, spec or None)
        return {"op": op, "pages": kept, "total": total, "angle": ang}

    # extract / delete
    spec = (pages or "").strip()
    if not spec:
        raise PagesError("bad_pages", "请填写页码，例如 1,3,5-7")
    sel = _parse(spec, total)
    if not sel:
        raise PagesError("bad_pages", "没有有效页码（1–%d）" % total)
    if op == "extract":
        kept = P.extract_pages(src, dst, spec)
        return {"op": op, "pages": kept, "total": total}
    else:  # delete
        if len(sel) >= total:
            raise PagesError("bad_pages", "不能删除全部页面，至少保留 1 页")
        kept = P.remove_pages(src, dst, spec)
        return {"op": op, "pages": kept, "total": total}


def _split(src, dst, total, ranges, every):
    """Split *src* into pieces, zip them into *dst*, return a report."""
    out_dir = dst + ".parts"
    os.makedirs(out_dir, exist_ok=True)
    try:
        ranges = (ranges or "").strip()
        every = every if isinstance(every, int) else None
        try:
            every = int(every) if every else 0
        except (TypeError, ValueError):
            every = 0
        if ranges:
            segs = []
            for part in ranges.replace("，", ",").split(","):
                part = part.strip()
                if not part:
                    continue
                if "-" in part:
                    a, b = part.split("-", 1)
                    try:
                        a = int(a)
                        b = int(b) if b.strip() else total
                    except ValueError:
                        raise PagesError("bad_pages", "拆分区间格式不正确（应为 1-3 这类）")
                    if a > b:
                        a, b = b, a
                    if a < 1 or b > total:
                        raise PagesError("bad_pages", "拆分区间超出文件页数")
                    segs.append((a, b))
                else:
                    try:
                        i = int(part)
                    except ValueError:
                        raise PagesError("bad_pages", "拆分区间格式不正确")
                    if i < 1 or i > total:
                        raise PagesError("bad_pages", "拆分区间超出文件页数")
                    segs.append((i, i))
            if not segs:
                raise PagesError("bad_pages", "请填写拆分区间，例如 1-3,5-7")
            outs = P.split_pdf(src, out_dir, ranges=segs)
        elif every and every > 1:
            if every > total:
                raise PagesError("bad_pages", "「每 N 页」不能大于总页数")
            outs = P.split_pdf(src, out_dir, every=every)
        else:
            outs = P.split_pdf(src, out_dir, every=1)
    except PagesError:
        raise
    except Exception:
        raise PagesError("split_failed", "拆分失败，请检查页码区间")

    outs = [p for p in outs if p and os.path.exists(p)]
    if not outs:
        raise PagesError("split_failed", "拆分没有产生任何文件")
    try:
        _zip(outs, dst)
    finally:
        for p in outs:
            try:
                os.remove(p)
            except OSError:
                pass
        shutil.rmtree(out_dir, ignore_errors=True)
    return {"op": "split", "files": len(outs), "pages": total}
