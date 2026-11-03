// DocCore - 演示文稿 DOM 实现。Design refs: docs/02 §3.7
#include "doccore/deck.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "doccore/pptx.h"

namespace doccore {
namespace deck {

namespace {

// 默认 16:9 版面：标题在上、正文在下，与 PowerPoint 的"标题和内容"版式接近。
constexpr int64_t kTitleX = 838200;    // 0.66 in
constexpr int64_t kTitleY = 365125;    // 0.29 in
constexpr int64_t kTitleCx = 10515600;
constexpr int64_t kTitleCy = 1325563;
constexpr int64_t kBodyX = 838200;
constexpr int64_t kBodyY = 1825625;
constexpr int64_t kBodyCx = 10515600;
constexpr int64_t kBodyCy = 4351338;

Shape MakePlaceholderShape(const std::string& type) {
  Shape shape;
  shape.kind = ShapeKind::Text;
  shape.placeholder = type;
  shape.name = type == "title" || type == "ctrTitle" ? "Title" : "Content";
  if (type == "title" || type == "ctrTitle") {
    shape.geom = ShapeGeometry{kTitleX, kTitleY, kTitleCx, kTitleCy};
  } else {
    shape.geom = ShapeGeometry{kBodyX, kBodyY, kBodyCx, kBodyCy};
  }
  shape.text.anchor = 1;
  return shape;
}

}  // namespace

// ---- 文本 ---------------------------------------------------------------

std::string Paragraph::PlainText() const {
  std::string out;
  for (const Run& run : runs) out += run.text;
  return out;
}

bool TextBody::empty() const noexcept {
  for (const Paragraph& paragraph : paragraphs) {
    for (const Run& run : paragraph.runs) {
      if (!run.text.empty()) return false;
    }
  }
  return true;
}

std::string TextBody::PlainText() const {
  std::string out;
  for (size_t i = 0; i < paragraphs.size(); ++i) {
    if (i != 0) out += '\n';
    out += paragraphs[i].PlainText();
  }
  return out;
}

// ---- 幻灯片 -------------------------------------------------------------

Shape* Slide::FindPlaceholder(std::string_view type) {
  for (Shape& shape : shapes_) {
    if (shape.placeholder == type) return &shape;
  }
  return nullptr;
}

const Shape* Slide::FindPlaceholder(std::string_view type) const {
  for (const Shape& shape : shapes_) {
    if (shape.placeholder == type) return &shape;
  }
  return nullptr;
}

Shape& Slide::AddPlaceholder(std::string type, ShapeGeometry geom, std::string prompt) {
  Shape shape;
  shape.kind = ShapeKind::Text;
  shape.placeholder = std::move(type);
  shape.geom = geom;
  shape.name = shape.placeholder == "title" || shape.placeholder == "ctrTitle" ? "Title" : "Content";
  if (!prompt.empty()) {
    Paragraph paragraph;
    Run run;
    run.text = std::move(prompt);
    paragraph.runs.push_back(std::move(run));
    shape.text.paragraphs.push_back(std::move(paragraph));
  }
  shapes_.push_back(std::move(shape));
  return shapes_.back();
}

// ---- 切换与动画 ---------------------------------------------------------

namespace {

// 顺序与 TransitionType 枚举一一对应。
constexpr const char* kTransitionNames[] = {
    "none",   "blinds",  "checker", "circle",   "comb",    "cover",
    "cut",    "diamond", "dissolve", "fade",    "newsflash", "plus",
    "pull",   "push",    "random",  "randomBar", "split",  "strips",
    "wedge",  "wheel",   "wipe",    "zoom"};
constexpr size_t kTransitionCount =
    sizeof(kTransitionNames) / sizeof(kTransitionNames[0]);

}  // namespace

const char* TransitionTypeName(TransitionType type) noexcept {
  const size_t i = static_cast<size_t>(type);
  return i < kTransitionCount ? kTransitionNames[i] : "none";
}

TransitionType TransitionTypeFromName(std::string_view name) {
  for (size_t i = 0; i < kTransitionCount; ++i) {
    if (name == kTransitionNames[i]) return static_cast<TransitionType>(i);
  }
  return TransitionType::None;
}

const char* TransitionSpeedName(TransitionSpeed speed) noexcept {
  switch (speed) {
    case TransitionSpeed::Slow: return "slow";
    case TransitionSpeed::Medium: break;
    case TransitionSpeed::Fast: return "fast";
  }
  return "med";
}

TransitionSpeed TransitionSpeedFromName(std::string_view name) {
  if (name == "slow") return TransitionSpeed::Slow;
  if (name == "fast") return TransitionSpeed::Fast;
  return TransitionSpeed::Medium;
}

const char* AnimationKindName(AnimationKind kind) noexcept {
  switch (kind) {
    case AnimationKind::Entrance: return "entrance";
    case AnimationKind::Emphasis: return "emphasis";
    case AnimationKind::Exit: return "exit";
    case AnimationKind::MotionPath: return "motionPath";
  }
  return "entrance";
}

const char* AnimationTriggerName(AnimationTrigger trigger) noexcept {
  switch (trigger) {
    case AnimationTrigger::OnClick: return "onClick";
    case AnimationTrigger::WithPrevious: return "withPrevious";
    case AnimationTrigger::AfterPrevious: return "afterPrevious";
  }
  return "onClick";
}

void Slide::set_transition(SlideTransition value) {
  transition_ = std::move(value);
  // 模型一旦被显式设置，读入时保留下来的原文就不能再用了，否则改动被静默吞掉。
  transition_xml_.clear();
}

void Slide::SetAnimations(std::vector<SlideAnimation> list) {
  animations_ = std::move(list);
  // 同上：DOM 成为权威，原始 <p:timing> 作废。
  timing_xml_.clear();
}

void Slide::ClearAnimations() {
  animations_.clear();
  timing_xml_.clear();
}

std::string Slide::PlainText() const {
  std::string out;
  for (const Shape& shape : shapes_) {
    std::string text;
    if (shape.kind == ShapeKind::Table) {
      // 表格按行拼，单元格之间用制表符，方便 grep / 目视核对。
      for (const auto& row : shape.table.rows) {
        // 用非空单元格拼行，避免空单元格留下多余的制表符。
        std::string line;
        for (const TableCell& cell : row) {
          if (cell.merged) continue;
          const std::string cell_text = cell.body.PlainText();
          if (cell_text.empty()) continue;
          if (!line.empty()) line += '\t';
          line += cell_text;
        }
        if (line.empty()) continue;
        if (!text.empty()) text += '\n';
        text += line;
      }
    } else {
      text = shape.text.PlainText();
    }
    if (text.empty()) continue;
    if (!out.empty()) out += '\n';
    out += text;
  }
  return out;
}

// ---- 演示文稿 -----------------------------------------------------------

Presentation Presentation::Create() {
  Presentation pres;
  pres.slides.clear();
  return pres;
}

Slide* Presentation::SlideAt(int index) {
  if (index < 0 || index >= slide_count()) return nullptr;
  return slides[static_cast<size_t>(index)].get();
}

const Slide* Presentation::SlideAt(int index) const {
  if (index < 0 || index >= slide_count()) return nullptr;
  return slides[static_cast<size_t>(index)].get();
}

Slide& Presentation::AddSlide() {
  auto slide = std::make_unique<Slide>();
  slide->set_name("Slide " + std::to_string(slides.size() + 1));
  slides.push_back(std::move(slide));
  return *slides.back();
}

Slide& Presentation::AddSlide(std::string layout_uri) {
  auto slide = std::make_unique<Slide>(std::move(layout_uri));
  slide->set_name("Slide " + std::to_string(slides.size() + 1));
  slides.push_back(std::move(slide));
  return *slides.back();
}

void Presentation::RemoveSlide(int index) {
  if (index < 0 || index >= slide_count()) return;
  slides.erase(slides.begin() + index);
}

std::string Presentation::ExtractText() const {
  std::string out;
  for (size_t i = 0; i < slides.size(); ++i) {
    out += "=== slide ";
    out += std::to_string(i + 1);
    out += " ===\n";
    out += slides[i]->PlainText();
    if (!slides[i]->notes().empty()) {
      out += "\n--- notes ---\n";
      out += slides[i]->notes();
    }
    out += "\n";
  }
  return out;
}

Presentation Presentation::Clone() const {
  Presentation out;
  out.slide_width = slide_width;
  out.slide_height = slide_height;
  out.default_font = default_font;
  out.default_size_pt = default_size_pt;
  out.default_text_color = default_text_color;
  out.master_background = master_background;
  out.unknown_parts = unknown_parts;
  out.slides.reserve(slides.size());
  for (const std::unique_ptr<Slide>& slide : slides) {
    out.slides.push_back(std::make_unique<Slide>(*slide));
  }
  return out;
}

// ---- 构造辅助 -----------------------------------------------------------

Shape& AddTextBox(Slide& slide, double x_pt, double y_pt, double width_pt, double height_pt,
                  const std::vector<std::string>& lines, double size_pt, bool bold, int color,
                  int align) {
  Shape shape;
  shape.kind = ShapeKind::Text;
  shape.geom = ShapeGeometry{PtToEmu(x_pt), PtToEmu(y_pt), PtToEmu(width_pt), PtToEmu(height_pt)};
  shape.name = "TextBox";
  shape.text.anchor = 0;
  for (const std::string& line : lines) {
    Paragraph paragraph;
    Run run;
    run.text = line;
    run.format.size_pt = size_pt;
    run.format.bold = bold;
    run.format.color = color;
    paragraph.runs.push_back(std::move(run));
    paragraph.align = align;
    shape.text.paragraphs.push_back(std::move(paragraph));
  }
  slide.shapes().push_back(std::move(shape));
  return slide.shapes().back();
}

Shape& AddPicture(Slide& slide, double x_pt, double y_pt, std::vector<uint8_t> bytes,
                  double width_pt, double height_pt) {
  Shape shape;
  shape.kind = ShapeKind::Picture;
  shape.name = "Picture";
  shape.picture.bytes = std::move(bytes);
  int width_px = 0;
  int height_px = 0;
  if (ImagePixelSize(shape.picture.bytes, &width_px, &height_px)) {
    shape.picture.pixel_width = width_px;
    shape.picture.pixel_height = height_px;
  }
  if (width_pt <= 0.0 && height_pt <= 0.0) {
    // 96 DPI 下的自然尺寸；没有头信息时退回 4:3 占位。
    width_pt = width_px > 0 ? width_px * 72.0 / 96.0 : 320.0;
    height_pt = height_px > 0 ? height_px * 72.0 / 96.0 : 240.0;
  } else if (width_pt <= 0.0) {
    width_pt = height_px > 0 ? height_pt * width_px / height_px : height_pt * 4.0 / 3.0;
  } else if (height_pt <= 0.0) {
    height_pt = width_px > 0 ? width_pt * height_px / width_px : width_pt * 3.0 / 4.0;
  }
  shape.geom = ShapeGeometry{PtToEmu(x_pt), PtToEmu(y_pt), PtToEmu(width_pt), PtToEmu(height_pt)};
  slide.shapes().push_back(std::move(shape));
  return slide.shapes().back();
}

Shape& AddTable(Slide& slide, double x_pt, double y_pt, double width_pt, double height_pt,
                int rows, int cols) {
  Shape shape;
  shape.kind = ShapeKind::Table;
  shape.name = "Table";
  shape.geom = ShapeGeometry{PtToEmu(x_pt), PtToEmu(y_pt), PtToEmu(width_pt), PtToEmu(height_pt)};
  const int safe_rows = std::max(1, rows);
  const int safe_cols = std::max(1, cols);
  const int64_t total_w = shape.geom.cx;
  const int64_t total_h = shape.geom.cy;
  for (int c = 0; c < safe_cols; ++c) {
    shape.table.col_widths.push_back(total_w / safe_cols);
  }
  for (int r = 0; r < safe_rows; ++r) {
    shape.table.row_heights.push_back(total_h / safe_rows);
    std::vector<TableCell> row(static_cast<size_t>(safe_cols));
    shape.table.rows.push_back(std::move(row));
  }
  slide.shapes().push_back(std::move(shape));
  return slide.shapes().back();
}

// ---- 图片尺寸 -----------------------------------------------------------

namespace {

bool PngSize(const std::vector<uint8_t>& bytes, int* width, int* height) {
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 24 || std::memcmp(bytes.data(), kSig, 8) != 0) return false;
  *width = static_cast<int>(ReadBe32(bytes.data() + 16));
  *height = static_cast<int>(ReadBe32(bytes.data() + 20));
  return *width > 0 && *height > 0;
}

bool JpegSize(const std::vector<uint8_t>& bytes, int* width, int* height) {
  if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < bytes.size()) {
    if (bytes[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t marker = bytes[i + 1];
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      i += 2;
      continue;
    }
    const size_t length = (static_cast<size_t>(bytes[i + 2]) << 8) | bytes[i + 3];
    // SOF0..SOF15（排除 DHT=C4、JPG=C8、DAC=CC）
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      *height = static_cast<int>((static_cast<size_t>(bytes[i + 5]) << 8) | bytes[i + 6]);
      *width = static_cast<int>((static_cast<size_t>(bytes[i + 7]) << 8) | bytes[i + 8]);
      return *width > 0 && *height > 0;
    }
    i += 2 + length;
  }
  return false;
}

}  // namespace

