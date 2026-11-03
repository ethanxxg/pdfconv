// DocCore - 幻灯片布局（绝对定位，不分页）-> Layout IR -> PDF/PNG。
// Design refs: docs/02 §3.7
//
// 与字处理不同，这里没有分页：一页幻灯片就是一个 LayoutPage，页面尺寸
// 等于幻灯片尺寸。文本框内部才做换行与垂直锚点。
#pragma once

#include <string>

#include "doccore/core.h"
#include "doccore/deck.h"
#include "doccore/layout.h"
#include "doccore/pdf.h"

namespace doccore {
namespace deck {

struct DeckLayoutOptions {
  const FontRegistry* fonts = nullptr;
  std::string default_family = "Calibri";
  double default_size_pt = 18.0;
  bool draw_shape_borders = true;
  int max_slides = 2000;
  WarningSink* warnings = nullptr;
};

LayoutDocument LayoutPresentation(const Presentation& pres, const DeckLayoutOptions& opts = {});

void SavePdfToFile(const Presentation& pres, const std::string& path,
                   const DeckLayoutOptions& layout_opts = {},
                   const PdfSaveOptions& pdf_opts = {});

}  // namespace deck
}  // namespace doccore
