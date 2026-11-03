"""Promote a validated output directory into the shipped folders.

Copies per file so a single Word-locked document cannot abort the whole run.

    python tools/promote.py work/to_docx_vN

Afterwards always run `python tools/verify_shipped.py work/to_docx_vN
docs/to_docx_own` to prove the shipped files really came from this code.
"""
import sys, os, glob, shutil, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def copy_into(src_dir, dst_dir):
    os.makedirs(dst_dir, exist_ok=True)
    ok = skip = 0
    failed = []
    for src in sorted(glob.glob(os.path.join(src_dir, "*.docx"))):
        name = os.path.basename(src)
        if name.startswith("~$"):
            continue
        dst = os.path.join(dst_dir, name)
        for attempt in range(4):
            try:
                shutil.copy2(src, dst)
                ok += 1
                break
            except (PermissionError, OSError) as e:
                if attempt == 3:
                    failed.append((name, str(e)))
                    skip += 1
                else:
                    time.sleep(1.5)
    print(f"{dst_dir}: copied={ok} failed={skip}")
    for n, e in failed:
        print(f"   LOCKED {n}  ({e})")
    return failed


def main(src):
    src = src if os.path.isabs(src) else os.path.join(ROOT, src)
    f1 = copy_into(src, os.path.join(ROOT, "docs", "to_docx_own"))
    f2 = copy_into(src, os.path.join(ROOT, "docs", "to_docx"))
    return 0 if not (f1 or f2) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
