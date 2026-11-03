"""DocForge -- desktop front-end for the pdfconv conversion core.

A PyQt6 single-window app: drag/queue PDFs on the left, pick an output format
(Word / Excel / PowerPoint) and output folder on the right, watch a real
progress bar at the bottom, then open the results.  All heavy lifting is done
by the existing pdfconv package, run off the GUI thread.
"""

import os
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from PyQt6.QtCore import Qt, QSize, QUrl
from PyQt6.QtGui import QColor, QDesktopServices, QDragEnterEvent, QDropEvent
from PyQt6.QtWidgets import (
    QApplication, QButtonGroup, QCheckBox, QFileDialog, QFrame, QHBoxLayout,
    QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox,
    QPushButton, QProgressBar, QSizePolicy, QStatusBar, QVBoxLayout, QWidget,
)

import pdfconv
from guards import slot as _slot
from guards import write_crash_log as _write_crash_log
from worker import ConversionWorker
from pagetools import PageToolsDialog

FMT_EXT = {"docx": ".docx", "xlsx": ".xlsx", "pptx": ".pptx", "pdf": ".pdf"}
FMT_LABEL = {"docx": "Word 文档", "xlsx": "Excel 表格",
             "pptx": "PPT 幻灯片", "pdf": "PDF"}



def _lbl(text, css=None):
    w = QLabel(text)
    if css:
        w.setStyleSheet(css)
    return w


