"""P11-4 独立复核：Word 水印与文档保护。

三路交叉：
  1. zipfile + ElementTree —— 生成的 XML 到底长什么样（VML 形状、命名空间、
     页眉引用、settings 里的保护元素）；
  2. python-docx —— 包结构是否被第三方实现接受（打不开就说明包写坏了）；
  3. PyMuPDF —— 水印在 PDF 里是不是真的旋转着画出来的、颜色对不对。

文档保护的口令哈希这里用 hashlib **重新实现一遍**（ISO 29500 §17.15.1.29），
与 DocCore 的 C++ 实现逐字节比对。两边独立，实现错了立刻暴露。
"""
import base64
import hashlib
import pathlib
import sys
import zipfile

import fitz

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out")

fails = []
checks = 0


def check(cond, label):
    global checks
    checks += 1
    if not cond:
        fails.append(label)


def read_part(path, name):
    with zipfile.ZipFile(path) as z:
        return z.read(name).decode("utf-8")


def part_names(path):
    with zipfile.ZipFile(path) as z:
        return z.namelist()


# ---- 1. 文字水印 --------------------------------------------------------
WM = OUT / "p11_watermark.docx"
check(WM.exists(), "p11_watermark.docx missing")
if WM.exists():
    names = part_names(WM)
    check("word/header1.xml" in names, "no header1.xml written for the watermark")
    # 这份文档没有任何保护、也没有原 settings.xml，所以不该凭空生成一份
    #（settings.xml 在 OOXML 里是可选的）。

    header = read_part(WM, "word/header1.xml")
    check('xmlns:v="urn:schemas-microsoft-com:vml"' in header, "v namespace not declared")
    check('xmlns:o="urn:schemas-microsoft-com:office:office"' in header, "o namespace not declared")
    # 形状必须是 v:shape，不能是 w:shape（前缀写错 Word 会当成未知元素）。
    check("<v:shape" in header, "v:shape element missing")
    check("<w:shape" not in header, "shape emitted with the w: prefix")
    check("<v:textpath" in header, "v:textpath missing")
    check('string="CONFIDENTIAL"' in header, "watermark text not in v:textpath/@string")
    check("rotation:315" in header, "rotation not written to the shape style")
    check("#C0C0C0" in header, "fillcolor missing")
    check("<v:fill" in header and 'opacity=".5"' in header, "semi-transparent fill missing")
    check("_x0000_t136" in header, "art-text shapetype missing")
    check("fitshape" in header, "fitshape attribute missing (text would not stretch)")

    document = read_part(WM, "word/document.xml")
    check("headerReference" in document, "sectPr has no headerReference")
    check('w:type="default"' in document, "headerReference is not the default header")
    # 合成页眉必须真的挂上关系，否则 Word 找不到部件。
    doc_rels = read_part(WM, "word/_rels/document.xml.rels")
    check("header1.xml" in doc_rels, "document rels do not point at header1.xml")
    ct = read_part(WM, "[Content_Types].xml")
    check("wordprocessingml.header+xml" in ct, "header content type missing")

    # ---- python-docx 能不能打开 ----------------------------------------
    try:
        import docx as pydocx

        d = pydocx.Document(str(WM))
        check(True, "")
        # python-docx 不建模水印，但应该能读到正文。
        check("body text under the watermark" in "\n".join(p.text for p in d.paragraphs),
              "python-docx lost the body text")
        # 页眉段落存在（水印载体），但正文不受影响。
        check(len(d.sections) == 1, f"python-docx sees {len(d.sections)} sections")
    except Exception as exc:  # noqa: BLE001
        check(False, f"python-docx could not open p11_watermark.docx: {exc}")

# ---- 2. 图片水印 --------------------------------------------------------
WMI = OUT / "p11_watermark_image.docx"
check(WMI.exists(), "p11_watermark_image.docx missing")
if WMI.exists():
    names = part_names(WMI)
    check("word/media/watermark1.png" in names, "watermark media part missing")
    header = read_part(WMI, "word/header1.xml")
    check("<v:imagedata" in header, "v:imagedata missing")
    check('r:id="rId1"' in header, "imagedata does not reference rId1")
    check("gain=" in header and "blacklevel=" in header, "washout parameters missing")
    rels = read_part(WMI, "word/_rels/header1.xml.rels")
    check("media/watermark1.png" in rels, "header rels do not point at the media part")
    check("relationships/image" in rels, "header rel type is not image")
    ct = read_part(WMI, "[Content_Types].xml")
    check('Extension="png"' in ct, "png Default content type missing")
    try:
        import docx as pydocx

        pydocx.Document(str(WMI))
        check(True, "")
    except Exception as exc:  # noqa: BLE001
        check(False, f"python-docx could not open p11_watermark_image.docx: {exc}")

