"""End-to-end test for PDF -> PDF/A archival conversion.

Assertions that matter -- the ones that separate a real PDF/A from a file that
merely has "PDF/A" in its metadata:

  * the catalog carries an /OutputIntents entry whose ICC stream is a real
    profile (acsp signature),
  * XMP carries pdfaid:part / pdfaid:conformance,
  * every font reference in the *output* resolves to an embedded font file,
  * JavaScript (document OpenAction and the Names tree) is gone from the bytes,
  * a legal /OpenAction destination survives -- stripping it would break
    "open at page 3",
  * a PDF whose fonts cannot be embedded (base-14) still converts, but says so
    in warnings instead of claiming conformance,
  * part 2 vs part 3 differ only where they should (embedded files),
  * wrong input type / file count / part are refused before any work is queued.
"""
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

import pymupdf as fitz

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("pdfa", pages=5000)
BASE = srv.base
DEV = "test-pdfa-0001"
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


def run_pdfa(fids, options, expect="done"):
    j = req("POST", "/api/v1/jobs",
            body={"task": "pdfa",
                  "files": [{"file_id": x} for x in fids],
                  "options": options})
    assert j["task"] == "pdfa", j
    st = None
    for _ in range(600):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.2)
    assert st["status"] == expect, st
    return st


def download(st):
    with urllib.request.urlopen(BASE + st["download"]) as resp:
        return resp.read(), resp.headers.get("Content-Type", "")


# ---- independent structural checks (deliberately not imported from the
# service module: the test should fail if the worker's own audit is wrong) ---
def embedded_fonts(path):
    """(references, embedded references) in a saved PDF."""
    doc = fitz.open(path)
    total = emb = 0
    try:
        for pno in range(doc.page_count):
            for f in doc.get_page_fonts(pno, full=True):
                total += 1
                if _has_font_file(doc, f[0]):
                    emb += 1
    finally:
        doc.close()
    return total, emb


def _has_font_file(doc, xref):
    seen = []
    if xref:
        seen.append(xref)
        kind, val = doc.xref_get_key(xref, "DescendantFonts")
        if kind == "array":
            # "5 0 R" -- not "5", so a plain isdigit() on the token fails
            for ref in re.findall(r"(\d+)\s+0\s+R", val or ""):
                seen.append(int(ref))
    for obj in seen:
        kind, val = doc.xref_get_key(obj, "FontDescriptor")
        if kind != "xref":
            continue
        fd = int(val.split()[0])
        for k in ("FontFile", "FontFile2", "FontFile3"):
            if doc.xref_get_key(fd, k)[0] != "null":
                return True
        return False
    return False


def intent_ok(path):
    """Does the catalog reference an OutputIntent with a real ICC profile?"""
    doc = fitz.open(path)
    try:
        cat = doc.pdf_catalog()
        kind, val = doc.xref_get_key(cat, "OutputIntents")
        if kind != "array":
            return False, "no /OutputIntents"
        ix = int(val.strip("[]").split()[0])
        if doc.xref_get_key(ix, "S")[1] != "/GTS_PDFA1":
            return False, "wrong /S"
        prof = int(doc.xref_get_key(ix, "DestOutputProfile")[1].split()[0])
        icc = doc.xref_stream(prof)
        if icc[36:40] != b"acsp":
            return False, "ICC signature is %r" % icc[36:40]
        if doc.xref_get_key(prof, "N")[1] != "3":
            return False, "wrong /N"
        return True, "%d-byte sRGB profile" % len(icc)
    finally:
        doc.close()


def pdfaid(path):
    doc = fitz.open(path)
    try:
        meta = doc.get_xml_metadata() or ""
    finally:
        doc.close()
    part = conf = None
    for chunk in meta.split("<"):
        if chunk.startswith("pdfaid:part>"):
            part = chunk.split(">")[1].strip()
        elif chunk.startswith("pdfaid:conformance>"):
            conf = chunk.split(">")[1].strip()
    return part, conf


