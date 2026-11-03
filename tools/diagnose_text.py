"""Quantify how much text in each PDF is *structurally* undecodable.

A CID/Type0 font with Identity encoding and no /ToUnicode cannot be mapped back
to Unicode by any converter -- there is no information in the file to do it
with.  That is a property of the PDF, not a bug we can fix, so it must be
*measured* rather than guessed at.

The criterion is deliberately narrow, because "no /ToUnicode" alone is NOT a
defect: a simple font (Type1/TrueType) without /ToUnicode still decodes
correctly, since MuPDF falls back to the font's built-in encoding and glyph
names.  Only CID fonts are genuinely unrecoverable, because Identity encoding
makes the code equal to the glyph id, and a glyph id carries no meaning without
a cmap.  So a character is counted as undecodable only when it was drawn with a
Type0 font that has Identity encoding and no /ToUnicode.

Usage:
    python tools/diagnose_text.py docs            # whole folder
    python tools/diagnose_text.py docs/a.pdf      # one file
"""
import os
import sys
import glob

import fitz


def _has_tounicode(doc, xref):
    try:
        kind, val = doc.xref_get_key(xref, "ToUnicode")
    except Exception:
        return False
    return kind not in ("null", "none") and bool(val)


def _font_status(doc, xref, subtype, encoding):
    """True when text drawn with this font is recoverable in principle."""
    if subtype != "Type0":
        return True                      # simple font: built-in encoding works
    if not encoding.startswith("Identity"):
        return True                      # a real CMap (Uni*-UCS2, GBK-EUC, ...)
    return _has_tounicode(doc, xref)


def scan(path):
    doc = fitz.open(path)
    per_page = []
    for pno in range(doc.page_count):
        page = doc[pno]
        total = bad = 0
        try:
            trace = page.get_texttrace()
        except Exception:
            continue
        # base font name -> recoverable?  resolved once per page
        cache = {}
        fonts = doc.get_page_fonts(pno)
        for span in trace:
            fname = span.get("font", "")
            ok = cache.get(fname)
            if ok is None:
                ok = True
                for xref, _ext, sub, base, _res, enc in fonts:
                    if base == fname or base.split("+")[-1] == fname.split("+")[-1]:
                        ok = _font_status(doc, xref, sub, enc)
                        break
                cache[fname] = ok
            n = len(span.get("chars", ()))
            total += n
            if not ok:
                bad += n
        per_page.append((total, bad))
    doc.close()
    total = sum(t for t, _ in per_page)
    bad = sum(b for _, b in per_page)
    pages_bad = sum(1 for t, b in per_page if t and b / t > 0.5)
    return dict(path=path, chars=total, undecodable=bad,
                ratio=(bad / total if total else 0.0),
                pages=len(per_page), pages_bad=pages_bad)


def main(argv):
    targets = []
    for a in argv:
        if os.path.isdir(a):
            targets += sorted(glob.glob(os.path.join(a, "*.pdf")))
        else:
            targets.append(a)
    if not targets:
        print("usage: python tools/diagnose_text.py <dir-or-pdf> [...]")
        return 2

    rows = [scan(p) for p in targets]
    rows.sort(key=lambda r: -r["ratio"])

    print(f"{'file':58} {'chars':>9} {'undec':>8} {'ratio':>7} {'badpages':>9}")
    print("-" * 96)
    affected = 0
    for r in rows:
        if r["ratio"] < 0.01 and not r["pages_bad"]:
            continue
        affected += 1
        name = os.path.basename(r["path"])
        print(f"{name[:58]:58} {r['chars']:9d} {r['undecodable']:8d} "
              f"{r['ratio']:6.1%} {r['pages_bad']:4d}/{r['pages']:<4d}")
    if not affected:
        print("(no document has undecodable text)")
    print("-" * 96)
    print(f"scanned={len(rows)}  affected={affected}  "
          f"total_chars={sum(r['chars'] for r in rows)}  "
          f"undecodable={sum(r['undecodable'] for r in rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
