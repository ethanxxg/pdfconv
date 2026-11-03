// DocCore - Layout IR, the frozen contract between layout and the PDF/PNG
// encoders. Design refs: docs/02 §3.2. Origin is top-left, Y grows downward.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace doccore {

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 255;
};

struct FontKey {
  std::string family;
  bool bold = false;
  bool italic = false;

  bool operator==(const FontKey& o) const {
    return family == o.family && bold == o.bold && italic == o.italic;
  }
};

struct DrawText {
  FontKey font;
  double size_pt = 12.0;
  Rgba color;
  double x = 0.0;  // baseline start, points
  double y = 0.0;
  // 绕 (x, y) 的旋转，**逆时针为正**，单位度（页面上看到的旋转方向）。
  // 0 表示水平；水印的 -45° 就是 315。PNG 编码器暂不处理文字，所以只影响
  // PDF 输出。
  double rotate_deg = 0.0;
  std::string text_utf8;
};

struct ImageData {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
};

struct DrawImage {
  ImageData image;
  double x = 0, y = 0, w_pt = 0, h_pt = 0;
};

enum class PathVerbKind { MoveTo, LineTo, CubicTo, Close };

struct PathVerb {
  PathVerbKind kind = PathVerbKind::MoveTo;
  double x1 = 0, y1 = 0;  // endpoint, or first control point for CubicTo
  double x2 = 0, y2 = 0;  // second control point
  double x3 = 0, y3 = 0;  // endpoint
};

struct DrawPath {
  std::vector<PathVerb> verbs;
  bool has_fill = true;
  bool has_stroke = false;
  Rgba fill;
  Rgba stroke;
  double stroke_pt = 1.0;
};

using DrawOp = std::variant<DrawText, DrawImage, DrawPath>;

// 页面上的可点击区域。坐标以页左上角为原点、单位 pt，与 DrawText/DrawPath 一致
// （PDF 写出时由 writer 转成 PDF 的左上原点）。
struct Link {
  double x = 0, y = 0, w_pt = 0, h_pt = 0;
  std::string uri;   // 外部 URI 动作；为空表示文档内跳转
  std::string dest;  // 内部命名目标（书签名）；为空表示外部链接
};

// 文档大纲（书签）节点。level 从 1 开始；page 为 0 基页号，y 为该页上距页顶
// 的 pt 数（与 DrawText 同坐标系）。
struct Bookmark {
  std::string title;
  int level = 1;
  int page = 0;
  double y = 0.0;
};

// 命名目标：w:bookmarkStart 之类在页面上留下的可跳转位置。
struct NamedDest {
  std::string name;
  double x = 0, y = 0;  // pt，左上原点
};

struct LayoutPage {
  double width_pt = 595.276;   // A4
  double height_pt = 841.89;
  std::vector<DrawOp> ops;
  std::vector<Link> links;
  std::vector<NamedDest> dests;
};

struct LayoutDocument {
  std::vector<LayoutPage> pages;
  // 按出现顺序排列的大纲条目（level 表达层级，写出时建树）。
  std::vector<Bookmark> bookmarks;
};

// Convenience builders used by tests and the CLI.
DrawPath MakeRect(double x, double y, double w, double h, Rgba fill, bool stroke = false,
                  Rgba stroke_color = {}, double stroke_pt = 1.0);
DrawPath MakeLine(double x1, double y1, double x2, double y2, Rgba color, double width_pt = 1.0);

}  // namespace doccore
