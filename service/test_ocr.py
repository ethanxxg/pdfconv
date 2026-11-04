"""End-to-end test for OCR on scanned PDFs.

What this proves:
  * OCR is enabled on the free tier (plan.ocr is true),
  * a scanned (image-only) PDF is refused with ``needs_ocr`` when OCR is off,
  * the same PDF converts to Word with OCR on, and the DOCX really contains
    the recognised text,
  * turning OCR on for a born-digital PDF is a harmless no-op.

The OCR engine (rapidocr_onnxruntime) is optional at runtime; if it is not
installed this suite skips rather than fails, so the rest of the service still
tests on a machine without it.
"""
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request

import pymupdf as fitz

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402
from pdfconv import ocr as _ocr  # noqa: E402

if not _ocr.available():
    print("SKIP: rapidocr_onnxruntime not installed")
    raise SystemExit(0)

os.chdir(ROOT)
srv = boot("ocr", pages=5000, extra_env={"PDFM_FREE_OCR": "1"})
BASE = srv.base
DEV = "test-ocr-0001"
os.makedirs("work/_bench", exist_ok=True)


def req(method, path, body=None, raw=None, ctype="application/pdf", expect=200):
    url = BASE + path
    h = {"X-Device-Id": DEV}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    elif raw is not None:
        data = raw
        h["Content-Type"] = ctype
    r = urllib.request.Request(url, data=data, headers=h, method=method)
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
    if isinstance(obj, dict) and set(obj) == {"detail"} \
            and isinstance(obj["detail"], dict):
        return obj["detail"]
    return obj


def upload(path):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        req("PUT", up["url"], raw=f.read(), ctype="application/pdf")
    return up["file_id"]


def convert(fid, tool, ocr=False, name="scan.pdf"):
    j = req("POST", "/api/v1/jobs",
            body={"task": "convert", "files": [{"file_id": fid}],
                  "options": {"tool": tool, "ocr": ocr, "name": name}})
    st = None
    for _ in range(600):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.25)
    return st


def download(st):
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        return resp.read()


def docx_text(blob):
    import docx
    d = docx.Document(io.BytesIO(blob))
    parts = [p.text for p in d.paragraphs]
    for t in d.tables:
        for row in t.rows:
            for cell in row.cells:
                parts.append(cell.text)
    return "\n".join(parts)


# ---- fixtures ------------------------------------------------------------
# A scanned page: render text to an image, then wrap the image in a PDF.  The
# result has no text layer at all -- exactly what a scanner produces.
from pdfconv.pages import images_to_pdf  # noqa: E402

scan_png = "work/_bench/ocr_scan.png"
doc = fitz.open()
p = doc.new_page()
p.insert_text((80, 320), "Invoice Total 42", fontsize=40)
pix = p.get_pixmap(dpi=200)
pix.save(scan_png)
doc.close()
scan_pdf = "work/_bench/ocr_scan.pdf"
images_to_pdf([scan_png], scan_pdf)

born_pdf = "work/_bench/ocr_born.pdf"
doc = fitz.open()
p = doc.new_page()
p.insert_text((72, 90), "Born digital text layer", fontsize=16)
doc.save(born_pdf)
doc.close()

print("1. OCR is enabled on the free tier")
cfg = req("GET", "/api/v1/config")
print("   plan=%s ocr=%s" % (cfg["plan"]["name"], cfg["plan"]["ocr"]))
assert cfg["plan"]["ocr"] is True, cfg["plan"]

print("2. a scanned PDF is refused with needs_ocr when OCR is off")
fid = upload(scan_pdf)
st = convert(fid, "pdf-to-word", ocr=False)
print("   status=%s code=%s" % (st["status"], (st.get("error") or {}).get("code")))
assert st["status"] == "failed", st
assert st["error"]["code"] == "needs_ocr", st["error"]

print("3. the same scanned PDF converts with OCR on")
fid = upload(scan_pdf)
st = convert(fid, "pdf-to-word", ocr=True)
print("   status=%s pages=%s ocr=%s run_time=%.1fs"
      % (st["status"], st.get("pages"), st["result"].get("ocr") if st.get("result") else None,
         st.get("run_time") or 0))
assert st["status"] == "done", st
assert st.get("result", {}).get("ocr") is True, st.get("result")
text = docx_text(download(st))
print("   extracted text: %r" % text.strip()[:80])
low = text.lower()
assert "42" in text or "invoice" in low, "OCR text missing from DOCX: %r" % text

print("4. OCR on a born-digital PDF is a no-op")
fid = upload(born_pdf)
st = convert(fid, "pdf-to-word", ocr=True)
print("   status=%s" % st["status"])
assert st["status"] == "done", st
text2 = docx_text(download(st))
print("   extracted text: %r" % text2.strip()[:80])
assert "born digital" in text2.lower(), text2

print("\nALL OK")