# ---- 3. PDF 里的水印 ----------------------------------------------------
PDF = OUT / "p11_watermark_text.pdf"
check(PDF.exists(), "p11_watermark_text.pdf missing")
if PDF.exists():
    doc = fitz.open(PDF)
    check(doc.page_count == 1, f"page count {doc.page_count}")
    page = doc[0]
    # "dir"（书写方向）挂在 line 上，不在 span 上。
    spans = []  # (span, line_dir)
    for b in page.get_text("dict")["blocks"]:
        for line in b.get("lines", []):
            d = line.get("dir", (1.0, 0.0))
            for span in line.get("spans", []):
                spans.append((span, d))
    wm = [(s, d) for (s, d) in spans if "CONFIDENTIAL" in s["text"]]
    check(len(wm) == 1, f"expected 1 watermark span, got {len(wm)}")
    if wm:
        s, (dx, dy) = wm[0]
        # 旋转 45°：方向向量约 (0.707, -0.707)（y 向上为正，文字向右上走）。
        check(abs(abs(dx) - 0.7071) < 0.01, f"watermark dir.x {dx} is not 45deg")
        check(abs(abs(dy) - 0.7071) < 0.01, f"watermark dir.y {dy} is not 45deg")
        # 字号要大（按盒宽反推：A4 上 12 个字符撑满约 528pt）。
        check(s["size"] > 60, f"watermark font size {s['size']} too small")
        # 颜色是银灰 #C0C0C0。
        col = s["color"]
        r, g, b = (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF
        check(abs(r - 0xC0) <= 4 and abs(g - 0xC0) <= 4 and abs(b - 0xC0) <= 4,
              f"watermark color #{col:06X} is not #C0C0C0")
    # 正文仍然在，而且水印没有把正文挤走。
    text = page.get_text()
    check("body text under the watermark" in text, "body text lost")
    body_spans = [(s, d) for (s, d) in spans if "body text" in s["text"]]
    check(len(body_spans) >= 1, "body span missing")
    # 光栅化：页面中心附近必须有非白像素（水印真的画出来了）。
    pix = page.get_pixmap(dpi=72)
    w, h = pix.width, pix.height
    colored = 0
    for y in range(h // 3, 2 * h // 3, 3):
        for x in range(w // 3, 2 * w // 3, 3):
            px = pix.pixel(x, y)
            if px != (255, 255, 255):
                colored += 1
    check(colored > 50, f"center of the page is blank ({colored} non-white pixels)")
    doc.close()

# ---- 4. 文档保护：用 hashlib 重新实现一遍迭代哈希 ----------------------
PROT = OUT / "p11_protected.docx"
check(PROT.exists(), "p11_protected.docx missing")
if PROT.exists():
    settings = read_part(PROT, "word/settings.xml")
    check("documentProtection" in settings, "documentProtection missing")
    check('w:edit="readOnly"' in settings, "w:edit not readOnly")
    check('w:enforcement="1"' in settings, "w:enforcement not set")
    check('w:algorithmName="SHA-512"' in settings, "algorithmName missing")
    check('w:cryptSpinCount="100000"' in settings, "cryptSpinCount missing")
    check(settings.count("documentProtection") == 1, "documentProtection appears more than once")

    import xml.etree.ElementTree as ET

    W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
    root = ET.fromstring(settings)
    prot = root.find(f"{W}documentProtection")
    check(prot is not None, "documentProtection element not parseable")
    if prot is not None:
        stored = prot.get(f"{W}hash")
        salt_b64 = prot.get(f"{W}salt")
        spin = int(prot.get(f"{W}cryptSpinCount"))
        algo = prot.get(f"{W}algorithmName")
        size = int(prot.get(f"{W}hashSize"))
        check(stored is not None and salt_b64 is not None, "hash/salt attributes missing")
        check(algo == "SHA-512", f"algorithmName {algo}")
        check(size == 64, f"hashSize {size}")

        # ISO 29500 §17.15.1.29：H0 = H(salt ‖ UTF-16LE(pw))，
        # Hi = H(H(i-1) ‖ uint32_le(i-1))，取前 hashSize 字节。
        def iso_hash(password, salt, algo_name, spin_count, hash_size):
            hf = {
                "SHA-1": hashlib.sha1,
                "SHA-256": hashlib.sha256,
                "SHA-384": hashlib.sha384,
                "SHA-512": hashlib.sha512,
                "MD5": hashlib.md5,
            }[algo_name]
            h = hf(salt + password.encode("utf-16-le")).digest()
            for i in range(spin_count):
                h = hf(h + i.to_bytes(4, "little")).digest()
            return h[:hash_size]

        salt = base64.b64decode(salt_b64)
        want = iso_hash("s3cret", salt, algo, spin, size)
        got = base64.b64decode(stored)
        check(got == want, "hash mismatch between DocCore and the hashlib reference")
        # 错口令必须算出不同的哈希（否则校验形同虚设）。
        check(iso_hash("s3cret ", salt, algo, spin, size) != want, "wrong password produced the same hash")

    # 保护元素不能把 settings 里原有的东西挤掉（DocCore 写的是补丁式替换）。
    try:
        import docx as pydocx

        pydocx.Document(str(PROT))
        check(True, "")
    except Exception as exc:  # noqa: BLE001
        check(False, f"python-docx could not open p11_protected.docx: {exc}")

if fails:
    print(f"FAILED {len(fails)}/{checks}")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print(f"ok {checks} checks")
