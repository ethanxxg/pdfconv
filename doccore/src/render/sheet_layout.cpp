// DocCore - 工作表打印布局。Design refs: docs/02 §3.4.3
#include "doccore/sheet_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace doccore {
namespace sheet {

// 单元格在版面上的落点。只有被某个内部超链接引用到的单元格才记录，用来给
// "Sheet2!A1" 这类文档内跳转生成命名目标。
struct CellSpot {
  int row = 0;
  int col = 0;
  size_t page = 0;  // 本工作表内的局部页号
  double x = 0;
  double y = 0;
};

namespace {

constexpr double kTwipsPerPoint = 20.0;

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

// 与 text_layout 同样的策略：有真字体就用真字宽，否则用确定性的回退值。
class Metrics {
 public:
  explicit Metrics(const FontRegistry* fonts) : fonts_(fonts) {}

  double Measure(const FontKey& key, double size_pt, const std::string& utf8) const {
    const std::u32string text = Utf8ToU32(utf8);
    double width = 0.0;
    for (char32_t cp : text) {
      const TtfFont* font = fonts_ != nullptr ? fonts_->Match(key) : nullptr;
      if (font != nullptr) {
        const uint16_t gid = font->GlyphForCodepoint(cp);
        if (gid != 0 || cp == U' ') {
          const double upem = font->units_per_em() != 0 ? font->units_per_em() : 1000.0;
          width += font->AdvanceWidth(gid) / upem * size_pt;
          continue;
        }
      }
      width += IsWide(cp) ? size_pt : size_pt * 0.5;
    }
    return width;
  }

  double Ascent(double size_pt) const { return size_pt * 0.75; }

