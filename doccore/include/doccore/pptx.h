// DocCore - PPTX 读写入口（docs/02 §3.7）。
//
//   deck::Presentation p = deck::Open({.path = "in.pptx"});
//   p.AddSlide().shapes() ... 或直接用 deck::AddTextBox 等辅助函数
//   deck::Save(p, "out.pptx");
//
// 读：presentation.xml 的幻灯片顺序与尺寸、每页 spTree 里的 sp/pic/graphicFrame、
// 主题与母版的默认字体、备注页；未识别部件原样保留。
// 写：生成一份固定的母版/版式/主题，幻灯片按 DOM 逐页写出。
#pragma once

#include <string>
#include <string_view>

#include "doccore/core.h"
#include "doccore/deck.h"
#include "doccore/stream.h"

namespace doccore {
namespace deck {

struct OpenOptions {
  std::string path;
  CancelToken* cancel = nullptr;
  WarningSink* warnings = nullptr;
};

// 抛 Error：Io / Corrupt（非 OOXML）/ Unsupported（不是演示文稿族）。
Presentation Open(const OpenOptions& opts);
Presentation OpenMemory(std::string_view bytes, WarningSink* warnings = nullptr,
                        CancelToken* cancel = nullptr);

struct SaveOptions {
  WarningSink* warnings = nullptr;
  CancelToken* cancel = nullptr;
};

void Save(const Presentation& pres, Stream& out, const SaveOptions& opts = {});
void SaveToFile(const Presentation& pres, const std::string& path, const SaveOptions& opts = {});

// ---- 构造辅助 -----------------------------------------------------------

// 在 (x, y) 处放一个 width x height 的文本框，内容是若干行（每行一个段落）。
Shape& AddTextBox(Slide& slide, double x_pt, double y_pt, double width_pt, double height_pt,
                  const std::vector<std::string>& lines, double size_pt = 18.0,
                  bool bold = false, int color = -1, int align = -1);

// 图片形状：字节里读 PNG/JPEG 头得到像素尺寸，未给尺寸时按 96 DPI 折算。
Shape& AddPicture(Slide& slide, double x_pt, double y_pt, std::vector<uint8_t> bytes,
                  double width_pt = 0.0, double height_pt = 0.0);

// 表格：rows x cols，列宽/行高等分。
Shape& AddTable(Slide& slide, double x_pt, double y_pt, double width_pt, double height_pt,
                int rows, int cols);

// 图片像素尺寸（PNG IHDR / JPEG SOF），失败返回 false。
bool ImagePixelSize(const std::vector<uint8_t>& bytes, int* width, int* height);

// 纯文本 / HTML 导出（与字处理、表格族保持同一套开关）。
std::string ToText(const Presentation& pres);
std::string ToHtml(const Presentation& pres);

}  // namespace deck
}  // namespace doccore
