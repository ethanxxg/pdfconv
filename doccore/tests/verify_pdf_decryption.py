"""P11-3 独立复核：确认 DocCore 解出的正文与 PyMuPDF（MuPDF）逐字一致。

夹具是 PyMuPDF 加密的，DocCore 只负责解密。这里做三件事：
  1. 从原始字节里读出 /V /R /Length /CFM，确认每个夹具真的是它声称的算法
     （否则"解密成功"可能只是因为文件根本没加密）；
  2. 确认正文在原始字节里找不到明文——加密必须是真的加密过；
  3. 把 DocCore 落盘的正文与 PyMuPDF 解出的正文做逐字比对。
     DocCore 用自己的安全处理器解，PyMuPDF 用 MuPDF 的解，两边独立。
"""
import pathlib
import re
import sys

import fitz

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out")
DATA = pathlib.Path(__file__).resolve().parent / "data"

LINES = [
    "Encrypted payload line one",
    "Second line with digits 0123456789",
    "Third line: punctuation !@#$%^&*()",
]

# 文件名 -> (期望 /V, 期望 /R, 期望 CFM)
CASES = [
    ("enc_rc4_40.pdf", 1, 2, None),
    ("enc_rc4_128.pdf", 2, 3, None),
    ("enc_aes_128.pdf", 4, 4, "AESV2"),
    ("enc_aes_256.pdf", 5, 6, "AESV3"),
]

fails = []
checks = 0


def check(cond, label):
    global checks
    checks += 1
    if not cond:
        fails.append(label)


def raw(path):
    return path.read_bytes()


def int_of(raw_bytes, key):
    m = re.search(rb"/" + key + rb"\s+(-?\d+)", raw_bytes)
    return int(m.group(1)) if m else None


for name, want_v, want_r, want_cfm in CASES:
    path = DATA / name
    if not path.exists():
        check(False, f"{name}: fixture missing")
        continue
    blob = raw(path)

    # ---- 1. 算法自证 -----------------------------------------------------
    check(b"/Filter/Standard" in blob or b"/Filter /Standard" in blob,
          f"{name}: /Filter /Standard missing")
    check(int_of(blob, b"V") == want_v, f"{name}: /V {int_of(blob, b'V')} != {want_v}")
    check(int_of(blob, b"R") == want_r, f"{name}: /R {int_of(blob, b'R')} != {want_r}")
    if want_cfm is not None:
        check(want_cfm.encode() in blob, f"{name}: /CFM {want_cfm} missing")

    # ---- 2. 正文必须真的被加密 ------------------------------------------
    for line in LINES:
        check(line.encode() not in blob, f"{name}: plaintext leaked into raw bytes: {line!r}")
    # /O 与 /U 是必需的 32 字节校验值（R5+ 为 48 字节）。
    check(b"/O" in blob and b"/U" in blob, f"{name}: /O or /U missing")

    # ---- 3. 与 DocCore 的抽取结果逐字比对 --------------------------------
    doc = fitz.open(path)
    check(doc.needs_pass, f"{name}: PyMuPDF says not encrypted")
    for pw in ("user", "owner"):
        # 每个口令都要重新打开：authenticate 会改变文档状态。
        d = fitz.open(path)
        check(d.authenticate(pw) > 0, f"{name}: PyMuPDF rejected {pw}")
        theirs = d[0].get_text()
        dump = OUT / (name[:-4] + f"_{pw}.txt")
        if not dump.exists():
            check(False, f"{name}: DocCore dump {dump.name} missing")
            d.close()
            continue
        ours = dump.read_text(encoding="utf-8", errors="replace")
        for line in LINES:
            check(line in ours, f"{name}/{pw}: DocCore lost {line!r}")
            check(line in theirs, f"{name}/{pw}: PyMuPDF lost {line!r}")
        # 去掉所有空白后必须完全相同：字体子集/换行差异会被抹平，
        # 但任何字符错误都会暴露。
        squash = lambda s: "".join(s.split())
        check(squash(ours) == squash(theirs),
              f"{name}/{pw}: text mismatch\n  DocCore: {squash(ours)!r}\n  MuPDF  : {squash(theirs)!r}")
        d.close()

    # 错口令必须被拒。
    d = fitz.open(path)
    check(d.authenticate("nope") <= 0, f"{name}: PyMuPDF accepted a wrong password")
    d.close()
    doc.close()

# ---- 只设所有者口令：空口令可读 ----------------------------------------
path = DATA / "enc_owner_only.pdf"
if not path.exists():
    check(False, "enc_owner_only.pdf: fixture missing")
else:
    # 用户口令为空时 MuPDF 打开即自动认证，所以 needs_pass 是 False——
    # 这正是"空口令可读"的意思。加密与否要看原始字节里的 /Encrypt。
    blob = raw(path)
    check(b"/Encrypt" in blob, "enc_owner_only.pdf: no /Encrypt in raw bytes")
    d = fitz.open(path)
    check(not d.needs_pass, "enc_owner_only.pdf: unexpectedly requires a password")
    text = d[0].get_text()
    for line in LINES:
        check(line in text, f"enc_owner_only.pdf: lost {line!r}")
    # 所有者口令同样能开（走 Algorithm 6 的路径）。
    d.close()
    d = fitz.open(path)
    check(d.authenticate("owner") > 0, "enc_owner_only.pdf: owner password rejected")
    d.close()

# ---- 未加密文件不应被误判 ----------------------------------------------
plain = DATA / "sample_pdf.pdf"
if plain.exists():
    d = fitz.open(plain)
    check(not d.needs_pass, "sample_pdf.pdf: unexpectedly encrypted")
    d.close()

if fails:
    print(f"FAILED {len(fails)}/{checks}")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print(f"ok {checks} checks")
