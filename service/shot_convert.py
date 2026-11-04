"""Drive the tool grid and the generic conversion page in a real browser.

The assertions are the parts a screenshot cannot prove: that the grid lists all
eight tools, that the shared nav is built from the same registry (so a new tool
cannot be invisible from the page you are already on), that the page refuses the
wrong file type before uploading, and that the downloaded file is really the
format the tool promised.
"""
import os
import sys

import pymupdf as fitz
from playwright.sync_api import sync_playwright

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("shot_convert", pages=5000)
BASE = srv.base

BENCH = "work/_bench"
os.makedirs(BENCH, exist_ok=True)

# 3 pages of real text: fast, but still multi-page.
src_pdf = os.path.join(BENCH, "ui_conv.pdf")
doc = fitz.open()
for i in range(3):
    p = doc.new_page()
    p.insert_text((72, 90), "Chapter %d" % (i + 1), fontsize=20)
    y = 130
    for line in range(12):
        p.insert_text((72, y), "Paragraph %d line %d: the quick brown fox." % (i + 1, line + 1),
                      fontsize=10)
        y += 16
doc.save(src_pdf)
doc.close()

import docx as _docx
src_docx = os.path.join(BENCH, "ui_conv.docx")
d = _docx.Document()
d.add_heading("Quarterly report", 1)
for i in range(12):
    d.add_paragraph("Paragraph %d of the Word document body." % (i + 1))
d.save(src_docx)

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

    # ---------------------------------------------------------------- grid
    pg.goto(BASE + "/tools", wait_until="networkidle")
    pg.wait_for_selector(".card")
    cards = pg.eval_on_selector_all(".card .t", "els => els.map(e => e.textContent.trim())")
    print("grid (%d):" % len(cards), cards)
    assert len(cards) == 12, cards
    nav = pg.eval_on_selector_all(".nav a", "els => els.map(e => e.textContent.trim())")
    print("nav  (%d):" % len(nav), nav)
    assert len(nav) == 13 and nav[-1] == "全部工具", nav
    pg.screenshot(path="work/_bench/conv_tools.png", full_page=True)

    # ------------------------------------------------- PDF -> Word (generic page)
    pg.goto(BASE + "/convert/pdf-to-word", wait_until="networkidle")
    print("title:", pg.inner_text("h1"), "| sub:", pg.inner_text(".sub")[:28])
    assert pg.inner_text("h1") == "PDF 转 Word"
    assert pg.is_disabled("#ocr"), "OCR must be locked on the free tier"

    # Wrong type must be refused in the browser, not after a 20 MB upload.
    pg.set_input_files("#picker", src_docx)
    pg.wait_for_timeout(300)
    print("wrong-type msg:", pg.inner_text("#msg"))
    assert "PDF" in pg.inner_text("#msg"), pg.inner_text("#msg")

    pg.set_input_files("#picker", src_pdf)
    pg.wait_for_function("document.querySelector('#thumb img') !== null", timeout=30000)
    print("file meta:", pg.inner_text("#fmeta"))
    assert "3 页" in pg.inner_text("#fmeta")
    pg.screenshot(path="work/_bench/conv_word.png", full_page=True)

    pg.click("#go")
    pg.wait_for_selector(".result.on", timeout=180000)
    pg.wait_for_timeout(300)
    print("result:", pg.inner_text("#rname"), "|", pg.inner_text("#rmeta"))
    assert ".docx" in pg.inner_text("#rname")
    href = pg.get_attribute("#rdl", "href")
    import urllib.request
    with urllib.request.urlopen(BASE + href, timeout=60) as r:
        ctype = r.headers.get("Content-Type", "")
        blob = r.read()
    print("ctype:", ctype, "| bytes:", len(blob), blob[:2])
    assert "wordprocessingml" in ctype and blob[:2] == b"PK"
    pg.screenshot(path="work/_bench/conv_word_result.png", full_page=True)

    # ------------------------------------------------- Word -> PDF (other direction)
    pg.goto(BASE + "/convert/word-to-pdf", wait_until="networkidle")
    print("title:", pg.inner_text("h1"), "| limits:", pg.inner_text("#limits"))
    assert pg.inner_text("h1") == "Word 转 PDF"
    assert pg.eval_on_selector("#optbox", "e => getComputedStyle(e).display") == "none", \
        "OCR block must be hidden when the source is not a PDF"

    pg.set_input_files("#picker", src_docx)
    pg.wait_for_timeout(300)
    print("file meta:", pg.inner_text("#fmeta"))
    pg.click("#go")
    pg.wait_for_selector(".result.on", timeout=120000)
    pg.wait_for_timeout(300)
    print("result:", pg.inner_text("#rname"), "|", pg.inner_text("#rmeta"))
    assert ".pdf" in pg.inner_text("#rname")
    href = pg.get_attribute("#rdl", "href")
    with urllib.request.urlopen(BASE + href, timeout=60) as r:
        ctype = r.headers.get("Content-Type", "")
        blob = r.read()
    print("ctype:", ctype, "| magic:", blob[:5])
    assert ctype.startswith("application/pdf") and blob[:5] == b"%PDF-"
    pg.screenshot(path="work/_bench/conv_pdf.png", full_page=True)

    print("console errors:", errors or "none")
    assert not errors, errors
    b.close()

print("CONVERT UI OK")
