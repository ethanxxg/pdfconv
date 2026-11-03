"""Verify that the shipped DOCX files really were produced by the current code.

Why this exists
---------------
A previous revision of this project shipped output generated *before* a final
tuning change, so the docs looked fine while not matching the code.  Byte
comparison cannot catch that class of mistake: python-docx names embedded image
parts from a random temp filename, so two runs of identical code never produce
identical bytes.  The stable thing to compare is the *semantic* content:

    per paragraph -> (text, alignment, run-level bold/italic/size/font/colour)

Usage
-----
    # 1. re-convert the corpus with the current code
    python -m pdfconv --batch docs work/to_docx_vN --force

    # 2. prove the shipped folder matches it
    python tools/verify_shipped.py work/to_docx_vN docs/to_docx_own

Exit code 0 means every document is semantically identical.
"""
import sys, os, glob
from docx import Document


def signature(path):
    """Semantic fingerprint of a DOCX, independent of zip/image naming."""
    d = Document(path)
    out = []
    for p in d.paragraphs:
        runs = []
        for r in p.runs:
            color = None
            try:
                if r.font.color is not None and r.font.color.rgb is not None:
                    color = str(r.font.color.rgb)
            except Exception:
                pass
            runs.append((
                r.text,
                bool(r.bold),
                bool(r.italic),
                None if r.font.size is None else round(r.font.size.pt, 2),
                r.font.name,
                color,
                bool(r.font.superscript),
                bool(r.font.subscript),
            ))
        out.append((p.text, str(p.alignment), tuple(runs)))
    tables = []
    for t in d.tables:
        tables.append(tuple(tuple(c.text for c in row.cells) for row in t.rows))
    return out, tuple(tables)


def main(ref_dir, ship_dir):
    ref = {os.path.basename(p): p for p in glob.glob(os.path.join(ref_dir, "*.docx"))
           if not os.path.basename(p).startswith("~$")}
    ship = {os.path.basename(p): p for p in glob.glob(os.path.join(ship_dir, "*.docx"))
            if not os.path.basename(p).startswith("~$")}

    print(f"reference (fresh run) = {ref_dir}  ({len(ref)} files)")
    print(f"shipped               = {ship_dir}  ({len(ship)} files)")

    only_ship = sorted(set(ship) - set(ref))
    if only_ship:
        print(f"WARNING: shipped files with no fresh counterpart: {len(only_ship)}")
        for f in only_ship[:3]:
            print("   ", f[:70])

    same, diff, missing = 0, [], []
    for name in sorted(ref):
        if name not in ship:
            missing.append(name)
            continue
        a, at = signature(ref[name])
        b, bt = signature(ship[name])
        if a == b and at == bt:
            same += 1
        else:
            diff.append((name, a, b))

    total = len(ref)
    print(f"\nidentical : {same}/{total}")
    if missing:
        print(f"missing from shipped: {len(missing)}")
        for m in missing[:5]:
            print("   ", m[:70])
    if diff:
        print(f"DIFFERENT : {len(diff)}")
        for name, a, b in diff[:10]:
            note = f"paragraphs {len(a)} vs {len(b)}"
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    note += f"; first diff at para {i}"
                    break
            print(f"   {note}  {name[:56]}")
    ok = (same == total and not missing)
    print("\nRESULT:", "shipped output matches the current code" if ok
          else "MISMATCH -- re-run the batch and promote")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
