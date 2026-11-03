"""Regression gate for a freshly converted set.

Uses token-level (not character-level) comparison: re-segmenting paragraphs
changes whitespace but must never lose words.  Also reports heading alignment
so the heading work is measured, not eyeballed.
"""
import sys, os, glob, re, collections
from docx import Document

TOKEN = re.compile(r"[A-Za-z0-9]{3,}")


def load(path):
    d = Document(path)
    parts = [p.text for p in d.paragraphs]
    for t in d.tables:
        for r in t.rows:
            for c in r.cells:
                parts.append(c.text)
    text = " ".join(parts)
    toks = collections.Counter(TOKEN.findall(text))
    center = left = heads = 0
    for p in d.paragraphs:
        s = p.text.strip()
        if not s or len(s) > 110:
            continue
        runs = [r for r in p.runs if r.text.strip()]
        if not runs or not all(r.bold for r in runs):
            continue
        heads += 1
        a = str(p.alignment)
        if "CENTER" in a:
            center += 1
        elif "LEFT" in a:
            left += 1
    return dict(toks=toks, chars=len(text), paras=len(d.paragraphs),
                heads=heads, center=center, left=left, tables=len(d.tables))


def main(base, new, thresh=0.02):
    b = {os.path.basename(p): p for p in glob.glob(os.path.join(base, "*.docx"))
         if not os.path.basename(p).startswith("~$")}
    n = {os.path.basename(p): p for p in glob.glob(os.path.join(new, "*.docx"))
         if not os.path.basename(p).startswith("~$")}
    common = sorted(set(b) & set(n))
    print(f"baseline={len(b)} new={len(n)} common={len(common)}")
    miss = sorted(set(b) - set(n))
    if miss:
        print(f"MISSING ({len(miss)}): {miss[:3]}")

    tb = tn = 0
    center = left = heads = 0
    rows = []
    for f in common:
        sb, sn = load(b[f]), load(n[f])
        tb += sum(sb["toks"].values())
        tn += sum(sn["toks"].values())
        center += sn["center"]; left += sn["left"]; heads += sn["heads"]
        lost = sum(max(0, sb["toks"][k] - sn["toks"].get(k, 0)) for k in sb["toks"])
        tot = max(sum(sb["toks"].values()), 1)
        rows.append((lost / tot, lost, f, sb, sn))
    rows.sort(reverse=True)
    print(f"tokens baseline={tb} new={tn} ({100.0 * tn / max(tb, 1):.2f}%)")
    print(f"heading blocks={heads}  left={left}  center={center}")
    bad = [r for r in rows if r[0] > thresh]
    print(f"docs losing >{thresh:.0%} tokens: {len(bad)}/{len(common)}")
    for r in rows[:10]:
        print(f"  {r[0]:6.2%} lost={r[1]:5d} paras {r[3]['paras']}->{r[4]['paras']} "
              f"tables {r[3]['tables']}->{r[4]['tables']}  {r[2][:52]}")
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