# ---- fixtures ------------------------------------------------------------
def _ttf():
    """A real TrueType file, so the fixture has a font that *can* be embedded."""
    for p in (r"C:\Windows\Fonts\arial.ttf",
              r"C:\Windows\Fonts\calibri.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf"):
        if os.path.exists(p):
            return p
    return None


TTF = _ttf()
print("0. font fixture: %s" % (TTF or "NONE (embedded-font checks limited)"))

emb_src = "work/_bench/pdfa_emb.pdf"
doc = fitz.open()
for i in range(3):
    p = doc.new_page()
    if TTF:
        p.insert_text((72, 90), "Archive page %d 归档" % (i + 1),
                      fontname="F0", fontfile=TTF, fontsize=20)
    else:
        p.insert_text((72, 90), "Archive page %d" % (i + 1), fontsize=20)
doc.save(emb_src)
doc.close()

base14_src = "work/_bench/pdfa_base14.pdf"
doc = fitz.open()
p = doc.new_page()
p.insert_text((72, 90), "Base14 Helvetica", fontname="helv", fontsize=20)
doc.save(base14_src)
doc.close()

js_src = "work/_bench/pdfa_js.pdf"
doc = fitz.open()
p = doc.new_page()
if TTF:
    p.insert_text((72, 90), "Scripted", fontname="F0", fontfile=TTF, fontsize=18)
else:
    p.insert_text((72, 90), "Scripted", fontsize=18)
cat = doc.pdf_catalog()
doc.xref_set_key(cat, "OpenAction",
                 "<< /S /JavaScript /JS (app.alert(1);) >>")
doc.xref_set_key(cat, "Names",
                 "<< /JavaScript << /Names [(x) << /S /JavaScript "
                 "/JS (app.alert(1);) >>] >> /Dests << /Names "
                 "[(y) [1 0 R /Fit]] >> >>")
doc.save(js_src)
doc.close()

dest_src = "work/_bench/pdfa_dest.pdf"
doc = fitz.open()
doc.new_page()
doc.new_page()
doc.xref_set_key(doc.pdf_catalog(), "OpenAction", "[2 0 R /Fit]")
doc.save(dest_src)
doc.close()

docx_src = "work/_bench/pdfa_doc.docx"
import docx as _docx  # noqa: E402

d = _docx.Document()
d.add_paragraph("not a pdf")
d.save(docx_src)

print("1. config lists the archive tool last")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
assert "pdfa" in ids, ids
assert ids[-1] == "pdfa", ids
print("   tools tail:", ids[-3:])

print("2. PDF -> PDF/A-2b: output intent, XMP, fonts")
fid = upload(emb_src)
st = run_pdfa([fid], {"part": "2", "name": "pdfa_emb.pdf"})
r = st["result"]
print("   report:", json.dumps({k: r[k] for k in
                               ("conformance", "pages", "fonts_total",
                                "fonts_embedded", "warnings")},
                              ensure_ascii=False))
assert r["conformance"] == "PDF/A-2b", r
assert r["part"] == "2", r
assert r["pages"] == 3, r
blob, ctype = download(st)
assert ctype.startswith("application/pdf"), ctype
out = "work/_bench/pdfa_out2.pdf"
with open(out, "wb") as f:
    f.write(blob)

ok, why = intent_ok(out)
print("   output intent:", why)
assert ok, why
part, conf = pdfaid(out)
print("   xmp: part=%s conformance=%s" % (part, conf))
assert part == "2" and conf == "B", (part, conf)

total, emb = embedded_fonts(out)
print("   fonts: %d/%d embedded" % (emb, total))
if TTF:
    assert total > 0 and emb == total, (emb, total)
    assert not r["warnings"], r["warnings"]
    assert r["fonts_embedded"] == total, r
else:
    print("   (skipped: no TTF on this machine)")

print("3. PDF/A-3b: same file, part 3 in XMP, embedded files allowed")
fid = upload(emb_src)
st3 = run_pdfa([fid], {"part": "3", "name": "pdfa_emb.pdf"})
assert st3["result"]["conformance"] == "PDF/A-3b", st3["result"]
blob, _ = download(st3)
out3 = "work/_bench/pdfa_out3.pdf"
with open(out3, "wb") as f:
    f.write(blob)