 private:
  const FontRegistry* fonts_;
};

// 只描边不填充的矩形。MakeRect 总是填充，网格线用它会把底色涂黑。
DrawPath StrokeRect(double x, double y, double w, double h, Rgba color, double width_pt) {
  DrawPath path;
  path.has_fill = false;
  path.has_stroke = true;
  path.stroke = color;
  path.stroke_pt = width_pt;
  PathVerb verb;
  verb.kind = PathVerbKind::MoveTo;
  verb.x1 = x;
  verb.y1 = y;
  path.verbs.push_back(verb);
  verb.kind = PathVerbKind::LineTo;
  verb.x1 = x + w;
  verb.y1 = y;
  path.verbs.push_back(verb);
  verb.x1 = x + w;
  verb.y1 = y + h;
  path.verbs.push_back(verb);
  verb.x1 = x;
  verb.y1 = y + h;
  path.verbs.push_back(verb);
  verb.kind = PathVerbKind::Close;
  path.verbs.push_back(verb);
  return path;
}

// 打印分页：把区间切成若干带。
struct Band {
  int first = 0;
  int last = 0;
};

std::vector<Band> SplitColumns(const Worksheet& ws, const Range& used, double limit_pt,
                               double char_width_pt) {
  std::vector<Band> bands;
  Band current;
  current.first = used.col1;
  current.last = used.col1;
  double width = 0.0;
  for (int col = used.col1; col <= used.col2; ++col) {
    const double w = ws.ColumnWidth(col) * char_width_pt;
    if (width > 0.0 && width + w > limit_pt) {
      bands.push_back(current);
      current.first = col;
      width = 0.0;
    }
    width += w;
    current.last = col;
  }
  bands.push_back(current);
  return bands;
}

std::vector<Band> SplitRows(const Worksheet& ws, const Range& used, double limit_pt) {
  std::vector<Band> bands;
  Band current;
  current.first = used.row1;
  current.last = used.row1;
  double height = 0.0;
  for (int row = used.row1; row <= used.row2; ++row) {
    const double h = ws.RowHeight(row);
    if (height > 0.0 && height + h > limit_pt) {
      bands.push_back(current);
      current.first = row;
      height = 0.0;
    }
    height += h;
    current.last = row;
  }
  bands.push_back(current);
  return bands;
}

const CellStyle& StyleFor(const Workbook& book, const Cell* cell) {
  static const CellStyle kDefault{};
  if (cell == nullptr || cell->style_id < 0) return kDefault;
  return book.StyleAt(cell->style_id);
}

// 前 col 列的左边界（工作表坐标，点）；与阅读端 SheetMap::ColLeft 同一套换算，
// 这样图表锚点解析出来的矩形和布局用的是同一把尺子。
double ColLeftSum(const Worksheet& ws, int col, double char_width_pt) {
  double x = 0.0;
  for (int c = 0; c < col && c < 16384; ++c) x += ws.ColumnWidth(c) * char_width_pt;
  return x;
}

// 前 row 行的上边界（工作表坐标，点）。
double RowTopSum(const Worksheet& ws, int row) {
  double y = 0.0;
  for (int r = 0; r < row && r < 1048576; ++r) y += ws.RowHeight(r);
  return y;
}

// 合并区域包含 (row, col) 时返回它；否则返回空。
const Range* MergeAt(const Worksheet& ws, int row, int col) {
  for (const Range& merge : ws.merges()) {
    if (row >= merge.row1 && row <= merge.row2 && col >= merge.col1 && col <= merge.col2) {
      return &merge;
    }
  }
  return nullptr;
}

std::string DisplayOf(const Workbook& book, const Worksheet& ws, int row, int col) {
  const Cell* cell = ws.Find(row, col);
  if (cell == nullptr) return std::string();
  const CellStyle& style = StyleFor(book, cell);
  return FormatValue(ws.GetValue(row, col), style.num_fmt, book.date1904);
}

// ---- 图表渲染（F-205）---------------------------------------------------
//
// 图表矩形给的是工作表坐标（点，原点 A1 左上），DrawPage 只做平移，所以柱高
// 与数值严格成比例：h = (v - axis_lo) / (axis_hi - axis_lo) * plot_h。

const Rgba kSeriesPalette[] = {
    {0x4F, 0x81, 0xBD, 0xFF}, {0xC0, 0x50, 0x4D, 0xFF}, {0x9B, 0xBB, 0x59, 0xFF},
    {0x80, 0x6A, 0xA6, 0xFF}, {0x4B, 0xAC, 0xC6, 0xFF}, {0xF7, 0x96, 0x46, 0xFF},
};
constexpr size_t kSeriesPaletteCount = sizeof(kSeriesPalette) / sizeof(kSeriesPalette[0]);
constexpr double kAxisFontPt = 8.0;

Rgba SeriesColor(const ChartSeries& s, size_t index) {
  if (s.accent >= 0) return RgbOf(s.accent);
  return kSeriesPalette[index % kSeriesPaletteCount];
}

std::string TickLabel(double v) {
  char buf[32];
  if (std::fabs(v) < 1e-9) return "0";
  if (std::fabs(v - std::nearbyint(v)) < 1e-9 && std::fabs(v) < 1e7) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.4g", v);
  }
  return buf;
}

// 把跨度切成 1/2/2.5/5/10 × 10^n 的"好看"步长。
double NiceStep(double span, int target_ticks) {
  if (!(span > 0.0)) return 1.0;
  const double raw = span / static_cast<double>(std::max(1, target_ticks));
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double norm = raw / mag;
  double step = 10.0;
  if (norm <= 1.0) step = 1.0;
  else if (norm <= 2.0) step = 2.0;
  else if (norm <= 2.5) step = 2.5;
  else if (norm <= 5.0) step = 5.0;
  return step * mag;
}

struct AxisScale {
  double lo = 0, hi = 1, step = 1;
};

AxisScale BuildScale(double data_lo, double data_hi, bool force_zero, int target) {
  double lo = force_zero ? std::min(0.0, data_lo) : data_lo;
  double hi = force_zero ? std::max(0.0, data_hi) : data_hi;
  if (!(hi > lo)) {
    lo = std::min(lo, 0.0) - 1.0;
    hi = std::max(hi, 0.0) + 1.0;
  }
  const double step = NiceStep(hi - lo, target);
  lo = std::floor(lo / step) * step;
  hi = std::ceil(hi / step) * step;
  if (!(hi > lo)) hi = lo + step;
  return {lo, hi, step};
}

size_t CategoryCount(const Chart& chart) {
  size_t n = 0;
  for (const ChartSeries& s : chart.series) {
    n = std::max(n, s.categories.empty() ? s.values.size() : s.categories.size());
  }
  return n;
}

std::string CategoryLabel(const Chart& chart, size_t index) {
  for (const ChartSeries& s : chart.series) {
    if (index < s.categories.size()) return s.categories[index];
    if (index < s.values.size()) return TickLabel(s.values[index]);
  }
  return std::string();
}

double SeriesValueAt(const ChartSeries& s, size_t index) {
  return index < s.values.size() ? s.values[index] : 0.0;
}

void AddChartText(LayoutPage* page, const Metrics& metrics, const std::string& family, bool bold,
                  double size, const std::string& text, double x, double baseline, Rgba color) {
  DrawText draw;
  draw.font.family = family;
  draw.font.bold = bold;
  draw.size_pt = size;
  draw.color = color;
  draw.x = x;
  draw.y = baseline;
  draw.text_utf8 = text;
  (void)metrics;
  page->ops.push_back(std::move(draw));
}

void AddCenteredText(LayoutPage* page, const Metrics& metrics, const std::string& family, bool bold,
                     double size, const std::string& text, double cx, double baseline,
                     Rgba color) {
  const FontKey key{family, bold, false};
  const double w = metrics.Measure(key, size, text);
  AddChartText(page, metrics, family, bold, size, text, cx - w / 2.0, baseline, color);
}

// 圆点标记：八段折线逼近圆，半径单位为点。
void AddMarker(LayoutPage* page, double cx, double cy, double r, Rgba color) {
  DrawPath path;
  path.has_fill = true;
  path.fill = color;
  for (int i = 0; i <= 8; ++i) {
    const double a = 2.0 * 3.14159265358979 * static_cast<double>(i) / 8.0;
    PathVerb verb;
    verb.kind = i == 0 ? PathVerbKind::MoveTo : PathVerbKind::LineTo;
    verb.x1 = cx + r * std::cos(a);
    verb.y1 = cy + r * std::sin(a);
    path.verbs.push_back(verb);
  }
  page->ops.push_back(std::move(path));
}

// 饼图/圆环的一个扇区，用折线逼近弧。
void AddSlice(LayoutPage* page, double cx, double cy, double r, double a0, double a1,
              Rgba color) {
  const int segments = std::max(2, static_cast<int>(std::fabs(a1 - a0) / (3.14159265358979 / 12.0)));
  DrawPath path;
  path.has_fill = true;
  path.fill = color;
  PathVerb move;
  move.kind = PathVerbKind::MoveTo;
  move.x1 = cx;
  move.y1 = cy;
  path.verbs.push_back(move);
  for (int i = 0; i <= segments; ++i) {
    const double a = a0 + (a1 - a0) * static_cast<double>(i) / static_cast<double>(segments);
    PathVerb verb;
    verb.kind = PathVerbKind::LineTo;
    verb.x1 = cx + r * std::cos(a);
    verb.y1 = cy + r * std::sin(a);
    path.verbs.push_back(verb);
  }
  path.verbs.push_back(PathVerb{PathVerbKind::Close, 0, 0, 0, 0});
  page->ops.push_back(std::move(path));
}

void DrawLegend(LayoutPage* page, const Metrics& metrics, const Chart& chart, double x, double y,
                double width, const std::string& family) {
  if (!chart.legend) return;
  double row = y + kAxisFontPt;
  for (size_t i = 0; i < chart.series.size(); ++i) {
    const ChartSeries& s = chart.series[i];
    if (s.name.empty() && i > 0) continue;
    const Rgba color = SeriesColor(s, i);
    page->ops.push_back(MakeRect(x, row - kAxisFontPt + 1.0, 7.0, 7.0, color, true,
                                 Rgba{0xFF, 0xFF, 0xFF, 0xFF}, 0.5));
    std::string name = s.name.empty() ? ("series " + std::to_string(i + 1)) : s.name;
    if (metrics.Measure(FontKey{family, false, false}, kAxisFontPt, name) > width - 12.0) {
      while (!name.empty() &&
             metrics.Measure(FontKey{family, false, false}, kAxisFontPt, name + "...") > width - 12.0) {
        name.pop_back();
      }
      name += "...";
    }
    AddChartText(page, metrics, family, false, kAxisFontPt, name, x + 11.0, row,
                 Rgba{0x40, 0x40, 0x40, 0xFF});
    row += 12.0;
  }
}

void DrawChartCartesian(const Chart& chart, double rx, double ry, double rw, double rh,
                        const Metrics& metrics, LayoutPage* page) {
  const std::string family = "Calibri";
  const Rgba ink{0x40, 0x40, 0x40, 0xFF};
  const Rgba grid{0xE0, 0xE0, 0xE0, 0xFF};
  const double title_h = chart.title.empty() ? 4.0 : 18.0;

  double data_lo = 0.0;
  double data_hi = 0.0;
  if (chart.stacked) {
    // 堆叠柱：每根柱的顶端是该分类各系列之和，纵轴上限必须按"合计"取，
    // 否则只按单个最大值定标，整根柱子会冲出绘图区。
    const size_t n = CategoryCount(chart);
    for (size_t i = 0; i < n; ++i) {
      double total = 0.0;
      for (const ChartSeries& s : chart.series) {
        if (i < s.values.size()) total += s.values[i];
      }
      data_hi = std::max(data_hi, total);
    }
    if (data_hi <= 0.0) data_hi = 1.0;
  } else {
    bool first = true;
    for (const ChartSeries& s : chart.series) {
      for (double v : s.values) {
        if (first) {
          data_lo = data_hi = v;
          first = false;
          continue;
        }
        data_lo = std::min(data_lo, v);
        data_hi = std::max(data_hi, v);
      }
    }
    if (first) {
      data_lo = 0.0;
      data_hi = 1.0;
    }
  }
  const bool force_zero = chart.kind != ChartKind::Line && chart.kind != ChartKind::Scatter;
  const AxisScale scale = BuildScale(data_lo, data_hi, force_zero, 4);

  // 刻度标签宽度决定绘图区左边界。
  double label_w = 0.0;
  for (double v = scale.lo; v <= scale.hi + scale.step * 1e-6; v += scale.step) {
    label_w = std::max(label_w,
                       metrics.Measure(FontKey{family, false, false}, kAxisFontPt, TickLabel(v)));
  }
  const double legend_w = chart.legend ? 74.0 : 0.0;
  const double plot_x0 = rx + 4.0 + label_w + 4.0;
  const double plot_x1 = rx + rw - 4.0 - legend_w;
  const double plot_y0 = ry + title_h;
  const double plot_y1 = ry + rh - kAxisFontPt - 8.0;
  if (plot_x1 - plot_x0 < 24.0 || plot_y1 - plot_y0 < 24.0) return;
  const double plot_w = plot_x1 - plot_x0;
  const double plot_h = plot_y1 - plot_y0;

  auto value_y = [&](double v) {
    const double t = (v - scale.lo) / (scale.hi - scale.lo);
    return plot_y1 - t * plot_h;
  };

  if (!chart.title.empty()) {
    AddCenteredText(page, metrics, family, true, 10.0, chart.title, rx + rw / 2.0,
                    ry + 12.0, ink);
  }

  // 网格线 + 数值轴刻度。
  for (double v = scale.lo; v <= scale.hi + scale.step * 1e-6; v += scale.step) {
    const double yy = std::round(value_y(v) * 100.0) / 100.0;
    page->ops.push_back(MakeLine(plot_x0, yy, plot_x1, yy, grid, 0.4));
    const std::string label = TickLabel(v);
    const double w = metrics.Measure(FontKey{family, false, false}, kAxisFontPt, label);
    AddChartText(page, metrics, family, false, kAxisFontPt, label, plot_x0 - 5.0 - w,
                 yy + kAxisFontPt * 0.35, ink);
  }
  // 绘图框与分类轴基线。
  page->ops.push_back(StrokeRect(plot_x0, plot_y0, plot_w, plot_h, ink, 0.75));

  const size_t cats = CategoryCount(chart);
  if (cats == 0) return;
  const double slot = plot_w / static_cast<double>(cats);
  const double zero_y = value_y(std::min(std::max(0.0, scale.lo), scale.hi));

  // 分类轴标签。
  for (size_t i = 0; i < cats; ++i) {
    const std::string label = CategoryLabel(chart, i);
    if (label.empty()) continue;
    const double cx = plot_x0 + slot * (static_cast<double>(i) + 0.5);
    std::string shown = label;
    if (metrics.Measure(FontKey{family, false, false}, kAxisFontPt, shown) > slot - 1.0) {
      while (!shown.empty() &&
             metrics.Measure(FontKey{family, false, false}, kAxisFontPt, shown + "…") > slot - 1.0) {
        shown.pop_back();
      }
      shown += "…";
      if (shown == "…") continue;
    }
    AddCenteredText(page, metrics, family, false, kAxisFontPt, shown, cx,
                    plot_y1 + kAxisFontPt + 3.0, ink);
  }

  if (chart.kind == ChartKind::Line || chart.kind == ChartKind::Scatter) {
    for (size_t s = 0; s < chart.series.size(); ++s) {
      const ChartSeries& series = chart.series[s];
      const size_t n = std::min(cats, series.values.size());
      if (n < 1) continue;
      const Rgba color = SeriesColor(series, s);
      DrawPath path;
      path.has_fill = false;
      path.has_stroke = true;
      path.stroke = color;
      path.stroke_pt = 1.5;
      for (size_t i = 0; i < n; ++i) {
        PathVerb verb;
        verb.kind = i == 0 ? PathVerbKind::MoveTo : PathVerbKind::LineTo;
        verb.x1 = plot_x0 + slot * (static_cast<double>(i) + 0.5);
        verb.y1 = value_y(series.values[i]);
        path.verbs.push_back(verb);
      }
      page->ops.push_back(std::move(path));
      for (size_t i = 0; i < n; ++i) {
        AddMarker(page, plot_x0 + slot * (static_cast<double>(i) + 0.5), value_y(series.values[i]),
                  2.0, color);
      }
    }
  } else if (chart.kind == ChartKind::Area) {
    for (size_t s = 0; s < chart.series.size(); ++s) {
      const ChartSeries& series = chart.series[s];
      const size_t n = std::min(cats, series.values.size());
      if (n < 1) continue;
      const Rgba color = SeriesColor(series, s);
      DrawPath path;
      path.has_fill = true;
      path.fill = color;
      for (size_t i = 0; i < n; ++i) {
        PathVerb verb;
        verb.kind = i == 0 ? PathVerbKind::MoveTo : PathVerbKind::LineTo;
        verb.x1 = plot_x0 + slot * (static_cast<double>(i) + 0.5);
        verb.y1 = value_y(series.values[i]);
        path.verbs.push_back(verb);
      }
      PathVerb down;
      down.kind = PathVerbKind::LineTo;
      down.x1 = plot_x0 + slot * (static_cast<double>(n - 1) + 0.5);
      down.y1 = zero_y;
      path.verbs.push_back(down);
      down.x1 = plot_x0 + slot * 0.5;
      path.verbs.push_back(down);
      path.verbs.push_back(PathVerb{PathVerbKind::Close, 0, 0, 0, 0});
      page->ops.push_back(std::move(path));
    }
  } else {
    // 柱形/条形：簇状或堆叠。组宽 = 槽宽 × 0.75，柱间留缝。
    const size_t series_count = chart.series.size();
    const double group_w = std::max(2.0, slot * 0.75);
    const double bar_w = std::max(1.0, group_w / static_cast<double>(series_count));
    // 堆叠是"按分类竖向累加"：每个分类有自己独立的累计上沿 stack_top[i]，
    // 系列 0 从 0 起，系列 1 从系列 0 的顶端接续，以此类推。
    std::vector<double> stack_top(cats, 0.0);
    for (size_t s = 0; s < series_count; ++s) {
      const ChartSeries& series = chart.series[s];
      const Rgba color = SeriesColor(series, s);
      const size_t n = std::min(cats, series.values.size());
      for (size_t i = 0; i < n; ++i) {
        const double cx = plot_x0 + slot * (static_cast<double>(i) + 0.5);
        const double bx = chart.stacked ? cx - group_w / 2.0
                                        : cx - group_w / 2.0 + bar_w * static_cast<double>(s);
        const double v = series.values[i];
        const double by = chart.stacked ? value_y(stack_top[i] + v) : value_y(v);
        const double bbottom = chart.stacked ? value_y(stack_top[i]) : zero_y;
        stack_top[i] += v;
        const double top = std::min(by, bbottom);
        const double height = std::fabs(bbottom - by);
        page->ops.push_back(MakeRect(bx, top, bar_w, height, color));
      }
    }
  }

  if (chart.legend && legend_w > 0.0) {
    DrawLegend(page, metrics, chart, rx + rw - legend_w + 4.0, plot_y0, legend_w - 8.0, family);
  }
}

void DrawChartPie(const Chart& chart, double rx, double ry, double rw, double rh,
                  const Metrics& metrics, LayoutPage* page) {
  const std::string family = "Calibri";
  const Rgba ink{0x40, 0x40, 0x40, 0xFF};
  const double title_h = chart.title.empty() ? 4.0 : 18.0;
  const double legend_w = chart.legend ? 74.0 : 0.0;
  const double plot_x0 = rx + 4.0;
  const double plot_x1 = rx + rw - 4.0 - legend_w;
  const double plot_y0 = ry + title_h;
  const double plot_y1 = ry + rh - 4.0;
  if (plot_x1 - plot_x0 < 24.0 || plot_y1 - plot_y0 < 24.0) return;

  const double cx = (plot_x0 + plot_x1) / 2.0;
  const double cy = (plot_y0 + plot_y1) / 2.0;
  const double radius = std::min(plot_x1 - plot_x0, plot_y1 - plot_y0) / 2.0 - 4.0;
  if (radius <= 2.0) return;

  if (!chart.title.empty()) {
    AddCenteredText(page, metrics, family, true, 10.0, chart.title, rx + rw / 2.0, ry + 12.0, ink);
  }

  const ChartSeries& series = chart.series.front();
  double total = 0.0;
  for (double v : series.values) {
    if (v > 0.0) total += v;
  }
  if (total <= 0.0) return;

  const double hole = chart.kind == ChartKind::Doughnut ? 0.55 : 0.0;
  double angle = -3.14159265358979 / 2.0;  // 12 点方向起笔
  for (size_t i = 0; i < series.values.size(); ++i) {
    const double v = series.values[i];
    if (!(v > 0.0)) continue;
    const double sweep = 2.0 * 3.14159265358979 * v / total;
    const Rgba color = SeriesColor(series, i);
    AddSlice(page, cx, cy, radius, angle, angle + sweep, color);
    if (hole > 0.0) {
      AddSlice(page, cx, cy, radius * hole, angle + sweep, angle, Rgba{0xFF, 0xFF, 0xFF, 0xFF});
    }
    const double share = v / total;
    if (share >= 0.05) {
      const double mid = angle + sweep / 2.0;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.0f%%", share * 100.0);
      AddCenteredText(page, metrics, family, true, kAxisFontPt, buf,
                      cx + radius * 0.68 * std::cos(mid), cy + radius * 0.68 * std::sin(mid) + 3.0,
                      Rgba{0xFF, 0xFF, 0xFF, 0xFF});
    }
    angle += sweep;
  }

  if (chart.legend && legend_w > 0.0) {
    // 饼图图例按分类命名；颜色沿用扇区在调色板里的位置。
    Chart named = chart;
    named.series.clear();
    for (size_t i = 0; i < series.values.size(); ++i) {
      ChartSeries one;
      one.name = i < series.categories.size() ? series.categories[i] : TickLabel(series.values[i]);
      one.values.push_back(1.0);
      named.series.push_back(std::move(one));
    }
    DrawLegend(page, metrics, named, rx + rw - legend_w + 4.0, plot_y0, legend_w - 8.0, family);
  }
}

void DrawChartOps(const Chart& chart, double rx, double ry, double rw, double rh,
                  const Metrics& metrics, LayoutPage* page) {
  if (rw <= 4.0 || rh <= 4.0) return;
  switch (chart.kind) {
    case ChartKind::Pie:
    case ChartKind::Doughnut:
      DrawChartPie(chart, rx, ry, rw, rh, metrics, page);
      break;
    case ChartKind::Column:
    case ChartKind::Bar:
    case ChartKind::Line:
    case ChartKind::Scatter:
    case ChartKind::Area:
      DrawChartCartesian(chart, rx, ry, rw, rh, metrics, page);
      break;
    default:
      break;
  }
}

// 按列带/行带把一页画进 page。
// 找到覆盖 (row,col) 的超链接。合并区域的从属格也命中，但只有左上角会画，
// 所以实际只会为左上角产出一次。
const Hyperlink* HyperlinkAt(const Worksheet& ws, int row, int col) {
  for (const Hyperlink& link : ws.hyperlinks()) {
    if (!link.range.valid()) continue;
    if (row < link.range.row1 || row > link.range.row2) continue;
    if (col < link.range.col1 || col > link.range.col2) continue;
    return &link;
  }
  return nullptr;
}

void DrawPage(const Workbook& book, const Worksheet& ws, const Metrics& metrics,
              const SheetLayoutOptions& opts, const std::vector<Band>& col_bands, size_t col_band,
              const std::vector<Band>& row_bands, size_t row_band, int repeat_first,
              int repeat_last, LayoutPage* page,
              const std::set<std::pair<int, int>>* watch, std::vector<CellSpot>* spots,
              size_t page_index) {
  const PrintSetup& print = ws.print();
  const double margin_left = print.margin_left / kTwipsPerPoint;
  const double margin_top = print.margin_top / kTwipsPerPoint;
  const double body_width = page->width_pt - margin_left - print.margin_right / kTwipsPerPoint;
  const double body_height = page->height_pt - margin_top - print.margin_bottom / kTwipsPerPoint;

  const Band& cols = col_bands[col_band];
  const Band& rows = row_bands[row_band];

  // 每页要画的行：标题行 + 本带的行。
  std::vector<int> rows_to_draw;
  if (repeat_first >= 0 && repeat_last >= repeat_first && row_band > 0) {
    for (int r = repeat_first; r <= repeat_last; ++r) rows_to_draw.push_back(r);
  }
  for (int r = rows.first; r <= rows.last; ++r) {
    if (repeat_first >= 0 && row_band > 0 && r >= repeat_first && r <= repeat_last) continue;
    rows_to_draw.push_back(r);
  }

  // 列偏移表
  std::vector<double> col_x;
  col_x.reserve(static_cast<size_t>(cols.last - cols.first + 2));
  double x = margin_left;
  for (int c = cols.first; c <= cols.last; ++c) {
    col_x.push_back(x);
    x += ws.ColumnWidth(c) * opts.char_width_pt;
  }
  col_x.push_back(x);

  double y = margin_top;
  for (int row : rows_to_draw) {
    const double row_height = ws.RowHeight(row);
    if (y + row_height > margin_top + body_height + 0.01 && y > margin_top) break;
    for (int col = cols.first; col <= cols.last; ++col) {
      const int index = col - cols.first;
      const double left = col_x[static_cast<size_t>(index)];
      double width = col_x[static_cast<size_t>(index) + 1] - left;
      double height = row_height;

      const Range* merge = MergeAt(ws, row, col);
      if (merge != nullptr) {
        // 只有合并区的左上角负责绘制，其余单元格跳过。
        if (merge->row1 != row || merge->col1 != col) continue;
        width = 0.0;
        for (int c = merge->col1; c <= merge->col2 && c <= cols.last; ++c) {
          width += ws.ColumnWidth(c) * opts.char_width_pt;
        }
        height = 0.0;
        for (int r = merge->row1; r <= merge->row2; ++r) height += ws.RowHeight(r);
      }

      const Cell* cell = ws.Find(row, col);
      const CellStyle& style = StyleFor(book, cell);
      // 条件格式（F-205）：命中的规则叠加在基础样式之上，覆盖填充/字色/加粗。
      const CfDxf* cf = MatchedConditionalDxf(book, ws, row, col);
      const int fill = (cf != nullptr && cf->fill >= 0) ? cf->fill : style.fill;
      if (fill >= 0) {
        page->ops.push_back(MakeRect(left, y, width, height, RgbOf(fill)));
      }
      if (opts.grid_lines) {
        page->ops.push_back(StrokeRect(left, y, width, height, opts.grid_color, 0.5));
      }
      if (style.border_mask != 0) {
        const Rgba ink{0x40, 0x40, 0x40, 0xFF};
        if ((style.border_mask & 1) != 0) {
          page->ops.push_back(MakeLine(left, y, left + width, y, ink, 0.75));
        }
        if ((style.border_mask & 2) != 0) {
          page->ops.push_back(MakeLine(left + width, y, left + width, y + height, ink, 0.75));
        }
        if ((style.border_mask & 4) != 0) {
          page->ops.push_back(MakeLine(left, y + height, left + width, y + height, ink, 0.75));
        }
        if ((style.border_mask & 8) != 0) {
          page->ops.push_back(MakeLine(left, y, left, y + height, ink, 0.75));
        }
      }

      // 超链接：外部 URI 直接落到链接注释；文档内位置留给 LayoutWorkbook 在
      // 全部页排完之后补命名目标。
      if (!ws.hyperlinks().empty()) {
        if (const Hyperlink* link = HyperlinkAt(ws, row, col)) {
          Link out;
          out.x = left;
          out.y = y;
          out.w_pt = width;
          out.h_pt = height;
          if (!link->target.empty()) {
            out.uri = link->target;
          } else {
            out.dest = link->location;
          }
          if (!out.uri.empty() || !out.dest.empty()) page->links.push_back(std::move(out));
        }
      }
      // 被内部链接引用到的单元格：记下落点，供命名目标使用。
      if (watch != nullptr && !watch->empty()) {
        if (watch->count({row, col}) != 0) {
          CellSpot spot;
          spot.row = row;
          spot.col = col;
          spot.page = page_index;
          spot.x = left;
          spot.y = y;
          spots->push_back(spot);
        }
      }

      const std::string text = DisplayOf(book, ws, row, col);
      if (text.empty()) continue;

      FontKey key;
      key.family = style.font.name.empty() ? opts.default_family : style.font.name;
      key.bold = style.font.bold || (cf != nullptr && cf->bold);
      key.italic = style.font.italic;
      const double size = style.font.size_pt > 0.0 ? style.font.size_pt : opts.default_size_pt;

      const double padding = opts.cell_padding_pt;
      const double available = std::max(0.0, width - padding * 2.0);
      std::string shown = text;
      if (metrics.Measure(key, size, shown) > available) {
        // P0 只做裁剪：按字符二分找到能放下的前缀。
        size_t low = 0;
        size_t high = shown.size();
        while (low < high) {
          const size_t mid = (low + high + 1) / 2;
          if (metrics.Measure(key, size, shown.substr(0, mid)) <= available) {
            low = mid;
          } else {
            high = mid - 1;
          }
        }
        shown = shown.substr(0, low);
        if (shown.empty()) continue;
      }
      const double text_width = metrics.Measure(key, size, shown);

      int halign = style.halign;
      if (halign < 0) {
        // Excel 默认：数字右对齐、文本左对齐、布尔居中。
        const Value value = ws.GetValue(row, col);
        halign = value.is_number() ? 3 : (value.kind == Value::Kind::Bool ? 2 : 1);
      }
      double tx = left + padding;
      if (halign == 2) tx = left + (width - text_width) / 2.0;
      else if (halign == 3) tx = left + width - padding - text_width;
      tx = std::max(tx, left + 0.5);

      const int valign = style.valign < 0 ? 2 : style.valign;  // 默认底部
      double baseline = y + height - 2.0 - size * 0.22;
      if (valign == 0) baseline = y + 2.0 + metrics.Ascent(size);
      else if (valign == 1) baseline = y + height / 2.0 + size * 0.35;

      DrawText draw;
      draw.font = key;
      draw.size_pt = size;
      int font_color = style.font.color;
      if (cf != nullptr && cf->font_color >= 0) font_color = cf->font_color;
      draw.color = font_color >= 0 ? RgbOf(font_color) : Rgba{0, 0, 0, 255};
      draw.x = tx;
      draw.y = baseline;
      draw.text_utf8 = shown;
      page->ops.push_back(std::move(draw));
    }
    y += row_height;
  }

  // 图表：工作表坐标 -> 本页坐标只做平移，锚点矩形随之落在页面上。
  const double sheet_left = ColLeftSum(ws, cols.first, opts.char_width_pt);
  const int first_row = (repeat_first >= 0 && repeat_last >= repeat_first && row_band > 0)
                            ? repeat_first
                            : rows.first;
  double repeat_h = 0.0;
  if (repeat_first >= 0 && repeat_last >= repeat_first && row_band > 0) {
    repeat_h = RowTopSum(ws, repeat_last + 1) - RowTopSum(ws, repeat_first);
  }
  const double sheet_top = RowTopSum(ws, first_row);
  const double page_top = margin_top + repeat_h;
  // 本页在工作表坐标里覆盖的区域：图表必须与它相交才画，避免每页都画一遍。
  const double band_right = ColLeftSum(ws, cols.last + 1, opts.char_width_pt);
  const double band_bottom = RowTopSum(ws, rows.last + 1);
  const Range used = ws.UsedRange();

  for (const Chart& chart : book.charts) {
    if (chart.sheet != ws.name()) continue;
    double cx = chart.x;
    double cy = chart.y;
    double cw = chart.w;
    double ch = chart.h;
    if (!chart.has_anchor) {
      // 没有锚点的图（程序里建的）默认放在数据右下方。
      cx = ColLeftSum(ws, used.col2 + 2, opts.char_width_pt);
      cy = RowTopSum(ws, used.row2 + 2);
      cw = 288.0;
      ch = 144.0;
    }
    const bool visible = cx + cw > sheet_left && cx < band_right && cy + ch > sheet_top - repeat_h &&
                         cy < band_bottom;
    if (!visible) continue;
    const double px = margin_left + (cx - sheet_left);
    const double py = page_top + (cy - sheet_top);
    DrawChartOps(chart, px, py, cw, ch, metrics, page);
  }
}

}  // namespace

