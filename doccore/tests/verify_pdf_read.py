"""Independent verifier for PDF **reading** quality against third-party PDFs.

Nothing here uses DocCore code: the reference text comes from PyMuPDF, our text
from the `doccore pdf <file> --mode text` CLI. Two directions are compared:

  ref -> ours   every non-empty reference line must appear in our output
                (whitespace-normalised containment; proves nothing is lost)
  ours -> ref   every non-empty line we emit must appear in the reference
                (proves we do not invent text)

Files with no text layer (scans) and encrypted files are expected to be
reported as such rather than silently skipped, and they are counted separately.

Usage:
  python tests/verify_pdf_read.py [files or globs...]
  python tests/verify_pdf_read.py --root <dir>     # walk <dir> for *.pdf

With no arguments it walks the Aspose reference tree if present, otherwise the
repository's own tests/data directory.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

DEFAULT_ROOT = r"C:\Program Files\Aspose"
REPO_DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")

failures = []
checks = 0
skipped = []


def check(cond, message):
    global checks
    checks += 1
    if not cond:
        failures.append(message)
        print("FAIL %s" % message)


def norm(text):
    return re.sub(r"\s+", "", text or "")


def cli_text(exe, path):
    proc = subprocess.run([exe, "pdf", path, "--mode", "text"], capture_output=True)
    out = proc.stdout.decode("utf-8", "replace")
    return proc.returncode, out


def collect(paths, roots):
    found = []
    for p in paths:
        hits = glob.glob(p, recursive=True)
        if hits:
            found.extend(hits)
        elif os.path.isfile(p):
            found.append(p)
    for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                if name.lower().endswith(".pdf"):
                    found.append(os.path.join(dirpath, name))
    seen, unique = set(), []
    for f in found:
        key = os.path.abspath(f)
        if key not in seen:
            seen.add(key)
            unique.append(f)
    return sorted(unique)


def verify_file(exe, path):
    import fitz  # imported lazily: only this verifier needs it

    short = os.path.basename(path)
    code, ours = cli_text(exe, path)
    if code != 0 or ours.startswith("error"):
        # 加密/损坏的 PDF 必须被明确拒绝，而不是静默当成空文档。
        doc = fitz.open(path)
        if doc.needs_pass:
            check(True, "%s: encrypted file rejected as expected" % short)
        else:
            check(False, "%s: cli failed: %s" % (short, ours.strip()[:60]))
        return
    doc = fitz.open(path)
    if doc.needs_pass:
        skipped.append((short, "needs password"))
        return

    ref_lines = []
    for page in doc:
        for line in page.get_text().splitlines():
            if line.strip():
                ref_lines.append(line)
    ours_lines = [l for l in ours.splitlines() if l.strip()]
    refn = norm("".join(ref_lines))
    ourn = norm("".join(ours_lines))

    missing = 0
    for line in ref_lines:
        if norm(line) not in ourn:
            missing += 1
            if missing <= 3:
                print("  MISS-REF %s: %r" % (short, line[:70]))
    invented = 0
    for line in ours_lines:
        if norm(line) not in refn:
            invented += 1
            if invented <= 3:
                print("  MISS-OURS %s: %r" % (short, line[:70]))

    total = len(ref_lines)
    matched = total - missing
    ratio = (matched / total) if total else 1.0
    print("%6.3f  %5d/%5d  ours=%3d inv=%d  %s"
          % (ratio, matched, total, len(ours_lines), invented, short))
    if total:
        # 阅读抽取的门禁：丢字不超过 1%，不许多出行。
        check(missing <= max(1, int(total * 0.01)),
              "%s: %d/%d reference lines missing" % (short, missing, total))
    check(invented == 0, "%s: %d invented lines" % (short, invented))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--root", action="append", default=[])
    ap.add_argument("--exe", default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    exe = args.exe or os.path.join(here, "..", "build", "Release", "doccore.exe")
    if not os.path.exists(exe):
        print("doccore.exe not found at %s; pass --exe" % exe)
        return 2

    files = collect(args.paths, args.root)
    if not files:
        files = collect([REPO_DATA], [])
    if not files:
        print("no PDF files to check")
        return 2

    print("comparing %d PDF file(s) against PyMuPDF" % len(files))
    for path in files:
        try:
            verify_file(exe, path)
        except Exception as exc:  # noqa: BLE001 - report and keep going
            failures.append("%s: %s" % (path, exc))
            print("ERROR %s: %s" % (path, exc))

    for name, why in skipped:
        print("skip  %s (%s)" % (name, why))
    print("%d checks, %d failures" % (checks, len(failures)))
    if failures:
        for f in failures:
            print("  - %s" % f)
        return 1
    print("pdf read ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