part3, conf3 = pdfaid(out3)
print("   xmp: part=%s conformance=%s" % (part3, conf3))
assert part3 == "3" and conf3 == "B", (part3, conf3)

print("4. JavaScript is stripped from the bytes")
fid = upload(js_src)
stj = run_pdfa([fid], {"part": "2", "name": "pdfa_js.pdf"})
rj = stj["result"]
print("   removed:", rj["removed"])
assert "OpenAction" in rj["removed"], rj["removed"]
assert "JavaScript" in rj["removed"], rj["removed"]
blob, _ = download(stj)
outj = "work/_bench/pdfa_js_out.pdf"
with open(outj, "wb") as f:
    f.write(blob)
raw = open(outj, "rb").read()
print("   residue /JavaScript=%d app.alert=%d"
      % (raw.count(b"/JavaScript"), raw.count(b"app.alert")))
assert raw.count(b"/JavaScript") == 0, "JavaScript survived"
assert raw.count(b"app.alert") == 0, "script body survived"
# the sibling /Dests tree must not have been collateral damage
doc = fitz.open(outj)
names = doc.xref_get_key(doc.pdf_catalog(), "Names")
doc.close()
assert names[0] == "dict" and "Dests" in names[1], names
print("   /Dests preserved:", names[1][:60])

print("5. a legal OpenAction destination is kept")
fid = upload(dest_src)
std = run_pdfa([fid], {"part": "2", "name": "pdfa_dest.pdf"})
assert std["result"]["removed"] == [], std["result"]["removed"]
blob, _ = download(std)
outd = "work/_bench/pdfa_dest_out.pdf"
with open(outd, "wb") as f:
    f.write(blob)
doc = fitz.open(outd)
oa = doc.xref_get_key(doc.pdf_catalog(), "OpenAction")
doc.close()
print("   OpenAction:", oa)
assert oa[0] == "array", oa

print("6. base-14 fonts cannot be embedded -> warned, not faked")
fid = upload(base14_src)
stb = run_pdfa([fid], {"part": "2", "name": "pdfa_base14.pdf"})
rb = stb["result"]
print("   fonts %d/%d, warnings: %s"
      % (rb["fonts_embedded"], rb["fonts_total"], rb["warnings"]))
assert rb["fonts_embedded"] < rb["fonts_total"], rb
assert rb["warnings"], "un-embeddable fonts must be reported"
assert any("字体" in w for w in rb["warnings"]), rb["warnings"]

print("7. wrong input type is refused")
fid = upload(docx_src,
             mime="application/vnd.openxmlformats-officedocument"
                  ".wordprocessingml.document")
e = req("POST", "/api/v1/jobs",
        body={"task": "pdfa", "files": [{"file_id": fid}],
              "options": {"part": "2"}}, expect=400)
print("   ", e["code"], e["message"])
assert e["code"] == "wrong_type", e

print("8. two files are refused")
f1 = upload(emb_src)
f2 = upload(emb_src)
e = req("POST", "/api/v1/jobs",
        body={"task": "pdfa", "files": [{"file_id": f1}, {"file_id": f2}],
              "options": {"part": "2"}}, expect=400)
print("   ", e["code"], e["message"])
assert e["code"] == "one_file", e

print("9. a bad part is refused before any work is queued")
fid = upload(emb_src)
e = req("POST", "/api/v1/jobs",
        body={"task": "pdfa", "files": [{"file_id": fid}],
              "options": {"part": "1"}}, expect=400)
print("   ", e["code"], e["message"])
assert e["code"] == "bad_part", e

print("10. the /pdfa page renders its own controls")
with urllib.request.urlopen(BASE + "/pdfa") as resp:
    html = resp.read().decode("utf-8")
for needle in ("归档版本", "PDF/A-2b", "PDF/A-3b", '"pdfa"'):
    assert needle in html, needle
assert "图片格式" not in html, "pdfa page leaked the image-format select"
print("   /pdfa: %d bytes, part selector present" % len(html))

srv.stop()
print("\nALL OK")
