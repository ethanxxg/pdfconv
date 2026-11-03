"""Convert a PDF and report the page count of the result.

Renders the DOCX back to PDF with the project's own renderer (LibreOffice
chokes on these large files) and compares the page count with the source.

Usage:  python tools/pages.py <in.pdf> [out.docx]
"""
import os
import sys
import time

import fitz

from pdfconv.convert import convert
from pdfconv.docx2pdf import convert_docx_pdf


def main(src, out=None):
    out = out or os.path.join("_cmp", "pg_" + os.path.basename(src)[:12] + ".docx")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    t0 = time.time()
    res = convert(src, out)
    t1 = time.time()
    rp = out[:-5] + ".pdf"
    n, p, t = convert_docx_pdf(out, rp)
    t2 = time.time()
    s = len(fitz.open(src))
    print("SRC   %-52s pages=%d" % (os.path.basename(src)[:52], s))
    print("DOCX  %-52s pages=%d  blocks=%d tables=%d figs=%d"
          % (os.path.basename(out)[:52], n, res[0], res[1], res[2]))
    print("      delta %+d   convert %.1fs  render %.1fs" % (n - s, t1 - t0, t2 - t1))


if __name__ == "__main__":
    main(*sys.argv[1:])
