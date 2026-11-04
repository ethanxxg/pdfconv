"""End-to-end test for the media tools (PDF -> image, image -> PDF).

Assertions that matter:
  * PDF -> PNG/JPG returns a zip with one real image per page,
  * image -> PDF returns a PDF with one page per input image,
  * wrong input types are refused with ``wrong_type``,
  * a bad format / DPI / op is refused before any work is queued,
  * the rendered /media/{tool} routes exist and 404 for an unknown tool.
"""
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zipfile

import pymupdf as fitz
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("media", pages=5000)
BASE = srv.base
DEV = "test-media-0001"
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


def upload(path, mime="application/pdf"):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime)
    return up["file_id"]


def run_media(fids, options):
    j = req("POST", "/api/v1/jobs",
            body={"task": "media",
                  "files": [{"file_id": x} for x in fids],
                  "options": options})
    assert j["task"] == "media", j
    st = None
    for _ in range(600):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.2)
    assert st["status"] == "done", st
    return st


def download(st):
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        return resp.read(), resp.headers.get("Content-Type", "")


# ---- fixtures ------------------------------------------------------------
pdf_src = "work/_bench/media_src.pdf"
doc = fitz.open()
for i in range(3):
    p = doc.new_page()
    p.insert_text((72, 72), "page %d" % (i + 1))
doc.save(pdf_src)
doc.close()

jpg_src = "work/_bench/media_a.jpg"
Image.new("RGB", (600, 400), (200, 30, 30)).save(jpg_src, "JPEG")
png_src = "work/_bench/media_b.png"
Image.new("RGB", (500, 700), (30, 120, 200)).save(png_src, "PNG")

print("1. config lists the two media tools")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
assert "pdf-to-image" in ids and "image-to-pdf" in ids, ids
print("   tools tail:", ids[-4:])

print("2. PDF -> PNG: zip with one PNG per page")
fid = upload(pdf_src)
st = run_media([fid], {"op": "pdf2img", "fmt": "png", "dpi": 72,
                       "name": "media_src.pdf"})
blob, ctype = download(st)
print("   content-type=%s, images=%s" % (ctype, st["result"].get("images")))
assert ctype.startswith("application/zip"), ctype
z = zipfile.ZipFile(io.BytesIO(blob))
names = z.namelist()
assert len(names) == 3, names
for nm in names:
    assert nm.lower().endswith(".png"), nm
    assert z.read(nm)[:4] == b"\x89PNG", nm
print("   members:", names)

print("3. PDF -> JPG: zip with one JPG per page")
fid = upload(pdf_src)
st = run_media([fid], {"op": "pdf2img", "fmt": "jpg", "dpi": 150,
                       "name": "media_src.pdf"})
blob, ctype = download(st)
assert ctype.startswith("application/zip"), ctype
z = zipfile.ZipFile(io.BytesIO(blob))
names = z.namelist()
assert len(names) == 3, names
for nm in names:
    assert nm.lower().endswith(".jpg"), nm
    assert z.read(nm)[:3] == b"\xff\xd8\xff", nm
print("   members:", names)

print("4. two images -> PDF with two pages")
fid_j = upload(jpg_src, "image/jpeg")
fid_p = upload(png_src, "image/png")
st = run_media([fid_j, fid_p], {"op": "img2pdf", "name": "album"})
blob, ctype = download(st)
print("   content-type=%s, pages=%d" % (ctype, st["pages"]))
assert ctype.startswith("application/pdf"), ctype
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 2, d.page_count
d.close()

print("5. a single image -> one-page PDF")
fid_j = upload(jpg_src, "image/jpeg")
st = run_media([fid_j], {"op": "img2pdf", "name": "one"})
blob, _ = download(st)
d = fitz.open(stream=blob, filetype="pdf")
print("   pages=%d" % d.page_count)
assert d.page_count == 1, d.page_count
d.close()

print("6. PDF -> image refuses a docx (wrong_type)")
import docx as _docx
probe = "work/_bench/media_probe.docx"
_d = _docx.Document(); _d.add_paragraph("hi"); _d.save(probe)
fid_d = upload(probe)
err = req("POST", "/api/v1/jobs",
          body={"task": "media", "files": [{"file_id": fid_d}],
                "options": {"op": "pdf2img", "fmt": "png", "dpi": 150}},
          expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "wrong_type", err

print("7. image -> PDF refuses a PDF input (wrong_type)")
fid_pdf = upload(pdf_src)
err = req("POST", "/api/v1/jobs",
          body={"task": "media", "files": [{"file_id": fid_pdf}],
                "options": {"op": "img2pdf"}}, expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "wrong_type", err

print("8. PDF -> image with two files is refused (one_file)")
fid_a = upload(pdf_src)
fid_b = upload(pdf_src)
err = req("POST", "/api/v1/jobs",
          body={"task": "media",
                "files": [{"file_id": fid_a}, {"file_id": fid_b}],
                "options": {"op": "pdf2img", "fmt": "png", "dpi": 150}},
          expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "one_file", err

print("9. a bad format / DPI / op is refused")
fid = upload(pdf_src)
e1 = req("POST", "/api/v1/jobs",
         body={"task": "media", "files": [{"file_id": fid}],
               "options": {"op": "pdf2img", "fmt": "gif", "dpi": 150}},
         expect=400)
e2 = req("POST", "/api/v1/jobs",
         body={"task": "media", "files": [{"file_id": fid}],
               "options": {"op": "pdf2img", "fmt": "png", "dpi": 123}},
         expect=400)
e3 = req("POST", "/api/v1/jobs",
         body={"task": "media", "files": [{"file_id": fid}],
               "options": {"op": "shred"}}, expect=400)
print("   ", e1.get("code"), "|", e2.get("code"), "|", e3.get("code"))
assert (e1.get("code"), e2.get("code"), e3.get("code")) == \
    ("bad_fmt", "bad_dpi", "bad_op")

print("10. the /media/{tool} routes render and 404 for unknown")
with urllib.request.urlopen(BASE + "/media/pdf-to-image") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "PDF 转图片" in html, r.status
assert "图片格式" in html and "分辨率" in html
with urllib.request.urlopen(BASE + "/media/image-to-pdf") as r:
    html2 = r.read().decode("utf-8")
assert r.status == 200 and "图片转 PDF" in html2, r.status
assert "选择 JPG / PNG 图片" in html2
print("   both media pages rendered")
req("GET", "/media/nope", expect=404)

print("\nALL OK")
