"""Drive the compress page in a real browser and capture both outcomes.

Two paths matter and they look nothing alike: a scan that shrinks by 90%, and an
already-optimised PDF where the honest answer is "no gain".  The second one is
the one users file tickets about, so it gets a screenshot too.
"""
import glob
import os
import sys
import urllib.request

from playwright.sync_api import sync_playwright

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("shot_compress", pages=5000)
BASE = srv.base
heavy = sorted(glob.glob("docs/075__*.pdf"))[0]
texty = sorted(glob.glob("docs/005__*.pdf"))[0]
print("heavy:", os.path.basename(heavy)[:44], "%.2f MB" % (os.path.getsize(heavy) / 1048576.0))
print("texty:", os.path.basename(texty)[:44], "%.2f MB" % (os.path.getsize(texty) / 1048576.0))

with sync_playwright() as pw:
    b = None
    for kw in ({"channel": "msedge"}, {"channel": "chrome"}, {}):
        try:
            b = pw.chromium.launch(**kw)
            break
        except Exception:
            pass
    if b is None:
        sys.exit("no browser available")

    pg = b.new_page(viewport={"width": 1100, "height": 1000})
    errors = []
    pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    pg.on("pageerror", lambda e: errors.append(str(e)))

    pg.goto(BASE + "/compress", wait_until="networkidle")
    print("limits:", pg.inner_text("#limits"))

    # -- path 1: a scan that really compresses -------------------------
    pg.set_input_files("#picker", heavy)
    pg.wait_for_function("document.querySelector('#thumb img') !== null", timeout=30000)
    print("file meta:", pg.inner_text("#fmeta"))
    pg.wait_for_timeout(300)
    pg.screenshot(path="work/_bench/cmp_loaded.png", full_page=True)

    pg.click(".level[data-level='extreme']")
    print("level selected:", pg.get_attribute(".level.on", "data-level"))
    pg.click("#go")
    pg.wait_for_selector(".result.on", timeout=180000)
    pg.wait_for_timeout(300)

    old = pg.inner_text("#rold")
    new = pg.inner_text("#rnew")
    saved = pg.inner_text("#rsave")
    print("result: %s -> %s  (%s)" % (old, new, saved))
    assert "省" in saved, saved

    href = pg.get_attribute("#rdl", "href")
    with urllib.request.urlopen(BASE + href) as r:
        blob = r.read()
    print("download: %d bytes, starts %r" % (len(blob), blob[:5]))
    assert blob[:5] == b"%PDF-"
    assert len(blob) < os.path.getsize(heavy) * 0.5
    pg.screenshot(path="work/_bench/cmp_done.png", full_page=True)

    # -- path 2: already-optimised, must not pretend -------------------
    pg.click("#again")
    pg.set_input_files("#picker", texty)
    pg.wait_for_function("document.querySelector('#thumb img') !== null", timeout=30000)
    pg.wait_for_timeout(300)
    pg.click(".level[data-level='recommended']")
    pg.click("#go")
    pg.wait_for_selector(".result.on", timeout=180000)
    pg.wait_for_timeout(400)
    print("text-only: %s -> %s (%s)"
          % (pg.inner_text("#rold"), pg.inner_text("#rnew"), pg.inner_text("#rsave")))
    print("notice  :", pg.inner_text("#msg"))
    pg.screenshot(path="work/_bench/cmp_optimal.png", full_page=True)

    print("console errors:", errors[:3] if errors else "none")
    b.close()
print("COMPRESS UI OK")
