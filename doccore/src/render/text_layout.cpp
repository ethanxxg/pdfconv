// DocCore - 字处理排版：text::Document -> Layout IR。Design refs: docs/02 §3.2
//
// 顺序：节 -> 块（段落/表格）-> 行 -> 页。行内换行用贪心算法，CJK 允许在
// 任意汉字间断开，拉丁按空格与连字符断开。分页在行级别发生；脚注在引用
// 所在页预留高度，正文区随即收窄，所以脚注不会压到正文。
#include "doccore/text_layout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doccore/image.h"

namespace doccore {
namespace text {
namespace {

// ---- units ---------------------------------------------------------------

constexpr double kPtPerTwip = 1.0 / 20.0;

inline double TwipsToPt(int twips) { return static_cast<double>(twips) * kPtPerTwip; }

// ---- glyph metrics -------------------------------------------------------

bool IsWideCodepoint(char32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

// 字体度量统一入口：注册了字体文件就用真实 advance，否则用固定估算
//（拉丁 0.5 em、CJK 1 em）。两条路径都必须确定性，因为分页测试不加载字体。
class TextMetrics {
 public:
  explicit TextMetrics(const FontRegistry* fonts) : fonts_(fonts) {}

  const TtfFont* Font(const FontKey& key) const {
    return fonts_ != nullptr ? fonts_->Match(key) : nullptr;
  }

  double Advance(const FontKey& key, double size_pt, char32_t cp) const {
    const TtfFont* font = Font(key);
    if (font != nullptr) {
      const uint16_t gid = font->GlyphForCodepoint(cp);
      if (gid != 0 || cp == U' ') {
        const double upem = font->units_per_em() != 0 ? font->units_per_em() : 1000.0;
        return font->AdvanceWidth(gid) / upem * size_pt;
      }
    }
    if (IsWideCodepoint(cp)) return size_pt;
    if (cp == U'\t') return size_pt * 2.0;
    return size_pt * 0.5;
  }

  double Measure(const FontKey& key, double size_pt, std::string_view utf8) const {
    const std::u32string text = Utf8ToU32(utf8);
    double w = 0.0;
    for (char32_t cp : text) w += Advance(key, size_pt, cp);
    return w;
  }

  double Ascent(const FontKey& key, double size_pt) const {
    const TtfFont* font = Font(key);
    const double upem =
        font != nullptr && font->units_per_em() != 0 ? font->units_per_em() : 1000.0;
    const double asc = font != nullptr ? font->ascender() : 800.0;
    return asc / upem * size_pt;
  }

  double Descent(const FontKey& key, double size_pt) const {
    const TtfFont* font = Font(key);
    const double upem =
        font != nullptr && font->units_per_em() != 0 ? font->units_per_em() : 1000.0;
    const double desc = font != nullptr ? font->descender() : -200.0;
    return -desc / upem * size_pt;
  }

  double LineHeight(const FontKey& key, double size_pt) const {
    return Ascent(key, size_pt) + Descent(key, size_pt);
  }

 private:
  const FontRegistry* fonts_;
};

// ---- style cascade (docs/02 §3.3.2) --------------------------------------

void MergeRun(RunFormat& dst, const RunFormat& src) {
  if (src.bold != Tri::Unset) dst.bold = src.bold;
  if (src.italic != Tri::Unset) dst.italic = src.italic;
  if (src.strike != Tri::Unset) dst.strike = src.strike;
  if (src.double_strike != Tri::Unset) dst.double_strike = src.double_strike;
  if (src.all_caps != Tri::Unset) dst.all_caps = src.all_caps;
  if (src.small_caps != Tri::Unset) dst.small_caps = src.small_caps;
  if (src.emboss != Tri::Unset) dst.emboss = src.emboss;
  if (src.imprint != Tri::Unset) dst.imprint = src.imprint;
  if (src.hidden != Tri::Unset) dst.hidden = src.hidden;
  if (src.no_proof != Tri::Unset) dst.no_proof = src.no_proof;
  if (src.size_half >= 0) dst.size_half = src.size_half;
  if (src.color >= 0) dst.color = src.color;
  if (src.highlight >= 0) dst.highlight = src.highlight;
  if (src.shading >= 0) dst.shading = src.shading;
  if (src.vert_align >= 0) dst.vert_align = src.vert_align;
  if (src.spacing_twips != 0) dst.spacing_twips = src.spacing_twips;
  if (src.kerning_half != 0) dst.kerning_half = src.kerning_half;
  if (src.position_half != 0) dst.position_half = src.position_half;
  if (!src.underline.empty()) dst.underline = src.underline;
  if (!src.font_ascii.empty()) dst.font_ascii = src.font_ascii;
  if (!src.font_hansi.empty()) dst.font_hansi = src.font_hansi;
  if (!src.font_eastasia.empty()) dst.font_eastasia = src.font_eastasia;
  if (!src.font_cs.empty()) dst.font_cs = src.font_cs;
  if (!src.lang.empty()) dst.lang = src.lang;
  if (!src.character_style.empty()) dst.character_style = src.character_style;
  if (src.outline_level >= 0) dst.outline_level = src.outline_level;
}

void MergeParagraph(ParagraphFormat& dst, const ParagraphFormat& src) {
  if (src.alignment >= 0) dst.alignment = src.alignment;
  if (src.line_rule >= 0) dst.line_rule = src.line_rule;
  if (src.line_twips >= 0) dst.line_twips = src.line_twips;
  if (src.before_twips >= 0) dst.before_twips = src.before_twips;
  if (src.after_twips >= 0) dst.after_twips = src.after_twips;
  if (src.before_autospacing >= 0) dst.before_autospacing = src.before_autospacing;
  if (src.after_autospacing >= 0) dst.after_autospacing = src.after_autospacing;
  if (src.context_before >= 0) dst.context_before = src.context_before;
  if (src.ind_left >= 0) dst.ind_left = src.ind_left;
  if (src.ind_right >= 0) dst.ind_right = src.ind_right;
  if (src.ind_first_line >= 0) dst.ind_first_line = src.ind_first_line;
  if (src.ind_first_line_chars >= 0) dst.ind_first_line_chars = src.ind_first_line_chars;
  if (src.keep_next != Tri::Unset) dst.keep_next = src.keep_next;
  if (src.keep_lines != Tri::Unset) dst.keep_lines = src.keep_lines;
  if (src.widow_control != Tri::Unset) dst.widow_control = src.widow_control;
  if (src.suppress_line_numbers != Tri::Unset) {
    dst.suppress_line_numbers = src.suppress_line_numbers;
  }
  if (src.page_break_before != Tri::Unset) dst.page_break_before = src.page_break_before;
  if (src.outline_level >= 0) dst.outline_level = src.outline_level;
  if (src.shading >= 0) dst.shading = src.shading;
  if (src.border_top >= 0) dst.border_top = src.border_top;
  if (src.border_bottom >= 0) dst.border_bottom = src.border_bottom;
  if (src.border_left >= 0) dst.border_left = src.border_left;
  if (src.border_right >= 0) dst.border_right = src.border_right;
  if (!src.num_id.empty()) dst.num_id = src.num_id;
  if (src.num_level >= 0) dst.num_level = src.num_level;
  if (src.tabs >= 0) dst.tabs = src.tabs;
  if (src.frame_props >= 0) dst.frame_props = src.frame_props;
  if (src.rtl) dst.rtl = src.rtl;
  if (src.snap_to_grid >= 0) dst.snap_to_grid = src.snap_to_grid;
  if (src.mirror_indents >= 0) dst.mirror_indents = src.mirror_indents;
}

// 样式链自底向上合并；basedOn 成环时按访问集合截断（fixture 的
// CycleA/CycleB 就是用来钉这个行为的）。
void ApplyStyleChain(const Document& doc, const std::string& style_id, ParagraphFormat* para,
                     RunFormat* run) {
  std::vector<const Style*> chain;
  std::set<std::string> seen;
  std::string id = style_id;
  while (!id.empty() && seen.insert(id).second) {
    const Style* s = doc.FindStyle(id);
    if (s == nullptr) break;
    chain.push_back(s);
    id = s->based_on;
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (para != nullptr) MergeParagraph(*para, (*it)->paragraph);
    if (run != nullptr) MergeRun(*run, (*it)->run);
  }
}

// 字符样式只叠加运行属性。
void ApplyCharacterStyle(const Document& doc, const std::string& style_id, RunFormat* run) {
  std::vector<const Style*> chain;
  std::set<std::string> seen;
  std::string id = style_id;
  while (!id.empty() && seen.insert(id).second) {
    const Style* s = doc.FindStyle(id);
    if (s == nullptr) break;
    chain.push_back(s);
    id = s->based_on;
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) MergeRun(*run, (*it)->run);
}

// ---- number formatting ---------------------------------------------------

std::string RomanNumeral(int value, bool upper) {
  if (value <= 0 || value > 3999) return std::to_string(value);
  static const int kValues[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
  static const char* kUpper[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX",
                                 "V", "IV", "I"};
  static const char* kLower[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix",
                                 "v", "iv", "i"};
  std::string out;
  int v = value;
  for (int i = 0; i < 13; ++i) {
    while (v >= kValues[i]) {
      out += upper ? kUpper[i] : kLower[i];
      v -= kValues[i];
    }
  }
  return out;
}

std::string AlphaLabel(int value, bool upper) {
  if (value <= 0) return std::to_string(value);
  std::string out;
  int v = value;
  while (v > 0 && out.size() < 8) {
    const int rem = (v - 1) % 26;
    out.insert(out.begin(), static_cast<char>((upper ? 'A' : 'a') + rem));
    v = (v - 1) / 26;
  }
  return out;
}

std::string FormatNumber(int value, const std::string& fmt) {
  if (fmt.empty() || fmt == "decimal") return std::to_string(value);
  if (fmt == "upperRoman") return RomanNumeral(value, true);
  if (fmt == "lowerRoman") return RomanNumeral(value, false);
  if (fmt == "upperLetter") return AlphaLabel(value, true);
  if (fmt == "lowerLetter") return AlphaLabel(value, false);
  if (fmt == "decimalZero") return value < 10 ? "0" + std::to_string(value) : std::to_string(value);
  if (fmt == "none") return std::string();
  return std::to_string(value);  // bullet 等：符号本身在 lvlText 里
}

// ---- numbering state -----------------------------------------------------

// 每个 numId 一份 9 级计数器；遇到更高级别时把更深层复位（Word 语义）。
class NumberingState {
 public:
  explicit NumberingState(const Document& doc) : doc_(doc) {}

  // 返回该段落的编号文本（已按 lvlText 组装）并推进计数器。
  std::string Next(const std::string& num_id, int level, std::string* suffix) {
    const NumberingInstance* inst = doc_.FindNumbering(num_id);
    if (inst == nullptr) return std::string();
    const AbstractNumbering* abs = doc_.FindAbstract(inst->abstract_id);
    if (abs == nullptr) return std::string();
    const int lvl = std::max(0, std::min(level < 0 ? 0 : level, 8));
    std::vector<int>& counters = counters_[num_id];
    if (counters.empty()) {
      counters.assign(9, 0);
      for (int i = 0; i < 9; ++i) counters[i] = StartOf(*inst, *abs, i) - 1;
    }
    counters[lvl] += 1;
    for (int i = lvl + 1; i < 9; ++i) counters[i] = StartOf(*inst, *abs, i) - 1;

    const NumberingLevel* l =
        lvl < static_cast<int>(abs->levels.size()) ? &abs->levels[lvl] : nullptr;
    if (l == nullptr) return std::string();
    if (suffix != nullptr) *suffix = l->suffix;
    if (l->num_fmt == "bullet") return l->lvl_text;
    return SubstituteLevels(l->lvl_text, counters, *abs);
  }

  const NumberingLevel* Level(const std::string& num_id, int level) const {
    const NumberingInstance* inst = doc_.FindNumbering(num_id);
    if (inst == nullptr) return nullptr;
    const AbstractNumbering* abs = doc_.FindAbstract(inst->abstract_id);
    if (abs == nullptr) return nullptr;
    const int lvl = std::max(0, std::min(level < 0 ? 0 : level, 8));
    return lvl < static_cast<int>(abs->levels.size()) ? &abs->levels[lvl] : nullptr;
  }

 private:
  static int StartOf(const NumberingInstance& inst, const AbstractNumbering& abs, int level) {
    if (level < static_cast<int>(inst.level_start_overrides.size()) &&
        inst.level_start_overrides[static_cast<size_t>(level)] >= 0) {
      return inst.level_start_overrides[static_cast<size_t>(level)];
    }
    if (level < static_cast<int>(abs.levels.size())) return abs.levels[level].start;
    return 1;
  }

  std::string SubstituteLevels(const std::string& pattern, const std::vector<int>& counters,
                               const AbstractNumbering& abs) const {
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
      const char c = pattern[i];
      if (c == '%' && i + 1 < pattern.size() && pattern[i + 1] >= '1' && pattern[i + 1] <= '9') {
        const int lvl = pattern[i + 1] - '1';
        const NumberingLevel* l =
            lvl < static_cast<int>(abs.levels.size()) ? &abs.levels[lvl] : nullptr;
        out += FormatNumber(counters[static_cast<size_t>(lvl)], l != nullptr ? l->num_fmt
                                                                            : "decimal");
        ++i;
        continue;
      }
      out.push_back(c);
    }
    return out;
  }

  const Document& doc_;
  std::map<std::string, std::vector<int>> counters_;
};

// ---- line breaking -------------------------------------------------------

struct InlinePiece {
  FontKey font;
  double size_pt = 11.0;
  Rgba color;
  std::string text;
  bool underline = false;
  double rise = 0.0;  // 上/下标基线偏移（pt）
  int break_kind = 0;  // 0 无 / 1 分页 / 2 换行（w:br）
  std::string link_uri;   // 非空：外部链接
  std::string link_dest;  // 非空：文档内书签跳转
  std::string bookmark;   // 非空：在此位置留下命名目标（零宽）
};

struct Atom {
  FontKey font;
  double size_pt = 11.0;
  Rgba color;
  std::string text;
  bool underline = false;
  double rise = 0.0;
  double width = 0.0;
  bool break_before = false;  // 可以在此原子之前换行
  int break_kind = 0;         // 0 无 / 1 分页 / 2 换行（w:br）
  std::string link_uri;       // 非空：外部链接
  std::string link_dest;      // 非空：文档内书签跳转
  std::string bookmark;       // 非空：在此位置留下命名目标（零宽）
};

struct LineBox {
  std::vector<Atom> atoms;
  double width = 0.0;
  double ascent = 0.0;
  double descent = 0.0;
  double height = 0.0;
  bool page_break_after = false;
  bool page_break_before = false;
};

bool IsSpace(char32_t cp) { return cp == U' ' || cp == U'\t' || cp == 0x00A0; }

void FinalizeLine(LineBox* line, const TextMetrics& metrics) {
  double width = 0.0;
  double ascent = 0.0;
  double descent = 0.0;
  for (const Atom& a : line->atoms) {
    width += a.width;
    ascent = std::max(ascent, metrics.Ascent(a.font, a.size_pt) + a.rise);
    descent = std::max(descent, metrics.Descent(a.font, a.size_pt) - a.rise);
  }
  line->width = width;
  line->ascent = ascent;
  line->descent = descent;
  line->height = ascent + descent;
}

// 把行内片段切成换行原子：CJK 每字一个原子，拉丁按词聚合，空格与连字符
// 之后允许换行。
std::vector<Atom> BuildAtoms(const std::vector<InlinePiece>& pieces, const TextMetrics& metrics) {
  std::vector<Atom> atoms;
  bool pending_break = true;
  auto flush_word = [&](const InlinePiece& piece, const std::string& word) {
    Atom a;
    a.font = piece.font;
    a.size_pt = piece.size_pt;
    a.color = piece.color;
    a.text = word;
    a.underline = piece.underline;
    a.rise = piece.rise;
    a.link_uri = piece.link_uri;
    a.link_dest = piece.link_dest;
    a.bookmark = piece.bookmark;
    a.width = metrics.Measure(a.font, a.size_pt, a.text);
    a.break_before = pending_break;
    atoms.push_back(std::move(a));
    pending_break = false;
  };
  for (const InlinePiece& piece : pieces) {
    if (piece.break_kind != 0) {
      Atom a;
      a.font = piece.font;
      a.size_pt = piece.size_pt;
      a.color = piece.color;
      a.break_before = true;
      a.break_kind = piece.break_kind;
      atoms.push_back(std::move(a));
      pending_break = true;
      continue;
    }
    if (piece.text.empty() && !piece.bookmark.empty()) {
      // 零宽书签锚点：不参与断行，单独成一个原子由 EmitLine 记目标。
      Atom a;
      a.font = piece.font;
      a.size_pt = piece.size_pt;
      a.color = piece.color;
      a.link_uri = piece.link_uri;
      a.link_dest = piece.link_dest;
      a.bookmark = piece.bookmark;
      a.break_before = pending_break;
      atoms.push_back(std::move(a));
      pending_break = false;
      continue;
    }
    const std::u32string text = Utf8ToU32(piece.text);
    std::string word;
    for (char32_t cp : text) {
      if (IsWideCodepoint(cp)) {
        if (!word.empty()) {
          flush_word(piece, word);
          word.clear();
        }
        Atom a;
        a.font = piece.font;
        a.size_pt = piece.size_pt;
        a.color = piece.color;
        a.text = U32ToUtf8(std::u32string(1, cp));
        a.underline = piece.underline;
        a.rise = piece.rise;
        a.link_uri = piece.link_uri;
        a.link_dest = piece.link_dest;
        a.bookmark = piece.bookmark;
        a.width = metrics.Advance(a.font, a.size_pt, cp);
        a.break_before = true;
        atoms.push_back(std::move(a));
        pending_break = true;
        continue;
      }
      const bool soft_break = IsSpace(cp) || cp == U'-' || cp == U'\u2010';
      word += U32ToUtf8(std::u32string(1, cp));
      if (soft_break) {
        flush_word(piece, word);
        word.clear();
        pending_break = true;
      }
    }
    if (!word.empty()) {
      flush_word(piece, word);
      pending_break = false;
    }
  }
  return atoms;
}

// 比可用宽度还长的原子按字符拆开，否则整行会溢出。
void SplitOverlong(std::vector<Atom>* atoms, double avail, const TextMetrics& metrics) {
  if (avail <= 0) return;
  std::vector<Atom> out;
  out.reserve(atoms->size());
  for (const Atom& a : *atoms) {
    if (a.width <= avail || a.text.size() <= 1) {
      out.push_back(a);
      continue;
    }
    const std::u32string text = Utf8ToU32(a.text);
    bool first = true;
    for (char32_t cp : text) {
      Atom piece = a;
      piece.text = U32ToUtf8(std::u32string(1, cp));
      piece.width = metrics.Advance(a.font, a.size_pt, cp);
      piece.break_before = first ? a.break_before : true;
      first = false;
      out.push_back(std::move(piece));
    }
  }
  *atoms = std::move(out);
}

// ---- flow ----------------------------------------------------------------

struct ResolvedParagraph {
  ParagraphFormat format;
  RunFormat base_run;
};

struct Flow {
  double y = 0.0;       // 下一个块的顶部
  double bottom = 0.0;  // 本区域的硬底
  bool paginate = true;
};

struct FootnoteEntry {
  const Note* note = nullptr;
  double height = 0.0;
};

class FlowLayout {
 public:
  FlowLayout(const Document& doc, const LayoutOptions& opts)
      : doc_(doc), opts_(opts), metrics_(opts.fonts), numbering_(doc) {}

  LayoutDocument Run(CancelToken* cancel);

 private:
  // 页
  void StartPage(const Section& s);
  void FinishPage();
  void NewPage();
  double ContentWidth() const { return right_ - left_; }
  double EffectiveBottom() const { return bottom_ - note_height_; }
  bool EnsureSpace(Flow* f, double height);

  // 样式与行内
  ResolvedParagraph Resolve(const Paragraph& p) const;
  RunFormat RunFor(const ResolvedParagraph& rp, const InlineItem& it) const;
  std::vector<InlinePiece> BuildPieces(const ResolvedParagraph& rp, const std::string& label,
                                       const RunFormat& label_run, const Paragraph& p) const;
  void EmitLine(const LineBox& line, double x, double top);
  void EmitRule(double x1, double y1, double x2, double y2, int color, double width_pt);

  // 块
  void PlaceBlocks(const std::vector<Block>& blocks, double x, double width, Flow* f);
  void PlaceParagraph(const Paragraph& p, double x, double width, Flow* f);
  void PlaceTable(const Table& t, double x, double width, Flow* f);
  double PlaceRow(const Row& row, double x, const std::vector<double>& cols, Flow* f, bool draw);
  double MeasureBlocks(const std::vector<Block>& blocks, double width);

  // 脚注与页眉页脚
  void ReserveFootnote(const Note& n);
  void DrawFootnotes();
  void DrawHeaderFooter(bool header);
  // 水印是整页装饰，等所有页都排完再逐页补，与正文流无关。
  void ApplyWatermark();

  void Warn(WarningCode code, const std::string& message) const;
  void CheckCancel(CancelToken* cancel) const;

  const Document& doc_;
  LayoutOptions opts_;
  TextMetrics metrics_;
  NumberingState numbering_;

  LayoutDocument out_;
  size_t page_ = 0;
  const Section* section_ = nullptr;
  double left_ = 0, right_ = 0, top_ = 0, bottom_ = 0;
  bool body_on_page_ = false;
  bool in_table_ = false;
  bool in_notes_ = false;
  std::vector<FootnoteEntry> notes_;
  double note_height_ = 0.0;
  std::vector<DrawOp>* target_ = nullptr;
  std::vector<DrawOp> scratch_;
};

void FlowLayout::Warn(WarningCode code, const std::string& message) const {
  if (opts_.warnings == nullptr) return;
  Warning w;
  w.code = static_cast<int>(code);
  w.module = "layout";
  w.message = message;
  opts_.warnings->Add(w);
}

void FlowLayout::CheckCancel(CancelToken* cancel) const {
  if (cancel != nullptr) cancel->ThrowIfCancelled();
}

void FlowLayout::StartPage(const Section& s) {
  LayoutPage page;
  page.width_pt = TwipsToPt(s.page.width_twips);
  page.height_pt = TwipsToPt(s.page.height_twips);
  out_.pages.push_back(std::move(page));
  page_ = out_.pages.size() - 1;
  if (static_cast<int>(out_.pages.size()) > opts_.max_pages) {
    throw Error(ErrorCode::LimitExceeded, "layout: page limit exceeded");
  }
  section_ = &s;
  left_ = TwipsToPt(s.page.margin_left);
  right_ = TwipsToPt(s.page.width_twips - s.page.margin_right);
  top_ = TwipsToPt(s.page.margin_top);
  bottom_ = TwipsToPt(s.page.height_twips - s.page.margin_bottom);
  body_on_page_ = false;
  notes_.clear();
  note_height_ = 0.0;
  target_ = &out_.pages[page_].ops;
}

void FlowLayout::NewPage() {
  FinishPage();
  StartPage(*section_);
}

bool FlowLayout::EnsureSpace(Flow* f, double height) {
  if (f->y + height <= f->bottom + 1e-6) return true;
  if (!f->paginate) return false;
  NewPage();
  f->y = top_;
  f->bottom = EffectiveBottom();
  return f->y + height <= f->bottom + 1e-6;
}

void FlowLayout::EmitRule(double x1, double y1, double x2, double y2, int color,
                          double width_pt) {
  target_->push_back(MakeLine(x1, y1, x2, y2, Rgba{static_cast<uint8_t>((color >> 16) & 0xFF),
                                                  static_cast<uint8_t>((color >> 8) & 0xFF),
                                                  static_cast<uint8_t>(color & 0xFF), 255},
                              width_pt));
}

bool SameStyle(const Atom& a, const Atom& b) {
  return a.font == b.font && a.size_pt == b.size_pt && a.color.r == b.color.r &&
         a.color.g == b.color.g && a.color.b == b.color.b && a.underline == b.underline &&
         a.rise == b.rise;
}

// 同一样式、同一条链接、且都不是书签锚点的原子才合成一个 DrawText。
bool MergeableRun(const Atom& a, const Atom& b) {
  return SameStyle(a, b) && a.link_uri == b.link_uri && a.link_dest == b.link_dest &&
         a.bookmark.empty() && b.bookmark.empty();
}

void FlowLayout::EmitLine(const LineBox& line, double x, double top) {
  const double baseline = top + line.ascent;
  double pen = x;
  size_t i = 0;
  while (i < line.atoms.size()) {
    const Atom& head = line.atoms[i];
    if (!head.bookmark.empty() && head.text.empty()) {
      // 零宽书签锚点：只记命名目标，不出字形。
      if (page_ < out_.pages.size()) {
        NamedDest d;
        d.name = head.bookmark;
        d.x = pen;
        d.y = baseline - head.rise;
        out_.pages[page_].dests.push_back(std::move(d));
      }
      ++i;
      continue;
    }
    std::string text = head.text;
    double width = head.width;
    size_t j = i + 1;
    // 同一样式且同一条链接的原子合成一个 DrawText，链接区域也按合成后的宽度记。
    while (j < line.atoms.size() && MergeableRun(line.atoms[j], head)) {
      text += line.atoms[j].text;
      width += line.atoms[j].width;
      ++j;
    }
    DrawText op;
    op.font = head.font;
    op.size_pt = head.size_pt;
    op.color = head.color;
    op.x = pen;
    op.y = baseline - head.rise;
    op.text_utf8 = text;
    target_->push_back(std::move(op));
    if (head.underline) {
      target_->push_back(MakeLine(pen, baseline - head.rise + 1.2, pen + width,
                                  baseline - head.rise + 1.2, head.color, 0.6));
    }
    if (!head.link_uri.empty() || !head.link_dest.empty()) {
      Link link;
      link.x = pen;
      link.y = baseline - head.rise - head.size_pt * 0.25;
      link.w_pt = width;
      link.h_pt = head.size_pt * 1.25;
      link.uri = head.link_uri;
      link.dest = head.link_dest;
      if (page_ < out_.pages.size()) out_.pages[page_].links.push_back(std::move(link));
    }
    pen += width;
    i = j;
  }
}

// ---- style resolution ----------------------------------------------------

Rgba ColorOf(int value) {
  if (value < 0) return Rgba{0, 0, 0, 255};
  return Rgba{static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
              static_cast<uint8_t>(value & 0xFF), 255};
}

InlinePiece MakePiece(const RunFormat& f, const std::string& text, const LayoutOptions& opts,
                      const std::string& link_uri, const std::string& link_dest);

InlinePiece MakePiece(const RunFormat& f, const std::string& text, const LayoutOptions& opts) {
  return MakePiece(f, text, opts, std::string(), std::string());
}

InlinePiece MakePiece(const RunFormat& f, const std::string& text, const LayoutOptions& opts,
                      const std::string& link_uri, const std::string& link_dest) {
  InlinePiece p;
  p.font.family = !f.font_ascii.empty()
                      ? f.font_ascii
                      : (!f.font_hansi.empty() ? f.font_hansi : opts.default_family);
  p.font.bold = f.bold == Tri::On;
  p.font.italic = f.italic == Tri::On;
  p.size_pt = f.size_half > 0 ? static_cast<double>(f.size_half) / 2.0 : opts.default_size_pt;
  p.color = ColorOf(f.color);
  p.text = text;
  p.underline = !f.underline.empty() && f.underline != "none";
  if (f.vert_align == 1) {
    p.rise = p.size_pt * 0.33;
  } else if (f.vert_align == 2) {
    p.rise = -p.size_pt * 0.15;
  }
  if (f.position_half != 0) p.rise += static_cast<double>(f.position_half) / 2.0;
  p.link_uri = link_uri;
  p.link_dest = link_dest;
  return p;
}

int OutlineLevelFromStyle(const Document& doc, const std::string& style_id);

ResolvedParagraph FlowLayout::Resolve(const Paragraph& p) const {
  ResolvedParagraph rp;
  if (doc_.has_doc_defaults) {
    rp.format = doc_.default_paragraph;
    rp.base_run = doc_.default_run;
  }
  ApplyStyleChain(doc_, p.format.style, &rp.format, &rp.base_run);
  MergeParagraph(rp.format, p.format);
  if (rp.format.alignment < 0) rp.format.alignment = 0;
  if (rp.format.outline_level < 0 && !p.format.style.empty()) {
    rp.format.outline_level = OutlineLevelFromStyle(doc_, p.format.style);
  }
  return rp;
}

// 从样式名推大纲级别：heading N / 标题 N（内置标题样式不一定写 outlineLvl）。
int OutlineLevelFromStyle(const Document& doc, const std::string& style_id) {
  const Style* s = doc.FindStyle(style_id);
  if (s == nullptr || s->name.empty()) return -1;
  std::string low;
  for (char c : s->name) low += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  // 取名字末尾的数字（"heading 3" / "标题 3" / "Heading3"）。
  int digits = 0;
  int value = 0;
  int place = 1;
  for (size_t i = low.size(); i-- > 0;) {
    const char c = low[i];
    if (c >= '0' && c <= '9') {
      value += (c - '0') * place;
      place *= 10;
      ++digits;
    } else if (digits > 0) {
      break;
    }
  }
  if (digits == 0) return -1;
  if (low.find("head") == std::string::npos && low.find("\xe6\xa0\x87\xe9\xa2\x98") == std::string::npos) {
    return -1;
  }
  if (value < 1 || value > 9) return -1;
  return value - 1;
}

RunFormat FlowLayout::RunFor(const ResolvedParagraph& rp, const InlineItem& it) const {
  RunFormat f = rp.base_run;
  if (!it.format.character_style.empty()) {
    ApplyCharacterStyle(doc_, it.format.character_style, &f);
  }
  MergeRun(f, it.format);
  return f;
}

std::vector<InlinePiece> FlowLayout::BuildPieces(const ResolvedParagraph& rp,
                                                 const std::string& label,
                                                 const RunFormat& label_run,
                                                 const Paragraph& p) const {
  std::vector<InlinePiece> pieces;
  if (!label.empty()) pieces.push_back(MakePiece(label_run, label, opts_));
  for (const InlineItem& it : p.runs) {
    switch (it.type) {
      case InlineItem::Type::Text:
        if (it.revision == 2) break;  // 接受修订视图：已删除内容不排版
        if (!it.text.empty()) {
          // 链接挂在 Text 行内项上：外部 URL 走 uri，w:anchor 走内部书签。
          pieces.push_back(MakePiece(RunFor(rp, it), it.text, opts_, it.hyperlink_target,
                                     it.hyperlink_anchor));
        }
        break;
      case InlineItem::Type::Tab: {
        // 制表位近似：补到下一个 36pt（0.5 英寸）位置。用空格而不是 U+0009，
        // 因为 PDF 字体里没有 tab 字形。
        const RunFormat f = RunFor(rp, it);
        const InlinePiece probe = MakePiece(f, " ", opts_);
        const double space_w = metrics_.Measure(probe.font, probe.size_pt, " ");
        int count = space_w > 0.0 ? static_cast<int>(std::lround(36.0 / space_w)) : 4;
        count = std::max(1, std::min(count, 32));
        pieces.push_back(MakePiece(f, std::string(static_cast<size_t>(count), ' '), opts_));
        break;
      }
      case InlineItem::Type::Break: {
        InlinePiece piece = MakePiece(RunFor(rp, it), std::string(), opts_);
        piece.break_kind = it.break_kind == 1 ? 1 : 2;
        pieces.push_back(std::move(piece));
        break;
      }
      case InlineItem::Type::Symbol: {
        InlinePiece piece = MakePiece(RunFor(rp, it), U32ToUtf8(std::u32string(1, it.symbol)), opts_);
        pieces.push_back(std::move(piece));
        break;
      }
      case InlineItem::Type::FootnoteRef:
      case InlineItem::Type::EndnoteRef: {
        // 上标编号（域与自动编号未求值，这里用 id 兜底）
        RunFormat f = RunFor(rp, it);
        f.vert_align = 1;
        pieces.push_back(MakePiece(f, std::to_string(it.note_id), opts_));
        break;
      }
      case InlineItem::Type::BookmarkStart: {
        // 零宽片段，只为在页面上留一个命名目标（w:hyperlink w:anchor 的落点）。
        InlinePiece piece = MakePiece(RunFor(rp, it), std::string(), opts_);
        piece.bookmark = it.text;
        pieces.push_back(std::move(piece));
        break;
      }
      case InlineItem::Type::Drawing:
        Warn(WarningCode::ImageSkipped, "layout: drawing skipped (no image decoder in this wave)");
        break;
      default:
        break;  // 域标记、书签、批注锚点、分隔线不占版面
    }
  }
  return pieces;
}

// ---- paragraph -----------------------------------------------------------

void FlowLayout::PlaceParagraph(const Paragraph& p, double x, double width, Flow* f) {
  const ResolvedParagraph rp = Resolve(p);
  const ParagraphFormat& pf = rp.format;

  if (!in_table_ && !in_notes_) {
    for (const InlineItem& it : p.runs) {
      if (it.type != InlineItem::Type::FootnoteRef) continue;
      for (const Note& n : doc_.footnotes) {
        if (n.id != it.note_id) continue;
        ReserveFootnote(n);
        break;
      }
    }
    if (f->paginate) f->bottom = EffectiveBottom();
  }

  // 编号标签先求值：级别上的缩进是段落缩进的兜底。
  std::string label;
  RunFormat label_run = rp.base_run;
  const NumberingLevel* lvl = nullptr;
  if (!pf.num_id.empty()) {
    std::string suffix = "tab";
    label = numbering_.Next(pf.num_id, pf.num_level, &suffix);
    lvl = numbering_.Level(pf.num_id, pf.num_level);
    if (lvl != nullptr) {
      if (!lvl->font_ascii.empty()) label_run.font_ascii = lvl->font_ascii;
      if (lvl->color >= 0) label_run.color = lvl->color;
      if (lvl->run_size_half >= 0) label_run.size_half = lvl->run_size_half;
    }
    if (!label.empty()) {
      if (suffix == "space") {
        label += " ";
      } else if (suffix != "nothing") {
        label += "\t";
      }
    }
  }

  int ind_left = pf.ind_left;
  int ind_right = pf.ind_right;
  int ind_first = pf.ind_first_line;
  if (lvl != nullptr) {
    if (ind_left < 0 && lvl->paragraph.ind_left >= 0) ind_left = lvl->paragraph.ind_left;
    if (ind_first == -1 && lvl->paragraph.ind_first_line != -1) {
      ind_first = lvl->paragraph.ind_first_line;
    }
  }
  const double left_indent = ind_left > 0 ? TwipsToPt(ind_left) : 0.0;
  const double right_indent = ind_right > 0 ? TwipsToPt(ind_right) : 0.0;
  const double first_indent = ind_first > 0 || ind_first < 0 ? TwipsToPt(ind_first) : 0.0;
  const double text_x = x + left_indent;
  double avail = width - left_indent - right_indent;
  if (avail < 1.0) avail = 1.0;
  double first_limit = avail - first_indent;
  if (first_limit < avail * 0.25) first_limit = avail;

  std::vector<Atom> atoms = BuildAtoms(BuildPieces(rp, label, label_run, p), metrics_);
  SplitOverlong(&atoms, std::max(avail, first_limit), metrics_);

  std::vector<LineBox> lines;
  {
    LineBox cur;
    double w = 0.0;
    double limit = first_limit;
    bool pending_page_break = false;
    auto push_line = [&](LineBox&& line) {
      if (pending_page_break) {
        line.page_break_before = true;
        pending_page_break = false;
      }
      lines.push_back(std::move(line));
    };
    for (const Atom& a : atoms) {
      if (a.break_kind != 0) {
        if (!cur.atoms.empty()) {
          FinalizeLine(&cur, metrics_);
          cur.page_break_after = (a.break_kind == 1);
          push_line(std::move(cur));
          cur = LineBox();
          w = 0.0;
          limit = avail;
        } else if (a.break_kind == 1) {
          // 行首的 w:br：分页发生在这一行之前
          pending_page_break = true;
        }
        continue;
      }
      if (!cur.atoms.empty() && a.break_before && w + a.width > limit + 1e-6) {
        FinalizeLine(&cur, metrics_);
        push_line(std::move(cur));
        cur = LineBox();
        w = 0.0;
        limit = avail;
      }
      w += a.width;
      cur.atoms.push_back(a);
    }
    if (!cur.atoms.empty()) {
      FinalizeLine(&cur, metrics_);
      push_line(std::move(cur));
    }
  }

  const InlinePiece base = MakePiece(rp.base_run, std::string(), opts_);
  std::vector<double> heights;
  double total = 0.0;
  for (LineBox& l : lines) {
    if (l.atoms.empty()) {
      l.ascent = metrics_.Ascent(base.font, base.size_pt);
      l.descent = metrics_.Descent(base.font, base.size_pt);
      l.height = l.ascent + l.descent;
    }
    double h = l.height;
    if (pf.line_twips >= 0) {
      const double exact = TwipsToPt(pf.line_twips);
      if (pf.line_rule == 2) {
        h = exact;
      } else if (pf.line_rule == 1) {
        h = std::max(h, exact);
      } else {
        h = h * (static_cast<double>(pf.line_twips) / 240.0);
      }
    }
    heights.push_back(h);
    total += h;
  }

  // 分页前：pageBreakBefore / keepLines 都是"整段下移"。
  const bool want_new_page =
      pf.page_break_before == Tri::On || (pf.keep_lines == Tri::On && body_on_page_ &&
                                          f->y + total > f->bottom + 1e-6);
  if (f->paginate && want_new_page && body_on_page_) {
    NewPage();
    f->y = top_;
    f->bottom = EffectiveBottom();
  }

  if (body_on_page_ && pf.before_twips > 0) f->y += TwipsToPt(pf.before_twips);

  bool first = true;
  for (size_t i = 0; i < lines.size(); ++i) {
    const double h = heights[i];
    if (f->paginate && f->y + h > f->bottom + 1e-6) {
      // 孤行：本页已有正文且这是本段第一行时整段下移（widowControl）。
      NewPage();
      f->y = top_;
      f->bottom = EffectiveBottom();
      first = true;
    }
    if (!f->paginate && f->y + h > f->bottom + 1e-6) break;
    LineBox placed = lines[i];
    if (placed.page_break_before && f->paginate && body_on_page_) {
      NewPage();
      f->y = top_;
      f->bottom = EffectiveBottom();
      first = true;
    }
    const double line_w = first ? first_limit : avail;
    double dx = 0.0;
    if (pf.alignment == 1) {
      dx = (line_w - placed.width) / 2.0;
    } else if (pf.alignment == 2) {
      dx = line_w - placed.width;
    }
    EmitLine(placed, text_x + (first ? first_indent : 0.0) + dx, f->y);
    if (i == 0 && pf.outline_level >= 0) {
      // 大纲书签：段首行落位处取页号与 y（段中可能换页，必须此刻取）。
      Bookmark bm;
      bm.level = pf.outline_level + 1;
      bm.page = static_cast<int>(page_);
      bm.y = f->y;
      for (const Atom& a : placed.atoms) bm.title += a.text;
      while (!bm.title.empty() && (bm.title.back() == ' ' || bm.title.back() == '\t')) {
        bm.title.pop_back();
      }
      if (!bm.title.empty()) out_.bookmarks.push_back(std::move(bm));
    }
    f->y += h;
    first = false;
    body_on_page_ = true;
    if (placed.page_break_after) {
      NewPage();
      f->y = top_;
      f->bottom = EffectiveBottom();
      first = true;
    }
  }

  if (pf.border_bottom >= 0) {
    EmitRule(x + left_indent, f->y - 1.0, x + width - right_indent, f->y - 1.0, pf.border_bottom,
             0.75);
  }
  if (pf.after_twips > 0) f->y += TwipsToPt(pf.after_twips);
}

// ---- tables --------------------------------------------------------------

void FlowLayout::PlaceTable(const Table& t, double x, double width, Flow* f) {
  std::vector<double> cols;
  double total = 0.0;
  for (int w : t.grid_twips) {
    const double cw = TwipsToPt(w);
    if (cw <= 0) continue;
    cols.push_back(cw);
    total += cw;
  }
  if (cols.empty()) {
    size_t n = 1;
    for (const Row& r : t.rows) n = std::max(n, r.cells.size());
    for (size_t i = 0; i < n; ++i) cols.push_back(width / static_cast<double>(n));
    total = width;
  }
  if (total > width && total > 0) {
    const double k = width / total;
    for (double& c : cols) c *= k;
  }
  const bool draw_grid = t.borders >= 0 || !t.style.empty();
  for (const Row& row : t.rows) PlaceRow(row, x, cols, f, draw_grid);
}

double FlowLayout::PlaceRow(const Row& row, double x, const std::vector<double>& cols, Flow* f,
                            bool draw) {
  const double padding = 5.4;  // 默认单元格内边距 108 缇
  double row_h = row.height_twips >= 0 ? TwipsToPt(row.height_twips) : 0.0;
  std::vector<double> widths(row.cells.size(), 0.0);
  for (size_t i = 0; i < row.cells.size(); ++i) {
    const Cell& cell = row.cells[i];
    const int span = std::max(1, cell.format.grid_span);
    double cw = 0.0;
    if (span > 1) {
      for (int k = 0; k < span && i + static_cast<size_t>(k) < cols.size(); ++k) {
        cw += cols[i + static_cast<size_t>(k)];
      }
    } else if (i < cols.size()) {
      cw = cols[i];
    }
    widths[i] = cw;
    if (cell.format.vmerge == 1) continue;  // 合并续行不重复排版
    const double avail = std::max(1.0, cw - 2.0 * padding);
    row_h = std::max(row_h, MeasureBlocks(cell.blocks, avail) + 2.0 * padding);
  }
  if (row_h <= 0.0) row_h = 12.0;

  if (f->paginate && f->y + row_h > f->bottom + 1e-6 && body_on_page_) {
    NewPage();
    f->y = top_;
    f->bottom = EffectiveBottom();
  }
  const double row_top = f->y;
  double cx = x;
  for (size_t i = 0; i < row.cells.size(); ++i) {
    const Cell& cell = row.cells[i];
    const double cw = widths[i];
    if (cell.format.vmerge != 1 && !cell.blocks.empty()) {
      Flow inner;
      inner.y = row_top + padding;
      inner.bottom = row_top + row_h;
      inner.paginate = false;
      const bool saved = in_table_;
      in_table_ = true;
      PlaceBlocks(cell.blocks, cx + padding, std::max(1.0, cw - 2.0 * padding), &inner);
      in_table_ = saved;
    }
    if (draw && cw > 0) {
      EmitRule(cx, row_top, cx, row_top + row_h, 0x808080, 0.5);
      EmitRule(cx, row_top, cx + cw, row_top, 0x808080, 0.5);
    }
    cx += cw;
  }
  if (draw) {
    EmitRule(cx, row_top, cx, row_top + row_h, 0x808080, 0.5);
    EmitRule(x, row_top + row_h, cx, row_top + row_h, 0x808080, 0.5);
  }
  f->y = row_top + row_h;
  body_on_page_ = true;
  return row_h;
}

double FlowLayout::MeasureBlocks(const std::vector<Block>& blocks, double width) {
  std::vector<DrawOp>* saved = target_;
  target_ = &scratch_;
  const size_t mark = scratch_.size();
  const bool saved_in_table = in_table_;
  in_table_ = true;  // 单元格内的脚注引用不在本页预留（Word 也不这么做）
  Flow f;
  f.y = 0.0;
  f.bottom = 1e9;
  f.paginate = false;
  PlaceBlocks(blocks, 0.0, width, &f);
  in_table_ = saved_in_table;
  scratch_.resize(mark);
  target_ = saved;
  return f.y;
}

void FlowLayout::PlaceBlocks(const std::vector<Block>& blocks, double x, double width, Flow* f) {
  for (const Block& b : blocks) {
    if (b.type == Block::Type::Table && b.table != nullptr) {
      PlaceTable(*b.table, x, width, f);
    } else {
      PlaceParagraph(b.paragraph, x, width, f);
    }
  }
}

// ---- footnotes, headers and footers --------------------------------------

void FlowLayout::ReserveFootnote(const Note& n) {
  for (const auto& e : notes_) {
    if (e.note == &n) return;  // 同一页只放一次
  }
  const double avail = std::max(1.0, ContentWidth() - 12.0);
  const double h = MeasureBlocks(n.blocks, avail);
  if (h <= 0.0) return;
  FootnoteEntry e;
  e.note = &n;
  e.height = h;
  notes_.push_back(e);
  note_height_ += h + 2.0;
}

void FlowLayout::DrawFootnotes() {
  if (notes_.empty() || target_ == nullptr) return;
  double total = 0.0;
  for (const auto& e : notes_) total += e.height + 2.0;
  double y = bottom_ - total;
  if (y < top_) y = top_;
  EmitRule(left_, y - 3.0, left_ + 72.0, y - 3.0, 0x000000, 0.5);
  const bool saved = in_notes_;
  in_notes_ = true;
  for (const auto& e : notes_) {
    Flow f;
    f.y = y;
    f.bottom = bottom_;
    f.paginate = false;
    PlaceBlocks(e.note->blocks, left_ + 12.0, std::max(1.0, ContentWidth() - 12.0), &f);
    y = f.y + 2.0;
  }
  in_notes_ = saved;
}

void FlowLayout::DrawHeaderFooter(bool header) {
  if (section_ == nullptr || target_ == nullptr) return;
  const std::vector<HeaderFooter>& list = header ? section_->headers : section_->footers;
  if (list.empty()) return;
  const HeaderFooter* pick = nullptr;
  for (const HeaderFooter& hf : list) {
    if (hf.kind == HeaderFooter::Kind::Default) pick = &hf;
  }
  if (pick == nullptr) pick = &list.front();
  const double page_h = TwipsToPt(section_->page.height_twips);
  Flow f;
  f.paginate = false;
  if (header) {
    f.y = TwipsToPt(section_->page.header_dist);
    f.bottom = top_;
  } else {
    f.y = page_h - TwipsToPt(section_->page.footer_dist);
    f.bottom = page_h - 1.0;
  }
  PlaceBlocks(pick->blocks, left_, ContentWidth(), &f);
}

void FlowLayout::FinishPage() {
  if (out_.pages.empty() || section_ == nullptr) return;
  DrawFootnotes();
  DrawHeaderFooter(true);
  DrawHeaderFooter(false);
}

LayoutDocument FlowLayout::Run(CancelToken* cancel) {
  if (doc_.sections.empty()) {
    Section defaults;
    StartPage(defaults);
    FinishPage();
    ApplyWatermark();
    return std::move(out_);
  }
  for (const Section& s : doc_.sections) {
    CheckCancel(cancel);
    StartPage(s);
    Flow f;
    f.y = top_;
    f.bottom = EffectiveBottom();
    f.paginate = true;
    PlaceBlocks(s.blocks, left_, ContentWidth(), &f);
    FinishPage();
  }
  ApplyWatermark();
  return std::move(out_);
}

// ---- 水印 -----------------------------------------------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;

// IR 是"y 向下"的坐标系，视觉上的逆时针 θ 对应
//   x' =  x·cosθ + y·sinθ
//   y' = -x·sinθ + y·cosθ
// PDF 写出时 Tm 用 [cosφ sinφ -sinφ cosφ]，φ = rotate_deg，两边视觉方向一致。
void RotateVec(double theta_deg, double* x, double* y) {
  const double rad = theta_deg * kPi / 180.0;
  const double c = std::cos(rad);
  const double s = std::sin(rad);
  const double ox = *x;
  const double oy = *y;
  *x = ox * c + oy * s;
  *y = -ox * s + oy * c;
}

}  // namespace

void FlowLayout::ApplyWatermark() {
  const Watermark& wm = doc_.watermark;
  if (!wm.present() || out_.pages.empty()) return;

  const FontKey key{wm.font.empty() ? opts_.default_family : wm.font, wm.bold, wm.italic};
  Rgba color{static_cast<uint8_t>((wm.color >> 16) & 0xFF),
             static_cast<uint8_t>((wm.color >> 8) & 0xFF),
             static_cast<uint8_t>(wm.color & 0xFF), 255};
  if (wm.semitransparent) color.a = 128;

  if (wm.kind == Watermark::Kind::Image) {
    ImageData image;
    if (!TryDecodeImage(wm.image_bytes, &image, opts_.warnings) || image.width <= 0) {
      Warn(WarningCode::ImageSkipped, "layout: watermark image could not be decoded");
      return;
    }
    if (wm.washout) {
      // 褪色：把像素往白色拉，同时压不透明度。Word 用 gain/blacklevel 做同样
      // 的事，这里按视觉效果等价处理。
      for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        image.rgba[i + 0] = static_cast<uint8_t>(image.rgba[i + 0] + (255 - image.rgba[i + 0]) * 3 / 5);
        image.rgba[i + 1] = static_cast<uint8_t>(image.rgba[i + 1] + (255 - image.rgba[i + 1]) * 3 / 5);
        image.rgba[i + 2] = static_cast<uint8_t>(image.rgba[i + 2] + (255 - image.rgba[i + 2]) * 3 / 5);
        image.rgba[i + 3] = static_cast<uint8_t>(image.rgba[i + 3] * 2 / 5);
      }
    }
    for (LayoutPage& page : out_.pages) {
      const double target_w = wm.box_width_pt > 0 ? wm.box_width_pt : page.width_pt * 0.5;
      const double scale = target_w / static_cast<double>(image.width);
      const double w = target_w;
      const double h = static_cast<double>(image.height) * scale;
      DrawImage di;
      di.image = image;
      di.x = (page.width_pt - w) / 2.0;
      di.y = (page.height_pt - h) / 2.0;
      di.w_pt = w;
      di.h_pt = h;
      page.ops.insert(page.ops.begin(), di);
    }
    return;
  }

