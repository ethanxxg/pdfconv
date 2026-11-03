// DocCore - PDF 反向重建实现。
// Design refs: docs/02 §3.5.4，docs/01 F-302 / F-310 / F-311 / F-312
//
// 处理顺序（与设计书子序一一对应，唯一调整是"表格检测先于分块"）：
//
//   ① 几何对象 → 视觉行（BuildVisualLines）
//   ③ 表格检测（DetectGridTables / DetectAlignedTables）——先做，把表内文字
//      从行流里摘走，否则表行会被 ② 合并成一个大段落
//   ② 分块、分栏、阅读顺序（GroupBlocks / AssignColumns / OrderBlocks）
//   ④⑤⑥ 三个方向的 DOM 填充
//
// ② 与 ③ 的先后互换是落地时补的细则：设计书按"分块→分栏→表格检测"写，
// 但块分组会把表格的每一行并进同一块，之后再切表就只能按块边界猜。先检测
// 表格、把落在格子里的视觉行摘掉，再对剩余行分块，结果与设计书一致且更稳。
#include "doccore/pdf_rebuild.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doccore/deflate.h"
#include "doccore/render.h"

namespace doccore {
namespace pdf {

std::string TextBlock::Text() const {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out += lines[i].text;
  }
  return out;
}

namespace {

// ---- 小工具 -------------------------------------------------------------

void Warn(WarningSink* sink, WarningCode code, const std::string& message,
          const std::string& path = std::string()) {
  if (sink == nullptr) return;
  Warning w;
  w.code = static_cast<int>(code);
  w.module = "pdf.rebuild";
  w.path = path;
  w.message = message;
  sink->Add(w);
}

std::string TrimCopy(std::string s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

int Twips(double pt) { return static_cast<int>(std::lround(pt * 20.0)); }

// 数字识别：允许前后空白、正负号、小数点、千分位逗号、货币符号。
// 识别成功返回 true。全角数字不认（PDF 里极少见，误判代价更大）。
bool ParseNumber(const std::string& raw, double* out) {
  const std::string s = TrimCopy(raw);
  if (s.empty()) return false;
  size_t i = 0;
  if (s[i] == '+' || s[i] == '-') ++i;
  if (i >= s.size()) return false;
  bool digits = false, dot = false, comma = false;
  std::string clean;
  if (s[0] == '-') clean.push_back('-');
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      digits = true;
      clean.push_back(c);
    } else if (c == '.' && !dot) {
      dot = true;
      clean.push_back(c);
    } else if (c == ',' && !comma) {
      comma = true;  // 千分位，直接丢掉
    } else if (c == '%' && i + 1 == s.size()) {
      // 百分号：按数值/100 处理，与电子表格语义一致。
      if (!digits) return false;
      *out = std::strtod(clean.c_str(), nullptr) / 100.0;
      return true;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  char* end = nullptr;
  const double v = std::strtod(clean.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

// 1 维聚类（单链接）：输入任意顺序，输出簇中心升序。
std::vector<double> ClusterCenters(std::vector<double> values, double tol) {
  if (values.empty()) return {};
  std::sort(values.begin(), values.end());
  std::vector<double> out;
  double sum = values.front();
  int count = 1;
  for (size_t i = 1; i < values.size(); ++i) {
    if (values[i] - values[i - 1] <= tol) {
      sum += values[i];
      ++count;
    } else {
      out.push_back(sum / static_cast<double>(count));
      sum = values[i];
      count = 1;
    }
  }
  out.push_back(sum / static_cast<double>(count));
  return out;
}

int CellIndex(const std::vector<double>& edges, double v) {
  if (edges.size() < 2) return -1;
  if (v < edges.front() || v > edges.back()) return -1;
  for (size_t i = 0; i + 1 < edges.size(); ++i) {
    if (v >= edges[i] && v <= edges[i + 1]) return static_cast<int>(i);
  }
  return static_cast<int>(edges.size()) - 2;
}

// ---- ① 视觉行 -----------------------------------------------------------

struct LineAccum {
  double baseline_pdf = 0;
  double left = 1e30, right = -1e30;
  double top_pdf = -1e30, bottom_pdf = 1e30;
  double best_size = -1;
  std::string text, family;
  bool bold = false, italic = false;
  int color = 0x000000;
  std::vector<VisualSegment> segments;
  double seg_left = 1e30, seg_right = -1e30;
  std::string seg_text;
  int seg_script = 0;  // 当前段的脚本：0 基线 / 1 上标 / 2 下标
  bool any = false;
};

void FlushSegment(LineAccum* acc) {
  if (acc->seg_right > acc->seg_left) {
    VisualSegment seg;
    seg.left = acc->seg_left;
    seg.right = acc->seg_right;
    seg.text = acc->seg_text;
    seg.vert_align = acc->seg_script;
    acc->segments.push_back(std::move(seg));
  }
  acc->seg_left = 1e30;
  acc->seg_right = -1e30;
  acc->seg_text.clear();
  acc->seg_script = 0;
}

std::vector<VisualLine> BuildVisualLines(const PdfPage& page, const RebuildOptions& opt) {
  const double page_h = page.size.height;
  const auto& runs = page.text_runs();

  std::vector<size_t> order;
  order.reserve(runs.size());
  for (size_t i = 0; i < runs.size(); ++i) order.push_back(i);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (runs[a].y != runs[b].y) return runs[a].y > runs[b].y;  // PDF 空间：y 大在上
    return runs[a].x < runs[b].x;
  });

  // ---- ① 原始视觉行：按严格基线容差把同一基线的 run 聚成一堆 ----
  // 上/下标（Δy 很小）会落进同一堆；分式分子/分母（Δy≈0.7×字号）会各自成堆。
  struct RawLine {
    double baseline = 0;
    double left = 1e30, right = -1e30;
    double top_pdf = -1e30, bottom_pdf = 1e30;
    double best_size = -1;
    std::vector<size_t> idx;  // 指向 order 的 run 序号
  };
  std::vector<RawLine> raw;
  {
    size_t i = 0;
    while (i < order.size()) {
      const double base = runs[order[i]].y;
      RawLine rl;
      rl.baseline = base;
      size_t j = i;
      while (j < order.size() &&
             std::abs(runs[order[j]].y - base) <= opt.line_baseline_tolerance_pt) {
        const PdfTextRun& r = runs[order[j]];
        rl.idx.push_back(order[j]);
        rl.left = std::min(rl.left, r.x);
        rl.right = std::max(rl.right, r.right());
        rl.top_pdf = std::max(rl.top_pdf, r.top());
        rl.bottom_pdf = std::min(rl.bottom_pdf, r.bottom());
        rl.best_size = std::max(rl.best_size, r.size_pt);
        ++j;
      }
      raw.push_back(std::move(rl));
      i = j;
    }
  }

  // ---- ② 把"窄的浮动脚本/分式"堆并回它所属的正文基准行 ----
  // 分式分子/分母只是单字宽、且 x 与基准行高度重叠、y 距基准行约 ≤1×字号；
  // 真正的下一行文字是整行宽（不满足"窄"条件），不会被误并。
  std::vector<bool> consumed(raw.size(), false);
  for (size_t a = 0; a < raw.size(); ++a) {
    if (consumed[a]) continue;
    RawLine& L = raw[a];
    const double wL = L.right - L.left;
    int bestB = -1;
    double bestScore = 0;
    for (size_t b = 0; b < raw.size(); ++b) {
      if (b == a || consumed[b]) continue;
      const RawLine& B = raw[b];
      const double wB = B.right - B.left;
      if (wB <= 0) continue;
      if (wL > 0.6 * wB) continue;  // 只吸收"窄"的浮动脚本/分式，整行宽的文本不吸收
      const double ov = std::min(L.right, B.right) - std::max(L.left, B.left);
      if (ov <= 0.25 * std::min(wL, wB)) continue;          // 需与基准行 x 重叠
      if (std::abs(L.baseline - B.baseline) > B.best_size) continue;  // y 距 ≤ 1×字号
      const double score = ov / std::min(wL, wB);
      if (score > bestScore) { bestScore = score; bestB = static_cast<int>(b); }
    }
    if (bestB >= 0) {
      RawLine& B = raw[bestB];
      for (size_t id : L.idx) {
        const PdfTextRun& r = runs[id];
        B.idx.push_back(id);
        B.left = std::min(B.left, r.x);
        B.right = std::max(B.right, r.right());
        B.top_pdf = std::max(B.top_pdf, r.top());
        B.bottom_pdf = std::min(B.bottom_pdf, r.bottom());
        B.best_size = std::max(B.best_size, r.size_pt);
      }
      consumed[a] = true;
    }
  }

  // ---- ③ 最终化：每行内 run 按 x 从左到右排列（同 x 时 y 大者在前，
  //         即分式分子先于分母），再切段标上/下标 ----
  std::vector<VisualLine> lines;
  for (size_t a = 0; a < raw.size(); ++a) {
    if (consumed[a]) continue;
    const RawLine& rl = raw[a];
    if (rl.idx.empty()) continue;
    std::vector<size_t> ids = rl.idx;
    std::sort(ids.begin(), ids.end(), [&](size_t x, size_t y) {
      if (runs[x].x != runs[y].x) return runs[x].x < runs[y].x;
      return runs[x].y > runs[y].y;  // 同 x：上（分子）在前
    });

    double base_size = 0.0;
    for (size_t id : ids) base_size = std::max(base_size, runs[id].size_pt);
    double base_y = rl.baseline;
    if (base_size > 0) {
      std::map<int, int> yhist;
      for (size_t id : ids) {
        if (runs[id].size_pt >= base_size * 0.9) {
          ++yhist[static_cast<int>(std::lround(runs[id].y / 0.5))];
        }
      }
      int best = 0;
      for (const auto& kv : yhist) {
        if (kv.second > best) { best = kv.second; base_y = kv.first * 0.5; }
      }
    }

    LineAccum acc;
    acc.baseline_pdf = base_y;
    bool pending_space = false;  // 中间夹过来的纯空格跑：下一个字前面要留一个空格
    for (size_t id : ids) {
      const PdfTextRun& run = runs[id];
      // 空跑/纯空格跑不进视觉行：Aspose 一类 producer 会为字距单独发 [( )] TJ，
      // 放进行里只会产出空白块；但它证明这里有个词间空格，记下来。
      if (run.text_utf8.empty() ||
          run.text_utf8.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (!acc.text.empty()) pending_space = true;
        continue;
      }
      // 上/下标判定（相对本行基准基线）：小字号或明显偏移基线的 run 视为上/下标。
      int script = 0;
      if (base_size > 0) {
        const double dev = run.y - base_y;
        if (run.size_pt < base_size * 0.85) {
          script = dev > 0 ? 1 : 2;  // 小字号：按 y 偏移定上下
        } else if (std::abs(dev) > base_size * 0.2) {
          script = dev > 0 ? 1 : 2;  // 同字号但明显抬高/压低（如分式分子分母）
        }
      }
      if (!acc.seg_text.empty() && run.x - acc.right > std::max(6.0, run.size_pt * 0.8)) {
        FlushSegment(&acc);
      }
      // 脚本变化要切段，便于把上/下标单独标出来。
      if (!acc.seg_text.empty() && script != acc.seg_script) {
        FlushSegment(&acc);
      }
      if (!acc.text.empty() &&
          (pending_space || run.x - acc.right > std::max(1.0, run.size_pt * 0.22))) {
        acc.text.push_back(' ');
        if (!acc.seg_text.empty()) acc.seg_text.push_back(' ');
      }
      pending_space = false;
      acc.text += run.text_utf8;
      acc.seg_text += run.text_utf8;
      acc.seg_script = script;
      acc.left = std::min(acc.left, run.x);
      acc.right = std::max(acc.right, run.right());
      acc.seg_left = std::min(acc.seg_left, run.x);
      acc.seg_right = std::max(acc.seg_right, run.right());
      acc.top_pdf = std::max(acc.top_pdf, run.top());
      acc.bottom_pdf = std::min(acc.bottom_pdf, run.bottom());
      if (run.size_pt > acc.best_size) {
        acc.best_size = run.size_pt;
        acc.family = run.family;
        acc.bold = run.bold;
        acc.italic = run.italic;
        acc.color = run.color;
      }
      acc.any = true;
    }
    FlushSegment(&acc);
    if (acc.any) {
      VisualLine line;
      line.baseline = page_h - acc.baseline_pdf;
      line.left = acc.left;
      line.right = acc.right;
      line.top = page_h - acc.top_pdf;
      line.bottom = page_h - acc.bottom_pdf;
      line.size_pt = acc.best_size > 0 ? acc.best_size : 12.0;
      line.text = acc.text;
      line.family = acc.family;
      line.bold = acc.bold;
      line.italic = acc.italic;
      line.color = acc.color;
      line.segments = std::move(acc.segments);
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

// ---- ③ 表格检测：框线优先 ----------------------------------------------

struct Seg1D {
  double lo = 0, hi = 0;  // 沿轴的两端（升序）
  double pos = 0;         // 固定坐标
};

// 一行水平线的覆盖率：成员线段并集占 [lo, hi] 的比例。
double Coverage(std::vector<std::pair<double, double>> spans, double lo, double hi) {
  if (hi <= lo) return 0.0;
  std::sort(spans.begin(), spans.end());
  double covered = 0.0;
  double cur_lo = spans.front().first;
  double cur_hi = spans.front().second;
  for (size_t i = 1; i < spans.size(); ++i) {
    if (spans[i].first <= cur_hi + 0.5) {
      cur_hi = std::max(cur_hi, spans[i].second);
    } else {
      covered += cur_hi - cur_lo;
      cur_lo = spans[i].first;
      cur_hi = spans[i].second;
    }
  }
  covered += cur_hi - cur_lo;
  return covered / (hi - lo);
}

void DetectGridTables(const PdfPage& page, const RebuildOptions& opt,
                      const std::vector<VisualLine>& lines, std::vector<DetectedTable>* out,
                      std::vector<bool>* consumed) {
  const double page_h = page.size.height;
  std::vector<Seg1D> hs, vs;
  for (const PdfLine& l : page.lines()) {
    if (l.horizontal()) {
      const double len = l.max_x() - l.min_x();
      if (len < opt.min_line_length_pt) continue;
      hs.push_back({l.min_x(), l.max_x(), page_h - (l.y1 + l.y2) * 0.5});
    } else if (l.vertical()) {
      const double len = l.max_y() - l.min_y();
      if (len < opt.min_line_length_pt) continue;
      vs.push_back({page_h - l.max_y(), page_h - l.min_y(), (l.x1 + l.x2) * 0.5});
    }
  }
  if (hs.size() < 2 || vs.size() < 2) return;

  // 按水平线的横向跨度分组：不同宽度叠放的两张表会落在不同组里。
  std::sort(hs.begin(), hs.end(), [](const Seg1D& a, const Seg1D& b) {
    if (a.lo != b.lo) return a.lo < b.lo;
    return a.hi < b.hi;
  });
  std::vector<std::vector<Seg1D>> groups;
  for (const Seg1D& h : hs) {
    bool placed = false;
    for (auto& g : groups) {
      if (std::abs(g.front().lo - h.lo) <= 6.0 && std::abs(g.front().hi - h.hi) <= 6.0) {
        g.push_back(h);
        placed = true;
        break;
      }
    }
    if (!placed) groups.push_back({h});
  }

  // 表格的每个格子常常单独成框（re + S），于是同一条框线被切成"每格一段"，
  // 按横向跨度分组会把一张表拆成几根竖条。这里把"共享至少两条横线、且横向
  // 首尾相接"的分组合并回去。
  bool merged = true;
  while (merged) {
    merged = false;
    for (size_t a = 0; a < groups.size() && !merged; ++a) {
      for (size_t b = a + 1; b < groups.size() && !merged; ++b) {
        std::vector<double> ya, yb;
        for (const Seg1D& h : groups[a]) ya.push_back(h.pos);
        for (const Seg1D& h : groups[b]) yb.push_back(h.pos);
        ya = ClusterCenters(ya, opt.grid_edge_tolerance_pt);
        yb = ClusterCenters(yb, opt.grid_edge_tolerance_pt);
        int shared = 0;
        for (double y : ya) {
          for (double z : yb) {
            if (std::abs(y - z) <= opt.grid_edge_tolerance_pt) {
              ++shared;
              break;
            }
          }
        }
        if (shared < 2) continue;
        double a_lo = 1e30, a_hi = -1e30, b_lo = 1e30, b_hi = -1e30;
        for (const Seg1D& h : groups[a]) {
          a_lo = std::min(a_lo, h.lo);
          a_hi = std::max(a_hi, h.hi);
        }
        for (const Seg1D& h : groups[b]) {
          b_lo = std::min(b_lo, h.lo);
          b_hi = std::max(b_hi, h.hi);
        }
        const double gap = std::max(a_lo, b_lo) - std::min(a_hi, b_hi);
        if (gap > 8.0) continue;
        groups[a].insert(groups[a].end(), groups[b].begin(), groups[b].end());
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(b));
        merged = true;
      }
    }
  }

  for (const auto& group : groups) {
    std::vector<double> ys;
    ys.reserve(group.size());
    for (const Seg1D& h : group) ys.push_back(h.pos);
    ys = ClusterCenters(ys, opt.grid_edge_tolerance_pt);
    if (ys.size() < 2) continue;

    double x_lo = 1e30, x_hi = -1e30;
    for (const Seg1D& h : group) {
      x_lo = std::min(x_lo, h.lo);
      x_hi = std::max(x_hi, h.hi);
    }
    if (x_hi - x_lo < 4.0) continue;

    const double y_lo = ys.front();
    const double y_hi = ys.back();

    // 每一条水平簇的覆盖率：太短的是下划线/装饰线，不是表格框线。
    bool solid = true;
    for (double y : ys) {
      std::vector<std::pair<double, double>> spans;
      for (const Seg1D& h : group) {
        if (std::abs(h.pos - y) <= opt.grid_edge_tolerance_pt * 1.5) spans.emplace_back(h.lo, h.hi);
      }
      if (spans.empty() || Coverage(spans, x_lo, x_hi) < 0.5) {
        solid = false;
        break;
      }
    }
    if (!solid) continue;

    // 竖边候选：落在表格横向范围内、且与表格纵向范围有交叠。
    std::vector<Seg1D> vcand;
    for (const Seg1D& v : vs) {
      if (v.pos < x_lo - opt.grid_edge_tolerance_pt || v.pos > x_hi + opt.grid_edge_tolerance_pt) {
        continue;
      }
      if (v.hi < y_lo - opt.grid_edge_tolerance_pt || v.lo > y_hi + opt.grid_edge_tolerance_pt) {
        continue;
      }
      vcand.push_back(v);
    }
    // 按 x 聚成列边，再逐个判定是不是真的列分隔线：
    //   ① 纵向覆盖：每格单独画一段的列边拼起来应接近整表高；
    //   ② 必须"够到"表格的上边或下边：斑马纹/单元格底色按格画矩形，竖边只
    //      跨被填的那几行，够不到表格的上下框线；有框线的表，列边从第一行
    //      顶画到最后一行底，一定够到其中一端。
    // 两条都过才算列边——只按覆盖率会放过"覆盖了 63% 行高的斑马纹右边"。
    std::sort(vcand.begin(), vcand.end(),
              [](const Seg1D& a, const Seg1D& b) { return a.pos < b.pos; });
    std::vector<double> xs;
    const double table_h = y_hi - y_lo;
    const double reach = std::max(10.0, opt.grid_edge_tolerance_pt * 4.0);
    size_t vi = 0;
    while (vi < vcand.size()) {
      size_t vj = vi;
      while (vj + 1 < vcand.size() &&
             vcand[vj + 1].pos - vcand[vj].pos <= opt.grid_edge_tolerance_pt) {
        ++vj;
      }
      std::vector<std::pair<double, double>> spans;
      bool touches_end = false;
      for (size_t k = vi; k <= vj; ++k) {
        spans.emplace_back(vcand[k].lo, vcand[k].hi);
        if (std::abs(vcand[k].lo - y_lo) <= reach || std::abs(vcand[k].hi - y_hi) <= reach) {
          touches_end = true;
        }
      }
      const double coverage = Coverage(spans, y_lo, y_hi);
      if (table_h <= 0.0 || (touches_end && coverage >= 0.5)) {
        xs.push_back((vcand[vi].pos + vcand[vj].pos) * 0.5);
      }
      vi = vj + 1;
    }
    xs = ClusterCenters(xs, opt.grid_edge_tolerance_pt);
    if (xs.size() < static_cast<size_t>(opt.min_table_cols + 1)) continue;
    if (ys.size() < static_cast<size_t>(opt.min_table_rows + 1)) continue;

    DetectedTable table;
    table.left = xs.front();
    table.right = xs.back();
    table.top = ys.front();
    table.bottom = ys.back();
    table.col_edges = xs;
    table.row_edges = ys;
    table.bordered = true;
    table.cells.assign(ys.size() - 1, std::vector<std::string>(xs.size() - 1));
    table.covered.assign(ys.size() - 1, std::vector<bool>(xs.size() - 1, false));

    // 表内视觉行归格。按"段"而不是按"行"归：一行跨三列时整行中心只会落进
    // 中间那格，必须逐段用段中心定列。
    for (size_t li = 0; li < lines.size(); ++li) {
      if ((*consumed)[li]) continue;
      const VisualLine& line = lines[li];
      const double cy = (line.top + line.bottom) * 0.5;
      if (cy < table.top - 1.0 || cy > table.bottom + 1.0) continue;
      const int r = CellIndex(table.row_edges, cy);
      if (r < 0) continue;
      bool touched = false;
      for (const VisualSegment& seg : line.segments) {
        const double cx = (seg.left + seg.right) * 0.5;
        if (cx < table.left - 1.0 || cx > table.right + 1.0) continue;
        const int c = CellIndex(table.col_edges, cx);
        if (c < 0) continue;
        std::string& cell = table.cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
        const std::string text = TrimCopy(seg.text);
        if (text.empty()) continue;
        if (!cell.empty()) cell.push_back('\n');
        cell += text;
        touched = true;
      }
      if (touched) (*consumed)[li] = true;
    }
    out->push_back(std::move(table));
  }
}

// ---- ③ 表格检测：无框线时按列对齐 --------------------------------------

void DetectAlignedTables(const RebuildOptions& opt, const std::vector<VisualLine>& lines,
                         std::vector<DetectedTable>* out, std::vector<bool>* consumed) {
  if (!opt.detect_aligned_tables) return;

  std::vector<size_t> order;
  for (size_t i = 0; i < lines.size(); ++i) {
    if ((*consumed)[i]) continue;
    const size_t segs = lines[i].segments.size();
    if (segs < 2 || segs < static_cast<size_t>(std::max(2, opt.aligned_table_min_cols))) continue;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return lines[a].top < lines[b].top; });

  size_t i = 0;
  while (i < order.size()) {
    std::vector<size_t> run{order[i]};
    const size_t seg_count = lines[order[i]].segments.size();
    size_t j = i + 1;
    while (j < order.size()) {
      const VisualLine& prev = lines[run.back()];
      const VisualLine& next = lines[order[j]];
      if (next.segments.size() != seg_count) break;
      if (next.top - prev.bottom > std::max(6.0, prev.size_pt * 1.5)) break;
      bool aligned = true;
      for (size_t s = 0; s < seg_count; ++s) {
        if (std::abs(next.segments[s].left - lines[run.front()].segments[s].left) > 6.0) {
          aligned = false;
          break;
        }
      }
      if (!aligned) break;
      run.push_back(order[j]);
      ++j;
    }
    if (static_cast<int>(run.size()) >= opt.aligned_table_min_rows) {
      DetectedTable table;
      table.bordered = false;
      table.col_edges.resize(seg_count + 1);
      for (size_t s = 0; s < seg_count; ++s) {
        double lo = 1e30, hi = -1e30;
        for (size_t idx : run) {
          lo = std::min(lo, lines[idx].segments[s].left);
          hi = std::max(hi, lines[idx].segments[s].right);
        }
        if (s == 0) {
          table.col_edges[0] = lo;
        } else {
          table.col_edges[s] = (table.col_edges[s] + lo) * 0.5;
        }
        table.col_edges[s + 1] = hi;
      }
      table.left = table.col_edges.front();
      table.right = table.col_edges.back();
      table.top = lines[run.front()].top;
      table.bottom = lines[run.back()].bottom;
      table.row_edges.push_back(table.top);
      for (size_t k = 0; k + 1 < run.size(); ++k) {
        table.row_edges.push_back((lines[run[k]].bottom + lines[run[k + 1]].top) * 0.5);
      }
      table.row_edges.push_back(table.bottom);
      table.cells.assign(run.size(), std::vector<std::string>(seg_count));
      table.covered.assign(run.size(), std::vector<bool>(seg_count, false));
      for (size_t r = 0; r < run.size(); ++r) {
        for (size_t s = 0; s < seg_count; ++s) {
          table.cells[r][s] = TrimCopy(lines[run[r]].segments[s].text);
        }
        (*consumed)[run[r]] = true;
      }
      out->push_back(std::move(table));
    }
    i = j;
  }
}

// ---- ② 分块 -------------------------------------------------------------

// 一条视觉行可能横跨两栏（左栏一行 + 右栏一行恰好同基线）。分块前必须按
// 栏间隙（> column_gap_pt）把它切成几段，否则两栏会被并成一条长行、再被
// 并进同一个块，阅读顺序就丢了。
std::vector<VisualLine> SplitByGutter(const VisualLine& line, double gutter) {
  std::vector<VisualLine> out;
  if (line.segments.size() <= 1) {
    out.push_back(line);
    return out;
  }
  size_t start = 0;
  for (size_t i = 1; i <= line.segments.size(); ++i) {
    const bool last = i == line.segments.size();
    const bool split = last || line.segments[i].left - line.segments[i - 1].right > gutter;
    if (!split) continue;
    VisualLine piece = line;
    piece.segments.assign(line.segments.begin() + static_cast<std::ptrdiff_t>(start),
                          line.segments.begin() + static_cast<std::ptrdiff_t>(i));
    piece.left = piece.segments.front().left;
    piece.right = piece.segments.back().right;
    piece.text.clear();
    for (size_t k = 0; k < piece.segments.size(); ++k) {
      if (k > 0) piece.text.push_back(' ');
      piece.text += piece.segments[k].text;
    }
    out.push_back(std::move(piece));
    start = i;
  }
  return out;
}

std::vector<TextBlock> GroupBlocks(const std::vector<VisualLine>& lines,
                                   const RebuildOptions& opt) {
  std::vector<VisualLine> fragments;
  for (const VisualLine& line : lines) {
    for (VisualLine& piece : SplitByGutter(line, opt.column_gap_pt)) {
      fragments.push_back(std::move(piece));
    }
  }

  std::vector<size_t> order(fragments.size());
  for (size_t i = 0; i < fragments.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (fragments[a].top != fragments[b].top) return fragments[a].top < fragments[b].top;
    return fragments[a].left < fragments[b].left;
  });

  std::vector<TextBlock> blocks;
  for (size_t idx : order) {
    const VisualLine& line = fragments[idx];
    bool appended = false;
    // 只往最近若干块里找：块按 top 递增产生，倒序扫描能最早命中。
    for (size_t back = 0; back < blocks.size() && back < 8; ++back) {
      TextBlock& block = blocks[blocks.size() - 1 - back];
      const double anchor = block.lines.front().left;
      if (std::abs(line.left - anchor) > opt.block_left_tolerance_pt) continue;
      const double gap = line.top - block.bottom;
      if (gap < -std::max(1.0, line.height()) ) continue;
      if (gap > opt.block_gap_factor * std::max(line.height(), block.size_pt)) continue;
      block.lines.push_back(line);
      block.bottom = std::max(block.bottom, line.bottom);
      block.right = std::max(block.right, line.right);
      block.left = std::min(block.left, line.left);
      if (line.size_pt > block.size_pt) {
        block.size_pt = line.size_pt;
        block.family = line.family;
        block.bold = line.bold;
        block.color = line.color;
      }
      appended = true;
      break;
    }
    if (appended) continue;
    TextBlock block;
    block.left = line.left;
    block.right = line.right;
    block.top = line.top;
    block.bottom = line.bottom;
    block.size_pt = line.size_pt;
    block.family = line.family;
    block.bold = line.bold;
    block.color = line.color;
    block.lines.push_back(line);
    blocks.push_back(std::move(block));
  }
  return blocks;
}

// ---- ② 分栏与阅读顺序 ---------------------------------------------------

void AssignColumns(std::vector<TextBlock>* blocks, double page_width, const RebuildOptions& opt) {
  if (blocks->empty()) return;
  double content_left = 1e30, content_right = -1e30;
  for (const TextBlock& b : *blocks) {
    content_left = std::min(content_left, b.left);
    content_right = std::max(content_right, b.right);
  }
  const double content_width = std::max(1.0, content_right - content_left);
  const double span_limit = content_width * 0.6;

  // 跨栏块（标题横幅）不参与分栏扫描，否则它会把两栏粘成一栏。
  std::vector<size_t> narrow;
  for (size_t i = 0; i < blocks->size(); ++i) {
    if ((*blocks)[i].right - (*blocks)[i].left <= span_limit) narrow.push_back(i);
    else (*blocks)[i].column = -1;
  }
  if (narrow.empty()) return;
  std::sort(narrow.begin(), narrow.end(),
            [&](size_t a, size_t b) { return (*blocks)[a].left < (*blocks)[b].left; });

  std::vector<std::pair<double, double>> bands;  // [left, right]
  for (size_t idx : narrow) {
    const TextBlock& b = (*blocks)[idx];
    if (!bands.empty() && b.left - bands.back().second <= opt.column_gap_pt) {
      bands.back().second = std::max(bands.back().second, b.right);
    } else {
      bands.emplace_back(b.left, b.right);
    }
  }
  if (bands.size() <= 1) {
    for (size_t idx : narrow) (*blocks)[idx].column = 0;
    return;
  }
  for (size_t idx : narrow) {
    const TextBlock& b = (*blocks)[idx];
    int best = 0;
    double best_gap = 1e30;
    for (size_t c = 0; c < bands.size(); ++c) {
      const double gap = std::abs(b.left - bands[c].first);
      if (gap < best_gap) {
        best_gap = gap;
        best = static_cast<int>(c);
      }
    }
    (*blocks)[idx].column = best;
  }
  (void)page_width;
}

void OrderBlocks(std::vector<TextBlock>* blocks) {
  // 跨栏块把阅读顺序切成若干段：段内按（栏, top），跨栏块本身按 top 插在段间。
  std::vector<size_t> spanning, normal;
  for (size_t i = 0; i < blocks->size(); ++i) {
    if ((*blocks)[i].column < 0) spanning.push_back(i);
    else normal.push_back(i);
  }
  std::sort(spanning.begin(), spanning.end(),
            [&](size_t a, size_t b) { return (*blocks)[a].top < (*blocks)[b].top; });

  std::vector<size_t> order;
  size_t cursor = 0;
  for (size_t sp : spanning) {
    const double cut = (*blocks)[sp].top;
    std::vector<size_t> head;
    std::vector<size_t> rest;
    for (size_t k = cursor; k < normal.size(); ++k) {
      if ((*blocks)[normal[k]].top < cut) head.push_back(normal[k]);
      else rest.push_back(normal[k]);
    }
    normal = rest;
    cursor = 0;
    std::sort(head.begin(), head.end(), [&](size_t a, size_t b) {
      if ((*blocks)[a].column != (*blocks)[b].column) {
        return (*blocks)[a].column < (*blocks)[b].column;
      }
      return (*blocks)[a].top < (*blocks)[b].top;
    });
    order.insert(order.end(), head.begin(), head.end());
    order.push_back(sp);
  }
  std::sort(normal.begin(), normal.end(), [&](size_t a, size_t b) {
    if ((*blocks)[a].column != (*blocks)[b].column) return (*blocks)[a].column < (*blocks)[b].column;
    return (*blocks)[a].top < (*blocks)[b].top;
  });
  order.insert(order.end(), normal.begin(), normal.end());

  std::vector<TextBlock> sorted;
  sorted.reserve(order.size());
  for (size_t idx : order) sorted.push_back(std::move((*blocks)[idx]));
  *blocks = std::move(sorted);
}

// ---- 正文字号与主字体 ---------------------------------------------------

double BodySize(const std::vector<VisualLine>& lines) {
  // 按字符数加权取众数，避免一两个大标题把正文基准带偏。
  std::map<int, int> weight;  // 字号（十分之一磅）-> 字符数
  for (const VisualLine& line : lines) {
    weight[static_cast<int>(std::lround(line.size_pt * 10.0))] +=
        static_cast<int>(line.text.size());
  }
  int best = 120, best_weight = -1;
  for (const auto& kv : weight) {
    if (kv.second > best_weight) {
      best_weight = kv.second;
      best = kv.first;
    }
  }
  return best / 10.0;
}

std::string DominantFamily(const std::vector<VisualLine>& lines) {
  std::map<std::string, int> weight;
  for (const VisualLine& line : lines) {
    if (!line.family.empty()) weight[line.family] += static_cast<int>(line.text.size());
  }
  std::string best;
  int best_weight = -1;
  for (const auto& kv : weight) {
    if (kv.second > best_weight) {
      best_weight = kv.second;
      best = kv.first;
    }
  }
  return best;
}

// ---- 图像像素 -----------------------------------------------------------

const char* NameOf(const ObjectPtr& obj, const char* key) {
  if (obj == nullptr) return nullptr;
  const ObjectPtr* found = obj->Find(key);
  if (found == nullptr || *found == nullptr || (*found)->kind != ObjKind::Name) return nullptr;
  return (*found)->text.c_str();
}

double NumberOf(const ObjectPtr& obj, const char* key, double fallback) {
  if (obj == nullptr) return fallback;
  const ObjectPtr* found = obj->Find(key);
  if (found == nullptr || *found == nullptr || !(*found)->is_number()) return fallback;
  return (*found)->number();
}

bool HasFilter(const ObjectPtr& obj, const char* want) {
  const ObjectPtr* filter = obj->Find("Filter");
  if (filter == nullptr || *filter == nullptr) return false;
  if ((*filter)->kind == ObjKind::Name) return (*filter)->text == want;
  if ((*filter)->kind == ObjKind::Array) {
    for (const ObjectPtr& item : (*filter)->array) {
      if (item != nullptr && item->kind == ObjKind::Name && item->text == want) return true;
    }
  }
  return false;
}

// 前向声明：图像解码路径会用到，定义在下方。
std::vector<uint8_t> InflateOrRaw(const ObjectPtr& stream, bool flate, bool* ok);

// 直接（非索引）色彩空间的分量数：DeviceRGB/Gray/CMYK 与 ICCBased(/N 或 /Alternate)。
// 返回 0 表示不支持。cs 可以已经是解析后的对象（含直接名、间接名、[/ICCBased ...]）。
int DirectColorSpaceComponents(const PdfFile& file, const ObjectPtr& cs) {
  const ObjectPtr r = file.Resolve(cs);
  if (r == nullptr) return 0;
  if (r->kind == ObjKind::Name) {
    if (r->text == "DeviceRGB") return 3;
    if (r->text == "DeviceGray") return 1;
    if (r->text == "DeviceCMYK") return 4;
    return 0;
  }
  if (r->kind == ObjKind::Array && !r->array.empty()) {
    const ObjectPtr head = file.Resolve(r->array.front());
    if (head != nullptr && head->is_name("ICCBased") && r->array.size() >= 2) {
      const ObjectPtr stream = file.Resolve(r->array[1]);
      if (stream != nullptr) {
        // /N 是分量数；缺失时退回 /Alternate 的名字。
        const int n = static_cast<int>(NumberOf(stream, "N", 0));
        if (n > 0) return n;
        const char* alternate = NameOf(stream, "Alternate");
        if (alternate != nullptr) {
          const std::string name(alternate);
          if (name == "DeviceRGB") return 3;
          if (name == "DeviceGray") return 1;
          if (name == "DeviceCMYK") return 4;
        }
      }
    }
  }
  return 0;
}

// 解析 [/Indexed <base> <hival> <table>]：成功则填 base_components 与查表表
// （已展开为 (hival+1)*base_components 字节，每项 base_components 字节）。
// 查表流本身可能带 FlateDecode（WGE 论文里多数如此）。
bool TryResolveIndexed(const PdfFile& file, const ObjectPtr& cs, int* base_components,
                       std::vector<uint8_t>* table, WarningSink* warnings) {
  if (cs == nullptr || cs->kind != ObjKind::Array || cs->array.size() < 4) return false;
  const ObjectPtr head = file.Resolve(cs->array[0]);
  if (head == nullptr || !head->is_name("Indexed")) return false;
  const int base_n = DirectColorSpaceComponents(file, cs->array[1]);
  if (base_n <= 0) return false;
  const ObjectPtr hival_obj = file.Resolve(cs->array[2]);
  if (hival_obj == nullptr || !hival_obj->is_number()) return false;
  const int hival = static_cast<int>(hival_obj->number());
  const ObjectPtr table_obj = file.Resolve(cs->array[3]);
  if (table_obj == nullptr || table_obj->kind != ObjKind::Stream) return false;
  bool ok = false;
  std::vector<uint8_t> raw = InflateOrRaw(table_obj, HasFilter(table_obj, "FlateDecode"), &ok);
  if (!ok) {
    Warn(warnings, WarningCode::ImageSkipped, "pdf rebuild: indexed lookup table did not inflate");
    return false;
  }
  const size_t expected = static_cast<size_t>(hival + 1) * static_cast<size_t>(base_n);
  if (raw.size() < expected) return false;
  *base_components = base_n;
  table->assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(expected));
  return true;
}

std::vector<uint8_t> InflateOrRaw(const ObjectPtr& stream, bool flate, bool* ok) {
  *ok = true;
  if (stream == nullptr) {
    *ok = false;
    return {};
  }
  if (!flate) return stream->raw;
  try {
    return ZlibDecompress(stream->raw.data(), stream->raw.size(), 256u * 1024u * 1024u);
  } catch (const Error&) {
    *ok = false;
    return {};
  }
}

// PNG/TIFF 预测器（PDF DecodeParms Predictor>=10）反算：把每行带一个 filter byte 的
// 数据还原成裸像素字节。colors/bpc/columns 与 PDF DecodeParms 一致，覆盖过滤类型 0..4。
// 与 pdf_read.cpp 里的 ApplyPngPredictor 同源，这里独立实现以免跨文件链接匿名命名空间。
std::vector<uint8_t> UndoPngPredictor(const std::vector<uint8_t>& in, int colors, int bpc,
                                      int columns) {
  if (columns <= 0 || colors <= 0) return in;
  const size_t bpp = static_cast<size_t>(std::max(1, (colors * bpc + 7) / 8));
  const size_t row_bytes = (static_cast<size_t>(columns) * static_cast<size_t>(colors) *
                                static_cast<size_t>(bpc) + 7) / 8;
  if (row_bytes == 0) return in;
  std::vector<uint8_t> out;
  out.reserve(in.size());
  std::vector<uint8_t> prev(row_bytes, 0);
  std::vector<uint8_t> cur(row_bytes, 0);
  size_t pos = 0;
  while (pos + 1 + row_bytes <= in.size()) {
    const int filter = in[pos++];
    std::memcpy(cur.data(), in.data() + pos, row_bytes);
    pos += row_bytes;
    switch (filter) {
      case 1:
        for (size_t i = bpp; i < row_bytes; ++i) cur[i] = uint8_t(cur[i] + cur[i - bpp]);
        break;
      case 2:
        for (size_t i = 0; i < row_bytes; ++i) cur[i] = uint8_t(cur[i] + prev[i]);
        break;
      case 3:
        for (size_t i = 0; i < row_bytes; ++i) {
          const int left = i >= bpp ? cur[i - bpp] : 0;
          cur[i] = uint8_t(cur[i] + ((left + prev[i]) >> 1));
        }
        break;
      case 4:
        for (size_t i = 0; i < row_bytes; ++i) {
          const int a = i >= bpp ? cur[i - bpp] : 0;
          const int b = prev[i];
          const int c = i >= bpp ? prev[i - bpp] : 0;
          const int p = a + b - c;
          const int pa = std::abs(p - a);
          const int pb = std::abs(p - b);
          const int pc = std::abs(p - c);
          const int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          cur[i] = uint8_t(cur[i] + pred);
        }
        break;
      default:
        break;
    }
    out.insert(out.end(), cur.begin(), cur.end());
    prev = cur;
  }
  return out;
}

// 把按 PDF 行对齐、位打包的样本（bpc<=8）解包成"每像素 components 个连续样本"的扁平数组。
// bpc==8 时字节即是样本，原样返回（行内无字节填充）；bpc<8 时按 MSB-first 跨行独立解包。
std::vector<uint8_t> UnpackSamples(const std::vector<uint8_t>& data, int bpc, int components,
                                   int width, int height) {
  if (bpc <= 0 || components <= 0 || width <= 0 || height <= 0) return {};
  if (bpc == 8) return data;
  const int samples_per_row = width * components;
  const int bits_per_row = samples_per_row * bpc;
  const int row_bytes = (bits_per_row + 7) / 8;
  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(samples_per_row) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    int bitpos = 0;
    for (int s = 0; s < samples_per_row; ++s) {
      int val = 0;
      for (int b = 0; b < bpc; ++b) {
        const size_t byte_index = static_cast<size_t>(y) * static_cast<size_t>(row_bytes) +
                                  static_cast<size_t>(bitpos / 8);
        const int bit_index = 7 - (bitpos % 8);
        const int bit = (data[byte_index] >> bit_index) & 1;
        val = (val << 1) | bit;
        ++bitpos;
      }
      out.push_back(static_cast<uint8_t>(val));
    }
  }
  return out;
}

}  // namespace

