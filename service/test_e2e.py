"""End-to-end smoke test for the merge service.

Boots its own server with the page allowance raised, because the corpus papers
are 10-30 pages and the shipped free tier allows 5.  See ``_harness.py``.
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
srv = boot("e2e", pages=5000)
BASE = srv.base
DEV = "test-device-0001"


def req(method, path, body=None, raw=None, headers=None, expect=200):
    url = BASE + path
    h = {"X-Device-Id": DEV}
    if headers:
        h.update(headers)
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    elif raw is not None:
        data = raw
        h["Content-Type"] = "application/pdf"
    r = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r) as resp:
            payload = resp.read()
            code = resp.status
    except urllib.error.HTTPError as e:
        payload = e.read()
        code = e.code
    if code != expect:
        raise AssertionError("%s %s -> %d (want %d): %s"
                             % (method, path, code, expect, payload[:300]))
    try:
        return json.loads(payload)
    except Exception:
        return payload


print("1. healthz")
h = req("GET", "/healthz")
print("   ", {k: h[k] for k in ("ok", "workers", "queue_depth")})

print("2. pick 3 pdfs from the corpus")
cands = [p for p in sorted(glob.glob("docs/*.pdf"))
         if 0.3 <= os.path.getsize(p) / 1048576.0 <= 2.0][:3]
for p in cands:
    print("    %5.2f MB  %s" % (os.path.getsize(p) / 1048576.0,
                                os.path.basename(p)[:52]))
want_pages = sum(fitz.open(p).page_count for p in cands)
print("    expected total pages:", want_pages)

print("3. create upload targets")
meta = [{"name": os.path.basename(p), "size": os.path.getsize(p)} for p in cands]
up = req("POST", "/api/v1/uploads", body={"files": meta})
print("    got", len(up["uploads"]), "targets")

print("4. PUT bodies")
for t, p in zip(up["uploads"], cands):
    with open(p, "rb") as f:
        blob = f.read()
    r = req("PUT", t["url"], raw=blob)
    assert r["size"] == len(blob), (r, len(blob))
print("    uploaded", sum(os.path.getsize(p) for p in cands), "bytes")

print("5. submit job")
job = req("POST", "/api/v1/jobs",
          body={"file_ids": [t["file_id"] for t in up["uploads"]]})
jid = job["job_id"]
print("    job", jid, job["status"])

print("6. poll")
t0 = time.time()
for _ in range(200):
    st = req("GET", "/api/v1/jobs/" + jid)
    if st["status"] in ("done", "failed"):
        break
    time.sleep(0.25)
print("    %s in %.2f s, pages=%s" % (st["status"], time.time() - t0,
                                     st.get("pages")))
assert st["status"] == "done", st
assert st["pages"] == want_pages, (st["pages"], want_pages)

print("7. download via signed url")
out = "work/_bench/e2e_merged.pdf"
os.makedirs("work/_bench", exist_ok=True)
with urllib.request.urlopen(BASE + st["download"]) as resp:
    blob = resp.read()
open(out, "wb").write(blob)
d = fitz.open(out)
print("    %d bytes, %d pages" % (len(blob), d.page_count))
assert d.page_count == want_pages

print("8. rejects a non-PDF")
up2 = req("POST", "/api/v1/uploads", body={"files": [{"name": "x.pdf", "size": 10}]})
req("PUT", up2["uploads"][0]["url"], raw=b"not a pdf at all", expect=415)
print("    rejected with 415")

print("9. rejects a single file job")
req("POST", "/api/v1/jobs", body={"file_ids": ["a"]}, expect=400)
print("    rejected with 400")

print("10. tampered signature is refused")
bad = st["download"].replace("sig=", "sig=0")
try:
    urllib.request.urlopen(BASE + bad)
    raise AssertionError("tampered link was served!")
except urllib.error.HTTPError as e:
    assert e.code == 403, e.code
print("    refused with 403")


def run_job(payload, label):
    j = req("POST", "/api/v1/jobs", body=payload)
    for _ in range(200):
        s = req("GET", "/api/v1/jobs/" + j["job_id"])
        if s["status"] in ("done", "failed"):
            break
        time.sleep(0.25)
    assert s["status"] == "done", (label, s)
    with urllib.request.urlopen(BASE + s["download"]) as resp:
        blob = resp.read()
    path = "work/_bench/%s.pdf" % label
    open(path, "wb").write(blob)
    return s, fitz.open(path)


ids = [t["file_id"] for t in up["uploads"]]

print("11. config endpoint")
cfg = req("GET", "/api/v1/config")
print("   ", {k: cfg[k] for k in ("max_files", "max_file_mb", "max_task_mb",
                                 "min_files", "ttl")})
assert cfg["min_files"] == 2
assert cfg["rotations"] == [0, 90, 180, 270]
assert cfg["max_files"] >= 2

print("12. per-file rotation is applied, order preserved")
rots = [0, 90, 180]
s2, d2 = run_job({"files": [{"file_id": ids[i], "rotate": rots[i]}
                            for i in range(len(ids))]}, "e2e_rot")
src = [fitz.open(p) for p in cands]
want = []
for doc, deg in zip(src, rots):
    want += [(pg.rotation + deg) % 360 for pg in doc]
got = [pg.rotation for pg in d2]
print("    pages=%d  rotations %s" % (d2.page_count, sorted(set(got))))
assert got == want, (got[:8], want[:8])

# order: the merged document must start with the first file's first page
a = d2[0].get_text().strip()[:60]
b = src[0][0].get_text().strip()[:60]
assert a == b, ("first page is not from the first file", a, b)
print("    first page text matches file #1")

print("13. invalid rotation rejected")
req("POST", "/api/v1/jobs",
    body={"files": [{"file_id": ids[0]},
                    {"file_id": ids[1], "rotate": 45}]}, expect=400)
print("    rejected with 400")

print("14. legacy file_ids payload still accepted")
s3, d3 = run_job({"file_ids": ids}, "e2e_legacy")
assert d3.page_count == want_pages
print("    ok, %d pages" % d3.page_count)

print("\nALL OK")