def _human_size(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%.1f MB" % (n / (1024.0 * 1024.0))


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("DocForge 文档工坊  ·  格式转换")
        self.resize(980, 640)
        self.setAcceptDrops(True)

        self.model = []          # list of dicts: path, name, size, status, out
        self.fmt = "docx"
        self.worker = None

        self._build_menu()
        self._build_central()
        self._build_footer()
        self.setStatusBar(QStatusBar())

        self._set_status("就绪 · 拖入 PDF 或点击「添加 PDF」")

    # ----------------------------------------------------------------- UI
    def _build_menu(self):
        mb = self.menuBar()
        f = mb.addMenu("文件")
        f.addAction("添加文件...", self._add_files_dialog)
        f.addAction("添加文件夹...", self._add_folder_dialog)
        f.addAction("设置输出目录...", self._pick_output)
        f.addSeparator()
        f.addAction("退出", self.close)
        t = mb.addMenu("工具")
        t.addAction("页面工具（合并/拆分/旋转/裁剪/水印…）...", self._open_pagetools)
        h = mb.addMenu("帮助")
        h.addAction("关于", self._about)

    def _build_central(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(14, 14, 14, 8)

        split = QHBoxLayout()
        split.setSpacing(14)
        root.addLayout(split, stretch=1)

        # ---- left: queue ----
        left = QVBoxLayout()
        left.setSpacing(8)
        left.addWidget(_lbl("文件队列", "font-weight:500;font-size:14px;"))
        add_row = QHBoxLayout()
        add_row.setSpacing(8)
        self.add_btn = QPushButton("添加文件")
        self.add_btn.clicked.connect(self._add_files_dialog)
        self.add_folder_btn = QPushButton("添加文件夹")
        self.add_folder_btn.clicked.connect(self._add_folder_dialog)
        add_row.addWidget(self.add_btn)
        add_row.addWidget(self.add_folder_btn)
        left.addLayout(add_row)
        self.queue = QListWidget()
        self.queue.setAlternatingRowColors(True)
        self.queue.setSelectionMode(QListWidget.SelectionMode.ExtendedSelection)
        left.addWidget(self.queue, stretch=1)
        self.hint = _lbl("支持拖拽 PDF 文件 / 整个文件夹",
                        "color:#888;font-size:12px;")
        left.addWidget(self.hint)
        split.addLayout(left, stretch=2)

        # ---- right: options ----
        right = QVBoxLayout()
        right.setSpacing(12)
        right.addWidget(_lbl("输出格式", "font-weight:500;font-size:14px;"))

        seg = QWidget()
        seg_layout = QHBoxLayout(seg)
        seg_layout.setContentsMargins(0, 0, 0, 0)
        seg_layout.setSpacing(0)
        self.fmt_group = QButtonGroup(self)
        self.fmt_group.setExclusive(True)
        for key in ("docx", "xlsx", "pptx", "pdf"):
            b = QPushButton(FMT_LABEL[key])
            b.setCheckable(True)
            b.setMinimumHeight(34)
            b.setProperty("fmt", key)
            self.fmt_group.addButton(b)
            seg_layout.addWidget(b)
            if key == self.fmt:
                b.setChecked(True)
        self.fmt_group.buttonClicked.connect(self._on_fmt)
        seg.setStyleSheet(
            "QPushButton{border:1px solid #c9c9c9;padding:6px 10px;"
            "background:#fff;color:#333;}"
            "QPushButton:checked{background:#185FA5;color:#fff;border:1px solid #185FA5;}"
        )
        right.addWidget(seg)

        # output settings
        box = QFrame()
        box.setFrameStyle(QFrame.Shape.StyledPanel)
        box_layout = QVBoxLayout(box)
        box_layout.setSpacing(10)
        box_layout.addWidget(_lbl("输出设置", "font-weight:500;"))

        row = QHBoxLayout()
        row.setSpacing(8)
        row.addWidget(QLabel("输出目录"))
        self.out_edit = QLineEdit()
        self.out_edit.setPlaceholderText("默认与源文件同目录")
        row.addWidget(self.out_edit, stretch=1)
        self.browse_btn = QPushButton("浏览")
        self.browse_btn.clicked.connect(self._pick_output)
        row.addWidget(self.browse_btn)
        self.open_out_btn = QPushButton("打开")
        self.open_out_btn.setToolTip("在资源管理器中打开输出目录")
        self.open_out_btn.clicked.connect(self._open_out_dir)
        row.addWidget(self.open_out_btn)
        box_layout.addLayout(row)

        self.open_folder = QCheckBox("完成后打开输出文件夹")
        self.open_folder.setChecked(True)      # answer the "where did it go?" question
        box_layout.addWidget(self.open_folder)
        right.addWidget(box)

        self.tools_btn = QPushButton("页面工具（合并 / 拆分 / 旋转 / 裁剪 / 水印 / 页码…）")
        self.tools_btn.setMinimumHeight(34)
        self.tools_btn.clicked.connect(self._open_pagetools)
        right.addWidget(self.tools_btn)

        right.addWidget(_lbl(
            "翻译、原位/双语落地、扫描件 OCR 将在后续版本加入。",
            "color:#888;font-size:12px;"))
        right.addStretch(1)
        split.addLayout(right, stretch=3)

    def _build_footer(self):
        foot = QWidget()
        fl = QHBoxLayout(foot)
        fl.setContentsMargins(0, 0, 0, 0)
        fl.setSpacing(12)
        self.progress = QProgressBar()
        self.progress.setValue(0)
        self.progress.setTextVisible(True)
        fl.addWidget(self.progress, stretch=1)
        self.run_btn = QPushButton("开始转换")
        self.run_btn.setMinimumWidth(110)
        self.run_btn.setStyleSheet(
            "QPushButton{background:#185FA5;color:#fff;padding:8px 14px;"
            "border:none;border-radius:6px;font-weight:500;}"
            "QPushButton:disabled{background:#9bb8d6;}")
        self.run_btn.clicked.connect(self._start)
        self.clear_btn = QPushButton("全部清除")
        self.clear_btn.setMinimumWidth(90)
        self.clear_btn.clicked.connect(self._clear)
        fl.addWidget(self.clear_btn)
        fl.addWidget(self.run_btn)

        central = self.centralWidget()
        central.layout().addWidget(foot)

    # ----------------------------------------------------------------- fmt
    @_slot
    def _on_fmt(self, btn):
        self.fmt = btn.property("fmt")

    # ----------------------------------------------------------------- queue
    def _accepted_suffixes(self):
        """Inputs that make sense for the currently selected output format.

        Output = PDF  -> we need Office documents as input.
        Output = Word/Excel/PPT -> we need PDFs as input.
        """
        return (".docx", ".xlsx", ".pptx") if self.fmt == "pdf" else (".pdf",)

    @_slot
    def _add_files(self, paths):
        suffixes = self._accepted_suffixes()
        added = 0
        for p in paths:
            if os.path.splitext(p)[1].lower() not in suffixes:
                continue
            if any(d["path"] == p for d in self.model):
                continue
            try:
                size = os.path.getsize(p)
            except OSError:
                size = 0
            self.model.append({
                "path": p, "name": os.path.basename(p),
                "size": _human_size(size), "status": "待处理", "out": None,
            })
            added += 1
        if added and not self.out_edit.text().strip():
            self.out_edit.setText(os.path.dirname(self.model[0]["path"]))
        self._render_all()

    @_slot
    def _add_files_dialog(self):
        if self.fmt == "pdf":
            filt = "Office 文档 (*.docx *.xlsx *.pptx)"
        else:
            filt = "PDF 文件 (*.pdf)"
        files, _ = QFileDialog.getOpenFileNames(self, "选择输入文件", "", filt)
        if files:
            self._add_files(files)

    @_slot
    def _add_folder_dialog(self):
        d = QFileDialog.getExistingDirectory(
            self, "选择文件夹", self.out_edit.text() or "")
        if not d:
            return
        suffixes = self._accepted_suffixes()
        files = [str(p) for p in Path(d).rglob("*")
                 if p.is_file() and p.suffix.lower() in suffixes]
        files.sort()
        if not files:
            QMessageBox.information(
                self, "提示",
                "该文件夹及子目录下没有找到 %s 文件。"
                % " / ".join(s.lstrip(".").upper() for s in suffixes))
            return
        was_empty = not self.out_edit.text().strip()
        self._add_files(files)
        if was_empty:
            self.out_edit.setText(d)

    def _render_all(self):
        self.queue.clear()
        for i, d in enumerate(self.model):
            item = QListWidgetItem(self._text(d))
            item.setData(Qt.ItemDataRole.UserRole, i)
            item.setToolTip(d.get("out") or d["path"])
            self.queue.addItem(item)
            self._color(i, d["status"])

    def _text(self, d):
        return "%s\n%s   ·   %s" % (d["name"], d["size"], d["status"])

    def _color(self, idx, status):
        item = self.queue.item(idx)
        if item is None:
            return
        if status.startswith("完成"):
            item.setForeground(QColor("#3B6D11"))
        elif status.startswith("失败"):
            item.setForeground(QColor("#A32D2D"))
        else:
            item.setForeground(QColor("#333333"))

    # ----------------------------------------------------------------- run
    @_slot
    def _start(self):
        if self.worker is not None and self.worker.isRunning():
            return
        outdir = self.out_edit.text().strip()
        if not outdir:
            outdir = (os.path.dirname(self.model[0]["path"])
                      if self.model else "")
        if not outdir:
            QMessageBox.warning(self, "提示", "请先设置输出目录。")
            return

        jobs = []
        for idx, d in enumerate(self.model):
            if d["status"].startswith("完成"):
                continue
            ext = FMT_EXT[self.fmt]
            stem = os.path.splitext(os.path.basename(d["path"]))[0]
            out = os.path.join(outdir, stem + ext)
            d["out"] = out
            jobs.append((idx, d["path"], out))

        if not jobs:
            self._set_status("没有需要转换的文件。")
            return

        self.run_btn.setEnabled(False)
        self.add_btn.setEnabled(False)
        self.clear_btn.setEnabled(False)
        self.progress.setValue(0)
        # NOTE: the worker derives the pipeline from (src_ext, dst_ext) itself,
        # so it takes the job list only -- do not pass the output format here.
        self.worker = ConversionWorker(jobs)
        self.worker.sig_progress.connect(self._on_progress)
        self.worker.sig_file_status.connect(self._on_file_status)
        self.worker.sig_log.connect(self._set_status)
        self.worker.sig_done.connect(self._on_done)
        self.worker.start()
        self._set_status("开始转换…")

    @_slot
    def _on_progress(self, value, maximum):
        self.progress.setValue(value)

    @_slot
    def _on_file_status(self, idx, ok, msg):
        if 0 <= idx < len(self.model):
            self.model[idx]["status"] = msg
            item = self.queue.item(idx)
            if item is not None:
                item.setText(self._text(self.model[idx]))
                item.setToolTip(self.model[idx].get("out")
                                or self.model[idx]["path"])
            self._color(idx, msg)

    @_slot
    def _on_done(self):
        self.run_btn.setEnabled(True)
        self.add_btn.setEnabled(True)
        self.clear_btn.setEnabled(True)
        self.progress.setValue(100)
        outdir = self.out_edit.text().strip()
        done = sum(1 for d in self.model if d["status"].startswith("完成"))
        failed = sum(1 for d in self.model if d["status"].startswith("失败"))
        # Tell the user WHERE the files went -- "完成" alone is not enough when
        # the output folder is the same as the source folder.
        outs = [d["out"] for d in self.model
                if d.get("out") and os.path.isfile(d["out"])]
        msg = "完成 · 成功 %d / 失败 %d" % (done, failed)
        if len(outs) == 1:
            msg += "  ·  %s" % os.path.basename(outs[0])
        elif outs:
            msg += "  ·  已生成 %d 个文件" % len(outs)
        self._set_status(msg)
        if outs:
            self.statusBar().setToolTip("\n".join(outs))
        if self.open_folder.isChecked() and outdir and os.path.isdir(outdir):
            QDesktopServices.openUrl(QUrl.fromLocalFile(outdir))

    @_slot
    def _clear(self):
        if self.worker is not None and self.worker.isRunning():
            return
        self.model = []
        self.queue.clear()
        self.progress.setValue(0)
        self._set_status("就绪")

    # ----------------------------------------------------------------- dnd
    def dragEnterEvent(self, e: QDragEnterEvent):
        try:
            if e.mimeData().hasUrls():
                e.acceptProposedAction()
        except Exception:  # noqa: BLE001
            pass

    def dropEvent(self, e: QDropEvent):
        try:
            suffixes = self._accepted_suffixes()
            paths = []
            for u in e.mimeData().urls():
                lp = u.toLocalFile()
                if not lp:
                    continue
                if os.path.isdir(lp):
                    paths.extend(str(p) for p in Path(lp).rglob("*")
                                 if p.is_file() and p.suffix.lower() in suffixes)
                elif os.path.splitext(lp)[1].lower() in suffixes:
                    paths.append(lp)
            if paths:
                self._add_files(paths)
                e.acceptProposedAction()
        except Exception:  # noqa: BLE001 - a broken drop must not kill the app
            _write_crash_log(traceback.format_exc())

    # ----------------------------------------------------------------- misc
    @_slot
    def _pick_output(self):
        d = QFileDialog.getExistingDirectory(self, "选择输出目录",
                                            self.out_edit.text() or "")
        if d:
            self.out_edit.setText(d)

    @_slot
    def _open_out_dir(self):
        d = self.out_edit.text().strip()
        if not d and self.model:
            d = os.path.dirname(self.model[0]["path"])
        if d and os.path.isdir(d):
            QDesktopServices.openUrl(QUrl.fromLocalFile(d))
        else:
            QMessageBox.information(
                self, "提示", "输出目录还不存在：\n%s" % (d or "（未设置）"))

    @_slot
    def _open_pagetools(self):
        PageToolsDialog(self).exec()

    @_slot
    def _set_status(self, msg):
        self.statusBar().showMessage(msg)

    @_slot
    def _about(self):
        QMessageBox.about(
            self, "关于 DocForge",
            "DocForge 文档工坊\n"
            "· 转换：PDF ↔ Word / Excel / PowerPoint\n"
            "· 页面工具：合并 / 拆分 / 删除页 / 提取页 / 旋转 / 裁剪 /\n"
            "  水印 / 页码 / PDF↔图片 / 压缩\n"
            "转换内核：pdfconv %s（自研排版重建，非 LibreOffice/Aspose）\n"
            "界面：PyQt6" % pdfconv.__version__)


def _selftest(app):
    """Frozen-build self test: `set DOCFORGE_SELFTEST=1` then run the exe.

    Runs a real 1-page PDF->DOCX conversion plus one page tool inside the
    packaged app, prints a PASS/FAIL line and exits with 0/1.  This catches
    the "window opens but the native libraries / models are missing" class of
    packaging bug that a plain launch test cannot see.

    The transcript is also written to ``%TEMP%\\docforge_selftest.log`` so the
    windowed (console-less) build can be checked too.
    """
    import tempfile
    import traceback

    import pdfconv
    import pdfconv.pages
    import pymupdf as fitz

    lines = []

    # A modal error box would hang an unattended self test.
    QMessageBox.critical = staticmethod(
        lambda *a, **k: QMessageBox.StandardButton.Ok)

    def log(msg):
        print(msg)
        lines.append(str(msg))

    ok = True
    tmp = tempfile.mkdtemp(prefix="docforge_selftest_")
    try:
        doc = fitz.open()
        page = doc.new_page()
        page.insert_text((72, 100), "DocForge self test 12345", fontsize=18)
        page.insert_text((72, 140), "Second line for layout.", fontsize=12)
        src = os.path.join(tmp, "in.pdf")
        doc.save(src)
        doc.close()

        dst = os.path.join(tmp, "out.docx")
        pdfconv.convert(src, dst)
        size = os.path.getsize(dst)
        log("[ok] pdf->docx   %d bytes" % size)
        ok &= size > 1000

        dst2 = os.path.join(tmp, "out.xlsx")
        pdfconv.convert_xlsx(src, dst2)
        log("[ok] pdf->xlsx   %d bytes" % os.path.getsize(dst2))

        dst3 = os.path.join(tmp, "merged.pdf")
        pdfconv.pages.merge_pdfs([src, src], dst3)
        n = len(fitz.open(dst3))
        log("[ok] merge       %d pages" % n)
        ok &= n == 2

        # Drive the real GUI button path.  This is what crashed in the first
        # shipped build (ConversionWorker got an unexpected `fmt=` keyword);
        # a direct pdfconv.convert() call above cannot catch that.
        log("[..] GUI 开始转换 (click 开始转换 button) ...")
        win = MainWindow()                     # not shown -> no window flashes
        for b in win.fmt_group.buttons():      # click the real format button
            if b.property("fmt") == "docx":
                b.click()
                break
        win.open_folder.setChecked(False)      # never pop Explorer while testing
        win._add_files([src])
        win.out_edit.setText(tmp)
        win.run_btn.click()                    # clicked(bool) -> _start(bool)
        deadline = time.time() + 120
        while (win.worker is not None and win.worker.isRunning()
               and time.time() < deadline):
            app.processEvents()
            time.sleep(0.02)
        for _ in range(50):                    # drain queued signals
            app.processEvents()
            time.sleep(0.01)
        status = win.model[0]["status"] if win.model else "-"
        log("[ok] GUI          %s" % status)
        ok &= status.startswith("完成")
        ok &= os.path.isfile(os.path.join(tmp, "in.docx"))
        win.deleteLater()

        log("FROZEN=%s  MEIPASS=%s" % (getattr(sys, "frozen", False),
                                       getattr(sys, "_MEIPASS", "-")))
        log("pymupdf=%s  pdfconv=%s" % (fitz.__doc__.split(":")[0],
                                        pdfconv.__version__))
        log("layout disabled = %s" %
            (sys.modules.get("pymupdf.layout", "absent") is None))
    except Exception:
        ok = False
        log(traceback.format_exc())
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    log("SELFTEST %s" % ("PASS" if ok else "FAIL"))
    try:
        with open(os.path.join(tempfile.gettempdir(), "docforge_selftest.log"),
                  "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    except OSError:
        pass
    return 0 if ok else 1


def main():
    app = QApplication(sys.argv)
    if os.environ.get("DOCFORGE_SELFTEST") == "1":
        sys.exit(_selftest(app))
    app.setStyleSheet(
        "QMainWindow, QWidget{font-family:'Microsoft YaHei UI','Segoe UI',sans-serif;}"
    )
    w = MainWindow()
    w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