// ---- 对外：图像像素 -----------------------------------------------------

namespace {

ExtractedImage ExtractImageObject(const PdfFile& file, const ObjectPtr& xobject,
                                  WarningSink* warnings) {
  ExtractedImage out;
  ObjectPtr obj = file.Resolve(xobject);
  if (obj == nullptr || !obj->is_dict()) return out;
  const char* subtype = NameOf(obj, "Subtype");
  if (subtype == nullptr || std::string(subtype) != "Image") return out;

  const int width = static_cast<int>(NumberOf(obj, "Width", 0));
  const int height = static_cast<int>(NumberOf(obj, "Height", 0));
  if (width <= 0 || height <= 0) return out;

  // JPEG 原样带走：DCTDecode 的流字节就是 .jpg 文件体。
  if (HasFilter(obj, "DCTDecode")) {
    out.mime = "image/jpeg";
    out.bytes = obj->raw;
    out.pixel_width = width;
    out.pixel_height = height;
    return out;
  }

  const bool flate = HasFilter(obj, "FlateDecode");
  const ObjectPtr* filter = obj->Find("Filter");
  const bool other_filter =
      filter != nullptr && *filter != nullptr && !flate && (*filter)->kind != ObjKind::Null;
  if (other_filter) {
    Warn(warnings, WarningCode::ImageSkipped, "pdf rebuild: unsupported image filter");
    return out;
  }

  bool ok = false;
  std::vector<uint8_t> data = InflateOrRaw(obj, flate, &ok);
  if (!ok) {
    Warn(warnings, WarningCode::ImageSkipped, "pdf rebuild: image stream did not inflate");
    return out;
  }

  const int bpc = static_cast<int>(NumberOf(obj, "BitsPerComponent", 8));
  if (bpc <= 0 || bpc > 8) {
    Warn(warnings, WarningCode::ImageSkipped,
         "pdf rebuild: unsupported BitsPerComponent=" + std::to_string(bpc));
    return out;
  }

  // 解析色彩空间：支持直接 Device*/ICCBased，以及 Indexed（基类为 ICCBased/Device*）。
  bool indexed = false;
  int base_components = 0;
  std::vector<uint8_t> table;
  int components = 0;  // 像素数据里每个像素的样本数（Indexed 时为 1，即索引）
  const ObjectPtr* cs_ptr = obj->Find("ColorSpace");
  if (cs_ptr != nullptr) {
    const ObjectPtr cs = file.Resolve(*cs_ptr);
    if (TryResolveIndexed(file, cs, &base_components, &table, warnings)) {
      indexed = true;
      components = 1;
    } else {
      components = DirectColorSpaceComponents(file, cs);
    }
  }
  if (components == 0) {
    Warn(warnings, WarningCode::ImageSkipped, "pdf rebuild: unsupported image colorspace");
    return out;
  }

  // PNG / TIFF 预测器（PDF DecodeParms Predictor>=10）。Indexed 像素数据是索引，
  // 故预测 colors=1；直接色彩空间 colors=分量数。
  const int predictor_colors = indexed ? 1 : components;
  const ObjectPtr* dp = obj->Find("DecodeParms");
  if (dp != nullptr) {
    const ObjectPtr p = file.Resolve(*dp);
    if (p != nullptr && p->is_dict()) {
      auto int_of = [&](const char* key, int fb) {
        const ObjectPtr* v = p->Find(key);
        if (v == nullptr) return fb;
        const ObjectPtr r = file.Resolve(*v);
        return (r != nullptr && r->is_number()) ? static_cast<int>(r->number()) : fb;
      };
      if (int_of("Predictor", 1) >= 10) {
        data = UndoPngPredictor(data, int_of("Colors", predictor_colors),
                                int_of("BitsPerComponent", bpc), int_of("Columns", width));
      }
    }
  }

  // 位解包：把行对齐打包的样本拆成"每像素 components 个连续样本"。
  std::vector<uint8_t> samples = UnpackSamples(data, bpc, components, width, height);
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (samples.size() < pixels * static_cast<size_t>(components)) {
    Warn(warnings, WarningCode::ImageSkipped, "pdf rebuild: image data shorter than declared size");
    return out;
  }

  // 透明度来自 /SMask（灰度图，与主图同尺寸）。
  std::vector<uint8_t> alpha(pixels, 255);
  const ObjectPtr* smask_ptr = obj->Find("SMask");
  if (smask_ptr != nullptr && *smask_ptr != nullptr) {
    ObjectPtr smask = file.Resolve(*smask_ptr);
    if (smask != nullptr && smask->is_dict()) {
      bool smask_ok = false;
      const std::vector<uint8_t> sm =
          InflateOrRaw(smask, HasFilter(smask, "FlateDecode"), &smask_ok);
      if (smask_ok && sm.size() >= pixels) {
        for (size_t i = 0; i < pixels; ++i) alpha[i] = sm[i];
      }
    }
  }

  // 样本 -> RGB：Indexed 先用索引查表展开成基色样本，再按基色的分量数转 RGB。
  const int out_components = indexed ? base_components : components;
  RasterImage raster;
  raster.width = width;
  raster.height = height;
  raster.rgba.resize(pixels * 4);
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* src = samples.data() + i * static_cast<size_t>(components);
    uint8_t comp[4] = {0, 0, 0, 0};
    if (indexed) {
      int idx = static_cast<int>(src[0]);
      const size_t entries = table.size() / static_cast<size_t>(base_components);
      if (idx < 0 || static_cast<size_t>(idx) >= entries) {
        idx = static_cast<int>(entries > 0 ? entries - 1 : 0);
      }
      const uint8_t* t =
          table.data() + static_cast<size_t>(idx) * static_cast<size_t>(base_components);
      for (int c = 0; c < base_components; ++c) comp[c] = t[c];
    } else {
      for (int c = 0; c < components; ++c) comp[c] = src[c];
    }
    uint8_t* dst = raster.rgba.data() + i * 4;
    if (out_components == 1) {
      dst[0] = dst[1] = dst[2] = comp[0];
    } else if (out_components == 3) {
      dst[0] = comp[0];
      dst[1] = comp[1];
      dst[2] = comp[2];
    } else if (out_components == 4) {  // CMYK：无 ICC 时按朴素公式转 RGB
      const double c = comp[0] / 255.0, m = comp[1] / 255.0, y = comp[2] / 255.0,
                   k = comp[3] / 255.0;
      dst[0] = static_cast<uint8_t>(std::lround(255.0 * (1.0 - c) * (1.0 - k)));
      dst[1] = static_cast<uint8_t>(std::lround(255.0 * (1.0 - m) * (1.0 - k)));
      dst[2] = static_cast<uint8_t>(std::lround(255.0 * (1.0 - y) * (1.0 - k)));
    } else {
      dst[0] = dst[1] = dst[2] = 0;
    }
    dst[3] = alpha[i];
  }

