# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for DocForge (single-file Windows exe).

Build:  python -m PyInstaller --noconfirm --clean app/DocForge.spec
Output: dist/DocForge.exe
"""

import os
import sys
from importlib.util import find_spec

from PyInstaller.utils.hooks import (
    collect_data_files, collect_dynamic_libs, collect_submodules,
)

SPEC_DIR = os.path.dirname(os.path.abspath(SPEC))          # .../app
ROOT = os.path.dirname(SPEC_DIR)                            # project root
APP = SPEC_DIR

for p in (ROOT, APP):
    if p not in sys.path:
        sys.path.insert(0, p)

# ---- data files bundled into the exe -------------------------------------
datas = [
    (os.path.join(ROOT, "pdfconv", "models"), "pdfconv/models"),  # OCR rec model
]
# python-docx / python-pptx ship default templates that Document()/Presentation()
# load at runtime; they must be present in the frozen app.
datas += collect_data_files("docx")
datas += collect_data_files("pptx")

binaries = []

# ---- hidden imports -------------------------------------------------------
hiddenimports = []
hiddenimports += collect_submodules("pdfconv")
hiddenimports += [
    "PyQt6.QtCore", "PyQt6.QtGui", "PyQt6.QtWidgets",
    "fitz", "docx", "openpyxl", "pptx", "lxml", "lxml.etree",
]

# PyMuPDF: `fitz` is only an alias for `pymupdf` in recent versions, and the
# generic hook can miss its native libraries (mupdfcpp64.dll, _mupdf.pyd ...),
# which makes `import fitz` fail at run time with a missing-file error.
# `pymupdf.layout` is deliberately NOT bundled: it carries ~50 MB of ONNX models
# under layout/resources/onnx and loads them eagerly at import time.  In a
# frozen app those data files are absent, so it raises FileNotFoundError (not
# ImportError) and kills start-up.  pdfconv disables it via sys.modules instead
# (see pdfconv/__init__.py), so we drop its modules to keep the exe lean.
hiddenimports += [m for m in collect_submodules("pymupdf", on_error="ignore")
                  if not m.startswith("pymupdf.layout")]
hiddenimports += ["pymupdf", "fitz"]
binaries += collect_dynamic_libs("pymupdf")

# OCR is optional: only bundle it when installed, otherwise the app still runs
# and simply skips scanned / undecodable pages.
if find_spec("rapidocr_onnxruntime"):
    hiddenimports += collect_submodules("rapidocr_onnxruntime")
    datas += collect_data_files("rapidocr_onnxruntime")
if find_spec("onnxruntime"):
    hiddenimports += ["onnxruntime", "onnxruntime.capi._pybind_state"]
    binaries += collect_dynamic_libs("onnxruntime")

a = Analysis(
    [os.path.join(APP, "main.py")],
    pathex=[APP, ROOT],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "tkinter", "matplotlib", "PyQt5", "PySide2", "PySide6",
        "IPython", "pytest", "notebook", "jupyter",
        "pymupdf.layout", "pymupdf4llm",
    ],
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="DocForge",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=bool(os.environ.get("DOCFORGE_CONSOLE")),
    disable_windowed_traceback=False,
    icon=os.path.join(APP, "DocForge.ico"),
    version=os.path.join(APP, "version_info.txt"),
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
