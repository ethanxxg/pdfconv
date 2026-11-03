# pdfconv — 自研 PDF → DOCX 转换器

一个**完全自主实现**的 PDF → DOCX 转换工程：所有转换逻辑（版面分析、段落/表格/插图重建、阅读顺序、字体/字号/颜色映射、DOCX 结构生成）都是本项目自己的代码。MuPDF 仅用于**读取** PDF，python-docx 仅用于**写出** OOXML。**不依赖** pdf2docx / LibreOffice / Aspose 等任何第三方转换引擎。

## 目录结构

```
pdfconv/
  __init__.py        # 包入口，导出 convert()
  __main__.py        # 支持 python -m pdfconv
  cli.py             # 命令行入口（单文件 / 批量）
  config.py          # 常量与正则（字体角色、XML 清洗、页边距、DPI）
  text.py            # 读取层：page_words / build_lines / 字符清洗 / 字体角色
  layout.py          # 版面探测：页面尺寸、分栏、栏间距、正文字号、行距、图形区
  structure.py       # 结构重建：分栏归属 / 段落分组 / 块分类 / 阅读顺序 / 页眉页脚过滤
  media.py           # 表格（find_tables）与插图（栅格+矢量簇裁剪）提取
  writer.py          # DOCX 输出：逐 run 还原字体名/字号/颜色/粗斜体/上下标
  convert.py         # 顶层编排：PDF -> DOCX
  headers.py         # 页眉/页脚探测与 PAGE 域
  serve.py           # 最小 HTTP 服务（stdlib），供其他语言调用
tools/
  verify_shipped.py  # 校验 docs/ 落盘文件是否真由当前代码产生（语义比对）
  validate.py        # 整批保真度校验（文本量 / 标题对齐 / 表格数）
  compare_text.py    # 与基线做去空白字符比对
  metrics.py         # 统计字体名 / 彩色 run / 表格 / 超链接 / 页眉
  diagnose_text.py   # 诊断哪些 PDF 的文本结构性不可解码（CID + Identity + 无 ToUnicode）
  report.py          # 现场测量并生成《与 Aspose 能力对比》文档
  promote.py         # 把某轮输出同步到 docs/to_docx_own 与 docs/to_docx
capi/
  pdfconv_capi.c/.h  # C ABI（嵌入 CPython），供 C/C++ 同事调用
README.md
requirements.txt
tests/
  test_smoke.py      # 冒烟测试：转换一篇样例并校验非空
work/                # 临时工作区，**不是产品代码**（见 work/README.md）
```

> **`pdfconv/` 是唯一的产品代码**，不依赖 `work/` 下任何文件，可单独拷走。
> `work/ownconv.py` 是早期单文件原型，已废弃，跑出来会比 `pdfconv` 差。

## 修改代码后必须做的事

改了 `pdfconv/` 之后，落盘的 `docs/` 产物就过期了。必须重跑并校验：

```bash
python -m pdfconv --batch docs work/to_docx_vN --force
python tools/validate.py docs/to_docx_own work/to_docx_vN     # 文本量/标题回归
python tools/promote.py work/to_docx_vN
python tools/verify_shipped.py work/to_docx_vN docs/to_docx_own
```

最后一条必须输出 `RESULT: shipped output matches the current code`。

**不能用文件哈希判断**：python-docx 用随机临时名命名内嵌图片部件，同一份代码跑两次
字节必然不同；`verify_shipped.py` 比的是语义内容（段落文本 + 对齐 + 逐 run 样式）。

对比报告也由脚本生成，不要手改（手改的数字一定会和代码脱节）：

```bash
python tools/report.py doccore/docs/自研转换器_vs_Aspose_能力对比.docx docs/to_docx_own docs
python tools/diagnose_text.py docs     # 哪些 PDF 的文本结构性不可解码
```

## 架构分层

| 层 | 用什么 | 语言 | 职责 |
|---|---|---|---|
| PDF 读取 | MuPDF（PyMuPDF `fitz`） | C | 只读：文本、字体、颜色、表格、图片、矢量路径 |
| 转换逻辑 | `pdfconv/*`（本项目） | Python | 版面分析、段落/表格/图重建、阅读顺序、字体映射、DOCX 结构 |
| DOCX 输出 | python-docx | Python | 直接生成 OOXML（.docx 即 zip+XML） |

## 安装

```bash
pip install -r requirements.txt
```

> 需要 `PyMuPDF` 与 `python-docx`。注意：部分环境（如本机托管版 Python 3.13）不带 `fitz`，请使用已安装 PyMuPDF 的解释器（如系统 Python 3.14）。

## 用法

```bash
# 单个文件
python -m pdfconv input.pdf output.docx

# 批量（跳过已存在；加 --force 强制重转）
python -m pdfconv --batch docs docs/to_docx --force
```