LayoutDocument LayoutSheetCore(const Workbook& book, const Worksheet& ws,
                               const SheetLayoutOptions& opts,
                               const std::set<std::pair<int, int>>* watch,
                               std::vector<CellSpot>* spots) {
  LayoutDocument out;
  const PrintSetup& print = ws.print();
  double paper_w = print.paper_width_twips / kTwipsPerPoint;
  double paper_h = print.paper_height_twips / kTwipsPerPoint;
  if (print.landscape) std::swap(paper_w, paper_h);
  if (paper_w < 72.0) paper_w = 595.276;
  if (paper_h < 72.0) paper_h = 841.89;

  Range used = ws.UsedRange();
  if (print.print_area.valid()) {
    used.row1 = std::max(used.row1, print.print_area.row1);
    used.col1 = std::max(used.col1, print.print_area.col1);
    used.row2 = std::min(used.row2, print.print_area.row2);
    used.col2 = std::min(used.col2, print.print_area.col2);
    if (used.row2 < used.row1 || used.col2 < used.col1) used = ws.UsedRange();
  }

  // 图表锚点常常落在数据区之外（例如 E2），把图表矩形折举行列下标并扩进 used，
  // 否则分页时根本不会产出承载图表的那一页。
  for (const Chart& chart : book.charts) {
    if (chart.sheet != ws.name()) continue;
    double cx = chart.x;
    double cy = chart.y;
    double cw = chart.w;
    double ch = chart.h;
    if (!chart.has_anchor) {
      cx = ColLeftSum(ws, used.col2 + 2, opts.char_width_pt);
      cy = RowTopSum(ws, used.row2 + 2);
      cw = 288.0;
      ch = 144.0;
    }
    const double right = cx + cw;
    const double bottom = cy + ch;
    auto col_at = [&](double x_pt) {
      int c = 0;
      while (c < 16384) {
        const double next = ColLeftSum(ws, c + 1, opts.char_width_pt);
        if (x_pt < next) break;
        ++c;
      }
      return c;
    };
    auto row_at = [&](double y_pt) {
      int r = 0;
      while (r < 1048576) {
        if (y_pt < RowTopSum(ws, r + 1)) break;
        ++r;
      }
      return r;
    };
    const int col_lo = col_at(std::max(0.0, cx));
    const int col_hi = col_at(std::max(0.0, right - 0.01));
    const int row_lo = row_at(std::max(0.0, cy));
    const int row_hi = row_at(std::max(0.0, bottom - 0.01));
    used.col1 = std::min(used.col1, col_lo);
    used.col2 = std::max(used.col2, col_hi);
    used.row1 = std::min(used.row1, row_lo);
    used.row2 = std::max(used.row2, row_hi);
  }

  const double body_w = paper_w - (print.margin_left + print.margin_right) / kTwipsPerPoint;
  const double body_h = paper_h - (print.margin_top + print.margin_bottom) / kTwipsPerPoint;
  const std::vector<Band> col_bands = SplitColumns(ws, used, std::max(36.0, body_w),
                                                  opts.char_width_pt);
  const std::vector<Band> row_bands = SplitRows(ws, used, std::max(36.0, body_h));

  const Metrics metrics(opts.fonts);
  int pages = 0;
  for (size_t rb = 0; rb < row_bands.size(); ++rb) {
    for (size_t cb = 0; cb < col_bands.size(); ++cb) {
      if (pages >= opts.max_pages) {
        if (opts.warnings != nullptr) {
          Warning warning;
          warning.code = static_cast<int>(WarningCode::LimitApproaching);
          warning.module = "sheet";
          warning.path = ws.name();
          warning.message = "print pagination stopped at max_pages";
          opts.warnings->Add(warning);
        }
        return out;
      }
      LayoutPage page;
      page.width_pt = paper_w;
      page.height_pt = paper_h;
      DrawPage(book, ws, metrics, opts, col_bands, cb, row_bands, rb, print.repeat_rows_first,
               print.repeat_rows_last, &page, watch, spots, out.pages.size());
      out.pages.push_back(std::move(page));
      ++pages;
    }
  }
  if (out.pages.empty()) {
    LayoutPage page;
    page.width_pt = paper_w;
    page.height_pt = paper_h;
    out.pages.push_back(std::move(page));
  }
  if (std::getenv("DOCCORE_DEBUG_SHEET") != nullptr) {
    std::fprintf(stderr, "[sheet] %s pages=%zu bands=%zux%zu used=%d,%d..%d,%d ops0=%zu charts=%zu\n",
                 ws.name().c_str(), out.pages.size(), col_bands.size(), row_bands.size(),
                 used.row1, used.col1, used.row2, used.col2,
                 out.pages.empty() ? 0 : out.pages[0].ops.size(),
                 std::count_if(book.charts.begin(), book.charts.end(),
                               [&ws](const Chart& c) { return c.sheet == ws.name(); }));
  }
  return out;
}

