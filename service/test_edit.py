"""End-to-end test for the six structured PDF editing operations.

Assertions that matter:
  * redaction removes the glyphs from the content stream, not just the pixels,
  * CJK watermarks and {page}/{total} tokens render as extractable text,
  * form filling bakes the values into ordinary page content,
  * in-place rewrite keeps the page count and replaces the right words,
  * malformed options are refused with 400 before the allowance is spent,
  * free-tier callers get 403 plan_limit for both jobs and field discovery.

Two servers are booted: one with PDFM_FREE_EDIT=1 for the functional path, one
with the shipped default so the paywall is exercised for real.
"""
import io
import json
import os
import sys
import time
import urllib.error
import urllib.request

import pymupdf as fitz
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("edit", pages=5000, jobs=0, extra_env={"PDFM_FREE_EDIT": "1"})
BASE = srv.base
DEV = "test-edit-0001"
BENCH = "work/_bench"
os.makedirs(BENCH, exist_ok=True)


def req(method, path, body=None, raw=None, ctype="application/pdf",
        expect=200, base=None, dev=None):
    url = (base or BASE) + path
    h = {"X-Device-Id": dev or DEV}
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
                             % (method, path, code, expect, payload[:400]))
    try:
        obj = json.loads(payload)
    except Exception:
        return payload
    if isinstance(obj, dict) and set(obj) == {"detail"} \
            and isinstance(obj["detail"], dict):
        return obj["detail"]
    return obj


def upload(path, mime="application/pdf", base=None, dev=None):
    meta = [{"name": os.path.basename(path), "size": os.path.getsize(path)}]
    up = req("POST", "/api/v1/uploads", body={"files": meta},
             base=base, dev=dev)["uploads"][0]
    with open(path, "rb") as f:
        blob = f.read()
    req("PUT", up["url"], raw=blob, ctype=mime, base=base, dev=dev)
    return up["file_id"]


def run_edit(fids, options, expect=200, base=None, dev=None):
    j = req("POST", "/api/v1/jobs",
            body={"task": "edit",
                  "files": [{"file_id": x} for x in fids],
                  "options": options},
            expect=expect, base=base, dev=dev)
    if expect != 200:
        return j
    assert j["task"] == "edit", j
    st = None
    for _ in range(600):
        st = req("GET", "/api/v1/jobs/" + j["job_id"], base=base, dev=dev)
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.2)
    assert st["status"] == "done", st
    return st


def download(st, base=None):
    with urllib.request.urlopen((base or BASE) + st["download"]) as resp:
        return resp.read(), resp.headers.get("Content-Type", ""), \
            resp.headers.get("Content-Disposition", "")


# ---- fixtures ------------------------------------------------------------
SRC = os.path.join(BENCH, "edit_src.pdf")
doc = fitz.open()
for i in range(3):
    p = doc.new_page(width=595, height=842)
    p.insert_text((72, 100), "Confidential Invoice Total 42",
                  fontsize=14, fontname="helv")
    p.insert_text((72, 140), "Sign here before Friday",
                  fontsize=12, fontname="helv")
    p.insert_text((72, 180), "Page %d of the contract" % (i + 1),
                  fontsize=11, fontname="helv")
doc.save(SRC)
doc.close()

SEAL = os.path.join(BENCH, "edit_seal.png")
im = Image.new("RGBA", (160, 160), (190, 20, 20, 255))
ImageDraw.Draw(im).ellipse((8, 8, 152, 152), outline=(255, 255, 255, 255),
                           width=8)
im.save(SEAL, "PNG")

FORM = os.path.join(BENCH, "edit_form.pdf")
doc = fitz.open()
p = doc.new_page(width=595, height=300)
w = fitz.Widget()
w.field_name = "buyer"
w.field_type = fitz.PDF_WIDGET_TYPE_TEXT
w.rect = fitz.Rect(72, 72, 320, 100)
w.field_value = ""
w.text_fontsize = 11
p.add_widget(w)
doc.save(FORM)
doc.close()

DOCX = os.path.join(BENCH, "edit_not_pdf.docx")
with open(DOCX, "wb") as f:
    f.write(b"PK\x03\x04" + b"\x00" * 64)