  out.mime = "image/png";
  out.bytes = EncodePng(raster);
  out.pixel_width = width;
  out.pixel_height = height;
  return out;
}

}  // namespace

ExtractedImage ExtractImage(const PdfFile& file, const PdfPage& page, std::string_view name,
                            WarningSink* warnings) {
  const ObjectPtr* found = page.FindXObject(name);
  if (found == nullptr || *found == nullptr) return {};
  return ExtractImageObject(file, *found, warnings);
}

// 落位时解析出的对象优先：图像也可能只挂在 Form XObject 的 /Resources 里，
// 这时按名字在页级资源中反而找不到。
ExtractedImage ExtractPlacedImage(const PdfFile& file, const PdfPage& page,
                                  const PdfImagePlacement& placed, WarningSink* warnings) {
  if (placed.xobject != nullptr) return ExtractImageObject(file, placed.xobject, warnings);
  return ExtractImage(file, page, placed.xobject_name, warnings);
}

// ---- 对外：单页建模 -----------------------------------------------------

PageModel Reconstructor::BuildPage(const PdfPage& page, const RebuildOptions& opt,
                                   WarningSink* warnings) {
  PageModel model;
  model.index = page.index;
  model.width = page.size.width;
  model.height = page.size.height;

  const double page_h = model.height;
  for (const PdfImagePlacement& img : page.images()) {
    PdfImagePlacement placed = img;
    placed.y = page_h - (img.y + img.h);  // PDF 左下原点 -> 左上原点
    model.images.push_back(placed);
  }

  const std::vector<VisualLine> lines = BuildVisualLines(page, opt);
  model.has_text = !lines.empty();

  // 扫描页：没有文本行，且有一张覆盖页面 80% 以上的图。
  if (lines.empty() && !model.images.empty()) {
    double best = 0.0;
    for (const PdfImagePlacement& img : model.images) {
      best = std::max(best, img.w * img.h);
    }
    const double area = std::max(1.0, model.width * model.height);
    if (best / area >= opt.scan_area_ratio) {
      model.scanned = true;
      return model;
    }
  }

  std::vector<bool> consumed(lines.size(), false);
  DetectGridTables(page, opt, lines, &model.tables, &consumed);
  DetectAlignedTables(opt, lines, &model.tables, &consumed);

  std::vector<VisualLine> rest;
  rest.reserve(lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!consumed[i]) rest.push_back(lines[i]);
  }

  model.blocks = GroupBlocks(rest, opt);
  AssignColumns(&model.blocks, model.width, opt);
  OrderBlocks(&model.blocks);

  // 无框线表格的识别是启发式的，必须让调用方知道。
  for (const DetectedTable& table : model.tables) {
    if (!table.bordered) {
      Warn(warnings, WarningCode::RebuildHeuristic,
           "pdf rebuild: table on page " + std::to_string(page.index + 1) +
               " detected by column alignment (no ruling lines)");
      break;
    }
  }
  return model;
}

