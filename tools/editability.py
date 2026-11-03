#!/usr/bin/env python3
"""Editability audit for PDF->DOCX reconstruction.

A faithful *render* back to PDF proves the visual surface is right, but it says
nothing about how *editable* the produced DOCX is in Word.  This tool measures
the structural richness of a DOCX directly from its OOXML parts and compares
our converter's output against the Aspose reference corpus in ``docs/to_docx_own``.

Counted per document:
    paragraphs   total body paragraphs
    headings     paragraphs carrying a built-in Heading N style
    bookmarks    bookmarkStart elements (outline / cross-ref anchors)
    lists        paragraphs using List Number / List Bullet
    tables       w:tbl count
    merges       cells covered by gridSpan or vMerge (real merged cells)
    header_rows  rows flagged w:tblHeader (repeat on every page)
    hyperlinks   w:hyperlink references
    equations    m:oMath / m:oMathPara native editable equations
    images       drawings / picture blips

Usage:
    python tools/editability.py docs --limit 12
    python tools/editability.py docs --all
    python tools/editability.py one.pdf            # also converts, then audits

With no corpus argument it converts every PDF under docs/ once and audits the
produced DOCX against the matching Aspose reference, writing a JSON summary.
"""
import os
import re
import sys
import json
import glob
import argparse
import zipfile
import xml.etree.ElementTree as ET

# allow running as a script: make the project root importable
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
M = "http://schemas.openxmlformats.org/officeDocument/2006/math"
REL = "http://schemas.openxmlformats.org/package/2006/relationships"
DRAW = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
PIC = "http://schemas.openxmlformats.org/drawingml/2006/picture"
A = "http://schemas.openxmlformats.org/drawingml/2006/main"

NS = {"w": W, "m": M, "r": REL, "wp": DRAW, "pic": PIC, "a": A}


def _q(uri, tag):
    return "{%s}%s" % (uri, tag)


def audit_docx(path):
    """Return a dict of structural counts parsed from the DOCX package."""
    c = dict(paragraphs=0, headings=0, bookmarks=0, lists=0, tables=0,
             merges=0, header_rows=0, hyperlinks=0, equations=0, images=0)
    try:
        z = zipfile.ZipFile(path)
    except Exception:
        return c
    doc_xml = None
    for n in z.namelist():
        if n.endswith("word/document.xml"):
            doc_xml = z.read(n)
            break
    if doc_xml is None:
        return c
    root = ET.fromstring(doc_xml)
    body = root.find(_q(W, "body"))
    if body is None:
        return c

    def pstyle_of(p):
        pPr = p.find(_q(W, "pPr"))
        if pPr is None:
            return None
        ps = pPr.find(_q(W, "pStyle"))
        if ps is None:
            return None
        return ps.get(_q(W, "val"))

    for p in body.iter(_q(W, "p")):
        c["paragraphs"] += 1
        st = pstyle_of(p)
        if st and re.match(r"^Heading\d+$", st):
            c["headings"] += 1
        elif st in ("ListNumber", "ListNumber1", "List Bullet", "ListBullet",
                    "List Number", "List Bullet"):
            c["lists"] += 1
        if p.find(".//" + _q(W, "bookmarkStart")) is not None:
            c["bookmarks"] += len(p.findall(".//" + _q(W, "bookmarkStart")))
        if p.find(".//" + _q(M, "oMath")) is not None or \
           p.find(".//" + _q(M, "oMathPara")) is not None:
            c["equations"] += 1
        if p.find(".//" + _q(W, "hyperlink")) is not None:
            c["hyperlinks"] += 1

    for tbl in body.iter(_q(W, "tbl")):
        c["tables"] += 1
        for tr in tbl.iter(_q(W, "tr")):
            trPr = tr.find(_q(W, "trPr"))
            if trPr is not None and trPr.find(_q(W, "tblHeader")) is not None:
                c["header_rows"] += 1
        for tc in tbl.iter(_q(W, "tc")):
            tcPr = tc.find(_q(W, "tcPr"))
            if tcPr is None:
                continue
            if tcPr.find(_q(W, "gridSpan")) is not None:
                c["merges"] += 1
            vm = tcPr.find(_q(W, "vMerge"))
            if vm is not None and vm.get(_q(W, "val")) != "restart":
                c["merges"] += 1

    # images: any drawing / picture anywhere in the body
    c["images"] = sum(1 for _ in body.iter(_q(W, "drawing"))) + \
                 sum(1 for _ in body.iter(_q(PIC, "pic")))
    z.close()
    return c


def heading_sequence(path):
    """Ordered list of heading texts (document order) -- used to measure
    reading-order fidelity against the Aspose reference."""
    seq = []
    _norm = lambda t: " ".join((t or "").split()).lower()
    try:
        z = zipfile.ZipFile(path)
    except Exception:
        return seq
    data = None
    for n in z.namelist():
        if n.endswith("word/document.xml"):
            data = z.read(n)
            break
    if data is None:
        return seq
    root = ET.fromstring(data)
    body = root.find(_q(W, "body"))
    if body is None:
        return seq
    for p in body.iter(_q(W, "p")):
        pPr = p.find(_q(W, "pPr"))
        if pPr is None:
            continue
        ps = pPr.find(_q(W, "pStyle"))
        if ps is None:
            continue
        if not re.match(r"^Heading\d+$", ps.get(_q(W, "val")) or ""):
            continue
        txt = "".join(t.text or "" for t in p.iter(_q(W, "t")))
        seq.append(_norm(txt))
    z.close()
    return seq


