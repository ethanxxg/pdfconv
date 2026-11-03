"""Whitespace-insensitive content comparison: the real "did we lose text" test.

Token multisets are misleading on math-heavy papers because subscript/superscript
re-joining changes token boundaries without changing content.
"""
import sys, os, re
from docx import Document


def blob(path):
    d = Document(path)
    parts = [p.text for p in d.paragraphs]
    for t in d.tables:
        for r in t.rows:
            for c in r.cells:
                parts.append(c.text)
    return re.sub(r"\s+", "", " ".join(parts))


def main(base, new):
    rows = []
    for f in sorted(os.listdir(new)):
        if not f.endswith(".docx") or f.startswith("~$"):
            continue
        o = os.path.join(base, f)
        if not os.path.exists(o):
            continue
        a = blob(o)
        b = blob(os.path.join(new, f))
        rows.append((len(b) / max(len(a), 1), len(a), len(b), f))
    rows.sort()
    tot_a = sum(r[1] for r in rows)
    tot_b = sum(r[2] for r in rows)
    print(f"docs={len(rows)}")
    print(f"chars(no ws) baseline={tot_a} new={tot_b} "
          f"({100.0 * tot_b / max(tot_a, 1):.2f}%)")
    worse = [r for r in rows if r[0] < 0.98]
    print(f"docs below 98%: {len(worse)}")
    for r in rows[:10]:
        print(f"  {r[0]:.4f}  {r[1]}->{r[2]}  {r[3][:56]}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
