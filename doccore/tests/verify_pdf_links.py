"""P11-1 独立复核：用 PyMuPDF 读回 DocCore 写出的 PDF，检查链接注释、命名
目标和大纲树是否真的可解析、能跳对页。

PyMuPDF（MuPDF）是第三方实现，不共享 DocCore 的任何代码，所以它的解析结果
是对 PDF 结构是否合规的独立判断。三个产物分别对应三条产出链：
  p11_links.pdf        纯 Layout IR（手工构造，覆盖两级大纲/中文标题/内部跳转）
  p11_docx_outline.pdf Word 标题样式 -> 大纲
  p11_deck_outline.pdf PPT 幻灯片标题 -> 大纲、run 超链接 -> 链接注释
  p11_sheet_links.pdf  Excel 单元格超链接 -> 链接注释 + 命名目标
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


def link_kinds(page):
    return [l["kind"] for l in page.get_links()]


def is_goto(link):
    # 命名目标形式的 GoTo 报 LINK_NAMED；显式数组形式报 LINK_GOTO。
    return link["kind"] in (fitz.LINK_GOTO, fitz.LINK_NAMED)


# ---- 1. 纯 IR：两级大纲 + 中文标题 + 外部/内部链接 ----------------------
doc = open_pdf("p11_links.pdf")
check(doc.page_count == 3, f"links: page count {doc.page_count} != 3")

links0 = doc[0].get_links()
check(len(links0) == 3, f"links: page0 links {len(links0)} != 3")
uri_links = [l for l in links0 if l["kind"] == fitz.LINK_URI]
goto_links = [l for l in links0 if is_goto(l)]
check(len(uri_links) == 2, f"links: URI links {len(uri_links)} != 2")
check(len(goto_links) == 1, f"links: GOTO links {len(goto_links)} != 1")

uris = sorted(l["uri"] for l in uri_links)
check("https://example.com/a?b=1&c=2" in uris, f"links: plain URI missing, got {uris}")
check(
    any(u.startswith("https://example.com/") and u.endswith("\u4e2d\u6587") for u in uris),
    f"links: non-ASCII URI not round-tripped, got {uris}",
)

if goto_links:
    check(goto_links[0]["page"] == 1, f"links: GoTo page {goto_links[0]['page']} != 1")

# 链接矩形必须落在页内，且面积非零。
for l in links0:
    r = l["from"]
    check(r.width > 0 and r.height > 0, f"links: degenerate rect {r}")
    check(r.x0 >= 0 and r.x1 <= doc[0].rect.width + 0.5, f"links: rect off-page {r}")

# 命名目标：PyMuPDF 解析成 {"page": n, "to": (x, y), "zoom": z}。
names = doc.resolve_names()
check("target_one" in names, f"links: named dest missing, got {sorted(names)}")
if "target_one" in names:
    entry = names["target_one"]
    check(entry.get("page") == 1, f"links: named dest page {entry.get('page')} != 1")
    to = entry.get("to")
    check(to is not None, "links: named dest has no point")
    if to is not None:
        check(abs(to[0] - 72.0) < 0.5, f"links: named dest x {to[0]} != 72")
        check(abs(to[1] - 441.89) < 0.5, f"links: named dest y {to[1]} != 441.89")

toc = doc.get_toc(simple=False)
check(len(toc) == 5, f"links: toc entries {len(toc)} != 5")
titles = [t[1] for t in toc]
expected = ["Chapter A", "Section A.1", "Section A.2", "Chapter B", "\u4e2d\u6587\u7ae0\u8282"]
check(titles == expected, f"links: toc titles {titles} != {expected}")
levels = [t[0] for t in toc]
check(levels == [1, 2, 2, 1, 1], f"links: toc levels {levels} != [1,2,2,1,1]")
pages = [t[2] for t in toc]
check(pages == [1, 1, 2, 3, 3], f"links: toc pages {pages} != [1,1,2,3,3]")
for t in toc:
    check(isinstance(t[3], dict) and "to" in t[3], f"links: toc entry {t[1]} has no dest point")

# ---- 2. Word 标题样式 -> 大纲 -------------------------------------------
doc = open_pdf("p11_docx_outline.pdf")
toc = doc.get_toc()
check(
    [(t[0], t[1]) for t in toc] == [(1, "Introduction"), (2, "Background"), (1, "Method")],
    f"docx: toc {[(t[0], t[1]) for t in toc]}",
)
# 位置必须自上而下严格递增（同一页内）。
if len(toc) == 3:
    ys = [t[3]["to"].y for t in doc.get_toc(simple=False)]
    check(ys[0] < ys[1] < ys[2], f"docx: toc y not ascending {ys}")

# ---- 3. PPT 标题 -> 大纲、run 超链接 -> 链接注释 ------------------------
doc = open_pdf("p11_deck_outline.pdf")
check(doc.page_count == 2, f"deck: page count {doc.page_count} != 2")
toc = doc.get_toc()
check(
    [(t[0], t[1], t[2]) for t in toc] == [(1, "Opening", 1), (1, "Second Slide", 2)],
    f"deck: toc {[(t[0], t[1], t[2]) for t in toc]}",
)
for i in range(doc.page_count):
    ls = doc[i].get_links()
    check(len(ls) == 1, f"deck: page {i} links {len(ls)} != 1")
    if ls:
        check(ls[0]["kind"] == fitz.LINK_URI, f"deck: page {i} not a URI link")
        check(ls[0]["uri"] == "https://example.com/deck", f"deck: page {i} uri {ls[0]['uri']}")
        # 链接只覆盖 "here" 三个词中的一段，必须比正文窄。
        r = ls[0]["from"]
        check(0 < r.width < 120, f"deck: page {i} link width {r.width} out of range")

# ---- 4. Excel 单元格超链接 -> 链接注释 + 命名目标 -----------------------
doc = open_pdf("p11_sheet_links.pdf")
check(doc.page_count == 1, f"sheet: page count {doc.page_count} != 1")
ls = doc[0].get_links()
check(len(ls) == 3, f"sheet: links {len(ls)} != 3")
uris = sorted(l["uri"] for l in ls if l["kind"] == fitz.LINK_URI)
check("https://example.com/x?a=1&b=2" in uris, f"sheet: external URI missing, got {uris}")
check("https://example.com/\u4e2d" in uris, f"sheet: non-ASCII URI missing, got {uris}")
names = doc.resolve_names()
check("Links!A1" in names, f"sheet: named dest missing, got {sorted(names)}")
if "Links!A1" in names:
    check(names["Links!A1"].get("page") == 0, "sheet: named dest on wrong page")
toc = doc.get_toc()
check([t[1] for t in toc] == ["Links"], f"sheet: toc {[t[1] for t in toc]} != ['Links']")

print(f"PyMuPDF {fitz.VersionBind} / MuPDF {fitz.VersionFitz}")
print(f"checks={checks} fails={len(fails)}")
for f in fails:
    print("FAIL", f)
sys.exit(1 if fails else 0)
