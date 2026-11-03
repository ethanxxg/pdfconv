"""Page-tools dialog for DocForge.

One dialog exposes every page-level operation (merge / split / remove / extract
/ rotate / crop / watermark / page numbers / PDF->images / images->PDF /
compress).  A dropdown picks the operation; a stacked panel shows only the
parameters that operation needs.  All work is delegated to pdfconv.pages and
runs on a background thread.
"""

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from PyQt6.QtCore import QThread, pyqtSignal
from PyQt6.QtWidgets import (
    QComboBox, QDialog, QDoubleSpinBox, QFileDialog, QFormLayout, QHBoxLayout,
    QLabel, QLineEdit, QListWidget, QMessageBox, QProgressBar, QPushButton,
    QSpinBox, QStackedWidget, QVBoxLayout, QWidget,
)

from guards import slot as _slot
from pdfconv import pages as pg

# key, label, input_mode(single|multi), output_mode(file|dir), default suffix
OPS = [
    ("merge", "合并 PDF", "multi", "file", "_merged.pdf"),
    ("split", "拆分 PDF", "single", "dir", ""),
    ("remove", "删除页", "single", "file", "_removed.pdf"),
    ("extract", "提取页", "single", "file", "_extracted.pdf"),
    ("rotate", "旋转页", "single", "file", "_rotated.pdf"),
    ("crop", "裁剪页边距", "single", "file", "_cropped.pdf"),
    ("watermark", "添加水印", "single", "file", "_wm.pdf"),
    ("pagenum", "添加页码", "single", "file", "_numbered.pdf"),
    ("pdf2img", "PDF 转图片", "single", "dir", ""),
    ("img2pdf", "图片转 PDF", "multi", "file", "_images.pdf"),
    ("compress", "压缩 PDF", "single", "file", "_compressed.pdf"),
]
OP_MAP = {o[0]: o for o in OPS}


class _Worker(QThread):
    ok = pyqtSignal(object)
    fail = pyqtSignal(str)

    def __init__(self, fn):
        super().__init__()
        self.fn = fn

    def run(self):
        try:
            self.ok.emit(self.fn())
        except Exception as exc:  # noqa: BLE001
            self.fail.emit(str(exc))


class PageToolsDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("页面工具")
        self.setMinimumWidth(600)
        self.inputs = []
        self.w = {}
        self.worker = None

        root = QVBoxLayout(self)
        root.setSpacing(12)

        top = QHBoxLayout()
        top.addWidget(QLabel("操作"))
        self.op_combo = QComboBox()
        for key, label, *_ in OPS:
            self.op_combo.addItem(label, key)
        self.op_combo.currentIndexChanged.connect(self._on_op)
        top.addWidget(self.op_combo, stretch=1)
        root.addLayout(top)

        self.stack = QStackedWidget()
        for key, *_ in OPS:
            self.stack.addWidget(self._build_page(key))
        root.addWidget(self.stack)

        # inputs
        root.addWidget(QLabel("输入文件"))
        self.input_list = QListWidget()
        self.input_list.setMaximumHeight(110)
        root.addWidget(self.input_list)
        in_row = QHBoxLayout()
        self.add_in_btn = QPushButton("添加文件")
        self.add_in_btn.clicked.connect(self._add_inputs)
        clr = QPushButton("清空输入")
        clr.clicked.connect(self._clear_inputs)
        in_row.addWidget(self.add_in_btn)
        in_row.addWidget(clr)
        in_row.addStretch(1)
        root.addLayout(in_row)

        # output
        out_row = QHBoxLayout()
        out_row.addWidget(QLabel("输出"))
        self.out_edit = QLineEdit()
        self.out_edit.setPlaceholderText("选择输出文件或目录")
        out_row.addWidget(self.out_edit, stretch=1)
        self.out_btn = QPushButton("浏览")
        self.out_btn.clicked.connect(self._pick_output)
        out_row.addWidget(self.out_btn)
        root.addLayout(out_row)

        self.progress = QProgressBar()
        self.progress.setValue(0)
        root.addWidget(self.progress)
        self.status = QLabel("就绪")
        self.status.setStyleSheet("color:#666;font-size:12px;")
        root.addWidget(self.status)

        run_row = QHBoxLayout()
        run_row.addStretch(1)
        self.close_btn = QPushButton("关闭")
        self.close_btn.clicked.connect(self.reject)
        self.run_btn = QPushButton("执行")
        self.run_btn.setStyleSheet(
            "QPushButton{background:#185FA5;color:#fff;padding:8px 16px;"
            "border:none;border-radius:6px;font-weight:500;}"
            "QPushButton:disabled{background:#9bb8d6;}")
        self.run_btn.clicked.connect(self._run)
        run_row.addWidget(self.close_btn)
        run_row.addWidget(self.run_btn)
        root.addLayout(run_row)

        self._on_op(0)

    # ------------------------------------------------------------ param pages
    def _build_page(self, key):
        page = QWidget()
        form = QFormLayout(page)
        form.setContentsMargins(0, 0, 0, 0)
        d = self.w[key] = {}

        if key == "split":
            d["every"] = QSpinBox()
            d["every"].setRange(1, 9999)
            d["every"].setValue(1)
            form.addRow("每 N 页拆分", d["every"])
        elif key == "rotate":
            d["angle"] = QComboBox()
            for lab, val in (("90°（顺时针）", 90), ("180°", 180), ("270°", 270)):
                d["angle"].addItem(lab, val)
            d["pages"] = QLineEdit()
            d["pages"].setPlaceholderText("留空=全部，如 1-3,5")
            form.addRow("角度", d["angle"])
            form.addRow("页码范围", d["pages"])
        elif key == "crop":
            for side in ("top", "bottom", "left", "right"):
                sb = QDoubleSpinBox()
                sb.setRange(0, 2000)
                sb.setDecimals(1)
                sb.setSuffix(" pt")
                d[side] = sb
                form.addRow({"top": "上边距", "bottom": "下边距",
                             "left": "左边距", "right": "右边距"}[side], sb)
        elif key in ("remove", "extract"):
            d["pages"] = QLineEdit()
            d["pages"].setPlaceholderText("如 2,4,6-8")
            form.addRow("页码", d["pages"])
        elif key == "watermark":
            d["text"] = QLineEdit()
            d["text"].setPlaceholderText("水印文字")
            d["opacity"] = QDoubleSpinBox()
            d["opacity"].setRange(0.05, 1.0)
            d["opacity"].setSingleStep(0.05)
            d["opacity"].setValue(0.15)
            d["angle"] = QComboBox()
            for lab, val in (("水平", 0), ("90°", 90), ("180°", 180), ("270°", 270)):
                d["angle"].addItem(lab, val)
            d["fontsize"] = QSpinBox()
            d["fontsize"].setRange(8, 200)
            d["fontsize"].setValue(48)
            form.addRow("文字", d["text"])
            form.addRow("透明度", d["opacity"])
            form.addRow("角度", d["angle"])
            form.addRow("字号", d["fontsize"])
        elif key == "pagenum":
            d["pos"] = QComboBox()
            for lab, val in (("底部居中", "bottom-center"), ("底部左", "bottom-left"),
                             ("底部右", "bottom-right"), ("顶部居中", "top-center"),
                             ("顶部左", "top-left"), ("顶部右", "top-right")):
                d["pos"].addItem(lab, val)
            d["start"] = QSpinBox()
            d["start"].setRange(1, 99999)
            d["start"].setValue(1)
            d["fontsize"] = QSpinBox()
            d["fontsize"].setRange(6, 48)
            d["fontsize"].setValue(10)
            form.addRow("位置", d["pos"])
            form.addRow("起始页码", d["start"])
            form.addRow("字号", d["fontsize"])
        elif key == "pdf2img":
            d["dpi"] = QSpinBox()
            d["dpi"].setRange(72, 600)
            d["dpi"].setValue(150)
            d["fmt"] = QComboBox()
            d["fmt"].addItems(["png", "jpg"])
            d["pages"] = QLineEdit()
            d["pages"].setPlaceholderText("留空=全部")
            form.addRow("分辨率 DPI", d["dpi"])
            form.addRow("格式", d["fmt"])
            form.addRow("页码范围", d["pages"])
        elif key == "merge":
            form.addRow(QLabel("按下方列表顺序合并 PDF"))
        elif key == "img2pdf":
            form.addRow(QLabel("按下方列表顺序把图片合成 PDF"))
        elif key == "compress":
            form.addRow(QLabel("结构压缩：清理冗余对象、压缩数据流"))
        return page

    # ------------------------------------------------------------ helpers
    def _current(self):
        return self.op_combo.currentData()

    @_slot
    def _on_op(self, idx):
        key = self.op_combo.itemData(idx)
        self.stack.setCurrentIndex(idx)
        multi = OP_MAP[key][2] == "multi"
        self.add_in_btn.setText("添加文件（可多选）" if multi else "添加文件")
        self.input_list.setMaximumHeight(110 if multi else 74)
        self._auto_out()

    def _auto_out(self):
        if not self.inputs:
            return
        key = self._current()
        suffix = OP_MAP[key][4]
        first = self.inputs[0]
        base_dir = os.path.dirname(first)
        if OP_MAP[key][3] == "dir":
            self.out_edit.setText(os.path.join(base_dir, "out_" + key))
        else:
            stem = os.path.splitext(os.path.basename(first))[0]
            self.out_edit.setText(os.path.join(base_dir, stem + suffix))

    @_slot
    def _add_inputs(self):
        key = self._current()
        multi = OP_MAP[key][2] == "multi"
        if key == "img2pdf":
            filt = "图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"
        else:
            filt = "PDF 文件 (*.pdf)"
        if multi:
            files, _ = QFileDialog.getOpenFileNames(self, "选择文件", "", filt)
        else:
            f, _ = QFileDialog.getOpenFileName(self, "选择文件", "", filt)
            files = [f] if f else []
        if not files:
            return
        if not multi:
            self.inputs = []
        for f in files:
            if f not in self.inputs:
                self.inputs.append(f)
        self._refresh_inputs()
        if not self.out_edit.text().strip():
            self._auto_out()

    @_slot
    def _clear_inputs(self):
        self.inputs = []
        self._refresh_inputs()

    def _refresh_inputs(self):
        self.input_list.clear()
        for p in self.inputs:
            self.input_list.addItem(os.path.basename(p))

    @_slot
    def _pick_output(self):
        key = self._current()
        if OP_MAP[key][3] == "dir":
            d = QFileDialog.getExistingDirectory(self, "选择输出目录",
                                                self.out_edit.text() or "")
            if d:
                self.out_edit.setText(d)
        else:
            f, _ = QFileDialog.getSaveFileName(self, "输出文件",
                                               self.out_edit.text() or "",
                                               "PDF 文件 (*.pdf)")
            if f:
                if not f.lower().endswith(".pdf"):
                    f += ".pdf"
                self.out_edit.setText(f)

    # ------------------------------------------------------------ run
    @_slot
    def _run(self):
        if self.worker is not None and self.worker.isRunning():
            return
        key = self._current()
        inputs = list(self.inputs)
        out = self.out_edit.text().strip()
        if not inputs:
            QMessageBox.warning(self, "提示", "请先添加输入文件。")
            return
        if OP_MAP[key][2] == "single" and len(inputs) != 1:
            QMessageBox.warning(self, "提示", "该操作只接受单个输入文件。")
            return
        if not out:
            QMessageBox.warning(self, "提示", "请设置输出位置。")
            return
        d = self.w[key]

        def job():
            if key == "merge":
                return "已合并 %d 页" % pg.merge_pdfs(inputs, out)
            if key == "split":
                outs = pg.split_pdf(inputs[0], out, every=d["every"].value())
                return "已拆分为 %d 个文件" % len(outs)
            if key == "remove":
                return "剩余 %d 页" % pg.remove_pages(inputs[0], out, d["pages"].text())
            if key == "extract":
                return "已提取 %d 页" % pg.extract_pages(inputs[0], out, d["pages"].text())
            if key == "rotate":
                return "已旋转 %d 页" % pg.rotate_pages(
                    inputs[0], out, d["angle"].currentData(), d["pages"].text() or None)
            if key == "crop":
                return "已裁剪 %d 页" % pg.crop_pages(
                    inputs[0], out, d["top"].value(), d["bottom"].value(),
                    d["left"].value(), d["right"].value())
            if key == "watermark":
                if not d["text"].text().strip():
                    raise ValueError("请输入水印文字")
                return "已加水印 %d 页" % pg.add_watermark(
                    inputs[0], out, d["text"].text(), d["opacity"].value(),
                    d["angle"].currentData(), d["fontsize"].value())
            if key == "pagenum":
                return "已加页码 %d 页" % pg.add_page_numbers(
                    inputs[0], out, d["pos"].currentData(), d["start"].value(),
                    d["fontsize"].value())
            if key == "pdf2img":
                outs = pg.pdf_to_images(inputs[0], out, d["dpi"].value(),
                                        d["fmt"].currentText(),
                                        d["pages"].text() or None)
                return "已导出 %d 张图片" % len(outs)
            if key == "img2pdf":
                return "已生成 %d 页" % pg.images_to_pdf(inputs, out)
            if key == "compress":
                before, after = pg.compress_pdf(inputs[0], out)
                pct = 100.0 * (before - after) / before if before else 0
                return "%.1f MB -> %.1f MB（省 %.0f%%）" % (
                    before / 1048576.0, after / 1048576.0, pct)
            raise ValueError("未知操作")

        self.run_btn.setEnabled(False)
        self.progress.setRange(0, 0)   # indeterminate
        self.status.setText("处理中…")
        self.worker = _Worker(job)
        self.worker.ok.connect(self._on_ok)
        self.worker.fail.connect(self._on_fail)
        self.worker.start()

    @_slot
    def _on_ok(self, msg):
        self.progress.setRange(0, 100)
        self.progress.setValue(100)
        self.run_btn.setEnabled(True)
        self.status.setText(str(msg))
        QMessageBox.information(self, "完成", str(msg))

    @_slot
    def _on_fail(self, err):
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.run_btn.setEnabled(True)
        self.status.setText("失败：" + err)
        QMessageBox.critical(self, "失败", err)
