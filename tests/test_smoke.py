"""Smoke test: convert one sample PDF and check the DOCX is valid and non-empty.

Run directly:  python tests/test_smoke.py
Or with pytest: pytest tests/ -q
"""
import glob
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from pdfconv import convert  # noqa: E402
from docx import Document    # noqa: E402


def _first_pdf():
    pdfs = sorted(glob.glob(os.path.join(ROOT, "docs", "*.pdf")))
    return pdfs[0] if pdfs else None


def test_convert_one():
    pdf = _first_pdf()
    if not pdf:
        print("SKIP: no sample PDF under docs/")
        return
    out = os.path.join(tempfile.mkdtemp(), "smoke.docx")
    nblk, ntbl, nfig = convert(pdf, out)
    assert os.path.exists(out), "output not created"
    d = Document(out)
    text = "\n".join(p.text for p in d.paragraphs)
    assert len(text.strip()) > 100, "output text too short"
    print(f"OK {os.path.basename(pdf)}: blocks={nblk} tables={ntbl} figs={nfig}, "
          f"chars={len(text)}")


if __name__ == "__main__":
    test_convert_one()
