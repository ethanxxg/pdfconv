"""End-to-end test for the page tools (split / extract / delete / rotate).

The assertions that matter:
  * extract keeps exactly the named pages, in ascending order,
  * delete drops exactly the named pages,
  * rotate writes /Rotate on the right pages (all, or a subset),
  * split returns a real zip whose members hold the expected pages,
  * a bad page spec is refused before any work is queued,
  * a non-PDF is refused with ``wrong_type``,
  * the rendered /pages/{tool} route exists and 404s for an unknown tool.

Runs on its own server with the page allowance raised.  A synthetic 10-page PDF
is used so every page count is exact and independent of the docs/ corpus.
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

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("pages", pages=5000)
BASE = srv.base
DEV = "test-pages-0001"
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


def run_pages(fid, options):
    j = req("POST", "/api/v1/jobs",
            body={"task": "pages", "files": [{"file_id": fid}],
                  "options": options})
    assert j["task"] == "pages", j
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


def open_pdf(blob):
    return fitz.open(stream=blob, filetype="pdf")


# ---- a deterministic 10-page source -------------------------------------
src = "work/_bench/pages_src.pdf"
doc = fitz.open()
for i in range(10):
    p = doc.new_page()
    p.insert_text((72, 72), "page %d" % (i + 1))
doc.save(src)
doc.close()
TOTAL = 10

print("1. config lists the four page tools")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
for tid in ("split", "extract", "delete", "rotate"):
    assert tid in ids, (tid, ids)
print("   tools:", ids)

print("2. extract keeps exactly the named pages")
fid = upload(src)
st = run_pages(fid, {"op": "extract", "pages": "1,3", "name": "pages_src.pdf"})
blob, ctype = download(st)
d = open_pdf(blob)
print("   %d pages out, content-type=%s" % (d.page_count, ctype))
assert st["pages"] == 2, st
assert d.page_count == 2
assert "page 1" in d[0].get_text()
assert "page 3" in d[1].get_text()
assert ctype.startswith("application/pdf"), ctype
d.close()

print("3. delete drops exactly the named pages")
fid = upload(src)
st = run_pages(fid, {"op": "delete", "pages": "1", "name": "pages_src.pdf"})
blob, _ = download(st)
d = open_pdf(blob)
print("   %d pages out (want %d), first page = %r"
      % (d.page_count, TOTAL - 1, d[0].get_text().strip()[:12]))
assert st["pages"] == TOTAL - 1, st
assert d.page_count == TOTAL - 1
assert "page 2" in d[0].get_text()
d.close()

print("4. rotate all pages by 90")
fid = upload(src)
st = run_pages(fid, {"op": "rotate", "angle": 90, "name": "pages_src.pdf"})
blob, _ = download(st)
d = open_pdf(blob)
rots = [p.rotation for p in d]
print("   rotations:", rots)
assert d.page_count == TOTAL
assert all(r == 90 for r in rots), rots
d.close()

print("5. rotate only page 2 by 180")
fid = upload(src)
st = run_pages(fid, {"op": "rotate", "angle": 180, "pages": "2",
                     "name": "pages_src.pdf"})
blob, _ = download(st)
d = open_pdf(blob)
print("   rotations:", [p.rotation for p in d])
assert d[0].rotation == 0
assert d[1].rotation == 180
assert all(p.rotation == 0 for p in d if p.number != 1)
d.close()

print("6. split every 2 pages -> zip of 5 two-page PDFs")
fid = upload(src)
st = run_pages(fid, {"op": "split", "every": 2, "name": "pages_src.pdf"})
blob, ctype = download(st)
print("   content-type=%s, files=%s" % (ctype, st["result"].get("files")))
assert ctype.startswith("application/zip"), ctype
z = zipfile.ZipFile(io.BytesIO(blob))
names = z.namelist()
assert len(names) == 5, names
for nm in names:
    part = open_pdf(z.read(nm))
    assert part.page_count == 2, (nm, part.page_count)
    part.close()
print("   members:", names)

print("7. split by ranges -> zip of 2 PDFs (2 and 3 pages)")
fid = upload(src)
st = run_pages(fid, {"op": "split", "ranges": "1-2,3-5",
                     "name": "pages_src.pdf"})
blob, _ = download(st)
z = zipfile.ZipFile(io.BytesIO(blob))
counts = sorted(open_pdf(z.read(n)).page_count for n in z.namelist())
print("   page counts per member:", counts)
assert counts == [2, 3], counts

print("8. an empty page spec is refused (400)")
fid = upload(src)
err = req("POST", "/api/v1/jobs",
          body={"task": "pages", "files": [{"file_id": fid}],
                "options": {"op": "extract", "pages": ""}}, expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "bad_pages", err

print("9. a page spec outside the document is refused")
fid = upload(src)
j = req("POST", "/api/v1/jobs",
        body={"task": "pages", "files": [{"file_id": fid}],
              "options": {"op": "extract", "pages": "99"}})
st = None
for _ in range(200):
    st = req("GET", "/api/v1/jobs/" + j["job_id"])
    if st["status"] in ("done", "failed"):
        break
    time.sleep(0.2)
print("   status:", st["status"], st.get("error"))
assert st["status"] == "failed"
assert st["error"]["code"] == "bad_pages", st["error"]

print("10. a non-PDF (docx) is refused with wrong_type")
import docx as _docx
probe = "work/_bench/pages_probe.docx"
_d = _docx.Document(); _d.add_paragraph("hi"); _d.save(probe)
fid_d = upload(probe)
err = req("POST", "/api/v1/jobs",
          body={"task": "pages", "files": [{"file_id": fid_d}],
                "options": {"op": "rotate", "angle": 90}}, expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "wrong_type", err

print("11. an unknown op is refused (400)")
fid = upload(src)
err = req("POST", "/api/v1/jobs",
          body={"task": "pages", "files": [{"file_id": fid}],
                "options": {"op": "shred"}}, expect=400)
print("   ", err.get("code"), "-", err.get("message"))
assert err.get("code") == "bad_op", err

print("12. the /pages/{tool} route renders and 404s for unknown")
with urllib.request.urlopen(BASE + "/pages/split") as r:
    html = r.read().decode("utf-8")
assert r.status == 200 and "拆分 PDF" in html, r.status
print("   /pages/split rendered", len(html), "bytes")
req("GET", "/pages/nope", expect=404)

print("\nALL OK")