def _lcs_ratio(a, b):
    """Longest-common-subsequence ratio -- fraction of headings whose order
    we reproduce relative to the larger sequence.  1.0 == identical order."""
    n, m = len(a), len(b)
    if n == 0 and m == 0:
        return 1.0
    if n == 0 or m == 0:
        return 0.0
    prev = [0] * (m + 1)
    for i in range(1, n + 1):
        cur = [0] * (m + 1)
        for j in range(1, m + 1):
            cur[j] = (prev[j - 1] + 1) if a[i - 1] == b[j - 1] else \
                     max(prev[j], cur[j - 1])
        prev = cur
    return prev[m] / float(max(n, m))


def _match_ref(pdf_name, ref_dir):
    base = os.path.basename(pdf_name)
    # strip leading number prefix "015__" and extension
    stem = re.sub(r"^\d+__", "", base)
    stem = re.sub(r"\.(pdf|PDF)$", "", stem).strip()
    best, best_score = None, 0
    for rf in os.listdir(ref_dir):
        if not rf.lower().endswith(".docx"):
            continue
        rstem = re.sub(r"^\d+__", "", rf)
        rstem = re.sub(r"\.(docx)$", "", rstem, flags=re.I).strip()
        score = 0
        sa, sb = stem.lower(), rstem.lower()
        for i in range(min(len(sa), len(sb))):
            if sa[i] == sb[i]:
                score += 1
            else:
                break
        if score > best_score:
            best_score, best = score, os.path.join(ref_dir, rf)
    return best


def summarize(records):
    """records: list of (pdf, ref_path, ours_dict, ref_dict, ours_seq, ref_seq)."""
    keys = ["paragraphs", "headings", "bookmarks", "lists", "tables",
            "merges", "header_rows", "hyperlinks", "equations", "images"]
    tot = {k: [0, 0] for k in keys}
    order_scores = []
    per_doc = []
    for pdf, ref_path, ours, refd, oseq, rseq in records:
        if ours is None or refd is None:
            continue
        row = {"pdf": os.path.basename(pdf)}
        for k in keys:
            o, r = ours[k], refd[k]
            tot[k][0] += o
            tot[k][1] += r
            row[k] = {"ours": o, "aspose": r}
        sc = _lcs_ratio(oseq, rseq)
        order_scores.append(sc)
        row["order"] = round(sc, 3)
        per_doc.append(row)
    n = len(per_doc)
    print("\n=== corpus editability: OURS vs ASPOSE (sum over %d docs) ==="
          % n)
    print("%-12s %10s %10s %8s" % ("metric", "ours", "aspose", "ratio"))
    print("-" * 44)
    for k in keys:
        o, r = tot[k]
        ratio = (o / r) if r else (1.0 if o == 0 else float("nan"))
        print("%-12s %10d %10d %7.2f" % (k, o, r, ratio))
    if order_scores:
        order_scores.sort()
        med = order_scores[n // 2]
        mean = sum(order_scores) / n
        worst = order_scores[0]
        print("-" * 44)
        print("%-12s %10s %10s %8s" % ("heading_order", "median", "mean", "worst"))
        print("%-12s %10.2f %10.2f %8.2f" % ("(LCS ratio)", med, mean, worst))
        print("  docs with order < 0.95: %d / %d"
              % (sum(1 for s in order_scores if s < 0.95), n))
    return {"totals": tot, "order": order_scores, "per_doc": per_doc}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf_dir", nargs="?", default="docs")
    ap.add_argument("--ref", default="docs/to_docx_own")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--out", default="work/tmp/editability.json")
    ap.add_argument("--convert-dir", default="work/tmp/ed_docx")
    args = ap.parse_args(argv)

    os.makedirs(args.convert_dir, exist_ok=True)
    pdfs = sorted(glob.glob(os.path.join(args.pdf_dir, "*.pdf")))
    if args.limit:
        pdfs = pdfs[:args.limit]

    records = []
    for i, pdf in enumerate(pdfs):
        ref = _match_ref(pdf, args.ref)
        if not ref or not os.path.exists(ref):
            print("skip (no ref):", os.path.basename(pdf))
            continue
        out_docx = os.path.join(args.convert_dir,
                                os.path.basename(pdf) + ".docx")
        # produce our DOCX only if missing (keeps re-runs fast)
        if not os.path.exists(out_docx):
            try:
                from pdfconv import convert
                convert(pdf, out_docx)
            except Exception as e:
                print("convert failed:", os.path.basename(pdf), e)
                continue
        ours = audit_docx(out_docx)
        refd = audit_docx(ref)
        oseq = heading_sequence(out_docx)
        rseq = heading_sequence(ref)
        records.append((pdf, ref, ours, refd, oseq, rseq))
        print("audited %3d/%-3d %-50s H %d/%d T %d/%d eq %d/%d"
              % (i + 1, len(pdfs), os.path.basename(pdf)[:50],
                 ours["headings"], refd["headings"],
                 ours["tables"], refd["tables"],
                 ours["equations"], refd["equations"]))

    summary = summarize(records)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, ensure_ascii=False, indent=2)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
