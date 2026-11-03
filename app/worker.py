"""Background conversion worker for the DocForge desktop app.

Runs a pdfconv conversion on a batch of files inside a QThread so the UI never
blocks.  The direction (PDF -> Office, or Office -> PDF) is decided by the
(source extension, output extension) pair, so the same worker handles all
six pipelines.  The page-level progress_cb supplied to the PDF->Office
converters is forwarded to Qt signals; Office->PDF runs file-level (no page
hook) and the progress bar simply completes per file.
"""

import os
import traceback

from PyQt6.QtCore import QThread, pyqtSignal

import pdfconv

# (src, dst) -> (converter, status_formatter, page_progress_supported)
_DISPATCH = {
    ("pdf", "docx"): (pdfconv.convert,
                      lambda r: "完成  ·  %d 块 / %d 表 / %d 图" % r, True),
    ("pdf", "xlsx"): (pdfconv.convert_xlsx,
                      lambda r: "完成  ·  %d 工作表 / %d 表" % r, True),
    ("pdf", "pptx"): (pdfconv.convert_pptx,
                      lambda r: "完成  ·  %d 幻灯片 / %d 图 / %d 表" % r, True),
    ("docx", "pdf"): (pdfconv.convert_docx_pdf,
                      lambda r: "完成  ·  %d 页 / %d 段 / %d 表" % r, False),
    ("xlsx", "pdf"): (pdfconv.convert_xlsx_pdf,
                      lambda r: "完成  ·  %d 页 / %d 工作表 / %d 单元格" % r, False),
    ("pptx", "pdf"): (pdfconv.convert_pptx_pdf,
                      lambda r: "完成  ·  %d 页 / %d 形状 / %d 表" % r, False),
}

_EXT_KEY = {".docx": "docx", ".xlsx": "xlsx", ".pptx": "pptx", ".pdf": "pdf"}


class ConversionWorker(QThread):
    # value, maximum  (overall percentage, 0..100)
    sig_progress = pyqtSignal(int, int)
    # queue_index, ok(bool), message(str)
    sig_file_status = pyqtSignal(int, bool, str)
    # human-readable log line
    sig_log = pyqtSignal(str)
    # finished (all jobs processed)
    sig_done = pyqtSignal()

    def __init__(self, jobs):
        """jobs: list of (queue_index, src_path, out_path).

        The (src_ext, out_ext) pair selects the converter at run time.
        """
        super().__init__()
        self.jobs = list(jobs)

    def run(self):
        total = len(self.jobs)
        if total == 0:
            self.sig_done.emit()
            return
        for i, (idx, src_path, out_path) in enumerate(self.jobs):
            name = os.path.basename(src_path)
            self.sig_log.emit("转换中: %s" % name)
            src_key = _EXT_KEY.get(os.path.splitext(src_path)[1].lower())
            dst_key = _EXT_KEY.get(os.path.splitext(out_path)[1].lower())
            entry = _DISPATCH.get((src_key, dst_key))
            if entry is None:
                self.sig_file_status.emit(
                    idx, False, "跳过：不支持 %s -> %s" % (src_key, dst_key))
                self.sig_progress.emit(int((i + 1) / total * 100), 100)
                continue
            converter, fmt_msg, page_progress = entry
            try:
                out_dir = os.path.dirname(out_path)
                if out_dir and not os.path.isdir(out_dir):
                    os.makedirs(out_dir, exist_ok=True)

                if page_progress:
                    def _cb(pno, n):
                        if n and n > 0:
                            frac = (i + pno / float(n)) / total
                            self.sig_progress.emit(int(frac * 100), 100)

                    result = converter(src_path, out_path, progress_cb=_cb)
                else:
                    result = converter(src_path, out_path)
                self.sig_file_status.emit(idx, True, fmt_msg(result))
            except Exception as exc:  # noqa: BLE001 - surface any failure to UI
                self.sig_file_status.emit(idx, False, "失败: %s" % exc)
                self.sig_log.emit("错误: %s\n%s" % (exc, traceback.format_exc()))
            self.sig_progress.emit(int((i + 1) / total * 100), 100)
        self.sig_log.emit("全部完成")
        self.sig_done.emit()