std::vector<PageModel> Reconstructor::BuildPages(const PdfFile& file, const RebuildOptions& opt,
                                                 WarningSink* warnings) {
  std::vector<PageModel> pages;
  pages.reserve(static_cast<size_t>(file.page_count()));
  for (int i = 0; i < file.page_count(); ++i) {
    pages.push_back(BuildPage(file.page(i), opt, warnings));
  }
  return pages;
}

void Reconstructor::RequireTextLayer(const std::vector<PageModel>& pages,
                                     const RebuildOptions& opt, WarningSink* warnings) {
  if (pages.empty()) {
    throw Error(ErrorCode::Unsupported, "pdf rebuild: document has no pages");
  }
  bool any_text = false;
  int scanned = 0;
  for (const PageModel& page : pages) {
    if (page.scanned) {
      ++scanned;
      continue;
    }
    if (page.has_text) any_text = true;
  }
  if (!any_text) {
    Warn(warnings, WarningCode::NoTextLayer,
         "pdf rebuild: no text layer on any page (scanned document)");
    if (!opt.ocr_if_no_text) {
      throw Error(ErrorCode::Unsupported,
                  "pdf rebuild: no text layer; enable OCR to rebuild a scanned PDF");
    }
    throw Error(ErrorCode::Unsupported,
                "pdf rebuild: OCR plugin is not available in this build");
  }
  if (scanned > 0) {
    Warn(warnings, WarningCode::NoTextLayer,
         "pdf rebuild: skipped " + std::to_string(scanned) + " scanned page(s) without text layer");
  }
}