  if (wm.text.empty()) return;
  // Word 把文字水印写成 font-size:1pt + fitshape，真实字号由形状盒拉伸得到。
  // 所以这里先在 1pt 下量一次宽度，再按"盒宽 / 单位宽"反推字号。
  const double unit = metrics_.Measure(key, 1.0, wm.text);
  if (unit <= 0.0) return;

  for (LayoutPage& page : out_.pages) {
    const double target = wm.box_width_pt > 0 ? wm.box_width_pt : page.width_pt * 0.8868;
    const double size = wm.size_pt > 0 ? static_cast<double>(wm.size_pt) : target / unit;
    if (size <= 0.0) continue;
    const double w = metrics_.Measure(key, size, wm.text);
    const double asc = metrics_.Ascent(key, size);
    const double desc = metrics_.Descent(key, size);

    // 让"未旋转的文字盒中心"落在页面中心：先算盒中心相对基线起点的偏移，
    // 旋转它，再用页面中心减掉。
    double ox = w / 2.0;
    double oy = (desc - asc) / 2.0;
    RotateVec(-wm.rotation, &ox, &oy);  // Word 的 rotation 顺时针为正

    DrawText t;
    t.font = key;
    t.size_pt = size;
    t.color = color;
    t.x = page.width_pt / 2.0 - ox;
    t.y = page.height_pt / 2.0 - oy;
    t.rotate_deg = -wm.rotation;
    t.text_utf8 = wm.text;
    // 水印在正文下面（Word 的 z-index 是负值）。
    page.ops.insert(page.ops.begin(), t);
  }
}

}  // namespace

// ---- public API ----------------------------------------------------------

LayoutDocument LayoutText(const Document& doc, const LayoutOptions& opts, CancelToken* cancel) {
  FlowLayout layout(doc, opts);
  return layout.Run(cancel);
}

std::string ExtractText(const Document& doc) {
  std::string out;
  std::string cell;

  std::function<void(const std::vector<Block>&, bool)> walk =
      [&](const std::vector<Block>& blocks, bool in_cell) {
        for (const Block& b : blocks) {
          if (b.type == Block::Type::Table && b.table != nullptr) {
            for (const Row& row : b.table->rows) {
              bool first_cell = true;
              for (const Cell& c : row.cells) {
                if (!first_cell) out += '\t';
                first_cell = false;
                walk(c.blocks, true);
              }
              out += '\n';
            }
            continue;
          }
          for (const InlineItem& it : b.paragraph.runs) {
            if (it.type == InlineItem::Type::Text && it.revision != 2) {
              out += it.text;
            } else if (it.type == InlineItem::Type::Tab) {
              out += '\t';
            } else if (it.type == InlineItem::Type::Break) {
              out += it.break_kind == 1 ? "\n\f\n" : "\n";
            } else if (it.type == InlineItem::Type::Symbol) {
              out += U32ToUtf8(std::u32string(1, it.symbol));
            }
          }
          out += in_cell ? ' ' : '\n';
        }
      };

  for (const Section& s : doc.sections) walk(s.blocks, false);
  if (!doc.footnotes.empty()) {
    out += "\n[footnotes]\n";
    for (const Note& n : doc.footnotes) {
      out += std::to_string(n.id) + " ";
      walk(n.blocks, false);
    }
  }
  if (!doc.endnotes.empty()) {
    out += "\n[endnotes]\n";
    for (const Note& n : doc.endnotes) {
      out += std::to_string(n.id) + " ";
      walk(n.blocks, false);
    }
  }
  return out;
}

namespace {

void AppendHtmlEscaped(std::string& out, const std::string& text) {
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
}

std::string HtmlRunStyle(const RunFormat& f, const LayoutOptions& opts) {
  std::string style;
  if (f.bold == Tri::On) style += "font-weight:bold;";
  if (f.italic == Tri::On) style += "font-style:italic;";
  if (!f.underline.empty() && f.underline != "none") style += "text-decoration:underline;";
  if (f.size_half > 0) {
    style += "font-size:" + std::to_string(static_cast<double>(f.size_half) / 2.0) + "pt;";
  } else {
    style += "font-size:" + std::to_string(opts.default_size_pt) + "pt;";
  }
  if (f.color >= 0) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(f.color) & 0xFFFFFFu);
    style += "color:";
    style += buf;
    style += ";";
  }
  return style;
}

}  // namespace