## 能力状态（相对 Aspose）

**深度（PDF→DOCX）**：文本、字体名、字号、颜色、粗体/斜体、上下标、标题层级与对齐、单/双栏阅读顺序、表格（含合并单元格）、插图、超链接、真实页眉页脚均已实现；差距在原生矢量、可编辑数学公式、书签、OCR、处理速度。

**广度（转换方向）**：目前完成 PDF→DOCX；PDF→XLSX、PDF→PPT、DOCX→PDF、XLSX→PDF、PPT→PDF 待开发。

**当前落盘基线（v9，69 篇）**：99.96% 文字量（对 v6 基线）、287 字体名、139,822 彩色 run、
9,144 上标 / 12,618 下标、55 原生表 / 10 处合并、1,654 插图、2,536 超链接、69 篇页眉 + PAGE 域。
`tools/validate.py` 通过（0/69 篇掉 >2% token），`tools/verify_shipped.py` 报 69/69 identical。
对比报告见 `doccore/docs/自研转换器_vs_Aspose_能力对比.docx`（由 `tools/report.py` 生成，勿手改）。

**已知不可修复**：1/69 篇（`安全牛-自主智能体Agentic.pdf`）为 Identity 编码 CID 字体且文件内无
ToUnicode，字形 id 到 Unicode 的信息在 PDF 里根本不存在（内嵌子集与公开版 Source Han Sans 字形
顺序不同，实测 gid 偏移量 1604/14320/2887/5776 并不恒定，无法反推）。`tools/diagnose_text.py`
可精确列出这类文件；不是实现缺陷，任何转换器同样受限。

## 设计取舍

- **分词在 span 粒度**：不直接用 MuPDF 的 `get_text("words")`。因为一个印出来的记号（如 `LIU2,3`）可能跨两个字号，按"词"取会丢掉上标。本项目自己把 span 文本切成 token，每个 token 精确继承所属 span 的字体/字号/颜色/粗斜体——这是上标、下标能达到原样精度的原因。
- **视觉行重组**：也不直接信任 MuPDF 的 `(block, line)` 编号。上标抬高基线会让引擎把同一视觉行拆成多段（作者行常被拆成 3~4 段并错序）。本项目在每个 block 内按纵向重叠重新聚类成"视觉行"，再按 x 排序，作者/单位行因此能按原顺序还原。
- **字体还原**：逐 run 写回 `font/size/color/flags`。二进制字体嵌入不做——多数论文用 Type1/PFA/CFF 字体，DOCX 无法嵌入（Aspose 同样受限）；保留字体名即为其实际可达上限。
- **标题对齐**按 PDF 实测几何决定：只有左右留白相等时才居中，否则左对齐（IEEE Access、多数 Elsevier 期刊的标题/章标题都是左对齐，旧版一律居中反而是错的）。
- **表格 / 插图 / 正文是三分判断，不是二分。** `find_tables` 对几乎任何有框线的区域都会返回"一张表"，且经常返回不可用的结构：一个 1×4 网格里塞进四段 40 行的正文，或者 35×20 的网格 95% 是空格。判定标准是"单元格里装的是值还是段落"（多行率 >50%、长文本 >15%、填充率 <25% 判为不可用）。
  被拒的区域**不等于**插图：只有区域内确实压着图像/矢量图时才裁剪成 PNG，否则它的词回到正常文本流。早期版本把被拒区域一律裁成图片，导致 027 篇一张真数据表（`AST3II004 93.000 −73.000 3179`）连同表头一起变成像素——文字量掉 2.7%，肉眼还看不出来。
- **单行网格是合法表格。** 位数组、分块示意图常印成一行 N 格（`0 1 1 0`、`Blk1 Blk2 Blk4`），不能因为"只有一行"就否掉；真正区分表格与误切分正文的是单元格内容形态，不是行数。
- **图区不许吞掉整段正文。** 柱状图的柱子常被画得比绘图区高，再靠裁剪路径裁掉——但 `get_drawings()` 给的是**未裁剪**的几何，柱子矩形能一路伸到正文里。聚类跟着长，图区框就盖住了下面的正文，框内每个词都被剔出正文流。039 篇的整个 CONCLUSION 小节就是这样消失的。判据：真图里不会出现"满栏宽的正文行"，所以遇到第一行满栏正文就在其上方截断（且截断后必须仍保留图区 35% 以上的高度，避免把宽图例误判成正文）。
- **页眉页脚**：能识别跨页重复的页眉/页脚文本，写成真正的 Word 页眉与 `PAGE` 域，并从正文中剔除。

## 测试

```bash
python -m pytest tests/ -q      # 或直接：python tests/test_smoke.py
```
