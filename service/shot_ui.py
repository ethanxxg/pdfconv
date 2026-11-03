"""Drive the real page through every control and capture the result.

Checks the things a screenshot alone cannot prove: that the rotate button
changes the thumbnail pixels and the badge, that sorting actually reorders the
grid (files are added in reverse so a working sort must change the order), that
the countdown really ticks, and that the download link serves a real PDF.
"""
import glob
import hashlib
import os
import sys
import urllib.request

from playwright.sync_api import sync_playwright

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("ui", pages=5000)
BASE = srv.base
files = [p for p in sorted(glob.glob("docs/*.pdf"))
         if 0.3 <= os.path.getsize(p) / 1048576.0 <= 1.5][:3]
# reverse the input order: corpus glob order is already name order, so adding
# them in order would make "sort by name" a no-op and hide a broken sort
files = files[::-1]
print("input order:", [os.path.basename(f)[:34] for f in files])


def names(pg):
    return pg.eval_on_selector_all(".card .nm", "els => els.map(e => e.textContent)")


def thumb_hash(pg, idx=0):
    src = pg.eval_on_selector_all(
        ".card .thumb img", "els => els.map(e => e.getAttribute('src'))")[idx]
    return hashlib.sha256(src.encode()).hexdigest()[:12] if src else None


with sync_playwright() as pw:
    b = None
    for kw in ({"channel": "msedge"}, {"channel": "chrome"}, {}):
        try:
            b = pw.chromium.launch(**kw)
            print("browser:", kw or "bundled chromium")
            break
        except Exception as e:
            print("launch failed with", kw, "->", str(e).splitlines()[0][:70])
    if b is None:
        sys.exit("no browser available")

    pg = b.new_page(viewport={"width": 1100, "height": 980})
    errors = []
    pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    pg.on("pageerror", lambda e: errors.append(str(e)))

    pg.goto(BASE, wait_until="networkidle")
    print("limits text:", pg.inner_text("#limits"))

    pg.set_input_files("#picker", files)
    pg.wait_for_function(
        "document.querySelectorAll('.thumb img').length >= %d" % len(files),
        timeout=30000)
    pg.wait_for_timeout(400)
    order_added = names(pg)
    print("grid after add :", [n[:22] for n in order_added])
    print("hint           :", pg.inner_text("#hint"))
    pg.screenshot(path="work/_bench/ui_loaded.png", full_page=True)

    # -- rotate ---------------------------------------------------------
    before = thumb_hash(pg)
    pg.click(".card:first-child .rot[data-dir='90']")
    pg.wait_for_timeout(250)
    pg.click(".card:first-child .rot[data-dir='90']")
    pg.wait_for_timeout(400)
    after = thumb_hash(pg)
    badge = pg.inner_text(".card:first-child .deg")
    print("rotate         : thumb %s -> %s, badge %s" % (before, after, badge))
    assert before != after, "thumbnail pixels did not change on rotate"
    assert "180" in badge, badge
    pg.screenshot(path="work/_bench/ui_rotated.png", full_page=True)

    # -- sort -----------------------------------------------------------
    pg.click(".sort[data-sort='name']")
    pg.wait_for_timeout(250)
    order_name = names(pg)
    print("grid by name   :", [n[:22] for n in order_name])
    assert order_name != order_added, "sort by name did not reorder anything"
    assert order_name == sorted(order_name), order_name

    pg.click(".sort[data-sort='orig']")
    pg.wait_for_timeout(250)
    print("grid original  :", [n[:22] for n in names(pg)])

    # -- drag reorder (best effort: HTML5 DnD is browser-dependent) ------
    try:
        cards = pg.locator(".card")
        pg.drag_and_drop(".card:nth-child(1)", ".card:nth-child(3)")
        pg.wait_for_timeout(300)
        print("after drag     :", [n[:22] for n in names(pg)])
    except Exception as e:
        print("drag skipped   :", str(e).splitlines()[0][:70])

    # -- merge ----------------------------------------------------------
    pg.click("#go")
    pg.wait_for_selector(".result.on", timeout=120000)
    pg.wait_for_timeout(300)
    print("result title   :", pg.inner_text(".result .ttl"))
    cd1 = pg.inner_text("#cd")
    pg.wait_for_timeout(2100)
    cd2 = pg.inner_text("#cd")
    print("countdown      : %s -> %s" % (cd1, cd2))
    assert cd1 != cd2, "countdown is not ticking"

    href = pg.get_attribute("#rdl", "href")
    print("download href  :", href[:70], "...")
    pg.screenshot(path="work/_bench/ui_done.png", full_page=True)

    with urllib.request.urlopen(BASE + href) as r:
        blob = r.read()
    print("download       : %d bytes, starts %r" % (len(blob), blob[:5]))
    assert blob[:5] == b"%PDF-"

    print("console errors :", errors[:3] if errors else "none")
    b.close()
print("UI OK")
