"""pdfconv -- a self-contained PDF -> DOCX / XLSX / PPTX converter.

Design goal
-----------
All *conversion* logic (layout analysis, paragraph / table / figure
reconstruction, reading order, typography mapping, DOCX emission) is our own
code.  MuPDF (via PyMuPDF) is used **only to read** the PDF; python-docx,
openpyxl and python-pptx write the output packages.  No pdf2docx / LibreOffice
/ Aspose / other converter is involved.
"""
import os as _os
import sys as _sys


def _stub_pymupdf_layout():
    """Disable PyMuPDF's optional ``layout`` sub-package before it is imported.

    PyMuPDF >= 2.0 tries ``import pymupdf.layout`` at import time.  That
    sub-package ships ~50 MB of ONNX models under ``layout/resources/onnx``
    and loads them eagerly, so in a PyInstaller bundle (where those data files
    are absent) it raises ``FileNotFoundError`` -- *not* ``ImportError`` --
    which slips past PyMuPDF's own ``try/except ImportError`` and kills the
    process at start-up with::

        [Errno 2] No such file or directory: '...\\pymupdf\\layout\\resources\\onnx\\layout_rf2.4.1+imf1.yaml'

    ``pymupdf4llm`` (imported at the tail of ``pymupdf/__init__``) has the
    same ``except ImportError`` fallback.  So putting ``None`` into
    ``sys.modules`` makes the import raise ``ModuleNotFoundError`` -- which
    *is* an ``ImportError`` -- and both call sites degrade gracefully.

    pdfconv never uses the layout/Markdown pipeline (we do our own layout
    analysis), so nothing is lost: no data files, no onnxruntime, no start-up
    penalty.  Set ``PDFCONV_ENABLE_LAYOUT=1`` to opt back in (e.g. when running
    from source and you *do* want ``pymupdf4llm``-style layout analysis).
    """
    if _os.environ.get("PDFCONV_ENABLE_LAYOUT") == "1":
        return
    if "pymupdf.layout" in _sys.modules:
        return
    # A ``None`` entry makes ``import pymupdf.layout`` raise ModuleNotFoundError
    # ("import of pymupdf.layout halted; None in sys.modules").
    _sys.modules["pymupdf.layout"] = None
    # PyMuPDF's page.find_tables() prints "Consider using the pymupdf_layout
    # package..." once; we disabled it on purpose, so silence the nag.
    _os.environ.setdefault("PYMUPDF_SUGGEST_LAYOUT_ANALYZER", "0")


_stub_pymupdf_layout()

from .convert import convert
from .docx2pdf import convert_docx_pdf
from .pptx import convert_pptx
from .pptx2pdf import convert_pptx_pdf
from .xlsx import convert_xlsx
from .xlsx2pdf import convert_xlsx_pdf
from .translate import translate_docx, translate_pdf
from .translate_inplace import translate_pdf_inplace

__all__ = ["convert", "convert_xlsx", "convert_pptx", "convert_docx_pdf",
           "convert_xlsx_pdf", "convert_pptx_pdf", "translate_docx",
           "translate_pdf", "translate_pdf_inplace"]
__version__ = "1.11.4"