EDIT_IDS = ["watermark", "redact", "annotate", "stamp", "form", "rewrite"]

print("1. config exposes the six edit tools and the plan switch")
cfg = req("GET", "/api/v1/config")
ids = [t["id"] for t in cfg["tools"]]
assert ids[-6:] == EDIT_IDS, ids[-6:]
assert cfg["plan"]["edit"] is True, cfg["plan"]
print("   tool tail:", ids[-6:])

print("2. all six /edit/{tool} pages render with their own controls")
for tid in EDIT_IDS:
    page = req("GET", "/edit/" + tid).decode("utf-8")
    assert "__TOOL_JSON__" not in page, tid
    assert '"op": "%s"' % tid in page, tid
    assert 'id="sec-%s"' % tid in page, tid
err = req("GET", "/edit/nope", expect=404)
assert err["code"] == "no_such_tool", err
print("   6 pages + 404 for unknown tool")

print("3. watermark: CJK + {page}/{total} + page spec")
fid = upload(SRC)
st = run_edit([fid], {"op": "watermark", "text": "机密 {page}/{total}",
                      "position": "bc", "size": 16, "opacity": 0.6,
                      "color": "#cc0000", "angle": 0, "pages": "2"})
blob, ctype, disp = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert len(out) == 3 and out[1].get_text().count("机密 2/3") == 1, \
    out[1].get_text()
assert "机密" not in out[0].get_text(), "page spec was ignored"
assert ctype == "application/pdf" and "edited" in disp, (ctype, disp)
print("   ", st["result"], "| disposition:", disp)

print("4. watermark: tiled diagonal CJK is extractable")
fid = upload(SRC)
st = run_edit([fid], {"op": "watermark", "text": "内部资料",
                      "position": "center", "size": 40, "opacity": 0.25,
                      "angle": 45, "tile": True})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert out[0].get_text().count("内部资料") >= 3, out[0].get_text()[:200]
print("   tiled repeats:", out[0].get_text().count("内部资料"))

print("5. redaction really deletes the words from the content stream")
fid = upload(SRC)
st = run_edit([fid], {"op": "redact",
                      "keywords": ["Invoice Total 42"], "color": "#000000"})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert st["result"]["areas"] == 3, st["result"]
for pg in out:
    assert "Invoice Total 42" not in pg.get_text(), pg.get_text()
assert b"Invoice Total 42" not in blob, "raw glyphs survived redaction"
print("   areas:", st["result"]["areas"], "| raw bytes clean")

print("6. redaction by normalised rectangle")
fid = upload(SRC)
st = run_edit([fid], {"op": "redact",
                      "rects": [{"page": 1, "x": 0.1, "y": 0.1,
                                 "w": 0.5, "h": 0.06}]})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert st["result"]["areas"] == 1 and len(out) == 3
print("   ", st["result"])

print("7. highlight annotation carries its note")
fid = upload(SRC)
st = run_edit([fid], {"op": "annotate", "kind": "highlight",
                      "keywords": ["Sign here"], "note": "法务复核",
                      "author": "tester", "color": "#ffd900"})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
# Keep the page alive: PyMuPDF annotation handles are bound to their page,
# and list(out[0].annots()) would otherwise unbind on the temporary's GC.
pg = out[0]
anns = list(pg.annots() or ())
assert len(anns) == 1 and anns[0].type[1] == "Highlight", anns
assert (anns[0].info or {}).get("content") == "法务复核", anns[0].info
print("   ", st["result"], "| note:", anns[0].info["content"])

print("8. stamp overlays a real image on every selected page")
fid, sid = upload(SRC), upload(SEAL, mime="image/png")
st = run_edit([fid, sid], {"op": "stamp", "position": "br",
                           "scale": 0.2, "opacity": 0.7, "rotation": 0})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert all(pg.get_images() for pg in out), "seal missing on some pages"
print("   ", st["result"])

print("9. form field discovery, filling and flattening")
fid = upload(FORM)
fields = req("POST", "/api/v1/edit/fields", body={"file_id": fid})
assert fields["count"] == 1 and fields["fields"][0]["name"] == "buyer", fields
st = run_edit([fid], {"op": "form",
                      "fields": {"buyer": "ACME 有限公司"},
                      "flatten": True})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
