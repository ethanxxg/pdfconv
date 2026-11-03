"""Corpus-scale translation QA.

For every PDF in a directory, translate it and report what the landing mode
actually did to the page:

* **pages** in / out -- ``inplace`` must never change this;
* **boxes** seen, **shrunk** (type had to go below 100%, floor 75%) and
  **overflow** (no scale fitted, so the block kept its source text);
* **collisions** -- line boxes that overlap, measured on the output and on
  the source, because a redraw that spills into the next paragraph shows up
  here and nowhere else;
* **han** -- Chinese characters actually present, so a document that silently
  translated nothing is visible.

``inplace`` reads text blocks straight from the PDF, so it needs no OCR and
runs over the whole corpus in minutes.  ``reflow`` is the full
convert -> translate -> render path and is much slower.

Usage
-----
    python tools/translate_qa.py docs --mode inplace --engine glossary \
        [--glossary work/tmp/gl.json] [--limit N] [--out work/tmp/qa.json]
"""
import argparse
import glob
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import fitz

from pdfconv.translate import GlossaryTranslator, EchoTranslator, translate_pdf

HAN = re.compile(r"[\u4e00-\u9fff]")


def _lines(page):
    out = []
    for b in page.get_text("dict")["blocks"]:
        if b.get("type") != 0:
            continue
        for ln in b.get("lines", []):
            r = fitz.Rect(ln["bbox"])
            if r.width > 1 and r.height > 1:
                out.append(r)
    return out


def _collisions(page, tol=1.2):
    ls = _lines(page)
    n = 0
    for i in range(len(ls)):
        a = ls[i]
        for j in range(i + 1, len(ls)):
            b = ls[j]
            if (min(a.x1, b.x1) - max(a.x0, b.x0) > tol and
                    min(a.y1, b.y1) - max(a.y0, b.y0) > 2 * tol):
                n += 1
    return n


def run_one(path, out_path, mode, engine, glossary):
    t0 = time.time()
    st, pages = translate_pdf(path, out_path, engine, src="en", tgt="zh",
                              glossary=glossary, mode=mode)
    src = fitz.open(path)
    dst = fitz.open(out_path)
    han = sum(len(HAN.findall(p.get_text())) for p in dst)
    coll = sum(_collisions(p) for p in dst)
    coll0 = sum(_collisions(p) for p in src)
    row = dict(
        doc=os.path.basename(path)[:46],
        pages_in=len(src), pages_out=len(dst),
        boxes=st.boxes, shrunk=st.shrunk, overflow=st.overflow,
        units=st.units, fallback=st.fallback,
        chars_in=st.chars_in, chars_out=st.chars_out,
        han=han, collisions=coll, collisions_src=coll0,
        seconds=round(time.time() - t0, 1),
    )
    src.close()
    dst.close()
    return row


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("docs")
    ap.add_argument("--out-dir", default="work/tmp/qa_out")
    ap.add_argument("--mode", default="inplace",
                    choices=("inplace", "reflow", "bilingual"))
    ap.add_argument("--engine", default="glossary",
                    choices=("glossary", "echo", "http"))
    ap.add_argument("--glossary")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out")
    a = ap.parse_args(argv)

    glossary = {}
    if a.glossary and os.path.exists(a.glossary):
        with open(a.glossary, encoding="utf-8") as f:
            glossary = json.load(f)
    if a.engine == "glossary":
        eng = GlossaryTranslator(glossary)
    elif a.engine == "echo":
        eng = EchoTranslator()
    else:
        from pdfconv.translate import HttpTranslator
        eng = HttpTranslator()

    files = sorted(glob.glob(os.path.join(a.docs, "*.pdf")))
    if a.limit:
        files = files[:a.limit]
    os.makedirs(a.out_dir, exist_ok=True)
    rows = []
    print("%-46s %5s %5s %5s %5s %5s %6s %6s" %
          ("document", "pin", "pout", "box", "shr", "ovf", "han", "coll"))
    for f in files:
        out = os.path.join(a.out_dir, os.path.splitext(os.path.basename(f))[0]
                           + ".zh.pdf")
        try:
            r = run_one(f, out, a.mode, eng, glossary)
        except Exception as e:                       # keep the run alive
            print("%-46s ERR %r" % (os.path.basename(f)[:46], e))
            continue
        rows.append(r)
        print("%-46s %5d %5d %5d %5d %5d %6d %6d" %
              (r["doc"], r["pages_in"], r["pages_out"], r["boxes"],
               r["shrunk"], r["overflow"], r["han"], r["collisions"]))

    if not rows:
        print("nothing measured")
        return 1
    tot = lambda k: sum(r[k] for r in rows)
    same = sum(1 for r in rows if r["pages_in"] == r["pages_out"])
    print("-" * 92)
    print("docs=%d  pages=%d->%d  page-preserved=%d/%d  boxes=%d  "
          "shrunk=%d (%.1f%%)  overflow=%d (%.2f%%)  han=%d"
          % (len(rows), tot("pages_in"), tot("pages_out"), same, len(rows),
             tot("boxes"), tot("shrunk"),
             100.0 * tot("shrunk") / max(1, tot("boxes")),
             tot("overflow"),
             100.0 * tot("overflow") / max(1, tot("boxes")), tot("han")))
    print("collisions: output %d vs source %d  (%d docs worse)"
          % (tot("collisions"), tot("collisions_src"),
             sum(1 for r in rows if r["collisions"] > r["collisions_src"])))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(rows, f, ensure_ascii=False, indent=1)
        print("wrote", a.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
