// DocCore - PDF 反向重建：几何对象 -> 块/栏/表 -> DOCX / XLSX / PPTX DOM。
// Design refs: docs/02 §3.5.4，docs/01 F-310 / F-311 / F-312
//
//   pdf::PdfFile file = pdf::PdfFile::OpenPath("in.pdf");
//   text::Document doc = pdf::Reconstructor::ToDocument(file, {});
//
// PDF 里没有段落、单元格和幻灯片，只有坐标上的字形、线和图像。所以这一层
// 先做"几何对象 -> 视觉行 -> 块 -> 栏 -> 表"的聚类，再把结果填进目标族的
// DOM。反方向不可能无损，调用方必须知道这是重建（转换总线把三条路径都标
// 为 Lossy）。
//
// 坐标约定：PageModel 及其下的所有矩形都用**左上原点、y 向下**的点坐标，
// 与 PdfTextRun 的 PDF 用户空间（左下原点、y 向上）相反。换算在
// BuildPage 里一次完成，之后的代码只认一种方向。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doccore/core.h"
#include "doccore/deck.h"
#include "doccore/pdf_read.h"
#include "doccore/sheet.h"
#include "doccore/text.h"

namespace doccore {
namespace pdf {

// ---- 选项 ---------------------------------------------------------------

struct RebuildOptions {
  bool ocr_if_no_text = false;      // 无文本层时是否调用 OCR 插件（本波未接入，仍返回 Unsupported）
  bool stack_same_columns = false;  // XLSX：连续页列结构相同时拼到同一张表

  // 聚类阈值（docs/02 §3.5.4）。默认值即设计书给定值，测试可覆盖。
  double line_baseline_tolerance_pt = 1.5;  // 同一视觉行的基线容差
  double block_left_tolerance_pt = 4.0;     // 左边缘对齐容差
  double block_gap_factor = 0.8;            // 行高倍数：块内相邻行的最大垂直间隙
  double column_gap_pt = 36.0;              // 分栏判定：块间横向间隙
  double min_line_length_pt = 2.0;          // 短于此长度的线段丢弃
  double scan_area_ratio = 0.8;             // 单图覆盖页面多少面积算扫描页

  // 表格检测
  double grid_edge_tolerance_pt = 2.5;      // 网格线坐标聚类容差
  int min_table_rows = 2;                   // 至少几行才算表
  int min_table_cols = 2;                   // 至少几列才算表
  bool detect_aligned_tables = true;        // 无框线时走对齐启发式
  int aligned_table_min_rows = 3;           // 对齐启发式至少连续几行
  // 对齐启发式至少几列。两段的"表"与双栏正文在几何上无法区分，按双栏正文
  // 处理（分块 + 分栏）比硬判成表更安全，所以下限是 3。
  int aligned_table_min_cols = 3;

  // DOCX 重建
  double heading_scale = 1.4;               // 字号 >= 正文字号 * 该倍数且独占一行 -> 标题
};

// ---- 中间模型 -----------------------------------------------------------

// 视觉行里的一段连续字形。同一行内出现宽间隙（> 0.8 字号）时切成多段，
// 无框线表格的"按列对齐"启发式靠它切列。
struct VisualSegment {
  double left = 0, right = 0;
  std::string text;
  int vert_align = 0;  // 0 基线 / 1 上标 / 2 下标（与 RunFormat::vert_align 同义）
};

// 视觉行：同一基线上的一组文本片段。
struct VisualLine {
  double left = 0, top = 0, right = 0, bottom = 0;  // top-down 坐标
  double baseline = 0;
  double size_pt = 12.0;
  std::string text;
  std::string family;
  bool bold = false;
  bool italic = false;
  int color = 0x000000;
  std::vector<VisualSegment> segments;

  double height() const noexcept { return bottom - top; }
};

// 文本块：左边缘对齐、垂直相邻的一组视觉行。
struct TextBlock {
  double left = 0, top = 0, right = 0, bottom = 0;
  double size_pt = 12.0;
  std::string family;
  bool bold = false;
  int color = 0x000000;
  int column = 0;  // 栏序号，0 起
  std::vector<VisualLine> lines;