// ---- ④ PDF -> DOCX ------------------------------------------------------

namespace {

text::RunFormat MakeRunFormat(const VisualLine& line) {
  text::RunFormat fmt;
  fmt.size_half = static_cast<int>(std::lround(line.size_pt * 2.0));
  if (line.bold) fmt.bold = text::Tri::On;
  if (line.italic) fmt.italic = text::Tri::On;
  if (line.color >= 0 && line.color != 0x000000) fmt.color = line.color;
  if (!line.family.empty()) {
    fmt.font_ascii = line.family;
    fmt.font_hansi = line.family;
    fmt.font_eastasia = line.family;
  }
  return fmt;
}

text::Block MakeParagraphBlock(const TextBlock& block) {
  text::Block out;
  out.type = text::Block::Type::Paragraph;
  text::Paragraph& para = out.paragraph;
  para.format.style = "Normal";
  for (size_t i = 0; i < block.lines.size(); ++i) {
    if (i > 0) {
      text::InlineItem brk;
      brk.type = text::InlineItem::Type::Break;
      brk.break_kind = 3;  // w:br w:type="textWrapping"
      para.runs.push_back(std::move(brk));
    }
    text::InlineItem item;
    item.type = text::InlineItem::Type::Text;
    if (block.lines[i].segments.empty()) {
      item.text = block.lines[i].text;
      item.format = MakeRunFormat(block.lines[i]);
      para.runs.push_back(std::move(item));
    } else {
      // 逐段输出，上/下标单独成段并标 vert_align，避免它们被当成普通正文挤在一起。
      for (const VisualSegment& seg : block.lines[i].segments) {
        text::InlineItem seg_item;
        seg_item.type = text::InlineItem::Type::Text;
        seg_item.text = seg.text;
        seg_item.format = MakeRunFormat(block.lines[i]);
        if (seg.vert_align != 0) seg_item.format.vert_align = seg.vert_align;
        para.runs.push_back(std::move(seg_item));
      }
    }
  }
  return out;
}

text::Block MakeTableBlock(const DetectedTable& table) {
  text::Block out;
  out.type = text::Block::Type::Table;
  out.table = std::make_shared<text::Table>();
  text::Table& tbl = *out.table;
  // 框线由 TableGrid 表格样式给出；表级 tblBorders 留空（-1）以免写出空元素。
  tbl.style = table.bordered ? "TableGrid" : std::string();
  tbl.borders = -1;
  tbl.layout_fixed = true;
  for (size_t c = 0; c + 1 < table.col_edges.size(); ++c) {
    tbl.grid_twips.push_back(Twips(table.col_edges[c + 1] - table.col_edges[c]));
  }
  for (size_t r = 0; r + 1 < table.row_edges.size(); ++r) {
    text::Row row;
    row.height_twips = Twips(table.row_edges[r + 1] - table.row_edges[r]);
    row.height_rule = 1;  // atLeast
    for (size_t c = 0; c + 1 < table.col_edges.size(); ++c) {
      text::Cell cell;
      cell.format.width_twips = Twips(table.col_edges[c + 1] - table.col_edges[c]);
      text::Block block;
      block.type = text::Block::Type::Paragraph;
      block.paragraph.format.style = "Normal";
      std::string text = r < table.cells.size() && c < table.cells[r].size()
                             ? table.cells[r][c]
                             : std::string();
      text::InlineItem item;
      item.type = text::InlineItem::Type::Text;
      item.text = text;
      item.format.size_half = 22;  // 11 pt
      block.paragraph.runs.push_back(std::move(item));
      cell.blocks.push_back(std::move(block));
      row.cells.push_back(std::move(cell));
    }
    tbl.rows.push_back(std::move(row));
  }
  return out;
}

void AddHeadingStyles(text::Document* doc) {
  // 标题样式基于 Normal，所以 Normal 必须先存在——否则 Word/python-docx 解析
  // 段落样式时会拿到空引用。
  text::Style normal;
  normal.id = "Normal";
  normal.name = "Normal";
  normal.kind = text::Style::Kind::Paragraph;
  normal.quick_format = true;
  normal.priority = 0;
  doc->styles.push_back(std::move(normal));
  for (int level = 1; level <= 3; ++level) {
    text::Style style;
    style.id = "Heading" + std::to_string(level);
    style.name = "heading " + std::to_string(level);
    style.kind = text::Style::Kind::Paragraph;
    style.based_on = "Normal";
    style.next = "Normal";
    style.quick_format = true;
    style.priority = 9;
    style.paragraph.outline_level = level - 1;
    style.paragraph.keep_next = text::Tri::On;
    style.paragraph.before_twips = level == 1 ? 240 : 160;
    style.paragraph.after_twips = 80;
    style.run.size_half = level == 1 ? 40 : (level == 2 ? 32 : 26);
    style.run.bold = text::Tri::On;
    doc->styles.push_back(std::move(style));
  }
  text::Style grid;
  grid.id = "TableGrid";
  grid.name = "Table Grid";
  grid.kind = text::Style::Kind::Table;
  grid.paragraph.border_top = 0x000000;
  grid.paragraph.border_bottom = 0x000000;
  grid.paragraph.border_left = 0x000000;
  grid.paragraph.border_right = 0x000000;
  doc->styles.push_back(std::move(grid));
}

}  // namespace

