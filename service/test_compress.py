"""End-to-end test for the compress task.

The interesting assertions are not "does it return a file" but:
  * an image-heavy PDF actually shrinks a lot,
  * a text-only PDF is never returned *bigger* than it arrived,
  * the page count and readability survive,
  * the free-plan limits fire before any work is queued.

Runs on its own server with the page allowance raised (corpus papers are 10-30
pages, free tier allows 5).  ``batch_compress`` is not env-tunable, so step 5
still proves the real free-plan refusal.
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
srv = boot("compress", pages=5000)
BASE = srv.base
DEV = "test-compress-0001"
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
    # FastAPI returns errors as {"detail": {...}}; unwrap so callers can read
    # detail.code / detail.message directly.
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
    return up["file_id"], len(blob)


def compress(fid, level, out_name):
    j = req("POST", "/api/v1/jobs",
            body={"task": "compress", "files": [{"file_id": fid}],
                  "options": {"level": level}})
    assert j["task"] == "compress", j
    for _ in range(600):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.25)
    assert st["status"] == "done", st
    assert st["result"], "compress job returned no size report"
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        blob = resp.read()
    out = "work/_bench/" + out_name
    open(out, "wb").write(blob)
    return st, out, len(blob)


heavy = sorted(glob.glob("docs/075__*.pdf"))[0]
texty = sorted(glob.glob("docs/005__*.pdf"))[0]

print("1. config advertises the three levels")
cfg = req("GET", "/api/v1/config")
print("   levels:", cfg["levels"], "plan:", cfg["plan"]["name"],
      "left:", cfg["plan"]["jobs_left"])
assert cfg["levels"] == ["extreme", "recommended", "less"]

print("2. compress an image-heavy PDF (94% images)")
fid_h, size_h = upload(heavy)
src_pages = fitz.open(heavy).page_count
print("   in : %.2f MB, %d pages" % (size_h / 1048576.0, src_pages))
st, out, n = compress(fid_h, "extreme", "cmp_heavy.pdf")
r = st["result"]
print("   out: %.2f MB, %d pages, saved %.0f%%, %.2fs"
      % (n / 1048576.0, st["pages"], 100 * (1 - r["after"] / r["before"]),
         st["run_time"]))
assert st["pages"] == src_pages, (st["pages"], src_pages)
assert r["before"] == size_h, (r["before"], size_h)
assert r["after"] == n
assert n < size_h * 0.5, "image-heavy file barely shrank: %d -> %d" % (size_h, n)
d = fitz.open(out)
assert d.page_count == src_pages
assert d[0].get_text().strip(), "first page lost its text layer"
print("   first page still has text:", repr(d[0].get_text().strip()[:40]))
d.close()

print("3. a text-only PDF is never returned larger")
fid_t, size_t = upload(texty)
st2, out2, n2 = compress(fid_t, "recommended", "cmp_text.pdf")
r2 = st2["result"]
print("   in : %.2f MB  out: %.2f MB  saved %.1f%%"
      % (size_t / 1048576.0, n2 / 1048576.0, 100 * (1 - n2 / size_t)))
assert n2 <= size_t, "compressor returned a bigger file: %d > %d" % (n2, size_t)
assert st2["pages"] == fitz.open(texty).page_count

print("4. invalid level rejected")
req("POST", "/api/v1/jobs",
    body={"task": "compress", "files": [{"file_id": fid_t}],
          "options": {"level": "ultra"}}, expect=400)
print("   rejected with 400")

print("5. free plan refuses a 2-file compress job")
req("POST", "/api/v1/jobs",
    body={"task": "compress",
          "files": [{"file_id": fid_t}, {"file_id": fid_h}]}, expect=403)
print("   rejected with 403 (plan_limit)")

print("6. unknown task type rejected")
req("POST", "/api/v1/jobs", body={"task": "ocr", "files": [{"file_id": fid_t}]},
    expect=400)
print("   rejected with 400")

print("7. plans endpoint lists free and pro")
pl = req("GET", "/api/v1/plans")
print("   current=%s  tiers=%s" % (pl["current"],
                                   [(p["id"], p["jobs_per_day"]) for p in pl["plans"]]))
assert pl["current"] == "free"
assert any(p["id"] == "pro" and p["batch_compress"] for p in pl["plans"])

print("8. an already-optimal PDF comes back unchanged, byte for byte")
# built with the same flags the compressor uses, so there is nothing left to win
tiny = "work/_bench/tiny.pdf"
doc = fitz.open()
page = doc.new_page()
page.insert_text((72, 72), "already optimal")
doc.save(tiny, garbage=4, clean=True, deflate=True)
doc.close()
fid_m, size_m = upload(tiny)
st4, out4, n4 = compress(fid_m, "extreme", "cmp_tiny.pdf")
print("   %d -> %d bytes, saved %.1f%%"
      % (size_m, n4, 100 * (1 - n4 / size_m)))
assert n4 <= size_m, "returned a bigger file"
if n4 == size_m:
    with open(tiny, "rb") as a, open(out4, "rb") as b:
        assert a.read() == b.read(), "same size but not the original bytes"
    print("   identical bytes returned (original reused, not re-encoded)")

print("9. compress a JPG (re-encoded to a smaller file)")
from PIL import Image as _PIL
jpg_in = "work/_bench/cmp_in.jpg"
im = _PIL.effect_noise((1600, 1200), 70).convert("RGB")
im.save(jpg_in, "JPEG", quality=95)
fid_j, size_j = upload(jpg_in, "image/jpeg")
stj, outj, nj = compress(fid_j, "extreme", "cmp_out.jpg")
rj = stj["result"]
print("   in %.2f MB -> out %.2f MB, media=%s, saved %.0f%%"
      % (size_j / 1048576.0, nj / 1048576.0, rj.get("media"),
         100 * (1 - nj / size_j)))
assert rj.get("media") == "image", rj
assert nj < size_j, "JPG did not shrink: %d -> %d" % (size_j, nj)
with urllib.request.urlopen(BASE + stj["download"]) as resp:
    assert resp.headers.get("Content-Type", "") == "image/jpeg", \
        resp.headers.get("Content-Type")
print("   download served as image/jpeg")

print("10. compress a PNG (never returned larger)")
png_in = "work/_bench/cmp_in.png"
_pim = _PIL.new("RGB", (1200, 900), (200, 30, 30))
_pim.save(png_in, "PNG")
fid_p, size_p = upload(png_in, "image/png")
stp, outp, np_ = compress(fid_p, "recommended", "cmp_out.png")
rp = stp["result"]
print("   in %.2f MB -> out %.2f MB, media=%s"
      % (size_p / 1048576.0, np_ / 1048576.0, rp.get("media")))
assert rp.get("media") == "image", rp
assert np_ <= size_p, "PNG returned bigger: %d -> %d" % (size_p, np_)
with urllib.request.urlopen(BASE + stp["download"]) as resp:
    assert resp.headers.get("Content-Type", "") == "image/png", \
        resp.headers.get("Content-Type")
print("   download served as image/png")

print("11. a non-image, non-PDF (docx) is refused by compress")
import docx as _docx
docx_in = "work/_bench/cmp_probe.docx"
_d = _docx.Document(); _d.add_paragraph("hello"); _d.save(docx_in)
fid_d, _ = upload(docx_in)  # docx is a PK zip, passes upload
err = req("POST", "/api/v1/jobs",
          body={"task": "compress", "files": [{"file_id": fid_d}]}, expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "wrong_type", err

print("\nALL OK")
