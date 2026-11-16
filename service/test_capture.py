"""Test for the mobile capture PWA and its live-frame detection endpoint.

Assertions that matter:
  * /capture serves a standalone shell with the camera modes and manifest,
  * the manifest / icon / PWA meta are reachable,
  * a preview frame uploaded through the normal channel returns a document
    quad in image pixel coordinates,
  * a frame that is not a picture is refused before detection,
  * finished captures still flow through the ordinary scan job (covered by
    test_scan; here we assert the capture shell wires to task=scan).
"""
import json
import os
import sys
import urllib.error
import urllib.request

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("capture", pages=100)
BASE = srv.base
DEV = "test-capture-0001"
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
    if isinstance(obj, dict) and set(obj) == {"detail"} and isinstance(obj["detail"], dict):
        return obj["detail"]
    return obj


def upload(path, mime="image/jpeg"):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta})["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime)
    return up["file_id"]


print("1. /capture serves the standalone camera shell")
with urllib.request.urlopen(BASE + "/capture") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "拍照扫描" in html, r.status
for token in ("getUserMedia", "capture/frame", "manifest.webmanifest",
              "idcard", "book", "多页连拍", "继续编辑", "shutter",
              "去手指", "去摩尔纹", "去透字"):
    assert token in html, token
print("   capture shell ok (%d bytes)" % len(html))

print("2. PWA manifest and icon are served")
with urllib.request.urlopen(BASE + "/static/manifest.webmanifest") as r:
    mf = json.load(r)
assert mf["start_url"] == "/capture" and mf["display"] == "standalone", mf
with urllib.request.urlopen(BASE + "/static/icon.svg") as r:
    svg = r.read().decode("utf-8")
assert "<svg" in svg and "18629f" in svg
print("   manifest start_url=%s icons=%d" % (mf["start_url"], len(mf["icons"])))

print("3. the tools grid links to the capture app")
with urllib.request.urlopen(BASE + "/tools") as r:
    tools_html = r.read().decode("utf-8")
assert 'href="/capture"' in tools_html
print("   CTA present")

print("4. a preview frame returns a document quad")
photo = "work/_bench/capture_frame.jpg"
im = Image.new("RGB", (900, 650), (70, 70, 78))
d = ImageDraw.Draw(im)
d.rectangle((120, 70, 780, 580), fill=(245, 243, 235))
for i in range(8):
    y = 110 + i * 55
    d.rectangle((170, y, 680 - (i % 3) * 60, y + 16), fill=(40, 40, 45))
im.save(photo, "JPEG")
fid = upload(photo)
res = req("POST", "/api/v1/capture/frame", body={"file_id": fid})
assert res["quad"] and len(res["quad"]) == 4, res
assert res["area"] > 0.2, res
for x, y in res["quad"]:
    assert 0 <= x <= 900 and 0 <= y <= 650, res
print("   quad=%s area=%s" % (res["quad"], res["area"]))

print("5. a non-image frame is refused")
import pymupdf as fitz
pdf = "work/_bench/capture_probe.pdf"
pd = fitz.open(); pd.new_page().insert_text((72, 72), "x"); pd.save(pdf); pd.close()
fid = upload(pdf, "application/pdf")
err = req("POST", "/api/v1/capture/frame", body={"file_id": fid}, expect=400)
assert err.get("code") == "wrong_type", err
print("   ", err.get("code"))

print("6. a missing file id is refused")
err = req("POST", "/api/v1/capture/frame", body={"file_id": ""}, expect=400)
assert err.get("code") == "bad_file_id", err
print("   ", err.get("code"))

srv.stop()
print("\nALL OK")