  bool empty() const noexcept { return lines.empty(); }
  std::string Text() const;  // 行间 '\n'
  bool single_line() const noexcept { return lines.size() == 1; }
};

// 检测出的表格。col_edges / row_edges 是递增坐标（top-down），
// 所以列数 = col_edges.size()-1，行数 = row_edges.size()-1。
struct DetectedTable {
  double left = 0, top = 0, right = 0, bottom = 0;
  std::vector<double> col_edges;
  std::vector<double> row_edges;
  std::vector<std::vector<std::string>> cells;  // [row][col]
  std::vector<std::vector<bool>> covered;       // [row][col] 被合并覆盖（占位空格）
  bool bordered = true;                         // false = 对齐启发式识别

  int rows() const noexcept { return static_cast<int>(cells.size()); }
  int cols() const noexcept {
    return cells.empty() ? 0 : static_cast<int>(cells.front().size());
  }
};

// 一页的重建素材。
struct PageModel {
  int index = 0;
  double width = 0, height = 0;  // 点
  std::vector<TextBlock> blocks;  // 阅读顺序
  std::vector<DetectedTable> tables;
  std::vector<PdfImagePlacement> images;  // 坐标已转 top-down
  bool scanned = false;                   // 只有整页图、没有文本层
  bool has_text = false;
};

// 从 PDF 图像 XObject 解出来的图片。mime 为 "image/png" 或 "image/jpeg"。
struct ExtractedImage {
  std::string mime;
  std::vector<uint8_t> bytes;
  int pixel_width = 0;
  int pixel_height = 0;
  bool empty() const noexcept { return bytes.empty(); }
};

// ---- 重建器 -------------------------------------------------------------

class Reconstructor {
 public:
  // 单页建模（① 几何对象 → ② 分块分栏 → ③ 表格检测）。
  static PageModel BuildPage(const PdfPage& page, const RebuildOptions& opt,
                             WarningSink* warnings = nullptr);

  static std::vector<PageModel> BuildPages(const PdfFile& file, const RebuildOptions& opt,
                                           WarningSink* warnings = nullptr);

  // ④ PDF → DOCX
  static text::Document ToDocument(const PdfFile& file, const RebuildOptions& opt = {},
                                   WarningSink* warnings = nullptr);

  // ⑤ PDF → XLSX
  static sheet::Workbook ToWorkbook(const PdfFile& file, const RebuildOptions& opt = {},
                                    WarningSink* warnings = nullptr);

  // ⑥ PDF → PPTX
  static deck::Presentation ToPresentation(const PdfFile& file, const RebuildOptions& opt = {},
                                           WarningSink* warnings = nullptr);

  // 扫描页（无文本层）统一在这里判定并抛出 Unsupported + NoTextLayer 告警，
  // 三个方向共用，避免"产出空文档冒充成功"。
  static void RequireTextLayer(const std::vector<PageModel>& pages, const RebuildOptions& opt,
                               WarningSink* warnings);
};

// 取出 PDF 里某张图的实际像素（Flate 的 DeviceRGB/DeviceGray 8 位重编码为 PNG，
// DCTDecode 原样给出 JPEG）。解不出来时返回空并告警 ImageSkipped。
ExtractedImage ExtractImage(const PdfFile& file, const PdfPage& page,
                            std::string_view xobject_name, WarningSink* warnings = nullptr);

// 同上，但直接用落位时解析出的对象：图像可能只挂在 Form XObject 的 /Resources 里，
// 按名字在页级资源中找不到。placed.xobject 为空时回退到按名字查。
ExtractedImage ExtractPlacedImage(const PdfFile& file, const PdfPage& page,
                                  const PdfImagePlacement& placed,
                                  WarningSink* warnings = nullptr);

}  // namespace pdf
}  // namespace doccore