pg = out[0]
assert st["result"]["filled"] == 1 and st["result"]["flattened"] is True
assert not list(pg.widgets() or ()), "widgets survived bake()"
assert "ACME 有限公司" in pg.get_text(), pg.get_text()
print("   ", fields["fields"][0], "->", out[0].get_text().strip())

print("10. in-place rewrite keeps the page count and replaces the words")
fid = upload(SRC)
st = run_edit([fid], {"op": "rewrite",
                      "pairs": [{"find": "contract", "replace": "agreement"},
                                {"find": "42", "replace": "四十二"}]})
blob, _, _ = download(st)
out = fitz.open(stream=blob, filetype="pdf")
assert len(out) == 3 and "agreement" in out[0].get_text()
assert st["result"]["rewritten"] >= 3, st["result"]
print("   ", st["result"])

print("11. bad options are refused with a stable code, before any job")
fid = upload(SRC)
cases = [
    ({"op": "nope"}, "bad_op"),
    ({"op": "watermark", "text": ""}, "bad_text"),
    ({"op": "watermark", "text": "x", "color": "red"}, "bad_color"),
    ({"op": "watermark", "text": "x", "position": "nowhere"}, "bad_position"),
    ({"op": "watermark", "text": "x", "angle": 17}, "bad_angle"),
    ({"op": "redact"}, "bad_keywords"),
    ({"op": "annotate", "kind": "highlight"}, "bad_keywords"),
    ({"op": "form", "fields": {}}, "bad_fields"),
    ({"op": "rewrite", "pairs": []}, "bad_pairs"),
    ({"op": "stamp", "position": "br"}, "wrong_file_count"),
]
for options, code in cases:
    got = run_edit([fid], options, expect=400)
    assert got["code"] == code, (options, got)
print("   %d rejections: %s" % (len(cases), ", ".join(c for _, c in cases)))

print("12. wrong input types and no-match are clean failures")
fid_docx = upload(DOCX)
got = req("POST", "/api/v1/jobs",
          body={"task": "edit", "files": [{"file_id": fid_docx}],
                "options": {"op": "redact", "keywords": ["x"]}},
          expect=400)
assert got["code"] == "wrong_type", got
fid_pdf = upload(SRC)
bad = req("POST", "/api/v1/jobs",
          body={"task": "edit",
                "files": [{"file_id": fid_pdf}, {"file_id": fid_pdf}],
                "options": {"op": "stamp", "position": "br"}},
          expect=400)
assert bad["code"] == "wrong_type", bad
fid_pdf2 = upload(SRC)
# The request is accepted (it is syntactically valid); the refusal arrives as
# a *failed job* with a stable code, because the worker cannot know a keyword
# is absent until it opens the PDF.
j = req("POST", "/api/v1/jobs",
        body={"task": "edit", "files": [{"file_id": fid_pdf2}],
              "options": {"op": "redact", "keywords": ["NOT IN THIS PDF"]}})
assert j["task"] == "edit", j
st2 = None
for _ in range(600):
    st2 = req("GET", "/api/v1/jobs/" + j["job_id"])
    if st2["status"] in ("done", "failed"):
        break
    time.sleep(0.2)
assert st2["status"] == "failed" and st2["error"]["code"] == "no_match", st2
print("   wrong_type + no_match surfaced correctly")

print("13. the shipped free tier refuses editing (403 plan_limit)")
gate = boot("editgate", pages=5000, jobs=0,
            extra_env={"PDFM_FREE_EDIT": "0"})
gcfg = req("GET", "/api/v1/config", base=gate.base, dev="test-gate")
assert gcfg["plan"]["edit"] is False, gcfg["plan"]
gfid = upload(SRC, base=gate.base, dev="test-gate")
got = run_edit([gfid], {"op": "watermark", "text": "x"},
               expect=403, base=gate.base, dev="test-gate")
assert got["code"] == "plan_limit", got
f = req("POST", "/api/v1/edit/fields", body={"file_id": gfid},
        expect=403, base=gate.base, dev="test-gate")
assert f["code"] == "plan_limit", f
print("   ", got["code"], "-", got["message"])
gate.stop()

print("\nALL OK")
srv.stop()
