# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for DocForge (onedir build — faster start-up).

Build:  python -m PyInstaller --noconfirm --clean app/DocForge_onedir.spec
Output: dist/DocForge/DocForge.exe  (ship the whole folder)
"""

import os
import sys
from importlib.util import find_spec

from PyInstaller.utils.hooks import (
    collect_data_files, collect_dynamic_libs, collect_submodules,
)

SPEC_DIR = os.path.dirname(os.path.abspath(SPEC))
ROOT = os.path.dirname(SPEC_DIR)
APP = SPEC_DIR

for p in (ROOT, APP):
    if p not in sys.path:
        sys.path.insert(0, p)

datas = [
    (os.path.join(ROOT, "pdfconv", "models"), "pdfconv/models"),
]
datas += collect_data_files("docx")
datas += collect_data_files("pptx")

binaries = []

hiddenimports = []
hiddenimports += collect_submodules("pdfconv")
hiddenimports += [
    "PyQt6.QtCore", "PyQt6.QtGui", "PyQt6.QtWidgets",
    "fitz", "docx", "openpyxl", "pptx", "lxml", "lxml.etree",
]

# PyMuPDF: `fitz` is only an alias for `pymupdf`; collect its native libraries
# explicitly (mupdfcpp64.dll, _mupdf.pyd ...) or `import fitz` fails at run time.
# `pymupdf.layout` is deliberately NOT bundled: it carries ~50 MB of ONNX models
# under layout/resources/onnx and loads them eagerly at import time.  In a
# frozen app those data files are absent, so it raises FileNotFoundError (not
# ImportError) and kills start-up.  pdfconv disables it via sys.modules instead
# (see pdfconv/__init__.py), so we drop its modules to keep the build lean.
hiddenimports += [m for m in collect_submodules("pymupdf", on_error="ignore")
                  if not m.startswith("pymupdf.layout")]
hiddenimports += ["pymupdf", "fitz"]
binaries += collect_dynamic_libs("pymupdf")

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
    [],
    exclude_binaries=True,
    name="DocForge",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=bool(os.environ.get("DOCFORGE_CONSOLE")),
    disable_windowed_traceback=False,
    icon=os.path.join(APP, "DocForge.ico"),
    version=os.path.join(APP, "version_info.txt"),
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name="DocForge",
)
