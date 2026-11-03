"""Render every regenerated DOCX to PDF and compare page counts to source.

Also reports token retention for documents whose PDF text layer is usable
(undecodable-CID PDFs have garbage text, so retention is meaningless there and
is skipped).  Writes work/vfix_pages.tsv.
"""
import os, re, collections, sys
import fitz
from docx import Document
from pdfconv.docx2pdf import convert_docx_pdf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "work", "to_docx_vfix")
DOCS = os.path.join(ROOT, "docs")
REND = os.path.join(ROOT, "work", "vfix_render")
os.makedirs(REND, exist_ok=True)
TSV = os.path.join(ROOT, "work", "vfix_pages.tsv")


def src_pdf_for(docx_name):
    base = os.path.splitext(docx_name)[0]
    p = os.path.join(DOCS, base + ".pdf")
    return p if os.path.exists(p) else None


def src_tokens(pdf):
    d = fitz.open(pdf)
    out = collections.Counter()
    garbage = 0
    for p in d:
        for b in p.get_text("dict")["blocks"]:
            if b.get("type", 0) != 0:
                continue
            for ln in b.get("lines") or ():
                t = "".join(s["text"] for s in ln["spans"])
                out.update(re.findall(r"[\u4e00-\u9fff]|[A-Za-z]{2,}", t))
    return out


def docx_tokens(path):
    d = Document(path)
    out = collections.Counter()
    for para in d.paragraphs:
        out.update(re.findall(r"[\u4e00-\u9fff]|[A-Za-z]{2,}", para.text))
    for t in d.tables:
        for row in t.rows:
            for c in row.cells:
                out.update(re.findall(r"[\u4e00-\u9fff]|[A-Za-z]{2,}", c.text))
    return out


rows = []
files = sorted(f for f in os.listdir(OUT) if f.lower().endswith(".docx"))
for i, f in enumerate(files):
    docx = os.path.join(OUT, f)
    pdf = src_pdf_for(f)
    if not pdf:
        rows.append((f, "NO_SRC", "", "", "", ""))
        continue
    sp = fitz.open(pdf)
    src_pages = len(sp)
    # quick undecodable-CID heuristic: many CJK chars render as rare/garbage?
    sp.close()
    rend = os.path.join(REND, f + ".pdf")
    try:
        n, _p, _t = convert_docx_pdf(docx, rend)
    except Exception as e:
        rows.append((f, src_pages, "RENDER_ERR", "", "", str(e)[:40]))
        continue
    retention = ""
    delta = n - src_pages
    try:
        s = src_tokens(pdf)
        b = docx_tokens(docx)
        # undecodable CID -> source token set is tiny/garbage; skip
        if sum(s.values()) > 50:
            rt = sum(b.values()) / max(1, sum(s.values()))
            retention = "%.3f" % rt
    except Exception:
        retention = "ERR"
    rows.append((f, src_pages, n, delta, retention, ""))
    print(f"[{i+1}/{len(files)}] {f[:40]:40} src={src_pages} out={n} d={delta} ret={retention}", flush=True)

with open(TSV, "w", encoding="utf-8") as fh:
    fh.write("file\tsrc_pages\tout_pages\tdelta\tretention\tnote\n")
    for r in rows:
        fh.write("\t".join(str(x) for x in r) + "\n")

bad = [r for r in rows if r[2] in ("NO_SRC", "RENDER_ERR") or (isinstance(r[3], int) and abs(r[3]) > 3)]
print(f"\nTOTAL {len(rows)}  PROBLEM {len(bad)}")
for r in bad:
    print("  ", r)
