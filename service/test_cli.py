"""End-to-end test for the capability catalog and the CLI client.

The CLI is a thin HTTP client, so the assertions here are about the contract,
not about re-testing the engines:

  * /api/v1/capabilities lists every tool with task/op + a JSON Schema,
  * `pdfkit list/info/schema` read that catalog (JSON and human forms),
  * `pdfkit run` coerces --opt values, honours the file-count floor, uploads,
    submits, polls and saves a real file,
  * an unknown capability and a bad --opt are refused before upload.
"""
import glob
import json
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("cli", pages=100)
BASE = srv.base
PY = sys.executable


def cli(*args, env=None):
    e = dict(os.environ)
    e["PDFKIT_URL"] = BASE
    if env:
        e.update(env)
    p = subprocess.run([PY, "-m", "service.cli"] + list(args),
                       capture_output=True, text=True, cwd=ROOT, env=e, timeout=300)
    return p.returncode, p.stdout, p.stderr


print("1. /api/v1/capabilities exposes every tool with a schema")
import urllib.request
with urllib.request.urlopen(BASE + "/api/v1/capabilities") as r:
    caps = json.load(r)["capabilities"]
ids = [c["id"] for c in caps]
assert len(caps) == 24, len(caps)
assert "scan" in ids and "merge" in ids and "image-to-text" in ids, ids
for c in caps:
    assert c["input_schema"]["type"] == "object", c["id"]
    assert "properties" in c["input_schema"], c["id"]
    assert c["task"] and c["min_files"] >= 1, c["id"]
scan = [c for c in caps if c["id"] == "scan"][0]
assert scan["input_schema"]["properties"]["mode"]["enum"] == ["color", "gray", "bw"], scan
print("   %d capabilities" % len(caps))

print("2. pdfkit list (human + json)")
rc, out, err = cli("list")
assert rc == 0 and "scan" in out and "image-to-text" in out, (rc, out, err)
rc, out, err = cli("list", "--json")
assert rc == 0 and json.loads(out)[0]["id"], (rc, out[:200], err)
print("   list ok")

print("3. pdfkit info / schema")
rc, out, err = cli("info", "scan")
assert rc == 0 and json.loads(out)["task"] == "scan", (rc, out[:200], err)
rc, out, err = cli("schema", "scan")
assert rc == 0 and json.loads(out)["type"] == "object", (rc, out[:200], err)
print("   info/schema ok")

print("4. pdfkit run: coerce --opt and save a real PDF")
fonts = glob.glob("C:/Windows/Fonts/msyh.ttc") + glob.glob("C:/Windows/Fonts/arial.ttf")
font = ImageFont.truetype(fonts[0], 40)
img = Image.new("RGB", (900, 500), "white")
d = ImageDraw.Draw(img)
d.rectangle((100, 80, 800, 420), fill=(244, 242, 234))
d.text((140, 150), "CLI scan 2026", font=font, fill="black")
photo = "work/_bench/cli_photo.png"
img.save(photo, "PNG")
out_path = "work/_bench/cli_out.pdf"
if os.path.exists(out_path):
    os.remove(out_path)
rc, out, err = cli("run", "scan", "-i", photo, "--opt", "mode=gray",
                   "--opt", "dpi=150", "--name", "cli-scan", "-o", out_path)
assert rc == 0, (rc, out, err)
assert os.path.exists(out_path) and os.path.getsize(out_path) > 100, out
import pymupdf as fitz
doc = fitz.open(out_path)
assert doc.page_count == 1, doc.page_count
doc.close()
print("   saved", out_path, os.path.getsize(out_path), "bytes")

print("5. merge needs two files (file-count floor enforced before upload)")
rc, out, err = cli("run", "merge", "-i", photo)
assert rc != 0 and "至少 2 个" in (out + err), (rc, out, err)
print("   ", (out + err).strip()[:80])

print("6. unknown capability and bad --opt are refused")
rc, out, err = cli("run", "no-such-tool", "-i", photo)
assert rc != 0 and "未知能力" in (out + err), (rc, out, err)
rc, out, err = cli("run", "scan", "-i", photo, "--opt", "bogus=1")
assert rc != 0 and "未知选项" in (out + err), (rc, out, err)
print("   both refused")

srv.stop()
print("\nALL OK")