LayoutDocument LayoutSheet(const Workbook& book, const Worksheet& ws,
                           const SheetLayoutOptions& opts) {
  std::vector<CellSpot> unused;
  return LayoutSheetCore(book, ws, opts, nullptr, &unused);
}

namespace {

// 把 "Sheet2!A1" / "A1" / "Sheet2!$A$1" 拆成 (表名, 行, 列)。表名可为空。
bool ParseLinkLocation(const std::string& location, std::string* sheet_name, int* row, int* col) {
  std::string cell = location;
  sheet_name->clear();
  const size_t bang = location.rfind('!');
  if (bang != std::string::npos) {
    *sheet_name = location.substr(0, bang);
    // 去掉表名两端可能出现的引号（'My Sheet'!A1）。
    if (sheet_name->size() >= 2 && (*sheet_name)[0] == '\'' && sheet_name->back() == '\'') {
      *sheet_name = sheet_name->substr(1, sheet_name->size() - 2);
    }
    // 引号内的 '' 是转义的单引号。
    std::string unquoted;
    for (size_t i = 0; i < sheet_name->size(); ++i) {
      if ((*sheet_name)[i] == '\'' && i + 1 < sheet_name->size() && (*sheet_name)[i + 1] == '\'') {
        unquoted.push_back('\'');
        ++i;
      } else {
        unquoted.push_back((*sheet_name)[i]);
      }
    }
    *sheet_name = unquoted;
    cell = location.substr(bang + 1);
  }
  CellRef ref;
  if (!ParseCellName(cell, &ref)) return false;
  *row = ref.row;
  *col = ref.col;
  return true;
}

}  // namespace