text::Document Reconstructor::ToDocument(const PdfFile& file, const RebuildOptions& opt,
                                         WarningSink* warnings) {
  const std::vector<PageModel> pages = BuildPages(file, opt, warnings);
  RequireTextLayer(pages, opt, warnings);

  text::Document doc;
  doc.has_doc_defaults = true;

  const PageModel& first = pages.front();
  text::Section section;
  section.page.width_twips = Twips(first.width);
  section.page.height_twips = Twips(first.height);
  section.page.margin_top = 720;
  section.page.margin_bottom = 720;
  section.page.margin_left = 720;
  section.page.margin_right = 720;
  section.column_count = 1;

  std::vector<VisualLine> all_lines;
  for (const PageModel& page : pages) {
    for (const TextBlock& block : page.blocks) {
      for (const VisualLine& line : block.lines) all_lines.push_back(line);
    }
  }
  const double body_size = all_lines.empty() ? 11.0 : BodySize(all_lines);
  const std::string family = all_lines.empty() ? std::string() : DominantFamily(all_lines);
  if (!family.empty()) {
    doc.default_run.font_ascii = family;
    doc.default_run.font_hansi = family;
    doc.default_run.font_eastasia = family;
  }
  doc.default_run.size_half = static_cast<int>(std::lround(body_size * 2.0));
  AddHeadingStyles(&doc);

  int image_seq = 0;
  bool first_page = true;
  for (const PageModel& page : pages) {
    if (page.scanned) continue;

    if (!first_page) {
      text::Block brk;
      brk.type = text::Block::Type::Paragraph;
      text::InlineItem item;
      item.type = text::InlineItem::Type::Break;
      item.break_kind = 1;  // 分页
      brk.paragraph.runs.push_back(std::move(item));
      section.blocks.push_back(std::move(brk));
    }
    first_page = false;

    // 块、表、图按纵向位置合并成一条流，尽量保持阅读顺序。
    struct Item {
      double top;
      int kind;  // 0 块 / 1 表 / 2 图
      size_t index;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < page.blocks.size(); ++i) items.push_back({page.blocks[i].top, 0, i});
    for (size_t i = 0; i < page.tables.size(); ++i) items.push_back({page.tables[i].top, 1, i});
    for (size_t i = 0; i < page.images.size(); ++i) items.push_back({page.images[i].y, 2, i});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
      if (a.top != b.top) return a.top < b.top;
      return a.kind < b.kind;
    });

    for (const Item& item : items) {
      if (item.kind == 0) {
        const TextBlock& block = page.blocks[item.index];
        text::Block out = MakeParagraphBlock(block);
        // 字号明显大于正文且独占一行的块当标题。
        const double ratio = body_size > 0 ? block.size_pt / body_size : 1.0;
        if (block.single_line() && ratio >= opt.heading_scale) {
          out.paragraph.format.style = "Heading1";
          out.paragraph.format.outline_level = 0;
        } else if (block.single_line() && ratio >= 1.15 && block.bold) {
          out.paragraph.format.style = "Heading2";
          out.paragraph.format.outline_level = 1;
        }
        section.blocks.push_back(std::move(out));
      } else if (item.kind == 1) {
        section.blocks.push_back(MakeTableBlock(page.tables[item.index]));
      } else {
        const PdfImagePlacement& placed = page.images[item.index];
        ExtractedImage image =
            ExtractPlacedImage(file, file.page(page.index), placed, warnings);
        if (image.empty()) continue;
        ++image_seq;
        text::UnknownPart part;
        part.uri = "/word/media/rebuild" + std::to_string(image_seq) +
                   (image.mime == "image/jpeg" ? ".jpg" : ".png");
        part.mime = image.mime;
        part.bytes = image.bytes;
        doc.unknown_parts.push_back(part);

        text::Block block;
        block.type = text::Block::Type::Paragraph;
        block.paragraph.format.style = "Normal";
        text::InlineItem run;
        run.type = text::InlineItem::Type::Drawing;
        run.drawing_target = part.uri;
        block.paragraph.runs.push_back(std::move(run));
        section.blocks.push_back(std::move(block));
      }
    }
  }

  if (section.blocks.empty()) {
    throw Error(ErrorCode::Unsupported, "pdf rebuild: nothing to write into the document");
  }
  doc.sections.push_back(std::move(section));
  return doc;
}

