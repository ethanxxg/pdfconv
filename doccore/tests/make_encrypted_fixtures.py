"""生成 P11-3 的加密 PDF 夹具。

用 PyMuPDF（MuPDF）而不是 DocCore 生成，这样"能不能读"才是独立判断：
自己加密自己解密，两边的错误会互相抵消。

产出（tests/data/）：
  enc_rc4_40.pdf    /V1 /R2 RC4 40 位，用户口令 user / 所有者口令 owner
  enc_rc4_128.pdf   /V2 /R3 RC4 128 位
  enc_aes_128.pdf   /V4 /R4 AES-128
  enc_aes_256.pdf   /V5 /R6 AES-256
  enc_owner_only.pdf  RC4 128 位，只设所有者口令（空用户口令即可打开）
"""
import pathlib
import sys

import fitz

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "tests/data")
OUT.mkdir(parents=True, exist_ok=True)

# 正文要包含 ASCII、数字与符号，方便逐字核对。
LINES = [
    "Encrypted payload line one",
    "Second line with digits 0123456789",
    "Third line: punctuation !@#$%^&*()",
]


def build_base() -> fitz.Document:
    doc = fitz.open()
    page = doc.new_page(width=595, height=842)
    y = 100
    for line in LINES:
        page.insert_text((72, y), line, fontsize=14)
        y += 24
    return doc


CASES = [
    ("enc_rc4_40.pdf", fitz.PDF_ENCRYPT_RC4_40),
    ("enc_rc4_128.pdf", fitz.PDF_ENCRYPT_RC4_128),
    ("enc_aes_128.pdf", fitz.PDF_ENCRYPT_AES_128),
    ("enc_aes_256.pdf", fitz.PDF_ENCRYPT_AES_256),
]

for name, method in CASES:
    doc = build_base()
    path = OUT / name
    doc.save(
        path,
        encryption=method,
        user_pw="user",
        owner_pw="owner",
        permissions=fitz.PDF_PERM_PRINT | fitz.PDF_PERM_COPY,
    )
    doc.close()
    print(f"wrote {path} ({path.stat().st_size} bytes)")

# 只设所有者口令：空用户口令应该能直接打开。
doc = build_base()
path = OUT / "enc_owner_only.pdf"
doc.save(path, encryption=fitz.PDF_ENCRYPT_RC4_128, user_pw="", owner_pw="owner")
doc.close()
print(f"wrote {path} ({path.stat().st_size} bytes)")

# 顺带确认每个夹具确实是加密的、且 PyMuPDF 自己能用 user 口令读回正文。
for name, _ in CASES:
    d = fitz.open(OUT / name)
    if not d.needs_pass:
        print(f"ERROR {name} is not encrypted", file=sys.stderr)
        sys.exit(1)
    if not d.authenticate("user"):
        print(f"ERROR {name} rejected user password", file=sys.stderr)
        sys.exit(1)
    text = d[0].get_text()
    for line in LINES:
        if line not in text:
            print(f"ERROR {name} lost text {line!r}", file=sys.stderr)
            sys.exit(1)
    print(f"ok {name} metadata={d.metadata.get('encryption')}")

d = fitz.open(OUT / "enc_owner_only.pdf")
if d.needs_pass and not d.authenticate(""):
    print("ERROR enc_owner_only.pdf rejected empty password", file=sys.stderr)
    sys.exit(1)
print("ok enc_owner_only.pdf opens with empty password")
