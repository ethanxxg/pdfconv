"""Diagnose page-count fidelity: compare a PDF's page geometry and text
volume against a DOCX produced from it.

The point is to separate the two things that decide the page count:

  capacity = (text frame area) / (line height x line width)

  * *volume*  -- how much text / how many figures the document carries.
    If this differs, the converter lost or duplicated content.
  * *capacity* -- how much fits on one page.  If this differs, the page
    count differs even with identical content.

Usage:  python tools/pagefit.py source.pdf out.docx
"""
import sys

import fitz
from docx import Document
from docx.shared import Pt


def _pt(v):
    """python-docx Length is EMU; 1 point = 12700 EMU."""
    return float(v) / 12700.0


def pdf_stats(path):
    doc = fitz.open(path)
    pw, ph = doc[0].rect.width, doc[0].rect.height
    # text frame: union of body-size span boxes over a sample of pages
    sizes = {}
    for pno in range(min(len(doc), 40)):
        for b in doc[pno].get_text("dict")["blocks"]:
            if b.get("type", 0) != 0:
                continue
            for ln in b.get("lines") or ():
                for sp in ln.get("spans") or ():
                    t = sp.get("text") or ""
                    if t.strip():
                        sizes[round(sp["size"], 1)] = sizes.get(
                            round(sp["size"], 1), 0) + len(t)
    body = max(sizes.items(), key=lambda kv: kv[1])[0] if sizes else 10.0
    x0 = y0 = 1e9
    x1 = y1 = -1e9
    for pno in range(min(len(doc), 40)):
        for b in doc[pno].get_text("dict")["blocks"]:
            if b.get("type", 0) != 0:
                continue
            for ln in b.get("lines") or ():
                for sp in ln.get("spans") or ():
                    if abs(sp["size"] - body) > 0.35:
                        continue
                    if not (sp.get("text") or "").strip():
                        continue
                    bb = sp["bbox"]
                    x0 = min(x0, bb[0])
                    x1 = max(x1, bb[2])
                    y0 = min(y0, bb[1])
                    y1 = max(y1, bb[3])
    nchars = 0
    for pno in range(len(doc)):
        nchars += len(doc[pno].get_text("text").split())
    return dict(pages=len(doc), page=(pw, ph), body=body,
                frame=(x0, y0, x1, y1), words=nchars)


def docx_stats(path):
    doc = Document(path)
    s = doc.sections[0]
    return dict(
        page=(_pt(s.page_width), _pt(s.page_height)),
        frame=(_pt(s.left_margin), _pt(s.top_margin),
               _pt(s.page_width - s.right_margin),
               _pt(s.page_height - s.bottom_margin)),
        body=float(doc.styles["Normal"].font.size.pt
                   if doc.styles["Normal"].font.size else 0),
        words=sum(len(p.text.split()) for p in doc.paragraphs),
        paras=len(doc.paragraphs),
        pictures=len(doc.inline_shapes),
    )


def main(src, out):
    p = pdf_stats(src)
    d = docx_stats(out)
    print("source  %s" % src)
    print("  pages  %d   page %.0fx%.0f  body %.1f  words %d"
          % (p["pages"], p["page"][0], p["page"][1], p["body"], p["words"]))
    print("  frame  x %.0f..%.0f (%.0f)  y %.0f..%.0f (%.0f)"
          % (p["frame"][0], p["frame"][2], p["frame"][2] - p["frame"][0],
             p["frame"][1], p["frame"][3], p["frame"][3] - p["frame"][1]))
    print("docx    %s" % out)
    print("  page %.0fx%.0f  body %.1f  words %d  paras %d  pics %d"
          % (d["page"][0], d["page"][1], d["body"], d["words"], d["paras"],
             d["pictures"]))
    print("  frame  x %.0f..%.0f (%.0f)  y %.0f..%.0f (%.0f)"
          % (d["frame"][0], d["frame"][2], d["frame"][2] - d["frame"][0],
             d["frame"][1], d["frame"][3], d["frame"][3] - d["frame"][1]))
    pw = (p["frame"][2] - p["frame"][0]) * (p["frame"][3] - p["frame"][1])
    dw = (d["frame"][2] - d["frame"][0]) * (d["frame"][3] - d["frame"][1])
    print("  frame area ratio docx/source = %.3f" % (dw / pw if pw else 0))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