// ---- ⑤ PDF -> XLSX ------------------------------------------------------

namespace {

double ColumnChars(double width_pt) {
  // Calibri 11 的 '0' 宽约 7 px @96dpi ≈ 5.25 pt，OOXML 列宽单位是字符数。
  const double chars = width_pt / 5.25;
  return std::min(60.0, std::max(3.0, chars));
}

void FillSheetFromTable(sheet::Worksheet* ws, const DetectedTable& table, int row_offset) {
  for (size_t c = 0; c + 1 < table.col_edges.size(); ++c) {
    ws->SetColumnWidth(static_cast<int>(c), ColumnChars(table.col_edges[c + 1] - table.col_edges[c]));
  }
  for (size_t r = 0; r < table.cells.size(); ++r) {
    const int row = row_offset + static_cast<int>(r);
    ws->SetRowHeight(row, std::max(12.0, table.row_edges[r + 1] - table.row_edges[r]));
    for (size_t c = 0; c < table.cells[r].size(); ++c) {
      const std::string text = TrimCopy(table.cells[r][c]);
      if (text.empty()) continue;
      double number = 0.0;
      if (ParseNumber(text, &number)) {
        ws->SetNumber(row, static_cast<int>(c), number);
      } else {
        ws->SetText(row, static_cast<int>(c), text);
      }
    }
  }
}

// 无网格时用视觉行的水平间隙切列：一行文本变成工作表的一行。
DetectedTable TableFromLines(const std::vector<VisualLine>& lines) {
  DetectedTable table;
  table.bordered = false;
  int cols = 1;
  for (const VisualLine& line : lines) cols = std::max(cols, static_cast<int>(line.segments.size()));
  if (cols < 1) cols = 1;
  table.cells.assign(lines.size(), std::vector<std::string>(static_cast<size_t>(cols)));
  table.covered.assign(lines.size(), std::vector<bool>(static_cast<size_t>(cols), false));
  table.col_edges.assign(static_cast<size_t>(cols) + 1, 0.0);
  for (size_t r = 0; r < lines.size(); ++r) {
    for (size_t c = 0; c < lines[r].segments.size(); ++c) {
      table.cells[r][c] = TrimCopy(lines[r].segments[c].text);
    }
  }
  double left = 1e30, right = -1e30;
  for (const VisualLine& line : lines) {
    left = std::min(left, line.left);
    right = std::max(right, line.right);
  }
  const double step = cols > 0 ? (right - left) / cols : 0.0;
  for (int c = 0; c <= cols; ++c) table.col_edges[static_cast<size_t>(c)] = left + step * c;
  table.row_edges.reserve(lines.size() + 1);
  table.top = lines.front().top;
  table.bottom = lines.back().bottom;
  table.row_edges.push_back(table.top);
  for (size_t r = 0; r + 1 < lines.size(); ++r) {
    table.row_edges.push_back((lines[r].bottom + lines[r + 1].top) * 0.5);
  }
  table.row_edges.push_back(table.bottom);
  return table;
}

bool SameColumnLayout(const sheet::Worksheet& ws, const DetectedTable& table, int used_cols) {
  if (static_cast<int>(table.col_edges.size()) - 1 != used_cols) return false;
  for (int c = 0; c < used_cols; ++c) {
    const double a = ws.ColumnWidth(c);
    const double b = ColumnChars(table.col_edges[static_cast<size_t>(c) + 1] -
                                 table.col_edges[static_cast<size_t>(c)]);
    if (std::abs(a - b) > std::max(2.0, b * 0.15)) return false;
  }
  return true;
}

}  // namespace