std::string ToHtml(const Document& doc) {
  LayoutOptions opts;
  std::string out =
      "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>DocCore</title>"
      "<style>body{font-family:Calibri,Arial,sans-serif;margin:24pt;}"
      "table{border-collapse:collapse;}td{border:1px solid #808080;padding:4pt;}"
      "p{margin:0 0 6pt 0;}</style></head><body>\n";

  std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& blocks) {
    for (const Block& b : blocks) {
      if (b.type == Block::Type::Table && b.table != nullptr) {
        out += "<table>\n";
        for (const Row& row : b.table->rows) {
          out += "<tr>";
          for (const Cell& c : row.cells) {
            out += "<td>";
            walk(c.blocks);
            out += "</td>";
          }
          out += "</tr>\n";
        }
        out += "</table>\n";
        continue;
      }
      out += "<p>";
      for (const InlineItem& it : b.paragraph.runs) {
        if (it.type == InlineItem::Type::Text && it.revision != 2) {
          out += "<span style=\"" + HtmlRunStyle(it.format, opts) + "\">";
          AppendHtmlEscaped(out, it.text);
          out += "</span>";
        } else if (it.type == InlineItem::Type::Tab) {
          out += "&emsp;";
        } else if (it.type == InlineItem::Type::Break) {
          out += it.break_kind == 1 ? "<br style=\"page-break-after:always\">" : "<br>";
        } else if (it.type == InlineItem::Type::Symbol) {
          AppendHtmlEscaped(out, U32ToUtf8(std::u32string(1, it.symbol)));
        }
      }
      out += "</p>\n";
    }
  };

  for (const Section& s : doc.sections) walk(s.blocks);
  out += "</body></html>\n";
  return out;
}

}  // namespace text
}  // namespace doccore
