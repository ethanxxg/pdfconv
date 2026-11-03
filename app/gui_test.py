"""Headless end-to-end GUI test for DocForge.

Two bugs shipped because the old `smoke_test.py` only imported modules:

1. `_start()` passed `fmt=` to `ConversionWorker`, which takes only `jobs`;
2. the `@slot` guard wrapper swallowed PyQt's argument trimming, so
   `clicked(bool)` -> `_add_files_dialog(self)` raised
   "takes 1 positional argument but 2 were given".

Neither shows up unless the *real widgets are clicked* -- importing proves
nothing, and calling `win._start()` directly hides the signal-argument issue
entirely.  So this script clicks buttons, exactly like a user.

Run:  python app/gui_test.py     (exit 0 = pass)
"""

import os
import sys
import tempfile
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for _p in (ROOT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import pymupdf as fitz
from PyQt6.QtWidgets import QApplication, QFileDialog, QMessageBox

# ---- capture dialogs instead of showing them (modal boxes would hang) ------
_errors = []       # critical / warning  -> any of these is a test failure
_infos = []        # information         -> expected on success paths


def _on_critical(*args, **kwargs):
    _errors.append(str(args[2]) if len(args) > 2 else str(args))
    return QMessageBox.StandardButton.Ok


def _on_info(*args, **kwargs):
    _infos.append(str(args[2]) if len(args) > 2 else str(args))
    return QMessageBox.StandardButton.Ok


QMessageBox.critical = staticmethod(_on_critical)
QMessageBox.warning = staticmethod(_on_critical)
QMessageBox.information = staticmethod(_on_info)

# ---- fake the native file pickers -----------------------------------------
_pick_files = []
_pick_dir = ""


def _fake_open(*args, **kwargs):
    return list(_pick_files), ""


def _fake_dir(*args, **kwargs):
    return _pick_dir


QFileDialog.getOpenFileNames = staticmethod(_fake_open)
QFileDialog.getExistingDirectory = staticmethod(_fake_dir)

import main as appmod  # noqa: E402  (after the patches above)

# ---- never really launch Explorer -----------------------------------------
_opened = []


class _FakeDesktop:
    @staticmethod
    def openUrl(url):
        _opened.append(url.toLocalFile())
        return True


appmod.QDesktopServices = _FakeDesktop


def pump(app, seconds):
    end = time.time() + seconds
    while time.time() < end:
        app.processEvents()
        time.sleep(0.01)


def set_fmt(win, fmt):
    """Click the real format button (exercises the buttonClicked slot)."""
    for b in win.fmt_group.buttons():
        if b.property("fmt") == fmt:
            b.click()
            return win.fmt == fmt
    return False


def wait_worker(app, win, timeout=180):
    if win.worker is None:
        return False
    deadline = time.time() + timeout
    while win.worker.isRunning() and time.time() < deadline:
        app.processEvents()
        time.sleep(0.02)
    pump(app, 0.5)                      # deliver the queued sig_done
    return not win.worker.isRunning()


def main():
    global _pick_files, _pick_dir

    app = QApplication(sys.argv)
    win = appmod.MainWindow()
    tmp = tempfile.mkdtemp(prefix="docforge_gui_test_")
    ok = True

    # ---- source PDF -------------------------------------------------------
    doc = fitz.open()
    page = doc.new_page()
    page.insert_text((72, 100), "DocForge GUI test 98765", fontsize=18)
    page.insert_text((72, 140), "A second paragraph for layout analysis.",
                     fontsize=12)
    src_pdf = os.path.join(tmp, "src.pdf")
    doc.save(src_pdf)
    doc.close()

    # ---- 1) PDF -> DOCX, driven entirely through real clicks --------------
    print("[1] 点击 添加文件 + 开始转换 (PDF -> DOCX)")
    ok &= set_fmt(win, "docx")
    _pick_files = [src_pdf]
    win.add_btn.click()                     # -> _add_files_dialog(bool)
    win.out_edit.setText(tmp)
    win.run_btn.click()                     # -> _start(bool)
    finished = wait_worker(app, win)
    status = win.model[0]["status"] if win.model else "-"
    out_docx = os.path.join(tmp, "src.docx")
    print("    queue=%d finished=%s status=%s" % (len(win.model), finished, status))
    ok &= len(win.model) == 1
    ok &= finished and status.startswith("完成")
    ok &= os.path.isfile(out_docx)
    ok &= win.progress.value() == 100
    ok &= win.run_btn.isEnabled()           # _on_done must re-enable it
    ok &= win.open_folder.isChecked()       # defaults to on -> Explorer opens
    ok &= [os.path.normpath(x) for x in _opened] == [os.path.normpath(tmp)]
    status_msg = win.statusBar().currentMessage()
    print("    status bar = %s" % status_msg)
    ok &= "src.docx" in status_msg          # names the produced file
    ok &= win.open_out_btn is not None      # explicit 打开 button exists

    # ---- 2) DOCX -> PDF (reverse direction + input filter) ---------------
    out2 = tempfile.mkdtemp(prefix="docforge_gui_test2_")
    print("[2] 点击 全部清除 + 添加文件 + 开始转换 (DOCX -> PDF)")
    win.clear_btn.click()
    ok &= len(win.model) == 0
    ok &= set_fmt(win, "pdf")
    _pick_files = [out_docx]
    win.add_btn.click()
    win.out_edit.setText(out2)
    win.run_btn.click()
    finished = wait_worker(app, win)
    status = win.model[0]["status"] if win.model else "-"
    out_pdf = os.path.join(out2, "src.pdf")
    print("    queue=%d finished=%s status=%s" % (len(win.model), finished, status))
    ok &= finished and status.startswith("完成")
    ok &= os.path.isfile(out_pdf)

    # ---- 3) 添加文件夹 (recursive rglob + suffix filter) ------------------
    print("[3] 点击 添加文件夹")
    win.clear_btn.click()
    ok &= set_fmt(win, "docx")
    _pick_dir = tmp                          # contains src.pdf (+ src.docx)
    win.add_folder_btn.click()
    names = sorted(d["name"] for d in win.model)
    print("    picked up = %s" % names)
    ok &= names == ["src.pdf"]

    # ---- 4) the input filter must reject the wrong kind of file ----------
    before = len(win.model)
    win._add_files([out_docx])               # docx is invalid for docx output
    print("[4] wrong-type input rejected = %s" % (len(win.model) == before))
    ok &= len(win.model) == before

    # ---- 5) page tools: every operation must build its parameter panel ----
    from pagetools import OPS, PageToolsDialog

    dlg = PageToolsDialog(None)
    bad = []
    for i, (key, _label, _mode, _out, _suf) in enumerate(OPS):
        try:
            dlg.op_combo.setCurrentIndex(i)  # -> _on_op(int)
            dlg._current()
        except Exception as exc:  # noqa: BLE001
            bad.append("%s: %s" % (key, exc))
    print("[5] page-tool panels built = %d/%d %s"
          % (len(OPS) - len(bad), len(OPS), bad or ""))
    ok &= not bad

    # ---- 6) run one real page operation (merge) by clicking --------------
    dlg.op_combo.setCurrentIndex(0)          # merge
    dlg.inputs = [src_pdf, src_pdf]
    merge_out = os.path.join(tmp, "merged.pdf")
    dlg.out_edit.setText(merge_out)
    dlg.run_btn.click()                      # -> _run(bool)
    deadline = time.time() + 120
    while (dlg.worker is not None and dlg.worker.isRunning()
           and time.time() < deadline):
        app.processEvents()
        time.sleep(0.02)
    pump(app, 0.5)
    merged_pages = len(fitz.open(merge_out)) if os.path.isfile(merge_out) else 0
    print("[6] merge via dialog = %d pages" % merged_pages)
    ok &= merged_pages == 2

    if _errors:
        ok = False
        print("!! error dialogs were raised:")
        for d in _errors:
            print("   -", d.replace("\n", " ")[:300])

    print("GUI TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
