"""End-to-end test for the standalone image-to-text tool.

Assertions that matter:
  * the tool is advertised and the page renders,
  * one image -> a real UTF-8 TXT containing the printed text,
  * several images -> a zip with one .txt per image,
  * DOCX output is a real Word document holding the text,
  * a blank picture fails with no_text (not a hang or an empty download),
  * wrong input types and bad options are refused before any work is queued.
"""
import glob
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request
import zipfile

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("ocrtext", pages=100)
BASE = srv.base
DEV = "test-ocrtext-0001"
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


def upload(path, mime="image/png"):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime)
    return up["file_id"]


def run_ocr(fids, options):
    j = req("POST", "/api/v1/jobs",
            body={"task": "ocr",
                  "files": [{"file_id": x} for x in fids],
                  "options": options})
    assert j["task"] == "ocr", j
    st = None
    for _ in range(900):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.2)
    return st


def download(st):
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        return resp.read(), resp.headers.get("Content-Type", "")


# ---- fixtures ------------------------------------------------------------
def make_text_image(path, lines):
    fonts = glob.glob("C:/Windows/Fonts/msyh.ttc") + \
            glob.glob("C:/Windows/Fonts/arial.ttf")
    font = ImageFont.truetype(fonts[0], 42)
    im = Image.new("RGB", (900, max(220, 110 * len(lines))), "white")
    d = ImageDraw.Draw(im)
    for i, line in enumerate(lines):
        d.text((40, 40 + i * 110), line, font=font, fill="black")
    im.save(path, "PNG")


img1 = "work/_bench/ocr_text_a.png"
img2 = "work/_bench/ocr_text_b.png"
blank = "work/_bench/ocr_text_blank.png"
make_text_image(img1, ["Invoice 42", "Hello World 2026"])
make_text_image(img2, ["Report 2026", "PDF Toolkit"])
Image.new("RGB", (400, 200), "white").save(blank, "PNG")

print("1. the image-to-text tool is advertised")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
assert "image-to-text" in ids, ids
t = [x for x in cfg["tools"] if x["id"] == "image-to-text"][0]
assert t["path"] == "/media/image-to-text" and t["src"] == "image", t

print("2. one image -> a TXT with the printed text")
fid = upload(img1)
st = run_ocr([fid], {"fmt": "txt", "name": "invoice"})
assert st["status"] == "done", st
blob, ctype = download(st)
assert "text/plain" in ctype, ctype
text = blob.decode("utf-8")
# OCR has minor per-run variance ("42" may pick up a stray glyph); assert
# the stable, distinctive parts rather than an exact byte match.
assert "Invoice" in text and "Hello World" in text and "2026" in text, text
assert st["result"]["lines"] >= 2 and st["result"]["boxes"] >= 2, st["result"]
print("   text=%r result=%s" % (text.strip(), st["result"]))

print("3. two images -> a zip with one TXT each")
f1 = upload(img1); f2 = upload(img2)
st = run_ocr([f1, f2], {"fmt": "txt"})
assert st["status"] == "done", st
blob, ctype = download(st)
assert ctype.startswith("application/zip"), ctype
z = zipfile.ZipFile(io.BytesIO(blob))
names = z.namelist()
assert len(names) == 2 and all(n.endswith(".txt") for n in names), names
alltext = " ".join(z.read(n).decode("utf-8") for n in names)
assert "Invoice" in alltext and "Report" in alltext and "2026" in alltext, alltext
print("   members:", names)

print("4. DOCX output contains the text")
fid = upload(img1)
st = run_ocr([fid], {"fmt": "docx"})
assert st["status"] == "done", st
blob, ctype = download(st)
assert ctype.startswith("application/vnd.openxmlformats"), ctype
import docx as _docx
doc = _docx.Document(io.BytesIO(blob))
doctext = "\n".join(p.text for p in doc.paragraphs)
assert "Invoice" in doctext and "Hello World" in doctext and "2026" in doctext, doctext
print("   paragraphs:", len(doc.paragraphs))

print("5. a blank picture fails with no_text")
fid = upload(blank)
st = run_ocr([fid], {"fmt": "txt"})
assert st["status"] == "failed", st
assert st["error"]["code"] == "no_text", st["error"]
print("   ", st["error"]["code"], "-", st["error"]["message"])

print("6. a PDF input is refused (wrong_type)")
import pymupdf as fitz
pdf_path = "work/_bench/ocr_probe_src.pdf"
pd = fitz.open(); pd.new_page().insert_text((72, 72), "x"); pd.save(pdf_path); pd.close()
fid = upload(pdf_path, "application/pdf")
err = req("POST", "/api/v1/jobs",
          body={"task": "ocr", "files": [{"file_id": fid}],
                "options": {"fmt": "txt"}}, expect=400)
assert err.get("code") == "wrong_type", err
print("   ", err.get("code"))

print("7. a bad format is refused")
fid = upload(img1)
err = req("POST", "/api/v1/jobs",
          body={"task": "ocr", "files": [{"file_id": fid}],
                "options": {"fmt": "pdf"}}, expect=400)
assert err.get("code") == "bad_fmt", err
print("   ", err.get("code"))

print("8. the /media/image-to-text page renders")
with urllib.request.urlopen(BASE + "/media/image-to-text") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "图片转文字" in html, r.status
assert "Word 文档" in html and "开始识别" in html
print("   page rendered")

print("\nALL OK")
