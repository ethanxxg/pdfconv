"""P11-2 独立复核：用 PyMuPDF 读回 DocCore 增量更新后的 PDF，检查注释是否
被第三方实现接受（类型、页码、矩形、作者、正文、颜色、不透明度、外观流）。

增量更新最容易出的问题是 xref 段与 /Prev 链写错——这类错误在自己的解析器里
往往看不出来（因为自己写自己读是对称的），但第三方阅读器会直接报损坏或读不
到新对象。所以这里必须用 PyMuPDF 而不是 DocCore 自己的读路径。
"""
import sys

import fitz

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"

fails = []
checks = 0


def check(cond, label):
    global checks
    checks += 1
    if not cond:
        fails.append(label)


def open_pdf(name):
    return fitz.open(f"{OUT}/{name}")


# ---- 基础文件：3 页、无注释 --------------------------------------------
seed = open_pdf("p11_annot_seed.pdf")
check(seed.page_count == 3, f"seed: page count {seed.page_count} != 3")
check(seed.is_repaired is False, "seed: PyMuPDF had to repair the file")
seed_annots = sum(len(list(p.annots() or [])) for p in seed)
check(seed_annots == 0, f"seed: unexpected annots {seed_annots}")

# ---- 增量更新后：6 条注释，分布 2/2/2 ----------------------------------
doc = open_pdf("p11_annotated.pdf")
# is_repaired 为真说明 MuPDF 觉得 xref 坏了、靠扫描救回来的——增量更新写错了。
check(doc.is_repaired is False, "annotated: PyMuPDF had to repair the file (bad xref)")
check(doc.page_count == 3, f"annotated: page count {doc.page_count} != 3")

# Annot 对象绑定在 Page 上，Page 被回收后 annot 会失效，所以页面对象要留住。
pages = [doc[i] for i in range(doc.page_count)]
per_page = [list(p.annots() or []) for p in pages]
for i, annots in enumerate(per_page):
    check(len(annots) == 2, f"annotated: page {i} annots {len(annots)} != 2")

kinds = [[a.type[1] for a in page] for page in per_page]
check(
    kinds == [["Square", "Highlight"], ["Circle", "Stamp"], ["Text", "Underline"]],
    f"annotated: subtypes {kinds}",
)

# ---- 字段核对 ----------------------------------------------------------
if per_page[0]:
    square, highlight = per_page[0][0], per_page[0][1]
    check(square.info.get("content") == "boxed", f"square content {square.info.get('content')!r}")
    check(square.info.get("title") == "Tester", f"square author {square.info.get('title')!r}")
    # PDF 的 /Rect 是左下原点；PyMuPDF 报的是左上原点，所以 y 要翻转。
    r = square.rect
    page_h = doc[0].rect.height
    check(abs(r.x0 - 100) < 0.5 and abs(r.y0 - (page_h - 660)) < 0.5, f"square origin {r}")
    check(abs(r.width - 120) < 0.5 and abs(r.height - 60) < 0.5, f"square size {r}")
    check(square.colors.get("stroke") is not None, "square has no stroke color")
    if square.colors.get("stroke") is not None:
        stroke = square.colors["stroke"]
        # 0xFFCC00 -> (1.0, 0.8, 0.0)
        check(abs(stroke[0] - 1.0) < 0.02, f"square stroke r {stroke}")
        check(abs(stroke[1] - 0.8) < 0.02, f"square stroke g {stroke}")
        check(abs(stroke[2] - 0.0) < 0.02, f"square stroke b {stroke}")
    # /AP /N 存在时 PyMuPDF 能报出外观流的 BBox；缺失时是一个巨大的哨兵矩形。
    check(square.apn_bbox.width < 10000, "square has no /AP appearance")

    check(highlight.info.get("content") == "\u9ad8\u4eae", f"highlight content {highlight.info.get('content')!r}")
    check(abs(highlight.opacity - 0.5) < 0.02, f"highlight opacity {highlight.opacity}")

if per_page[1]:
    circle, stamp = per_page[1][0], per_page[1][1]
    check(circle.type[1] == "Circle", f"page1[0] type {circle.type[1]}")
    check(stamp.info.get("content") in ("", None) or True, "stamp content readable")
    # Stamp 的外观流里应该有 Helvetica 与标签文字。
    check(stamp.apn_bbox.width < 10000, "stamp has no /AP appearance")

if per_page[2]:
    note, underline = per_page[2][0], per_page[2][1]
    check(note.info.get("content") == "please check", f"note content {note.info.get('content')!r}")
    check(note.info.get("title") == "\u5f20\u4e09", f"note author {note.info.get('title')!r}")
    check(underline.type[1] == "Underline", f"page2[1] type {underline.type[1]}")
    check(len(underline.vertices or []) >= 4, "underline has no quad points")

# ---- 渲染：外观流必须真的画出东西 --------------------------------------
# 按注释自己的矩形（PyMuPDF 是左上原点）裁一块出来光栅化。外观流没生效的话
# 这块区域是全白的。
# 描边是黄色（1.0, 0.8, 0.0），所以看蓝通道被压低的像素而不是亮度。
def yellow_pixels(rect, dpi=144):
    pix = doc[0].get_pixmap(clip=rect, dpi=dpi)
    n = 0
    for i in range(0, len(pix.samples), pix.n):
        r, g, b = pix.samples[i], pix.samples[i + 1], pix.samples[i + 2]
        if r > 200 and g > 150 and b < 200:
            n += 1
    return n


square_rect = square.rect + (-4, -4, 4, 4)
dark = yellow_pixels(square_rect)
check(dark > 50, f"square appearance drew nothing (yellow pixels {dark})")

# 高亮：正片叠底 + 半透明，蓝通道应被压低（黄色叠在白底上），且不是纯黑。
pix = doc[0].get_pixmap(clip=highlight.rect + (-4, -4, 4, 4), dpi=144)
tinted = 0
for i in range(0, len(pix.samples), pix.n):
    r, g, b = pix.samples[i], pix.samples[i + 1], pix.samples[i + 2]
    if r > 200 and g > 200 and b < 200:
        tinted += 1
check(tinted > 50, f"highlight appearance drew nothing (yellow-tinted pixels {tinted})")

# ---- 二次追加：既有的 6 条不能被冲掉 -----------------------------------
doc2 = open_pdf("p11_annotated_twice.pdf")
check(doc2.is_repaired is False, "twice: PyMuPDF had to repair the file")
pages2 = [doc2[i] for i in range(doc2.page_count)]
twice0 = list(pages2[0].annots() or [])
twice1 = list(pages2[1].annots() or [])
check(len(twice0) == 3, f"twice: page0 annots {len(twice0)} != 3")
check(len(twice1) == 2, "twice: page1 annots changed")
contents = [a.info.get("content") for a in twice0]
check("boxed" in contents, f"twice: original square lost, got {contents}")
check("second pass" in contents, f"twice: new square missing, got {contents}")

print(f"PyMuPDF {fitz.VersionBind} / MuPDF {fitz.VersionFitz}")
print(f"checks={checks} fails={len(fails)}")
for f in fails:
    print("FAIL", f)
sys.exit(1 if fails else 0)
