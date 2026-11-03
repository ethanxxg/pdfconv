// DocCore - 字处理排版：text::Document -> Layout IR（docs/02 §3.2）。
//
// 排版只依赖 Layout IR 与字体度量，不依赖任何文件格式：PDF/PNG/XPS 编码器
// 拿到的都是同一份页 + 绘制指令，所以"同一文档连续排版两次分页一致"这条
// 验收标准可以脱离输出格式单独测试。
//
// 单位：DOM 用缇（1/20 pt），IR 用 pt。换算只在排版入口发生。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/layout.h"
#include "doccore/pdf.h"
#include "doccore/text.h"

namespace doccore {
namespace text {

struct LayoutOptions {
  // 为空时用内置度量（拉丁 0.5 em、CJK 1 em），用于无字体环境的测试。
  const FontRegistry* fonts = nullptr;
  std::string default_family = "Calibri";
  double default_size_pt = 11.0;
  // 页面安全上限：超过即抛 LimitExceeded，避免畸形文档把内存打满。
  int max_pages = 4000;
  // 表格与图片的兜底尺寸（pt），文档未给出可用宽度时使用。
  double fallback_table_width_pt = 400.0;
  // 排版期告警（缺字体替换、图片占位、超长表格等）。
  WarningSink* warnings = nullptr;
};

// 排版：节 -> 段落/表格 -> 行 -> 页，产出可直接交给编码器的页列表。
// 抛 Error：LimitExceeded（页数超限）/ Cancelled。
LayoutDocument LayoutText(const Document& doc, const LayoutOptions& opts = {},
                          CancelToken* cancel = nullptr);

// 纯文本导出：按阅读顺序拼接，保留段落换行、制表符与表格单元格分隔。
std::string ExtractText(const Document& doc);

// HTML 导出：单文件、内联样式，图片以占位框表示。
std::string ToHtml(const Document& doc);

}  // namespace text
}  // namespace doccore
