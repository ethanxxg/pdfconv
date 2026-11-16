"""Browser flow test for the capture PWA with a fake camera device.

Uses Chromium's fake media stream so it runs headless.  The assertions cover
the parts a static HTML check cannot: that getUserMedia actually feeds the
video element, that shutter clicks produce reviewable shots, that the mode
switch resets state and enforces the id-card two-shot rule, and that no JS
errors are thrown while the live frame-detection loop is running.
"""
import os
import sys
import time

from playwright.sync_api import sync_playwright

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("shot_capture", pages=100)
BASE = srv.base


def shots(pg):
    return pg.evaluate("shots.length")


def wait_shots(pg, n):
    for _ in range(30):
        if shots(pg) >= n:
            return True
        time.sleep(0.4)
    return False


with sync_playwright() as pw:
    args = ["--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream"]
    b = None
    for kw in ({"channel": "msedge", "args": args},
               {"channel": "chrome", "args": args},
               {"args": args}):
        try:
            b = pw.chromium.launch(**kw)
            break
        except Exception:
            pass
    if b is None:
        sys.exit("no browser available")

    ctx = b.new_context(viewport={"width": 420, "height": 900},
                        permissions=["camera"])
    pg = ctx.new_page()
    errors = []
    pg.on("console", lambda m: errors.append("console:" + m.text)
          if m.type == "error" else None)
    pg.on("pageerror", lambda e: errors.append("page:" + str(e)))

    print("1. camera starts and the shell renders")
    pg.goto(BASE + "/capture", wait_until="networkidle")
    pg.wait_for_function(
        "document.querySelector('#video').readyState >= 2", timeout=15000)
    assert pg.inner_text(".caphead h1") == "拍照扫描"
    assert pg.locator(".modes button").count() == 4
    vw, vh = pg.eval_on_selector("#video", "v => [v.videoWidth, v.videoHeight]")
    assert vw > 0 and vh > 0, (vw, vh)
    print("   video %dx%d, 4 modes" % (vw, vh))

    print("2. single-page shutter captures one shot")
    pg.click("#shutter")
    assert wait_shots(pg, 1), shots(pg)
    assert not pg.is_disabled("#done")
    print("   shots=%d" % shots(pg))

    print("3. switching mode resets the filmstrip")
    pg.click('.modes button[data-mode="multi"]')
    assert shots(pg) == 0
    print("   reset ok")

    print("4. multi-page takes two shots")
    pg.click("#shutter"); assert wait_shots(pg, 1)
    pg.click("#shutter"); assert wait_shots(pg, 2)
    assert pg.locator(".shot").count() == 2
    print("   shots=%d" % shots(pg))

    print("5. id-card requires exactly two shots and shows a landscape guide")
    pg.click('.modes button[data-mode="idcard"]')
    box = pg.eval_on_selector(
        ".guide .box", "e => { const r = e.getBoundingClientRect(); return [r.width, r.height]; }")
    assert box[0] > box[1], box       # 1.586:1 card frame, not the portrait page frame
    assert shots(pg) == 0
    assert pg.is_disabled("#done")
    pg.click("#shutter"); assert wait_shots(pg, 1)
    assert pg.is_disabled("#done"), "one side must not enable submit"
    pg.click("#shutter"); assert wait_shots(pg, 2)
    assert not pg.is_disabled("#done"), "two sides must enable submit"
    print("   front+back gate ok")

    print("6. live frame detection runs without errors")
    # The detection loop uploads preview frames every ~0.5s; give it time to
    # fire several times and make sure nothing throws.
    time.sleep(2.5)
    assert not errors, errors
    print("   no console/page errors")

    pg.screenshot(path="work/_bench/capture_flow.png", full_page=True)
    b.close()

srv.stop()
print("\nCAPTURE FLOW OK")
