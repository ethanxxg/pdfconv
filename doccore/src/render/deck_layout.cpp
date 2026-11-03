// DocCore - 幻灯片布局：一页幻灯片 -> 一个 LayoutPage，绝对定位、不分页。
// Design refs: docs/02 §3.7
#include "doccore/deck_layout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doccore/image.h"
#include "doccore/render.h"

namespace doccore {
namespace deck {
namespace {

// PowerPoint 单倍行距约等于 1.2 倍字号，和 LibreOffice 一致。
constexpr double kSingleLineFactor = 1.2;

void Warn(WarningSink* sink, const std::string& message, const std::string& path) {
  if (sink == nullptr) return;
  Warning w;
  w.code = static_cast<int>(WarningCode::ImageSkipped);
  w.module = "deck_layout";
  w.path = path;
  w.message = message;
  sink->Add(w);
}

Rgba RgbOf(int rgb) {
  Rgba c;
  c.r = static_cast<uint8_t>((rgb >> 16) & 0xFF);
  c.g = static_cast<uint8_t>((rgb >> 8) & 0xFF);
  c.b = static_cast<uint8_t>(rgb & 0xFF);
  c.a = 255;
  return c;
}

bool IsWide(char32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

// 有真字体就用真字宽，否则用确定性的回退值（ASCII 半宽、CJK 全宽）。
class Metrics {
 public:
  explicit Metrics(const FontRegistry* fonts) : fonts_(fonts) {}

  double Measure(const FontKey& key, double size_pt, const std::string& utf8) const {
    double width = 0.0;
    for (char32_t cp : Utf8ToU32(utf8)) width += Advance(key, size_pt, cp);
    return width;
  }

  double Advance(const FontKey& key, double size_pt, char32_t cp) const {
    const TtfFont* font = fonts_ != nullptr ? fonts_->Match(key) : nullptr;
    if (font != nullptr) {
      const uint16_t gid = font->GlyphForCodepoint(cp);
      if (gid != 0 || cp == U' ') {
        const double upem = font->units_per_em() != 0 ? font->units_per_em() : 1000.0;
        return font->AdvanceWidth(gid) / upem * size_pt;
      }
    }
    return IsWide(cp) ? size_pt : size_pt * 0.5;
  }

  double Ascent(double size_pt) const { return size_pt * 0.8; }

 private:
  const FontRegistry* fonts_;
};

// 行内一段超链接：uri + 相对行首的宽度区间（pt）。空 uri 表示普通文本。
struct LinkSeg {
  std::string uri;
  double w0 = 0.0, w1 = 0.0;
};

// 一行已排版文本：内容 + 行首相对文本框左上角的偏移 + 行高。
struct Line {
  std::string text;
  double width = 0.0;
  double height = 0.0;
  double size_pt = 0.0;
  FontKey font;
  Rgba color;
  int align = 0;  // 0 左 / 1 中 / 2 右 / 3 两端（按左处理）
  double space_before = 0.0;
  std::vector<LinkSeg> links;
};

// 按宽度折行。CJK 逐字可断，西文按空格断；超长单词硬断。
void WrapParagraph(const Paragraph& paragraph, const RunFormat& base, const Metrics& metrics,
                   double available, std::vector<Line>* out) {
  Line current;
  std::string run_uri;  // 当前 run 的超链接，mark() 读它
  current.font.family = base.family;
  current.font.bold = base.bold;
  current.font.italic = base.italic;
  current.size_pt = base.size_pt;
  current.color = RgbOf(base.color >= 0 ? base.color : 0x000000);
  current.align = paragraph.align < 0 ? 0 : paragraph.align;
  current.height = base.size_pt * kSingleLineFactor;
  current.space_before = out->empty() ? 0.0 : paragraph.space_before_pt;

  auto flush = [&] {
    out->push_back(current);
    current.text.clear();
    current.links.clear();
    current.width = 0.0;
    current.space_before = 0.0;
  };

  // 把 [w0, w1) 归入当前 run 的链接区间；相邻同类区间合并，避免碎块。
  auto mark = [&](double w0, double w1) {
    if (run_uri.empty()) return;
    if (!current.links.empty() && current.links.back().uri == run_uri &&
        current.links.back().w1 >= w0 - 1e-6) {
      current.links.back().w1 = std::max(current.links.back().w1, w1);
      return;
    }
    current.links.push_back(LinkSeg{run_uri, w0, w1});
  };

  for (const Run& run : paragraph.runs) {
    run_uri = run.hyperlink;
    FontKey key;
    key.family = run.format.family.empty() ? base.family : run.format.family;
    key.bold = run.format.bold || base.bold;
    key.italic = run.format.italic || base.italic;
    const double size = run.format.size_pt > 0.0 ? run.format.size_pt : base.size_pt;
    const Rgba color = RgbOf(run.format.color >= 0 ? run.format.color
                                                   : (base.color >= 0 ? base.color : 0x000000));
    // 字号变化按行内最大字号算行高。
    if (size > current.size_pt) {
      current.size_pt = size;
      current.height = size * kSingleLineFactor;
    }
    // 行内样式混排时行首样式为准（P0 简化，导出 PDF 的字体切换留在 P9）。
    if (current.font.family.empty()) current.font.family = key.family;

    const std::u32string text = Utf8ToU32(run.text);
    size_t i = 0;
    while (i < text.size()) {
      const char32_t cp = text[i];
      if (cp == U'\n') {
        flush();
        current.color = color;
        ++i;
        continue;
      }
      // 取一个可断单元：CJK 单字，西文取到下一个空格。
      size_t end = i + 1;
      if (!IsWide(cp) && cp != U' ') {
        while (end < text.size() && text[end] != U' ' && text[end] != U'\n' &&
               !IsWide(text[end])) {
          ++end;
        }
      }
      const std::string piece = U32ToUtf8(std::u32string_view(text).substr(i, end - i));
      const double advance = metrics.Measure(key, size, piece);
      if (available > 0.0 && !current.text.empty() && current.width + advance > available) {
        flush();
        current.color = color;
        current.size_pt = size;
        current.height = size * kSingleLineFactor;
        if (cp == U' ') {  // 折行处的空格丢掉
          i = end;
          continue;
        }
      }
      // 单个单元比整行还宽时硬断。
      if (available > 0.0 && current.text.empty() && advance > available) {
        std::string hard;
        double used = 0.0;
        for (char32_t c : Utf8ToU32(piece)) {
          const std::string one = U32ToUtf8(std::u32string(1, c));
          const double w = metrics.Measure(key, size, one);
          mark(used, used + w);
          if (used + w > available && !hard.empty()) {
            current.text = hard;
            current.width = used;
            flush();
            current.color = color;
            current.size_pt = size;
            current.height = size * kSingleLineFactor;
            hard.clear();
            used = 0.0;
          }
          hard += one;
          used += w;
        }
        current.text += hard;
        current.width += used;
        i = end;
        continue;
      }
      mark(current.width, current.width + advance);
      current.text += piece;
      current.width += advance;
      i = end;
    }
  }
  flush();
}

// 文本框内排版：返回所有行（含段落间距），并给出总高度。
std::vector<Line> LayoutTextBody(const Presentation& pres, const TextBody& body, const Shape& shape,
                                 const Metrics& metrics, double available) {
  std::vector<Line> lines;
  const double fallback_size =
      (shape.placeholder == "title" || shape.placeholder == "ctrTitle") ? 40.0
                                                                       : pres.default_size_pt;
  for (const Paragraph& paragraph : body.paragraphs) {
    RunFormat base;
    base.family = pres.default_font;
    base.size_pt = fallback_size;
    base.color = pres.default_text_color;
    // 段落里第一个 run 的样式作为基准，后续 run 各自覆盖。
    if (!paragraph.runs.empty() && paragraph.runs.front().format.size_pt > 0.0) {
      base.size_pt = paragraph.runs.front().format.size_pt;
    }
    WrapParagraph(paragraph, base, metrics, available, &lines);
    if (!lines.empty() && paragraph.space_after_pt > 0.0) {
      lines.back().height += paragraph.space_after_pt;
    }
  }
  return lines;
}

void EmitTextBody(LayoutPage* page, const Presentation& pres, const TextBody& body,
                  const Shape& shape, const Metrics& metrics, double x, double y, double w,
                  double h) {
  const double inset_l = static_cast<double>(shape.kind == ShapeKind::Table ? 91440 : body.inset_left);
  const double inset_t = static_cast<double>(shape.kind == ShapeKind::Table ? 45720 : body.inset_top);
  const double inset_r = static_cast<double>(shape.kind == ShapeKind::Table ? 91440 : body.inset_right);
  const double inset_b = static_cast<double>(shape.kind == ShapeKind::Table ? 45720 : body.inset_bottom);
  const double left = x + static_cast<double>(inset_l) / kEmuPerPoint;
  const double top = y + static_cast<double>(inset_t) / kEmuPerPoint;
  const double available = std::max(0.0, w - static_cast<double>(inset_l + inset_r) / kEmuPerPoint);
  const double box_height = std::max(0.0, h - static_cast<double>(inset_t + inset_b) / kEmuPerPoint);

  const std::vector<Line> lines = LayoutTextBody(pres, body, shape, metrics, available);
  if (lines.empty()) return;

  double total = 0.0;
  for (const Line& line : lines) total += line.space_before + line.height;

  double cursor = top;
  if (body.anchor == 1) cursor = top + (box_height - total) / 2.0;
  else if (body.anchor == 2) cursor = top + box_height - total;
  if (cursor < top && body.anchor != 0) cursor = top;

  for (const Line& line : lines) {
    cursor += line.space_before;
    if (line.text.empty()) {
      cursor += line.height;
      continue;
    }
    double tx = left;
    if (line.align == 1) tx = left + (available - line.width) / 2.0;
    else if (line.align == 2) tx = left + available - line.width;
    if (tx < left) tx = left;

    DrawText draw;
    draw.font = line.font;
    draw.font.family = line.font.family.empty() ? pres.default_font : line.font.family;
    draw.size_pt = line.size_pt;
    draw.color = line.color;
    draw.x = tx;
    draw.y = cursor + metrics.Ascent(line.size_pt);
    draw.text_utf8 = line.text;
    page->ops.push_back(std::move(draw));
    // 行内超链接 -> 页面链接注释（与文本同一矩形区域）。
    for (const LinkSeg& seg : line.links) {
      if (seg.w1 <= seg.w0) continue;
      Link link;
      link.uri = seg.uri;
      link.x = tx + seg.w0;
      link.y = cursor;
      link.w_pt = seg.w1 - seg.w0;
      link.h_pt = line.height;
      page->links.push_back(std::move(link));
    }
    cursor += line.height;
  }
}

void DrawShape(LayoutPage* page, const Presentation& pres, const Shape& shape,
               const DeckLayoutOptions& opts, const Metrics& metrics) {
  const double x = static_cast<double>(shape.geom.x) / kEmuPerPoint;
  const double y = static_cast<double>(shape.geom.y) / kEmuPerPoint;
  const double w = static_cast<double>(shape.geom.cx) / kEmuPerPoint;
  const double h = static_cast<double>(shape.geom.cy) / kEmuPerPoint;

  switch (shape.kind) {
    case ShapeKind::Picture: {
      ImageData image;
      if (!shape.picture.bytes.empty() && TryDecodeImage(shape.picture.bytes, &image, opts.warnings)) {
        DrawImage draw;
        draw.image = std::move(image);
        draw.x = x;
        draw.y = y;
        draw.w_pt = w;
        draw.h_pt = h;
        page->ops.push_back(std::move(draw));
      } else {
        // 解不出来也要占住位置：画一个带边框的浅灰框，位置与形状完全一致。
        page->ops.push_back(MakeRect(x, y, w, h, Rgba{0xEE, 0xEE, 0xEE, 255}, true,
                                     Rgba{0xAA, 0xAA, 0xAA, 255}, 0.75));
        if (shape.picture.bytes.empty()) {
          Warn(opts.warnings, "picture shape has no image data", shape.name);
        }
      }
      break;
    }
    case ShapeKind::Table: {
      double row_y = y;
      for (size_t row_index = 0; row_index < shape.table.rows.size(); ++row_index) {
        const std::vector<TableCell>& row = shape.table.rows[row_index];
        const double row_h = row_index < shape.table.row_heights.size()
                                 ? static_cast<double>(shape.table.row_heights[row_index]) /
                                       kEmuPerPoint
                                 : 370840.0 / kEmuPerPoint;
        double col_x = x;
        for (size_t col_index = 0; col_index < row.size(); ++col_index) {
          const TableCell& cell = row[col_index];
          if (cell.merged) continue;
          double cell_w = 0.0;
          for (int span = 0; span < std::max(1, cell.col_span); ++span) {
            const size_t at = col_index + static_cast<size_t>(span);
            cell_w += at < shape.table.col_widths.size()
                          ? static_cast<double>(shape.table.col_widths[at]) / kEmuPerPoint
                          : 914400.0 / kEmuPerPoint;
          }
          double cell_h = 0.0;
          for (int span = 0; span < std::max(1, cell.row_span); ++span) {
            const size_t at = row_index + static_cast<size_t>(span);
            cell_h += at < shape.table.row_heights.size()
                          ? static_cast<double>(shape.table.row_heights[at]) / kEmuPerPoint
                          : 370840.0 / kEmuPerPoint;
          }
          if (cell.fill >= 0) {
            page->ops.push_back(MakeRect(col_x, row_y, cell_w, cell_h, RgbOf(cell.fill)));
          }
          if (opts.draw_shape_borders) {
            page->ops.push_back(MakeRect(col_x, row_y, cell_w, cell_h, Rgba{0, 0, 0, 0}, true,
                                         RgbOf(shape.table.border_color), 0.75));
          }
          EmitTextBody(page, pres, cell.body, shape, metrics, col_x, row_y, cell_w, cell_h);
          col_x += cell_w;
        }
        row_y += row_h;
      }
      break;
    }
    case ShapeKind::Text:
    default: {
      if (shape.fill >= 0) page->ops.push_back(MakeRect(x, y, w, h, RgbOf(shape.fill)));
      if (opts.draw_shape_borders && shape.line_color >= 0) {
        page->ops.push_back(MakeRect(x, y, w, h, Rgba{0, 0, 0, 0}, true,
                                     RgbOf(shape.line_color),
                                     shape.line_width_pt > 0 ? shape.line_width_pt : 0.75));
      }
      EmitTextBody(page, pres, shape.text, shape, metrics, x, y, w, h);
      break;
    }
  }
}

}  // namespace

LayoutDocument LayoutPresentation(const Presentation& pres, const DeckLayoutOptions& opts) {
  FontRegistry empty_registry;
  const FontRegistry* fonts = opts.fonts != nullptr ? opts.fonts : &empty_registry;
  const Metrics metrics(fonts);

  LayoutDocument out;
  const int count = std::min(pres.slide_count(), opts.max_slides);
  if (count <= 0) {
    LayoutPage page;
    page.width_pt = static_cast<double>(pres.slide_width) / kEmuPerPoint;
    page.height_pt = static_cast<double>(pres.slide_height) / kEmuPerPoint;
    out.pages.push_back(std::move(page));
    return out;
  }
  for (int i = 0; i < count; ++i) {
    const Slide* slide = pres.SlideAt(i);
    if (slide == nullptr) continue;
    LayoutPage page;
    page.width_pt = static_cast<double>(pres.slide_width) / kEmuPerPoint;
    page.height_pt = static_cast<double>(pres.slide_height) / kEmuPerPoint;
    const int background = slide->background() >= 0 ? slide->background()
                                                    : pres.master_background;
    if (background >= 0) {
      page.ops.push_back(MakeRect(0.0, 0.0, page.width_pt, page.height_pt, RgbOf(background)));
    }
    for (const Shape& shape : slide->shapes()) {
      DrawShape(&page, pres, shape, opts, metrics);
    }
    out.pages.push_back(std::move(page));
    // 每页幻灯片生成一条大纲：优先标题占位符文本，缺失时退化成 "Slide N"。
    const Shape* title = slide->FindPlaceholder("title");
    if (title == nullptr) title = slide->FindPlaceholder("ctrTitle");
    std::string title_text = title != nullptr ? title->text.PlainText() : std::string();
    if (title_text.empty()) title_text = "Slide " + std::to_string(i + 1);
    Bookmark bm;
    bm.title = title_text;
    bm.level = 1;
    bm.page = static_cast<int>(out.pages.size()) - 1;
    bm.y = title != nullptr ? static_cast<double>(title->geom.y) / kEmuPerPoint : 0.0;
    out.bookmarks.push_back(std::move(bm));
  }
  if (out.pages.empty()) {
    LayoutPage page;
    page.width_pt = static_cast<double>(pres.slide_width) / kEmuPerPoint;
    page.height_pt = static_cast<double>(pres.slide_height) / kEmuPerPoint;
    out.pages.push_back(std::move(page));
  }
  return out;
}

void SavePdfToFile(const Presentation& pres, const std::string& path,
                   const DeckLayoutOptions& layout_opts, const PdfSaveOptions& pdf_opts) {
  FontRegistry empty_registry;
  const FontRegistry* fonts =
      layout_opts.fonts != nullptr ? layout_opts.fonts : &empty_registry;
  DeckLayoutOptions opts = layout_opts;
  opts.fonts = fonts;
  const LayoutDocument laid = LayoutPresentation(pres, opts);
  PdfSaveOptions pdf = pdf_opts;
  if (pdf.title.empty()) pdf.title = path;
  const std::vector<uint8_t> bytes = PdfWriter::ToBytes(laid, *fonts, pdf, opts.warnings);
  WriteFileAtomically(path, bytes);
}

}  // namespace deck
}  // namespace doccore
