"""Exercise the free tier in a real browser, at the shipped limits.

The server-side limits are covered by test_limits.py.  What this checks is the
part users actually see: that the page refuses an over-page selection *before*
uploading, and that a 4-page merge still goes through with the single daily
allowance.

Runs its own server so the limits are the real ones (5 pages / 15 MB / 1 job),
not the raised values the functional suites use.
"""
import os
import shutil
import subprocess
import sys
import time

import pymupdf as fitz
from playwright.sync_api import sync_playwright

PORT = 8083
BASE = "http://127.0.0.1:%d" % PORT
DATA = "service/data_ui"


def make_pdf(path, pages, text="page"):
    doc = fitz.open()
    for i in range(pages):
        page = doc.new_page()
        page.insert_text((72, 96), "%s %d" % (text, i + 1))
    doc.save(path, garbage=4, deflate=True)
    doc.close()
    return path


os.makedirs("work/_bench", exist_ok=True)
p2a = make_pdf("work/_bench/ui_2a.pdf", 2, "invoice A")
p2b = make_pdf("work/_bench/ui_2b.pdf", 2, "invoice B")
p6 = make_pdf("work/_bench/ui_6.pdf", 6, "long report")

shutil.rmtree(DATA, ignore_errors=True)
env = dict(os.environ, PDFM_WORKERS="1", PDFM_DATA=DATA)
for k in ("PDFM_MAX_PAGES", "PDFM_MAX_FILE_MB", "PDFM_FREE_JOBS_PER_DAY"):
    env.pop(k, None)
srv = subprocess.Popen(
    [sys.executable, "-m", "uvicorn", "service.main:app",
     "--host", "127.0.0.1", "--port", str(PORT), "--log-level", "warning"],
    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    for _ in range(80):
        try:
            import urllib.request
            urllib.request.urlopen(BASE + "/healthz", timeout=1).read()
            break
        except Exception:
            time.sleep(0.25)
    else:
        raise SystemExit("server did not start")

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

        print("1. merge page advertises the real free tier")
        pg.goto(BASE + "/", wait_until="networkidle")
        print("   ", pg.inner_text("#limits"))
        assert "5 页" in pg.inner_text("#limits")

        print("2. 2 + 6 pages is refused in the browser, before any upload")
        pg.set_input_files("#picker", [p2a, p6])
        pg.wait_for_function(
            "document.querySelector('#hint').textContent.includes('8 页')", timeout=20000)
        pg.wait_for_timeout(200)
        print("   hint :", pg.inner_text("#hint"))
        print("   error:", pg.inner_text("#msg"))
        assert "最多 5 页" in pg.inner_text("#msg"), pg.inner_text("#msg")
        assert pg.is_disabled("#go"), "merge button should be disabled"
        pg.screenshot(path="work/_bench/lim_merge_over.png", full_page=True)

        print("3. compress page refuses a 6-page file on the client")
        pg.goto(BASE + "/compress", wait_until="networkidle")
        print("   ", pg.inner_text("#limits"))
        pg.set_input_files("#picker", p6)
        pg.wait_for_function(
            "document.querySelector('#msg').textContent.length > 0", timeout=20000)
        print("   error:", pg.inner_text("#msg"))
        assert "最多 5 页" in pg.inner_text("#msg"), pg.inner_text("#msg")
        assert pg.is_disabled("#go")
        assert pg.locator(".file.on").count() == 0, "file card should be cleared"
        pg.screenshot(path="work/_bench/lim_compress_over.png", full_page=True)

        print("4. a 4-page merge goes through on the single free allowance")
        pg.goto(BASE + "/", wait_until="networkidle")
        pg.set_input_files("#picker", [p2a, p2b])
        pg.wait_for_function(
            "document.querySelector('#hint').textContent.includes('4 页')", timeout=20000)
        assert not pg.is_disabled("#go"), "4 pages should be allowed"
        pg.click("#go")
        pg.wait_for_selector(".result.on", timeout=120000)
        print("   ", pg.inner_text(".result .ttl"))
        pg.screenshot(path="work/_bench/lim_merge_ok.png", full_page=True)

        print("5. allowance is now spent, page says so before uploading again")
        pg.goto(BASE + "/", wait_until="networkidle")
        pg.set_input_files("#picker", [p2a, p2b])
        pg.wait_for_function(
            "document.querySelector('#hint').textContent.includes('4 页')", timeout=20000)
        pg.wait_for_timeout(200)
        print("   hint:", pg.inner_text("#hint"))
        print("   msg :", pg.inner_text("#msg"))
        assert "额度已用完" in pg.inner_text("#hint"), pg.inner_text("#hint")
        assert pg.is_disabled("#go"), "merge must be blocked once the quota is gone"
        pg.screenshot(path="work/_bench/lim_no_quota.png", full_page=True)

        print("console errors:", errors[:3] if errors else "none")
        b.close()
finally:
    srv.terminate()
    try:
        srv.wait(timeout=10)
    except Exception:
        srv.kill()
    shutil.rmtree(DATA, ignore_errors=True)

print("LIMITS UI OK")