sheet::Workbook Reconstructor::ToWorkbook(const PdfFile& file, const RebuildOptions& opt,
                                          WarningSink* warnings) {
  const std::vector<PageModel> pages = BuildPages(file, opt, warnings);
  RequireTextLayer(pages, opt, warnings);

  sheet::Workbook book = sheet::Workbook::Create();
  book.sheets.clear();

  for (const PageModel& page : pages) {
    if (page.scanned) continue;

    // 一页里的表与表外文本都要落到表里：按纵向顺序排成"段"，每段占若干行，
    // 段间空一行。表外文本走"按文本间隙切列"的退化路径。
    struct Chunk {
      double top;
      DetectedTable table;
      bool text = false;
    };
    std::vector<Chunk> chunks;
    std::vector<VisualLine> pending;  // 连续的表外文本合并成一段，避免每块都空一行
    double pending_top = 0;
    auto flush_pending = [&]() {
      if (pending.empty()) return;
      Chunk chunk;
      chunk.top = pending_top;
      chunk.table = TableFromLines(pending);
      chunk.text = true;
      chunks.push_back(std::move(chunk));
      pending.clear();
    };
    for (const DetectedTable& table : page.tables) {
      flush_pending();
      Chunk chunk;
      chunk.top = table.top;
      chunk.table = table;
      chunks.push_back(std::move(chunk));
    }
    for (const TextBlock& block : page.blocks) {
      if (block.empty()) continue;
      if (pending.empty()) pending_top = block.top;
      for (const VisualLine& line : block.lines) pending.push_back(line);
    }
    flush_pending();
    if (chunks.empty()) continue;
    std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) {
      if (a.top != b.top) return a.top < b.top;
      return a.text && !b.text;
    });
    if (page.tables.empty()) {
      Warn(warnings, WarningCode::RebuildHeuristic,
           "pdf rebuild: page " + std::to_string(page.index + 1) +
               " has no table grid; columns placed by text gaps");
    }

    // 连续页列结构一致时追加到上一张表（F-311 业务规则）。
    if (opt.stack_same_columns && book.sheet_count() > 0 &&
        chunks.front().table.bordered) {
      sheet::Worksheet* last = book.SheetAt(book.sheet_count() - 1);
      const sheet::Range used = last->UsedRange();
      const int used_cols = used.valid() ? used.col2 - used.col1 + 1 : 0;
      if (used.valid() && SameColumnLayout(*last, chunks.front().table, used_cols)) {
        int row = used.row2 + 1;
        DetectedTable head = chunks.front().table;
        // 跳过与上一页表头完全相同的重复表头行。
        if (head.rows() > 1) {
          bool same_header = true;
          for (int c = 0; c < used_cols; ++c) {
            const std::string prev = sheet::FormatValue(last->GetValue(used.row1, c));
            const std::string cur = TrimCopy(head.cells[0][static_cast<size_t>(c)]);
            if (prev != cur) {
              same_header = false;
              break;
            }
          }
          if (same_header) {
            head.cells.erase(head.cells.begin());
            head.covered.erase(head.covered.begin());
            head.row_edges.erase(head.row_edges.begin());
          }
        }
        FillSheetFromTable(last, head, row);
        row += head.rows() + 1;
        for (size_t c = 1; c < chunks.size(); ++c) {
          if (chunks[c].table.rows() == 0) continue;
          FillSheetFromTable(last, chunks[c].table, row);
          row += chunks[c].table.rows() + (chunks[c].text ? 0 : 1);
        }
        continue;
      }
    }

    sheet::Worksheet& ws = book.AddSheet("Page" + std::to_string(page.index + 1));
    int row = 0;
    for (const Chunk& chunk : chunks) {
      if (chunk.table.rows() == 0) continue;
      if (!chunk.text && row > 0) ++row;  // 表前留一行空白，避免和上文黏在一起
      FillSheetFromTable(&ws, chunk.table, row);
      row += chunk.table.rows();
    }
  }

  if (book.sheet_count() == 0) {
    throw Error(ErrorCode::Unsupported, "pdf rebuild: no page produced a worksheet");
  }
  book.active_sheet = book.SheetAt(0)->name();
  return book;
}

// ---- ⑥ PDF -> PPTX ------------------------------------------------------

namespace {

deck::Shape MakeTextBox(const TextBlock& block) {
  deck::Shape shape;
  shape.kind = deck::ShapeKind::Text;
  shape.geom.x = deck::PtToEmu(block.left);
  shape.geom.y = deck::PtToEmu(block.top);
  shape.geom.cx = deck::PtToEmu(std::max(1.0, block.right - block.left) + 4.0);
  shape.geom.cy = deck::PtToEmu(std::max(1.0, block.bottom - block.top) + 2.0);
  shape.text.wrap = true;
  shape.text.anchor = 0;
  shape.text.inset_left = 0;
  shape.text.inset_top = 0;
  shape.text.inset_right = 0;
  shape.text.inset_bottom = 0;
  for (const VisualLine& line : block.lines) {
    deck::Paragraph para;
    deck::Run run;
    run.text = line.text;
    run.format.family = line.family;
    run.format.size_pt = line.size_pt;
    run.format.bold = line.bold;
    run.format.italic = line.italic;
    run.format.color = line.color;
    para.runs.push_back(std::move(run));
    shape.text.paragraphs.push_back(std::move(para));
  }
  return shape;
}

deck::Shape MakeTableShape(const DetectedTable& table) {
  deck::Shape shape;
  shape.kind = deck::ShapeKind::Table;
  shape.geom.x = deck::PtToEmu(table.left);
  shape.geom.y = deck::PtToEmu(table.top);
  shape.geom.cx = deck::PtToEmu(table.right - table.left);
  shape.geom.cy = deck::PtToEmu(table.bottom - table.top);
  for (size_t c = 0; c + 1 < table.col_edges.size(); ++c) {
    shape.table.col_widths.push_back(deck::PtToEmu(table.col_edges[c + 1] - table.col_edges[c]));
  }
  for (size_t r = 0; r + 1 < table.row_edges.size(); ++r) {
    shape.table.row_heights.push_back(deck::PtToEmu(table.row_edges[r + 1] - table.row_edges[r]));
  }
  for (size_t r = 0; r < table.cells.size(); ++r) {
    std::vector<deck::TableCell> row;
    for (size_t c = 0; c < table.cells[r].size(); ++c) {
      deck::TableCell cell;
      deck::Paragraph para;
      deck::Run run;
      run.text = table.cells[r][c];
      run.format.size_pt = 14.0;
      para.runs.push_back(std::move(run));
      cell.body.paragraphs.push_back(std::move(para));
      row.push_back(std::move(cell));
    }
    shape.table.rows.push_back(std::move(row));
  }
  return shape;
}

}  // namespace

deck::Presentation Reconstructor::ToPresentation(const PdfFile& file, const RebuildOptions& opt,
                                                 WarningSink* warnings) {
  const std::vector<PageModel> pages = BuildPages(file, opt, warnings);
  RequireTextLayer(pages, opt, warnings);

  deck::Presentation pres = deck::Presentation::Create();
  pres.slide_width = deck::PtToEmu(pages.front().width);
  pres.slide_height = deck::PtToEmu(pages.front().height);
  pres.slides.clear();

  for (const PageModel& page : pages) {
    if (page.scanned) continue;
    deck::Slide& slide = pres.AddSlide();
    slide.set_name("Page " + std::to_string(page.index + 1));

    for (const DetectedTable& table : page.tables) {
      if (table.rows() < opt.min_table_rows || table.cols() < opt.min_table_cols) continue;
      slide.shapes().push_back(MakeTableShape(table));
    }
    for (const TextBlock& block : page.blocks) {
      if (block.empty()) continue;
      slide.shapes().push_back(MakeTextBox(block));
    }
    for (const PdfImagePlacement& placed : page.images) {
      ExtractedImage image =
          ExtractPlacedImage(file, file.page(page.index), placed, warnings);
      if (image.empty()) continue;
      deck::Shape shape;
      shape.kind = deck::ShapeKind::Picture;
      shape.geom.x = deck::PtToEmu(placed.x);
      shape.geom.y = deck::PtToEmu(placed.y);
      shape.geom.cx = deck::PtToEmu(placed.w);
      shape.geom.cy = deck::PtToEmu(placed.h);
      shape.picture.mime = image.mime;
      shape.picture.bytes = std::move(image.bytes);
      shape.picture.pixel_width = image.pixel_width;
      shape.picture.pixel_height = image.pixel_height;
      slide.shapes().push_back(std::move(shape));
    }
  }

  if (pres.slide_count() == 0) {
    throw Error(ErrorCode::Unsupported, "pdf rebuild: no page produced a slide");
  }
  return pres;
}

}  // namespace pdf
}  // namespace doccore
