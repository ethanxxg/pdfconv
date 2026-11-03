// DocCore - 工作表打印布局（分页 + 网格 + 对齐），输出 Layout IR。
// Design refs: docs/02 §3.4.3
//
// 分页规则（与 Excel 的"自动分页"一致到像素级近似）：
//   列宽按字符数换算成点（1 字符 ≈ 7px ≈ 5.25pt），累加到超过可打印宽度就换页；
//   行高本来就是点，同样累加。列带 × 行带 = 页数，列优先展开（先横向翻页）。
// 标题行（print_title_rows）在每一页重复。
#pragma once

#include <string>

#include "doccore/core.h"
#include "doccore/layout.h"
#include "doccore/pdf.h"
#include "doccore/sheet.h"

namespace doccore {
namespace sheet {

struct SheetLayoutOptions {
  const FontRegistry* fonts = nullptr;
  std::string default_family = "Calibri";
  double default_size_pt = 11.0;
  // 1 个字符宽度的点数。Calibri 11 的 MDW 是 7px，96dpi 下约 5.25pt。
  double char_width_pt = 5.25;
  bool grid_lines = true;
  Rgba grid_color{0xD0, 0xD0, 0xD0, 0xFF};
  double cell_padding_pt = 2.0;
  int max_pages = 2000;
  WarningSink* warnings = nullptr;
};

// 单张表 -> 若干页。
LayoutDocument LayoutSheet(const Workbook& book, const Worksheet& ws,
                           const SheetLayoutOptions& opts = {});

// 整本工作簿：每张表接在后面，页码连续。
LayoutDocument LayoutWorkbook(const Workbook& book, const SheetLayoutOptions& opts = {});

// 一步到位写 PDF（布局 + 编码）。
void SavePdfToFile(const Workbook& book, const std::string& path,
                   const SheetLayoutOptions& layout_opts = {},
                   const PdfSaveOptions& pdf_opts = {});

}  // namespace sheet
}  // namespace doccore