bool ImagePixelSize(const std::vector<uint8_t>& bytes, int* width, int* height) {
  if (width == nullptr || height == nullptr) return false;
  return PngSize(bytes, width, height) || JpegSize(bytes, width, height);
}

// ---- 文本导出 -----------------------------------------------------------

std::string ToText(const Presentation& pres) { return pres.ExtractText(); }

namespace {

std::string HtmlEsc(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

}  // namespace

std::string ToHtml(const Presentation& pres) {
  std::string out;
  out += "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"/><title>Presentation</title>";
  out += "<style>body{font-family:Calibri,Arial,sans-serif;margin:24px}"
         ".slide{border:1px solid #ddd;border-radius:6px;padding:18px 22px;margin:0 0 20px 0}"
         ".slide h2{font-size:14pt;color:#666;margin:0 0 10px 0}"
         ".notes{color:#666;font-size:10pt;border-top:1px dashed #ccc;margin-top:10px;padding-top:8px}"
         "table{border-collapse:collapse;margin:8px 0}td{border:1px solid #ccc;padding:4px 8px}"
         "</style></head><body>\n";
  for (int i = 0; i < pres.slide_count(); ++i) {
    const Slide* slide = pres.SlideAt(i);
    if (slide == nullptr) continue;
    out += "<div class=\"slide\"><h2>Slide " + std::to_string(i + 1) + "</h2>\n";
    for (const Shape& shape : slide->shapes()) {
      if (shape.kind == ShapeKind::Table) {
        out += "<table>\n";
        for (const std::vector<TableCell>& row : shape.table.rows) {
          out += "<tr>";
          for (const TableCell& cell : row) {
            if (cell.merged) continue;
            out += "<td>" + HtmlEsc(cell.body.PlainText()) + "</td>";
          }
          out += "</tr>\n";
        }
        out += "</table>\n";
        continue;
      }
      if (shape.kind == ShapeKind::Picture) {
        out += "<p><em>[image " + std::to_string(shape.picture.pixel_width) + "x" +
               std::to_string(shape.picture.pixel_height) + "]</em></p>\n";
        continue;
      }
      const std::string text = shape.text.PlainText();
      if (text.empty()) continue;
      const bool is_title = shape.placeholder == "title" || shape.placeholder == "ctrTitle";
      out += is_title ? "<h1>" : "<p>";
      out += HtmlEsc(text);
      out += is_title ? "</h1>\n" : "</p>\n";
    }
    if (!slide->notes().empty()) {
      out += "<div class=\"notes\">" + HtmlEsc(slide->notes()) + "</div>\n";
    }
    out += "</div>\n";
  }
  out += "</body></html>\n";
  return out;
}

}  // namespace deck
}  // namespace doccore
