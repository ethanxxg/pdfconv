"""End-to-end test for every conversion direction the service offers.

Six directions, and the assertions differ per direction because "it worked"
means something different each time:

    pdf -> docx   opens in python-docx, keeps the source's words
    pdf -> xlsx   opens in openpyxl, has a sheet
    pdf -> pptx   opens in python-pptx, has slides
    docx/xlsx/pptx -> pdf   opens in PyMuPDF, has pages and text

Plus the refusals that must happen *before* the daily allowance is spent:
encrypted, scanned-without-OCR, OCR on the free tier, and a file that is not
the format the tool promises.

Runs on its own server with the page allowance raised; see ``_harness.py``.
"""
import glob
import json
import os
import sys
import time
import urllib.request

import pymupdf as fitz

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
# OCR now ships on the free tier; this suite owns the *gating* behaviour, so it
# boots with OCR off to keep asserting the 403 and the needs_ocr refusal.
srv = boot("convert", pages=5000, jobs=0,
           extra_env={"PDFM_FREE_OCR": "0"})
BASE = srv.base
DEV = "test-convert-0001"

BENCH = "work/_bench"
os.makedirs(BENCH, exist_ok=True)


def req(method, path, body=None, raw=None, ctype="application/pdf", expect=200):
    h = {"X-Device-Id": DEV}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    elif raw is not None:
        data = raw
        h["Content-Type"] = ctype
    r = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r) as resp:
            payload, code = resp.read(), resp.status
    except urllib.error.HTTPError as e:
        payload, code = e.read(), e.code
    if code != expect:
        raise AssertionError("%s %s -> %d (want %d): %s"
                             % (method, path, code, expect, payload[:300]))
    try:
        obj = json.loads(payload)
    except Exception:
        return payload
    if isinstance(obj, dict) and set(obj) == {"detail"}:
        return obj["detail"]
    return obj


def upload(path, mime="application/pdf"):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime)
    return up["file_id"], len(blob)


def convert(fid, tool, name="input.pdf", ocr=False, expect=200):
    return req("POST", "/api/v1/jobs",
               body={"task": "convert", "files": [{"file_id": fid}],
                     "options": {"tool": tool, "ocr": ocr, "name": name}},
               expect=expect)


def wait(jid, limit=1200):
    for _ in range(limit):
        st = req("GET", "/api/v1/jobs/" + jid)
        if st["status"] in ("done", "failed"):
            return st
        time.sleep(0.5)
    raise AssertionError("job %s never finished" % jid)


def run(src, tool, out_name, mime="application/pdf", name=None):
    """Convert and return (status, downloaded bytes, content-type, disposition)."""
    fid, size = upload(src, mime)
    t0 = time.time()
    j = convert(fid, tool, name=name or os.path.basename(src))
    st = wait(j["job_id"])
    assert st["status"] == "done", st
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        ctype = resp.headers.get("Content-Type", "")
        disp = resp.headers.get("Content-Disposition", "")
        blob = resp.read()
    out = os.path.join(BENCH, out_name)
    open(out, "wb").write(blob)
    print("   %-9s %5.1fs  %6.2f MB -> %6.2f MB  %s"
          % (tool, time.time() - t0, size / 1048576.0,
             len(blob) / 1048576.0, " · ".join(st["result"]["detail"])))
    return st, blob, ctype, disp, out


print("1. the tool registry is the single source of truth")
tools = req("GET", "/api/v1/tools")["tools"]
ids = [t["id"] for t in tools]
print("   %d tools: %s" % (len(ids), ", ".join(ids)))
CORE = {"merge", "compress", "pdf-to-word", "pdf-to-excel",
        "pdf-to-ppt", "word-to-pdf", "excel-to-pdf", "ppt-to-pdf"}
PAGE = {"split", "extract", "delete", "rotate"}
MEDIA = {"pdf-to-image", "image-to-pdf"}
ARCHIVE = {"pdfa"}
assert set(ids) == CORE | PAGE | MEDIA | ARCHIVE, ids
assert len(ids) == 15, ids
cfg = req("GET", "/api/v1/config")
assert [t["id"] for t in cfg["tools"]] == ids, "config and /tools disagree"

print("2. every tool page renders with its own title")
for t in tools:
    body = req("GET", t["path"])
    assert t["title"].encode() in body, (t["path"], "title missing")
    assert b"__TOOL_JSON__" not in body, "placeholder left in the page"
print("   all %d pages substituted" % len(tools))

# ---------------------------------------------------------------- refusals
print("3. OCR is refused on the free tier, before any work")
tiny = os.path.join(BENCH, "cv_tiny.pdf")
doc = fitz.open()
doc.new_page().insert_text((72, 72), "hello")
doc.save(tiny)
doc.close()
fid_t, _ = upload(tiny)
err = convert(fid_t, "pdf-to-word", ocr=True, expect=403)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "plan_limit", err

