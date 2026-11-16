"""End-to-end test for photo scanning (crop / deskew / flatten / enhance).

Assertions that matter:
  * the tool is advertised in config,
  * a photo -> PDF scan produces a real PDF with one page per photo,
  * a photo -> images scan produces a zip of cleaned JPG/PNG,
  * grayscale / bw / dpi options are reflected in the result report,
  * disabling every enhancement still produces a valid PDF (no stage crashes),
  * wrong input types and bad options are refused before any work is queued,
  * the /media/scan page renders and exposes the controls.
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
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("scan", pages=100, workers=2)
BASE = srv.base
DEV = "test-scan-0001"
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


def upload(path, mime="image/jpeg"):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime)
    return up["file_id"]


def run_scan(fids, options):
    j = req("POST", "/api/v1/jobs",
            body={"task": "scan",
                  "files": [{"file_id": x} for x in fids],
                  "options": options})
    assert j["task"] == "scan", j
    st = None
    for _ in range(900):
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
# A "photo" of a bright sheet on a darker desk: crop should find the sheet,
# deskew should not run wild, and flatten should lift the shadow.
def make_photo(path, angle=6):
    im = Image.new("RGB", (900, 650), (70, 70, 78))
    d = ImageDraw.Draw(im)
    d.rectangle((120, 70, 780, 580), fill=(245, 243, 235))
    for i in range(9):
        y = 110 + i * 50
        d.rectangle((170, y, 700 - (i % 3) * 60, y + 16), fill=(40, 40, 45))
    if angle:
        im = im.rotate(angle, expand=False, fillcolor=(70, 70, 78))
    im.save(path, "JPEG")


photo1 = "work/_bench/scan_a.jpg"
photo2 = "work/_bench/scan_b.png"
make_photo(photo1, angle=6)
Image.open(photo1).save(photo2, "PNG")

print("1. config advertises the scan tool")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
assert "scan" in ids, ids
scan = [t for t in cfg["tools"] if t["id"] == "scan"][0]
assert scan["path"] == "/media/scan" and scan["src"] == "image", scan
print("   tools:", len(ids))

print("2. two photos -> PDF with two pages, defaults")
f1 = upload(photo1)
f2 = upload(photo2, "image/png")
st = run_scan([f1, f2], {"name": "my scan"})
blob, ctype = download(st)
assert ctype.startswith("application/pdf"), ctype
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 2, d.page_count
first = d[0].get_pixmap(dpi=72)
assert first.width > 100 and first.height > 100, (first.width, first.height)
d.close()
assert st["result"]["mode"] == "color" and st["result"]["dpi"] == 200, st["result"]
print("   pages=%d, result=%s" % (st["pages"], st["result"]))

print("3. photo -> images zip with grayscale + 150 dpi")
fid = upload(photo1)
st = run_scan([fid], {"fmt": "images", "mode": "gray", "dpi": 150})
blob, ctype = download(st)
assert ctype.startswith("application/zip"), ctype
z = zipfile.ZipFile(io.BytesIO(blob))
names = z.namelist()
assert len(names) == 1 and names[0].lower().endswith((".jpg", ".png")), names
im = Image.open(io.BytesIO(z.read(names[0])))
assert im.mode == "RGB", im.mode
print("   member=%s size=%s result=%s" % (names[0], im.size, st["result"]))

print("4. black-and-white mode still produces a valid PDF")
fid = upload(photo1)
st = run_scan([fid], {"mode": "bw", "dpi": 300})
blob, _ = download(st)
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 1, d.page_count
d.close()
assert st["result"]["mode"] == "bw" and st["result"]["dpi"] == 300, st["result"]
print("   ", st["result"])

print("5. all enhancement stages off -> still a valid PDF")
fid = upload(photo1)
st = run_scan([fid], {"crop": False, "deskew": False, "flatten": False,
                      "denoise": False, "sharpen": False})
blob, _ = download(st)
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 1, d.page_count
d.close()
print("   ", st["result"])

print("6. a PDF input is refused (wrong_type)")
pdf_path = "work/_bench/scan_src.pdf"
pd = fitz.open()
pd.new_page().insert_text((72, 72), "not a photo")
pd.save(pdf_path)
pd.close()
fid = upload(pdf_path)
err = req("POST", "/api/v1/jobs",
          body={"task": "scan", "files": [{"file_id": fid}],
                "options": {"mode": "color"}}, expect=400)
assert err.get("code") == "wrong_type", err
print("   ", err.get("code"), "-", err.get("message"))

print("7. bad options are refused")
fid = upload(photo1)
cases = [
    ({"mode": "sepia"}, "bad_mode"),
    ({"dpi": 72}, "bad_dpi"),
    ({"fmt": "gif"}, "bad_fmt"),
]
for opts, code in cases:
    e = req("POST", "/api/v1/jobs",
            body={"task": "scan", "files": [{"file_id": fid}],
                  "options": opts}, expect=400)
    assert e.get("code") == code, (opts, e)
    print("   ", opts, "->", e.get("code"))

print("8. the /media/scan page renders with its controls")
with urllib.request.urlopen(BASE + "/media/scan") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "拍照扫描" in html, r.status
for token in ("自动裁边", "拉正矫正", "去除阴影", "黑白", "dpi"):
    assert token in html, token
print("   scan page rendered")

print("9. id-card tool is advertised")
assert "idcard" in ids, ids
card_tool = [t for t in cfg["tools"] if t["id"] == "idcard"][0]
assert card_tool["path"] == "/media/idcard", card_tool

# A card-like photo: bright rectangle with 1.586 aspect on a darker desk.
def make_card(path, label):
    im = Image.new("RGB", (900, 650), (60, 62, 70))
    d = ImageDraw.Draw(im)
    cw, ch = 720, 454
    x0, y0 = 90, 98
    d.rectangle((x0, y0, x0 + cw, y0 + ch), fill=(238, 236, 228))
    d.rectangle((x0 + 30, y0 + 40, x0 + 190, y0 + 170), fill=(120, 150, 170))
    for i in range(6):
        y = y0 + 200 + i * 38
        d.rectangle((x0 + 30, y, x0 + 560 - (i % 2) * 90, y + 14), fill=(45, 45, 52))
    d.text((x0 + 30, y0 + 415), label, fill=(30, 30, 35))
    im.save(path, "JPEG")


print("10. two card photos -> one A4 PDF page")
cf = "work/_bench/card_front.jpg"
cb = "work/_bench/card_back.jpg"
make_card(cf, "FRONT"); make_card(cb, "BACK")
fid_f = upload(cf); fid_b = upload(cb)
st = run_scan([fid_f, fid_b], {"op": "idcard", "mode": "color", "dpi": 200})
blob, ctype = download(st)
assert ctype.startswith("application/pdf"), ctype
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 1, d.page_count
p = d[0]
# A4 is 595 x 842 PDF points (PIL writes the DPI into the page size).
assert 590 < p.rect.width < 600 and 838 < p.rect.height < 846, p.rect
d.close()
assert st["result"]["op"] == "idcard" and st["result"]["images"] == 2, st["result"]
print("   ", st["result"])

print("11. one card photo still produces a centred A4 page")
fid = upload(cf)
st = run_scan([fid], {"op": "idcard"})
blob, _ = download(st)
d = fitz.open(stream=blob, filetype="pdf")
assert d.page_count == 1, d.page_count
d.close()
assert st["result"]["images"] == 1, st["result"]
print("   ", st["result"])

print("12. id-card refuses a zip format and more than two photos")
fid = upload(cf)
e = req("POST", "/api/v1/jobs",
        body={"task": "scan", "files": [{"file_id": fid}],
              "options": {"op": "idcard", "fmt": "images"}}, expect=400)
assert e.get("code") == "bad_fmt", e
e2 = req("POST", "/api/v1/jobs",
         body={"task": "scan", "files": [{"file_id": fid}] * 3,
               "options": {"op": "idcard"}}, expect=400)
assert e2.get("code") == "wrong_file_count", e2
print("   ", e.get("code"), "|", e2.get("code"))

print("13. the /media/idcard page renders with front/back slots")
with urllib.request.urlopen(BASE + "/media/idcard") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "证件正面" in html and "证件背面" in html, r.status
assert "idcard" in html
print("   idcard page rendered")

req("GET", "/media/nope", expect=404)
print("\nALL OK")
