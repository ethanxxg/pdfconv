"""Merge worker body.

This runs inside a process pool, so everything here must be importable with no
side effects and take only picklable arguments.  It deliberately does *not*
reuse pdfconv.pages.merge_pdfs(): that helper saves with ``garbage=4,
clean=True``, which is right for a desktop tool chasing the smallest file but
measured 8.4x slower than ``garbage=1`` on this corpus (2.404 s vs 0.285 s for
10 MB) while producing output only 0.2% larger.  A server cannot afford that.
"""
import os

import pymupdf as fitz

# hard ceilings, enforced again here because the API layer is not the only
# caller (a retry from the queue can arrive with stale metadata)
MAX_PAGES = 5000
MAX_OUT_BYTES = 400 * 1024 * 1024


class MergeError(Exception):
    """A failure we can explain to the user, as opposed to a crash."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; see the note in
        # convert.py -- without this the code is lost to a TypeError.
        return (self.__class__, (self.code, self.message))


def merge_task(paths, out_path, rotations=None, max_pages=None,
               expect_pages=None):
    """Concatenate *paths* in order into *out_path*.  Returns page count.

    *rotations* is a parallel list of degrees (0/90/180/270) applied to every
    page of the matching source before it is copied in.  The rotation is written
    into the page's /Rotate entry rather than by transforming its content
    stream: /Rotate is what every viewer honours, it costs nothing, and it keeps
    the original geometry intact so a later crop or text extraction still works.

    *max_pages* is the plan's page allowance, checked as pages are appended so a
    2000-page document is refused after the first source rather than after all
    of them have been copied.  MAX_PAGES stays as the absolute safety ceiling.
    """
    limit = int(max_pages or MAX_PAGES)
    out = fitz.open()
    try:
        for i, p in enumerate(paths):
            deg = 0
            if rotations:
                try:
                    deg = int(rotations[i]) % 360
                except (TypeError, ValueError, IndexError):
                    deg = 0
            if deg % 90:
                raise MergeError("bad_rotation", "旋转角度只能是 90 的整数倍")
            try:
                src = fitz.open(p)
            except Exception:
                raise MergeError("bad_pdf",
                                 "第 %d 个文件不是有效的 PDF" % (i + 1))
            try:
                if src.needs_pass:
                    raise MergeError("encrypted",
                                     "第 %d 个文件已加密，请先解除密码"
                                     % (i + 1))
                if src.page_count == 0:
                    raise MergeError("empty",
                                     "第 %d 个文件没有页面" % (i + 1))
                if deg:
                    # relative to whatever rotation the source already had, so
                    # a page scanned sideways and then rotated once in the UI
                    # lands upright instead of being forced to 90
                    for page in src:
                        page.set_rotation((page.rotation + deg) % 360)
                out.insert_pdf(src)
            finally:
                src.close()
            if len(out) > limit:
                raise MergeError(
                    "too_many_pages",
                    "合并结果共 %d 页，最多支持 %d 页" % (len(out), limit))

        out.save(out_path, garbage=1, deflate=True)
        n = len(out)
    finally:
        out.close()

    if os.path.getsize(out_path) > MAX_OUT_BYTES:
        os.remove(out_path)
        raise MergeError("too_large", "合并结果超过 400 MB")
    return n