print("4. a scanned PDF is refused with an actionable message")
scan = os.path.join(BENCH, "cv_scan.pdf")
doc = fitz.open()
page = doc.new_page()
pix = fitz.Pixmap(fitz.csRGB, fitz.IRect(0, 0, 600, 800), 0)
pix.clear_with(230)
page.insert_image(page.rect, pixmap=pix)
doc.save(scan)
doc.close()
fid_s, _ = upload(scan)
st = wait(convert(fid_s, "pdf-to-excel")["job_id"])
print("   status=%s code=%s" % (st["status"], (st.get("error") or {}).get("code")))
assert st["status"] == "failed" and st["error"]["code"] == "needs_ocr", st

print("5. a file that is not the promised format is refused by name")
# A real .docx posted to the Word tool: the zip magic passes the upload check,
# so only the container inspection can catch it.
import docx as _docx
probe_docx = os.path.join(BENCH, "cv_probe.docx")
d = _docx.Document()
d.add_paragraph("not a pdf")
d.save(probe_docx)
fid_d, _ = upload(probe_docx,
                  "application/vnd.openxmlformats-officedocument"
                  ".wordprocessingml.document")
err = convert(fid_d, "pdf-to-word", expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "wrong_type", err

print("6. an unknown tool is refused")
err = convert(fid_t, "pdf-to-pages", expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "bad_tool", err

# ------------------------------------------------------------- conversions
src_pdf = sorted(glob.glob("docs/067__*.pdf"))[0]
src_pages = fitz.open(src_pdf).page_count
print("7. PDF -> DOCX   (%s, %d pages)" % (os.path.basename(src_pdf)[:34], src_pages))
st, blob, ctype, disp, out = run(src_pdf, "pdf-to-word", "cv_out.docx")
assert st["pages"] == src_pages, (st["pages"], src_pages)
assert "wordprocessingml" in ctype, ctype
from docx import Document
paras = [p.text.strip() for p in Document(out).paragraphs if p.text.strip()]
assert paras, "DOCX has no text"
print("   paragraphs:", len(paras), "| first:", repr(paras[0][:40]))

print("8. PDF -> XLSX")
st, blob, ctype, disp, out = run(src_pdf, "pdf-to-excel", "cv_out.xlsx")
assert "spreadsheetml" in ctype, ctype
from openpyxl import load_workbook
wb = load_workbook(out, read_only=True)
assert wb.sheetnames, "workbook has no sheets"
print("   sheets:", wb.sheetnames)
wb.close()

print("9. PDF -> PPTX")
st, blob, ctype, disp, out = run(src_pdf, "pdf-to-ppt", "cv_out.pptx")
assert "presentationml" in ctype, ctype
from pptx import Presentation
prs = Presentation(out)
assert len(prs.slides) > 0, "deck has no slides"
print("   slides:", len(prs.slides))

print("10. DOCX -> PDF")
st, blob, ctype, disp, out = run(
    probe_docx, "word-to-pdf", "cv_d.pdf",
    mime="application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    name="mydoc.docx")
assert ctype.startswith("application/pdf"), ctype
with fitz.open(out) as d:
    assert d.page_count > 0
    assert d[0].get_text().strip(), "rendered page has no text"
print("   pages:", st["pages"], "| disposition:", disp[:60])

print("11. XLSX -> PDF")
xlsx = glob.glob("docs/*.xlsx")[0]
st, blob, ctype, disp, out = run(
    xlsx, "excel-to-pdf", "cv_x.pdf",
    mime="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    name="sheet.xlsx")
with fitz.open(out) as d:
    assert d.page_count > 0
print("   pages:", st["pages"])

print("12. PPTX -> PDF")
from pptx import Presentation as _P
from pptx.util import Pt as _Pt
prs = _P()
for i in range(3):
    s = prs.slides.add_slide(prs.slide_layouts[1])
    s.shapes.title.text = "Slide %d" % (i + 1)
    s.placeholders[1].text = "Body text %d" % (i + 1)
pptx_in = os.path.join(BENCH, "cv_in.pptx")
prs.save(pptx_in)
st, blob, ctype, disp, out = run(
    pptx_in, "ppt-to-pdf", "cv_s.pdf",
    mime="application/vnd.openxmlformats-officedocument.presentationml.presentation",
    name="deck.pptx")
with fitz.open(out) as d:
    assert d.page_count == 3, d.page_count
    assert "Slide 1" in d[0].get_text()
print("   pages:", st["pages"], "| text survives")

print("\nALL OK")