LayoutDocument LayoutWorkbook(const Workbook& book, const SheetLayoutOptions& opts) {
  // 第一遍：收集所有内部跳转的目标单元格，按工作表分组。
  std::vector<std::set<std::pair<int, int>>> watch(static_cast<size_t>(book.sheet_count()));
  for (int i = 0; i < book.sheet_count(); ++i) {
    const Worksheet* ws = book.SheetAt(i);
    if (ws == nullptr) continue;
    for (const Hyperlink& link : ws->hyperlinks()) {
      if (!link.target.empty() || link.location.empty()) continue;
      std::string target_sheet;
      int row = 0;
      int col = 0;
      if (!ParseLinkLocation(link.location, &target_sheet, &row, &col)) continue;
      int index = i;
      if (!target_sheet.empty()) {
        index = -1;
        for (int k = 0; k < book.sheet_count(); ++k) {
          const Worksheet* cand = book.SheetAt(k);
          if (cand != nullptr && cand->name() == target_sheet) {
            index = k;
            break;
          }
        }
      }
      if (index < 0) continue;
      watch[static_cast<size_t>(index)].insert({row, col});
    }
  }

  // 第二遍：排每张表，同时记录被引用单元格的落点。
  LayoutDocument out;
  std::vector<size_t> sheet_offset(static_cast<size_t>(book.sheet_count()), 0);
  std::vector<std::vector<CellSpot>> spots(static_cast<size_t>(book.sheet_count()));
  for (int i = 0; i < book.sheet_count(); ++i) {
    const Worksheet* ws = book.SheetAt(i);
    if (ws == nullptr) continue;
    sheet_offset[static_cast<size_t>(i)] = out.pages.size();
    LayoutDocument one = LayoutSheetCore(book, *ws, opts, &watch[static_cast<size_t>(i)],
                                         &spots[static_cast<size_t>(i)]);
    for (LayoutPage& page : one.pages) out.pages.push_back(std::move(page));
  }
  if (out.pages.empty()) {
    LayoutPage page;
    out.pages.push_back(std::move(page));
  }

  // 第三遍：给文档内跳转补命名目标（名字就是 location 原文，与链接的 /Dest
  // 一致），并给每张表补一条大纲。
  for (int i = 0; i < book.sheet_count(); ++i) {
    const Worksheet* ws = book.SheetAt(i);
    if (ws == nullptr) continue;
    for (const Hyperlink& link : ws->hyperlinks()) {
      if (!link.target.empty() || link.location.empty()) continue;
      std::string target_sheet;
      int row = 0;
      int col = 0;
      if (!ParseLinkLocation(link.location, &target_sheet, &row, &col)) continue;
      int index = i;
      if (!target_sheet.empty()) {
        index = -1;
        for (int k = 0; k < book.sheet_count(); ++k) {
          const Worksheet* cand = book.SheetAt(k);
          if (cand != nullptr && cand->name() == target_sheet) {
            index = k;
            break;
          }
        }
      }
      if (index < 0) continue;
      for (const CellSpot& spot : spots[static_cast<size_t>(index)]) {
        if (spot.row != row || spot.col != col) continue;
        const size_t page = sheet_offset[static_cast<size_t>(index)] + spot.page;
        if (page >= out.pages.size()) break;
        NamedDest dest;
        dest.name = link.location;
        dest.x = spot.x;
        dest.y = spot.y;
        out.pages[page].dests.push_back(std::move(dest));
        break;
      }
    }
    Bookmark bm;
    bm.title = ws->name();
    bm.level = 1;
    bm.page = static_cast<int>(sheet_offset[static_cast<size_t>(i)]);
    if (bm.page >= 0 && static_cast<size_t>(bm.page) < out.pages.size()) {
      out.bookmarks.push_back(std::move(bm));
    }
  }
  return out;
}

void SavePdfToFile(const Workbook& book, const std::string& path,
                   const SheetLayoutOptions& layout_opts, const PdfSaveOptions& pdf_opts) {
  FontRegistry empty_registry;
  const FontRegistry* fonts = layout_opts.fonts != nullptr ? layout_opts.fonts : &empty_registry;
  SheetLayoutOptions opts = layout_opts;
  opts.fonts = fonts;
  const LayoutDocument laid = LayoutWorkbook(book, opts);
  PdfSaveOptions pdf = pdf_opts;
  if (pdf.title.empty()) pdf.title = path;
  const std::vector<uint8_t> bytes = PdfWriter::ToBytes(laid, *fonts, pdf, opts.warnings);
  WriteFileAtomically(path, bytes);
}

}  // namespace sheet
}  // namespace doccore
