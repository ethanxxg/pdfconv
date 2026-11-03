// DocCore - sheet DOM -> XLSX writer. Design refs: docs/02 §3.4.1
//
// 输出部件固定为 7 类：内容类型、包关系、workbook、workbook 关系、
// 每张表、共享字符串、样式。未识别部件（theme、打印设置、docProps 等）
// 原样写回并补上内容类型，保证"打开再保存"不丢东西。
#include "doccore/xlsx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "doccore/zip_write.h"
#include "doccore/opc.h"

namespace doccore {
namespace sheet {
namespace {

// ---- XML 输出 -----------------------------------------------------------

std::string EscText(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string EscAttr(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

using Attrs = std::vector<std::pair<std::string, std::string>>;

class Buf {
 public:
  void Decl() { out_ += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"; }

  void Open(const std::string& name, Attrs attrs = {}) {
    out_ += '<';
    out_ += name;
    for (const auto& kv : attrs) {
      out_ += ' ';
      out_ += kv.first;
      out_ += "=\"";
      out_ += EscAttr(kv.second);
      out_ += '"';
    }
    out_ += '>';
    stack_.push_back({name, out_.size()});
  }

  void Close() {
    if (stack_.empty()) return;
    const Frame frame = stack_.back();
    stack_.pop_back();
    if (frame.mark == out_.size()) {
      // 空元素折叠成 <x/>：体积小一半，Excel 也这么写。
      out_.resize(out_.size() - 1);
      out_ += "/>";
      return;
    }
    out_ += "</";
    out_ += frame.name;
    out_ += '>';
  }

  void Leaf(const std::string& name, Attrs attrs = {}) {
    Open(name, attrs);
    Close();
  }

  // 文本叶子；需要保留空白时自动加 xml:space="preserve"。
  void TextLeaf(const std::string& name, const std::string& value, Attrs attrs = {}) {
    const bool preserve = !value.empty() && (value.front() == ' ' || value.back() == ' ' ||
                                             value.find('\n') != std::string::npos);
    if (preserve) attrs.emplace_back("xml:space", "preserve");
    Open(name, attrs);
    out_ += EscText(value);
    Close();
  }

  void Raw(const std::string& s) { out_ += s; }

  const std::string& str() const noexcept { return out_; }
  std::vector<uint8_t> bytes() const { return std::vector<uint8_t>(out_.begin(), out_.end()); }

 private:
  struct Frame {
    std::string name;
    size_t mark = 0;
  };
  std::string out_;
  std::vector<Frame> stack_;
};

std::string NumText(double value) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.15g", value);
  if (std::strtod(buf, nullptr) != value) {
    std::snprintf(buf, sizeof(buf), "%.17g", value);
  }
  return buf;
}

std::string BoolText(bool value) { return value ? "1" : "0"; }

std::vector<uint8_t> StrBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// "FFRRGGBB" 形式的颜色属性。
std::string ColorText(int rgb) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "FF%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
  return buf;
}

std::string FormatCode(const CellStyle& style) {
  if (!style.num_fmt.pattern.empty()) return style.num_fmt.pattern;
  const std::string_view builtin = BuiltinNumberFormat(style.num_fmt_id);
  return std::string(builtin);
}

// 需要自定义 numFmt 项的 id：>= 164 且格式串非空。
bool NeedsCustomFormat(const CellStyle& style) {
  return style.num_fmt_id >= 164 && !FormatCode(style).empty();
}

// ---- 样式表 -------------------------------------------------------------

struct StylePlan {
  std::vector<CellStyle> xfs;
  std::map<int, std::string> custom_formats;  // id -> formatCode
  std::vector<CellFont> fonts;
  std::vector<int> fills;    // -1 = 无填充
  std::vector<int> borders;  // 位掩码
  std::vector<int> font_of;  // xf -> font index
  std::vector<int> fill_of;
  std::vector<int> border_of;
};

std::string FontKey(const CellFont& font) {
  std::string key = font.name + "|" + NumText(font.size_pt) + "|";
  key += font.bold ? 'b' : '-';
  key += font.italic ? 'i' : '-';
  key += font.underline ? 'u' : '-';
  key += font.strike ? 's' : '-';
  key += "|" + std::to_string(font.color);
  return key;
}

StylePlan PlanStyles(const Workbook& book) {
  StylePlan plan;
  std::map<std::string, int> font_index;
  std::map<int, int> fill_index;
  std::map<int, int> border_index;

  // fills / borders 的 0 号是约定俗成的 none。
  plan.fills.push_back(-1);
  plan.borders.push_back(0);

  for (const CellStyle& style : book.styles) {
    const std::string key = FontKey(style.font);
    auto it = font_index.find(key);
    if (it == font_index.end()) {
      const int index = static_cast<int>(plan.fonts.size());
      plan.fonts.push_back(style.font);
      font_index.emplace(key, index);
      plan.font_of.push_back(index);
    } else {
      plan.font_of.push_back(it->second);
    }

    auto fit = fill_index.find(style.fill);
    if (fit == fill_index.end()) {
      const int index = static_cast<int>(plan.fills.size());
      plan.fills.push_back(style.fill);
      fill_index.emplace(style.fill, index);
      plan.fill_of.push_back(index);
    } else {
      plan.fill_of.push_back(fit->second);
    }

    auto bit = border_index.find(style.border_mask);
    if (bit == border_index.end()) {
      const int index = static_cast<int>(plan.borders.size());
      plan.borders.push_back(style.border_mask);
      border_index.emplace(style.border_mask, index);
      plan.border_of.push_back(index);
    } else {
      plan.border_of.push_back(bit->second);
    }

    if (NeedsCustomFormat(style)) plan.custom_formats[style.num_fmt_id] = FormatCode(style);
    plan.xfs.push_back(style);
  }
  if (plan.xfs.empty()) {
    plan.xfs.push_back(CellStyle{});
    plan.fonts.push_back(CellFont{});
    plan.fills.push_back(-1);
    plan.borders.push_back(0);
    plan.font_of.push_back(0);
    plan.fill_of.push_back(0);
    plan.border_of.push_back(0);
  }
  return plan;
}

std::string StylesXml(const StylePlan& plan, const std::vector<CfDxf>& dxfs) {
  Buf x;
  x.Decl();
  x.Open("styleSheet", {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}});

  if (!plan.custom_formats.empty()) {
    x.Open("numFmts", {{"count", std::to_string(plan.custom_formats.size())}});
    for (const auto& kv : plan.custom_formats) {
      x.Leaf("numFmt", {{"numFmtId", std::to_string(kv.first)}, {"formatCode", kv.second}});
    }
    x.Close();
  }

  x.Open("fonts", {{"count", std::to_string(plan.fonts.size())}});
  for (const CellFont& font : plan.fonts) {
    x.Open("font");
    if (font.bold) x.Leaf("b");
    if (font.italic) x.Leaf("i");
    if (font.strike) x.Leaf("strike");
    if (font.underline) x.Leaf("u");
    x.Leaf("sz", {{"val", NumText(font.size_pt)}});
    if (font.color >= 0) x.Leaf("color", {{"rgb", ColorText(font.color)}});
    x.Leaf("name", {{"val", font.name}});
    x.Close();
  }
  x.Close();

  x.Open("fills", {{"count", std::to_string(plan.fills.size())}});
  for (int fill : plan.fills) {
    x.Open("fill");
    if (fill < 0) {
      x.Leaf("patternFill", {{"patternType", "none"}});
    } else {
      x.Open("patternFill", {{"patternType", "solid"}});
      x.Leaf("fgColor", {{"rgb", ColorText(fill)}});
      x.Leaf("bgColor", {{"indexed", "64"}});
      x.Close();
    }
    x.Close();
  }
  x.Close();

  x.Open("borders", {{"count", std::to_string(plan.borders.size())}});
  for (int mask : plan.borders) {
    x.Open("border");
    const char* sides[4] = {"top", "right", "bottom", "left"};
    for (int i = 0; i < 4; ++i) {
      if ((mask & (1 << i)) != 0) {
        x.Leaf(sides[i], {{"style", "thin"}});
      } else {
        x.Leaf(sides[i]);
      }
    }
    x.Leaf("diagonal");
    x.Close();
  }
  x.Close();

  x.Open("cellStyleXfs", {{"count", "1"}});
  x.Leaf("xf", {{"numFmtId", "0"}, {"fontId", "0"}, {"fillId", "0"}, {"borderId", "0"}});
  x.Close();

  x.Open("cellXfs", {{"count", std::to_string(plan.xfs.size())}});
  for (size_t i = 0; i < plan.xfs.size(); ++i) {
    const CellStyle& style = plan.xfs[i];
    const int font = plan.font_of[i];
    const int fill = plan.fill_of[i];
    const int border = plan.border_of[i];
    const bool has_alignment = style.halign >= 0 || style.valign >= 0 || style.wrap;
    x.Open("xf", {{"numFmtId", std::to_string(style.num_fmt_id)},
                  {"fontId", std::to_string(font)},
                  {"fillId", std::to_string(fill)},
                  {"borderId", std::to_string(border)},
                  {"xfId", "0"},
                  {"applyNumberFormat", style.num_fmt_id != 0 ? "1" : "0"},
                  {"applyFont", font != 0 ? "1" : "0"},
                  {"applyFill", fill != 0 ? "1" : "0"},
                  {"applyBorder", border != 0 ? "1" : "0"},
                  {"applyAlignment", has_alignment ? "1" : "0"}});
    if (has_alignment) {
      std::vector<std::pair<std::string, std::string>> attrs;
      if (style.halign == 1) attrs.emplace_back("horizontal", "left");
      else if (style.halign == 2) attrs.emplace_back("horizontal", "center");
      else if (style.halign == 3) attrs.emplace_back("horizontal", "right");
      else if (style.halign == 0) attrs.emplace_back("horizontal", "general");
      if (style.valign == 0) attrs.emplace_back("vertical", "top");
      else if (style.valign == 1) attrs.emplace_back("vertical", "center");
      else if (style.valign == 2) attrs.emplace_back("vertical", "bottom");
      if (style.wrap) attrs.emplace_back("wrapText", "1");
      x.Leaf("alignment", attrs);
    }
    x.Close();
  }
  x.Close();

  x.Open("cellStyles", {{"count", "1"}});
  x.Leaf("cellStyle", {{"name", "Normal"}, {"xfId", "0"}, {"builtinId", "0"}});
  x.Close();

  // 条件格式的差异格式表：填充色写在 bgColor（OOXML 对 dxf 的约定）。
  if (!dxfs.empty()) {
    x.Open("dxfs", {{"count", std::to_string(dxfs.size())}});
    for (const CfDxf& dxf : dxfs) {
      x.Open("dxf");
      if (dxf.bold || dxf.font_color >= 0) {
        x.Open("font");
        if (dxf.bold) x.Leaf("b");
        if (dxf.font_color >= 0) x.Leaf("color", {{"rgb", ColorText(dxf.font_color)}});
        x.Close();
      }
      if (dxf.fill >= 0) {
        x.Open("fill");
        x.Open("patternFill");
        x.Leaf("bgColor", {{"rgb", ColorText(dxf.fill)}});
        x.Close();
        x.Close();
      }
      x.Close();
    }
    x.Close();
  }

  x.Close();
  return x.str();
}

}  // namespace

// ---- 工作表 -------------------------------------------------------------

namespace {

struct CellOut {
  std::string type;   // "" = 数字
  std::string value;  // <v> 内容
  std::string inline_text;
  bool has_value = false;
};

CellOut DescribeCell(const Workbook& wb, const Cell& cell) {
  CellOut out;
  if (cell.has_formula()) {
    const Value cached = cell.cached;
    switch (cached.kind) {
      case Value::Kind::Text:
        out.type = "str";
        out.value = cached.text;
        out.has_value = true;
        return out;
      case Value::Kind::Bool:
        out.type = "b";
        out.value = BoolText(cached.boolean);
        out.has_value = true;
        return out;
      case Value::Kind::Error:
        out.type = "e";
        out.value = CellErrorText(cached.error);
        out.has_value = true;
        return out;
      case Value::Kind::Number:
        out.value = NumText(cached.number);
        out.has_value = true;
        return out;
      case Value::Kind::Empty:
        return out;
    }
    return out;
  }
  switch (cell.kind) {
    case CellKind::Empty:
      return out;
    case CellKind::Number:
      out.value = NumText(cell.number);
      out.has_value = true;
      return out;
    case CellKind::Bool:
      out.type = "b";
      out.value = BoolText(cell.boolean);
      out.has_value = true;
      return out;
    case CellKind::Error:
      out.type = "e";
      out.value = CellErrorText(cell.error);
      out.has_value = true;
      return out;
    case CellKind::Text:
      if (cell.text_id >= 0 && static_cast<size_t>(cell.text_id) < wb.shared_strings.size()) {
        out.type = "s";
        out.value = std::to_string(cell.text_id);
        out.has_value = true;
      } else if (cell.cached.is_text()) {
        out.type = "inlineStr";
        out.inline_text = cell.cached.text;
      }
      return out;
    case CellKind::Formula:
      return out;
  }
  return out;
}

int FindDxf(const std::vector<CfDxf>& dxfs, const CfDxf& want) {
  for (size_t i = 0; i < dxfs.size(); ++i) {
    if (dxfs[i].fill == want.fill && dxfs[i].font_color == want.font_color &&
        dxfs[i].bold == want.bold) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string SheetXml(const Workbook& wb, const Worksheet& ws, const std::vector<CfDxf>& dxfs) {
  // 行优先视图：存储是列优先（稀疏），导出时必须按行写。
  std::map<int, std::map<int, const Cell*>> rows;
  for (const auto& column : ws.columns()) {
    for (const auto& row : column.second) {
      const Cell& cell = row.second;
      if (cell.empty() && cell.style_id < 0) continue;
      rows[row.first][column.first] = &cell;
    }
  }

  const Range used = ws.UsedRange();
  Buf x;
  x.Decl();
  x.Open("worksheet", {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
                       {"xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}});

  x.Leaf("dimension", {{"ref", CellName(used.row1, used.col1) + ":" + CellName(used.row2, used.col2)}});

  x.Open("sheetViews");
  x.Open("sheetView", {{"workbookViewId", "0"}});
  x.Close();
  x.Close();
  x.Leaf("sheetFormatPr", {{"defaultRowHeight", "15"}});

  if (!ws.col_widths().empty()) {
    x.Open("cols");
    for (const auto& kv : ws.col_widths()) {
      x.Leaf("col", {{"min", std::to_string(kv.first + 1)},
                     {"max", std::to_string(kv.first + 1)},
                     {"width", NumText(kv.second)},
                     {"customWidth", "1"}});
    }
    x.Close();
  }

  x.Open("sheetData");
  for (const auto& row : rows) {
    Attrs row_attrs{{"r", std::to_string(row.first + 1)}};
    auto height = ws.row_heights().find(row.first);
    if (height != ws.row_heights().end()) {
      row_attrs.emplace_back("ht", NumText(height->second));
      row_attrs.emplace_back("customHeight", "1");
    }
    x.Open("row", row_attrs);
    for (const auto& column : row.second) {
      const Cell& cell = *column.second;
      const CellOut described = DescribeCell(wb, cell);
      Attrs attrs{{"r", CellName(row.first, column.first)}};
      if (cell.style_id >= 0) attrs.emplace_back("s", std::to_string(cell.style_id));
      if (!described.type.empty()) attrs.emplace_back("t", described.type);
      if (!described.has_value && !cell.has_formula() && described.inline_text.empty()) {
        x.Leaf("c", attrs);  // 只有样式
        continue;
      }
      x.Open("c", attrs);
      if (cell.has_formula()) {
        const std::string text =
            cell.formula_id >= 0 && static_cast<size_t>(cell.formula_id) < ws.formulas().size()
                ? ws.formulas()[static_cast<size_t>(cell.formula_id)].text
                : std::string();
        if (!text.empty()) x.TextLeaf("f", text);
      }
      if (!described.inline_text.empty()) {
        x.Open("is");
        x.TextLeaf("t", described.inline_text);
        x.Close();
      } else if (described.has_value) {
        x.TextLeaf("v", described.value);
      }
      x.Close();
    }
    x.Close();
  }
  x.Close();

  if (!ws.merges().empty()) {
    x.Open("mergeCells", {{"count", std::to_string(ws.merges().size())}});
    for (const Range& merge : ws.merges()) {
      x.Leaf("mergeCell",
             {{"ref", CellName(merge.row1, merge.col1) + ":" + CellName(merge.row2, merge.col2)}});
    }
    x.Close();
  }

  // 条件格式（F-205）：cfRule 的 dxfId 指向 styles.xml 的 <dxfs> 下标。
  for (const ConditionalFormat& cf : ws.conditional()) {
    if (!cf.range.valid()) continue;
    x.Open("conditionalFormatting",
           {{"sqref", CellName(cf.range.row1, cf.range.col1) + ":" +
                          CellName(cf.range.row2, cf.range.col2)}});
    for (const ConditionalRule& rule : cf.rules) {
      const char* type = rule.type == CfType::Expression ? "expression" : "cellIs";
      Attrs rattrs{{"type", type},
                   {"operator", CfOperatorName(rule.op)},
                   {"priority", std::to_string(rule.priority)}};
      int dxf_id = FindDxf(dxfs, rule.dxf);
      if (dxf_id < 0) dxf_id = rule.dxf_id;
      if (dxf_id >= 0) rattrs.emplace_back("dxfId", std::to_string(dxf_id));
      x.Open("cfRule", rattrs);
      if (rule.type == CfType::Expression) {
        if (!rule.formula.empty()) x.TextLeaf("formula", rule.formula);
      } else {
        for (double v : rule.operands) x.TextLeaf("formula", NumText(v));
      }
      x.Close();
    }
    x.Close();
  }

  // 数据验证（F-205）：<dataValidations> 紧跟 <conditionalFormatting>。
  if (!ws.validations().empty()) {
    x.Open("dataValidations", {{"count", std::to_string(ws.validations().size())}});
    for (const DataValidation& dv : ws.validations()) {
      // sqref 由各段拼回（空格分隔）。
      std::string sqref;
      for (size_t i = 0; i < dv.ranges.size(); ++i) {
        const Range& r = dv.ranges[i];
        if (i != 0) sqref += ' ';
        sqref += CellName(r.row1, r.col1) + ":" + CellName(r.row2, r.col2);
      }
      Attrs a{{"type", ValidationTypeName(dv.type)},
              {"sqref", sqref},
              {"allowBlank", dv.allow_blank ? "1" : "0"},
              {"showErrorMessage", dv.show_error ? "1" : "0"}};
      if (dv.type != ValidationType::List && dv.op != ValidationOp::None) {
        a.emplace_back("operator", ValidationOpName(dv.op));
      }
      const char* es = dv.error_style == 2 ? "warning" : dv.error_style == 3 ? "information" : "stop";
      a.emplace_back("errorStyle", es);
      x.Open("dataValidation", a);
      if (!dv.error_title.empty()) x.TextLeaf("errorTitle", dv.error_title);
      if (!dv.error_message.empty()) x.TextLeaf("error", dv.error_message);
      if (!dv.prompt_title.empty()) x.TextLeaf("promptTitle", dv.prompt_title);
      if (!dv.prompt_message.empty()) x.TextLeaf("prompt", dv.prompt_message);
      if (!dv.formula1.empty()) x.TextLeaf("formula1", dv.formula1);
      if (!dv.formula2.empty()) x.TextLeaf("formula2", dv.formula2);
      x.Close();
    }
    x.Close();
  }

  // 超链接（CT_Worksheet 顺序：dataValidations 之后、printOptions 之前）。
  // 外部链接按出现顺序拿 rId1..N，必须与包阶段写 _rels 的顺序一致。
  if (!ws.hyperlinks().empty()) {
    x.Open("hyperlinks");
    int rid = 0;
    for (const Hyperlink& link : ws.hyperlinks()) {
      if (!link.range.valid()) continue;
      Attrs a{{"ref", CellName(link.range.row1, link.range.col1) + ":" +
                          CellName(link.range.row2, link.range.col2)}};
      if (!link.target.empty()) a.emplace_back("r:id", "rId" + std::to_string(++rid));
      if (!link.location.empty()) a.emplace_back("location", link.location);
      if (!link.display.empty()) a.emplace_back("display", link.display);
      if (!link.tooltip.empty()) a.emplace_back("tooltip", link.tooltip);
      x.Leaf("hyperlink", a);
    }
    x.Close();
  }

  const PrintSetup& print = ws.print();
  x.Leaf("pageMargins", {{"left", NumText(print.margin_left / 1440.0)},
                         {"right", NumText(print.margin_right / 1440.0)},
                         {"top", NumText(print.margin_top / 1440.0)},
                         {"bottom", NumText(print.margin_bottom / 1440.0)},
                         {"header", "0.3"},
                         {"footer", "0.3"}});
  Attrs setup{{"paperSize", "9"}};
  if (print.landscape) setup.emplace_back("orientation", "landscape");
  if (print.fit_to_width > 0) {
    setup.emplace_back("fitToWidth", std::to_string(print.fit_to_width));
    setup.emplace_back("fitToHeight", std::to_string(print.fit_to_height));
  }
  x.Leaf("pageSetup", setup);

  x.Close();
  return x.str();
}

std::string SharedStringsXml(const Workbook& wb) {
  Buf x;
  x.Decl();
  x.Open("sst", {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
                 {"count", std::to_string(wb.shared_strings.size())},
                 {"uniqueCount", std::to_string(wb.shared_strings.size())}});
  for (const std::string& text : wb.shared_strings) {
    x.Open("si");
    x.TextLeaf("t", text);
    x.Close();
  }
  x.Close();
  return x.str();
}

// workbook.xml 的 rId 分配顺序：sheets(1..N) -> [sharedStrings] -> styles -> pivotCaches。
// WorkbookXml 与 WorkbookRelsXml 都从这里取起点，两边必须一致。
int FirstPivotCacheRid(const Workbook& wb, bool has_strings) {
  int next = wb.sheet_count() + 1;
  if (has_strings) ++next;  // sharedStrings
  ++next;                   // styles
  return next;
}

// cache_uris 与 wb.pivot_tables 一一对应（绝对部件路径，可能为空）。
// <pivotCaches> 按 CT_Workbook 的顺序必须排在 definedNames 之后、calcPr 之前。
std::string WorkbookXml(const Workbook& wb, const std::vector<std::string>& cache_uris,
                        int cache_base_rid) {
  Buf x;
  x.Decl();
  x.Open("workbook", {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
                      {"xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}});
  if (wb.date1904) x.Leaf("workbookPr", {{"date1904", "1"}});
  x.Open("bookViews");
  x.Leaf("workbookView", {{"activeTab", "0"}});
  x.Close();
  x.Open("sheets");
  for (int i = 0; i < wb.sheet_count(); ++i) {
    const Worksheet* ws = wb.SheetAt(i);
    if (ws == nullptr) continue;
    x.Leaf("sheet", {{"name", ws->name()},
                     {"sheetId", std::to_string(i + 1)},
                     {"r:id", "rId" + std::to_string(i + 1)}});
  }
  x.Close();
  if (!wb.names.empty()) {
    x.Open("definedNames");
    for (const DefinedName& dn : wb.names) {
      x.TextLeaf("definedName", dn.refers_to, {{"name", dn.name}});
    }
    x.Close();
  }
  if (!cache_uris.empty()) {
    x.Open("pivotCaches");
    for (size_t i = 0; i < cache_uris.size(); ++i) {
      // cacheId 从 1 起，与 pivotTableDefinition 里的 cacheId 对应。
      x.Leaf("pivotCache", {{"cacheId", std::to_string(i + 1)},
                            {"r:id", "rId" + std::to_string(cache_base_rid + static_cast<int>(i))}});
    }
    x.Close();
  }
  x.Close();
  return x.str();
}

std::string WorkbookRelsXml(const Workbook& wb, bool has_strings,
                            const std::vector<std::string>& cache_uris) {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  for (int i = 0; i < wb.sheet_count(); ++i) {
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(i + 1)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"},
            {"Target", "worksheets/sheet" + std::to_string(i + 1) + ".xml"}});
  }
  int next = wb.sheet_count() + 1;
  if (has_strings) {
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(next++)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings"},
            {"Target", "sharedStrings.xml"}});
  }
  x.Leaf("Relationship",
         {{"Id", "rId" + std::to_string(next++)},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"},
          {"Target", "styles.xml"}});
  // pivotCacheDefinition 挂在 workbook 下（而不是工作表下），Target 相对 xl/。
  for (const std::string& uri : cache_uris) {
    if (uri.empty()) continue;
    std::string target = uri;
    if (target.compare(0, 4, "/xl/") == 0) target = target.substr(4);
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(next++)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheDefinition"},
            {"Target", target}});
  }
  x.Close();
  return x.str();
}

std::string RootRelsXml() {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  x.Leaf("Relationship", {{"Id", "rId1"},
                          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"},
                          {"Target", "xl/workbook.xml"}});
  x.Close();
  return x.str();
}

struct PartOut {
  std::string uri;   // "/xl/workbook.xml"
  std::string mime;
  std::string data;
};

std::string ContentTypesXml(const std::vector<PartOut>& parts) {
  Buf x;
  x.Decl();
  x.Open("Types", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/content-types"}});
  x.Leaf("Default", {{"Extension", "rels"},
                     {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
  x.Leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
  std::set<std::string> seen;
  for (const PartOut& part : parts) {
    if (part.mime.empty() || part.mime == "application/xml") continue;
    if (!seen.insert(part.uri).second) continue;
    x.Leaf("Override", {{"PartName", part.uri}, {"ContentType", part.mime}});
  }
  x.Close();
  return x.str();
}

std::string PartNameFor(const std::string& uri) {
  // "/xl/workbook.xml" -> "xl/workbook.xml"（zip 条目不带前导斜杠）
  return uri.empty() || uri[0] != '/' ? uri : uri.substr(1);
}

// 把 Part-URI 目标折算成相对 base_dir 的 Target（OPC 关系一律用相对目标）。
// "/xl/charts/chart1.xml" 相对 "/xl/drawings" -> "../charts/chart1.xml"。
std::string RelativeTarget(std::string_view base_dir, std::string_view target) {
  std::vector<std::string> a;
  std::vector<std::string> b;
  auto split = [](std::vector<std::string>* out, std::string_view path) {
    size_t i = 0;
    while (i < path.size()) {
      size_t j = path.find('/', i);
      if (j == std::string_view::npos) j = path.size();
      if (j > i) out->push_back(std::string(path.substr(i, j - i)));
      i = j + 1;
    }
  };
  split(&a, base_dir);
  split(&b, target);
  size_t common = 0;
  while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
  std::string out;
  for (size_t i = common; i < a.size(); ++i) out += "../";
  for (size_t i = common; i < b.size(); ++i) {
    out += b[i];
    if (i + 1 < b.size()) out += "/";
  }
  if (out.empty()) out = ".";
  return out;
}

// 原样透传部件自带的关系（图表 -> 调色板/样式、绘图 -> 图表）。
std::string PreservedRelsXml(const std::vector<UnknownRel>& rels, std::string_view base_dir) {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  for (const UnknownRel& rel : rels) {
    std::vector<std::pair<std::string, std::string>> attrs = {
        {"Id", rel.id}, {"Type", rel.type}};
    if (rel.external) {
      attrs.push_back({"Target", rel.target});
      attrs.push_back({"TargetMode", "External"});
    } else {
      attrs.push_back({"Target", RelativeTarget(base_dir, rel.target)});
    }
    x.Leaf("Relationship", attrs);
  }
  x.Close();
  return x.str();
}

// ---- 数据透视表（F-206）--------------------------------------------------
//
// 三个部件一起写：pivotTable（布局与字段归属）、pivotCacheDefinition（字段与
// 共享项）、pivotCacheRecords（源数据快照）。三者缺一 Excel 会认为缓存失效。

const char* kCtPivotTable =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotTable+xml";
const char* kCtPivotCache =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml";
const char* kCtPivotRecords =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheRecords+xml";
const char* kRelPivotTable =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotTable";
const char* kRelPivotCache =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheDefinition";
const char* kRelPivotRecords =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheRecords";

const char* AxisAttr(PivotAxis axis) {
  switch (axis) {
    case PivotAxis::Row: return "axisRow";
    case PivotAxis::Column: return "axisCol";
    case PivotAxis::Page: return "axisPage";
    case PivotAxis::Data: return "axisValues";
    case PivotAxis::None: break;
  }
  return "";
}

// 记录条数：写 cacheDefinition 的 recordCount 用，不复制整份数据。
size_t PivotSourceRowsCount(const PivotTable& pt) {
  if (!pt.source.valid()) return 0;
  return static_cast<size_t>(std::max(0, pt.source.row2 - pt.source.row1));
}

// 透视表输出的行列跨度：两行表头 + 数据行；行标签列 + 列组合 × 数据字段。
void PivotSpan(const PivotTable& pt, int* rows, int* cols) {
  const int label_cols = std::max<int>(1, static_cast<int>(pt.row_fields.size()));
  const int ndata = std::max<int>(1, static_cast<int>(pt.data_fields.size()));
  const int ncols = std::max<int>(1, static_cast<int>(pt.column_items.size()));
  *rows = 2 + static_cast<int>(pt.row_items.size());
  *cols = label_cols + ncols * ndata;
}

std::string PivotTableXml(const PivotTable& pt, int cache_id) {
  Buf x;
  x.Decl();
  x.Open("pivotTableDefinition",
         {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
          {"xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"},
          {"name", pt.name},
          {"cacheId", std::to_string(cache_id)},
          {"dataOnRows", "0"},
          {"applyNumberFormats", "0"},
          {"applyBorderFormats", "0"},
          {"applyFontFormats", "0"},
          {"applyPatternFormats", "0"},
          {"applyAlignmentFormats", "0"},
          {"applyWidthHeightFormats", "1"},
          {"dataCaption", "值"},
          {"updatedVersion", "6"},
          {"minRefreshableVersion", "3"},
          {"useAutoFormatting", "1"},
          {"itemPrintTitles", "1"},
          {"createdVersion", "6"},
          {"indent", "0"},
          {"outline", "1"},
          {"outlineData", "1"},
          {"multipleFieldFilters", "0"}});
  {
    int rows = 0, cols = 0;
    PivotSpan(pt, &rows, &cols);
    const std::string ref = CellName(pt.destination.row, pt.destination.col) + ":" +
                            CellName(pt.destination.row + rows - 1,
                                     pt.destination.col + cols - 1);
    x.Leaf("location", {{"ref", ref},
                        {"firstHeaderRow", "1"},
                        {"firstDataRow", "1"},
                        {"firstDataCol", "1"}});
  }
  x.Open("pivotFields", {{"count", std::to_string(pt.fields.size())}});
  for (size_t i = 0; i < pt.fields.size(); ++i) {
    const PivotField pf = i < pt.pivot_fields.size() ? pt.pivot_fields[i] : PivotField{};
    std::vector<std::pair<std::string, std::string>> attrs;
    if (pf.axis != PivotAxis::None) attrs.push_back({"axis", AxisAttr(pf.axis)});
    if (pf.data_field) attrs.push_back({"dataField", "1"});
    attrs.push_back({"showAll", pf.show_all ? "1" : "0"});
    x.Open("pivotField", attrs);
    // 行/列/页字段要把 item 列出来，Excel 才认为字段的取值集合是"已知"的。
    if (pf.axis != PivotAxis::None && !pt.fields[i].shared_items.empty()) {
      x.Open("items", {{"count", std::to_string(pt.fields[i].shared_items.size())}});
      for (size_t k = 0; k < pt.fields[i].shared_items.size(); ++k) {
        x.Leaf("item", {{"x", std::to_string(k)}});
      }
      x.Close();
    }
    x.Close();
  }
  x.Close();

  auto field_list = [&](const char* name, const std::vector<int>& fields) {
    if (fields.empty()) return;
    x.Open(name, {{"count", std::to_string(fields.size())}});
    for (int f : fields) x.Leaf("field", {{"x", std::to_string(f)}});
    x.Close();
  };
  field_list("rowFields", pt.row_fields);
  field_list("colFields", pt.column_fields);
  field_list("pageFields", pt.page_fields);

  auto items = [&](const char* name, const std::vector<PivotItem>& list) {
    if (list.empty()) return;
    x.Open(name, {{"count", std::to_string(list.size())}});
    for (const PivotItem& item : list) {
      std::vector<std::pair<std::string, std::string>> attrs;
      if (item.grand) attrs.push_back({"t", "grand"});
      x.Open("i", attrs);
      if (item.indices.empty()) {
        x.Leaf("x");
      } else {
        for (size_t k = 0; k < item.indices.size(); ++k) {
          if (item.indices[k] <= 0) {
            x.Leaf("x");  // 0 是默认值，Excel 写成空元素
          } else {
            x.Leaf("x", {{"v", std::to_string(item.indices[k])}});
          }
        }
      }
      x.Close();
    }
    x.Close();
  };
  items("rowItems", pt.row_items);
  items("colItems", pt.column_items);

  if (!pt.data_fields.empty()) {
    x.Open("dataFields", {{"count", std::to_string(pt.data_fields.size())}});
    for (const PivotDataField& df : pt.data_fields) {
      std::vector<std::pair<std::string, std::string>> attrs;
      if (!df.name.empty()) attrs.push_back({"name", df.name});
      attrs.push_back({"fld", std::to_string(df.field_index)});
      // subtotal 的默认值就是 sum，但显式写出来更利于其它实现读取。
      attrs.push_back({"subtotal", PivotAggregateName(df.aggregate)});
      attrs.push_back({"baseField", "0"});
      attrs.push_back({"baseItem", "0"});
      if (!df.number_format.empty()) attrs.push_back({"numFmtId", df.number_format});
      x.Leaf("dataField", attrs);
    }
    x.Close();
  }
  x.Leaf("pivotTableStyleInfo", {{"name", pt.style_name},
                                 {"showRowHeaders", "1"},
                                 {"showColHeaders", "1"},
                                 {"showRowStripes", "0"},
                                 {"showColStripes", "0"},
                                 {"showLastColumn", "1"}});
  x.Close();
  return x.str();
}

std::string PivotCacheXml(const PivotTable& pt) {
  Buf x;
  x.Decl();
  x.Open("pivotCacheDefinition",
         {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
          {"xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"},
          {"r:id", "rId1"},
          {"refreshOnLoad", "1"},
          {"refreshedBy", "DocCore"},
          {"createdVersion", "6"},
          {"refreshedVersion", "6"},
          {"minRefreshableVersion", "3"},
          {"recordCount", std::to_string(static_cast<int>(PivotSourceRowsCount(pt)))}});
  x.Open("cacheSource", {{"type", "worksheet"}});
  const std::string ref =
      CellName(pt.source.row1, pt.source.col1) + ":" + CellName(pt.source.row2, pt.source.col2);
  x.Leaf("worksheetSource", {{"ref", ref}, {"sheet", pt.source_sheet}});
  x.Close();
  x.Open("cacheFields", {{"count", std::to_string(pt.fields.size())}});
  for (const PivotCacheField& f : pt.fields) {
    x.Open("cacheField", {{"name", f.name}, {"numFmtId", "0"}});
    if (!f.shared_items.empty()) {
      x.Open("sharedItems", {{"count", std::to_string(f.shared_items.size())}});
      for (const std::string& item : f.shared_items) x.Leaf("s", {{"v", item}});
      x.Close();
    } else if (f.has_numbers) {
      x.Leaf("sharedItems", {{"containsSemiMixedTypes", "0"},
                             {"containsString", "0"},
                             {"containsNumber", "1"},
                             {"containsInteger", "1"},
                             {"minValue", NumText(f.min_value)},
                             {"maxValue", NumText(f.max_value)}});
    } else {
      x.Leaf("sharedItems", {{"containsSemiMixedTypes", "1"}});
    }
    x.Close();
  }
  x.Close();
  x.Close();
  return x.str();
}

std::string PivotRecordsXml(const Workbook& wb, const PivotTable& pt) {
  const std::vector<std::vector<Value>> rows = PivotSourceRows(wb, pt);
  Buf x;
  x.Decl();
  x.Open("pivotCacheRecords",
         {{"xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main"},
          {"xmlns:r", "http://schemas.openxmlformats.org/officeDocument/2006/relationships"},
          {"r:id", "rId1"},
          {"count", std::to_string(rows.size())}});
  for (const std::vector<Value>& row : rows) {
    x.Open("r");
    for (size_t c = 0; c < row.size() && c < pt.fields.size(); ++c) {
      const Value& v = row[c];
      switch (v.kind) {
        case Value::Kind::Empty:
          x.Leaf("m");
          break;
        case Value::Kind::Number:
          x.Leaf("n", {{"v", NumText(v.number)}});
          break;
        case Value::Kind::Bool:
          x.Leaf("b", {{"v", v.boolean ? "1" : "0"}});
          break;
        case Value::Kind::Error:
          x.Leaf("e", {{"v", "#VALUE!"}});
          break;
        case Value::Kind::Text: {
          // 字符串字段用共享项下标引用；缓存里没有时退回内联 <s>。
          int idx = -1;
          const std::vector<std::string>& items = pt.fields[c].shared_items;
          for (size_t k = 0; k < items.size(); ++k) {
            if (items[k] == v.text) {
              idx = static_cast<int>(k);
              break;
            }
          }
          if (idx >= 0) {
            x.Leaf("x", {{"v", std::to_string(idx)}});
          } else {
            x.Leaf("s", {{"v", v.text}});
          }
          break;
        }
      }
    }
    x.Close();
  }
  x.Close();
  return x.str();
}

}  // namespace

void Save(const Workbook& book, Stream& out, const SaveOptions& opts) {
  const Workbook* wb = &book;
  std::unique_ptr<Workbook> recalculated;
  if (opts.recalculate) {
    recalculated = std::make_unique<Workbook>(book.Clone());  // 不改动调用方的工作簿
    recalculated->Recalculate(opts.cancel);
    wb = recalculated.get();
  }

  const StylePlan styles = PlanStyles(*wb);
  const bool has_strings = !wb->shared_strings.empty();

  // 收集写回用的 dxf 表：以工作簿原表为起点，再把各规则里"按值存在但没登记"的
  // dxf 去重补进来，保证每条 cfRule 都能拿到一个稳定的 dxfId。
  std::vector<CfDxf> out_dxfs = wb->dxfs;
  for (int i = 0; i < wb->sheet_count(); ++i) {
    const Worksheet* ws = wb->SheetAt(i);
    if (ws == nullptr) continue;
    for (const ConditionalFormat& cf : ws->conditional()) {
      for (const ConditionalRule& rule : cf.rules) {
        if (FindDxf(out_dxfs, rule.dxf) >= 0) continue;
        if (rule.dxf.fill < 0 && rule.dxf.font_color < 0 && !rule.dxf.bold) continue;
        out_dxfs.push_back(rule.dxf);
      }
    }
  }

  // ---- 数据透视表：只有被刷新过（或新建）的表才重新生成部件 ----------
  //
  // 读入但没动过的表，原部件留在 unknown_parts 里按字节写回——透视表的 XML
  // 里有大量我们没建模的布局开关，重新生成必然丢东西。一旦刷新过，DOM 才是
  // 权威，这时要把原部件从透传列表里剔掉，否则会出现两份同名部件。
  //
  // 这里先把 URI 算齐（workbook.xml 的 <pivotCaches> 与 xl/_rels/workbook.xml.rels
  // 都要引用它们），再按需生成部件内容。
  std::set<std::string> regenerated;
  std::vector<std::string> pivot_table_uris;   // 与 wb->pivot_tables 一一对应
  std::vector<std::string> pivot_cache_uris;
  for (size_t i = 0; i < wb->pivot_tables.size(); ++i) {
    const PivotTable& pt = wb->pivot_tables[i];
    const std::string suffix = std::to_string(i + 1);
    if (!pt.dirty) {
      pivot_table_uris.push_back(pt.part_uri);
      // 缓存部件没有被重新生成，仍按原路径透传；但要记住它，否则 workbook.xml
      // 里补不回去，Excel 会把这组透视表当成无源缓存。
      pivot_cache_uris.push_back(pt.cache_uri);
      continue;
    }
    const std::string table_uri = "/xl/pivotTables/pivotTable" + suffix + ".xml";
    const std::string cache_uri = "/xl/pivotCache/pivotCacheDefinition" + suffix + ".xml";
    const std::string records_uri = "/xl/pivotCache/pivotCacheRecords" + suffix + ".xml";
    pivot_table_uris.push_back(table_uri);
    pivot_cache_uris.push_back(cache_uri);
    // 原来的三个部件（如果有）都不要再写。
    if (!pt.part_uri.empty()) regenerated.insert(pt.part_uri);
    regenerated.insert(table_uri);
    regenerated.insert(cache_uri);
    regenerated.insert(records_uri);
  }

  std::vector<PartOut> parts;
  parts.push_back({"/xl/workbook.xml",
                   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml",
                   WorkbookXml(*wb, pivot_cache_uris, FirstPivotCacheRid(*wb, has_strings))});
  for (int i = 0; i < wb->sheet_count(); ++i) {
    const Worksheet* ws = wb->SheetAt(i);
    if (ws == nullptr) continue;
    parts.push_back({"/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml",
                     "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml",
                     SheetXml(*wb, *ws, out_dxfs)});
  }
  if (has_strings) {
    parts.push_back({"/xl/sharedStrings.xml",
                     "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml",
                     SharedStringsXml(*wb)});
  }
  parts.push_back({"/xl/styles.xml",
                   "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml",
                   StylesXml(styles, out_dxfs)});
  for (size_t i = 0; i < wb->pivot_tables.size(); ++i) {
    const PivotTable& pt = wb->pivot_tables[i];
    if (!pt.dirty) continue;
    const std::string records_uri = "/xl/pivotCache/pivotCacheRecords" +
                                    std::to_string(i + 1) + ".xml";
    parts.push_back({pivot_table_uris[i], kCtPivotTable, PivotTableXml(pt, static_cast<int>(i) + 1)});
    parts.push_back({pivot_cache_uris[i], kCtPivotCache, PivotCacheXml(pt)});
    parts.push_back({records_uri, kCtPivotRecords, PivotRecordsXml(*wb, pt)});
  }
  for (const UnknownPart& unknown : wb->unknown_parts) {
    if (regenerated.count(unknown.uri) != 0) continue;
    parts.push_back({unknown.uri, unknown.mime,
                     std::string(unknown.bytes.begin(), unknown.bytes.end())});
  }

  if (opts.cancel != nullptr) opts.cancel->ThrowIfCancelled();
  std::vector<ZipWriteEntry> entries;
  entries.push_back({"[Content_Types].xml", StrBytes(ContentTypesXml(parts))});
  entries.push_back({"_rels/.rels", StrBytes(RootRelsXml())});
  entries.push_back({"xl/_rels/workbook.xml.rels",
                     StrBytes(WorkbookRelsXml(*wb, has_strings, pivot_cache_uris))});
  for (const PartOut& part : parts) {
    entries.push_back({PartNameFor(part.uri), StrBytes(part.data)});
  }
  // 透传部件自带的关系：图表要能指到调色板/样式，绘图要能指到图表。
  for (const UnknownPart& unknown : wb->unknown_parts) {
    if (regenerated.count(unknown.uri) != 0) continue;
    if (unknown.rels.empty()) continue;
    entries.push_back({PartNameFor(RelsPathFor(unknown.uri)),
                       StrBytes(PreservedRelsXml(unknown.rels, PartDirectory(unknown.uri)))});
  }
  // 透视表的部件关系：pivotTable -> cacheDefinition -> records。三个部件之间
  // 的链条断一环，Excel 就会把透视表当成损坏的缓存。
  for (size_t i = 0; i < wb->pivot_tables.size(); ++i) {
    const PivotTable& pt = wb->pivot_tables[i];
    if (!pt.dirty) continue;  // 未重新生成的关系已经随 unknown_parts 透传
    const std::string table_uri = pivot_table_uris[i];
    const std::string cache_uri = pivot_cache_uris[i];
    const std::string records_uri = "/xl/pivotCache/pivotCacheRecords" +
                                    std::to_string(i + 1) + ".xml";
    {
      std::vector<UnknownRel> rels;
      UnknownRel r;
      r.id = "rId1";
      r.type = kRelPivotCache;
      r.target = cache_uri;
      rels.push_back(std::move(r));
      entries.push_back({PartNameFor(RelsPathFor(table_uri)),
                         StrBytes(PreservedRelsXml(rels, PartDirectory(table_uri)))});
    }
    {
      std::vector<UnknownRel> rels;
      UnknownRel r;
      r.id = "rId1";
      r.type = kRelPivotRecords;
      r.target = records_uri;
      rels.push_back(std::move(r));
      entries.push_back({PartNameFor(RelsPathFor(cache_uri)),
                         StrBytes(PreservedRelsXml(rels, PartDirectory(cache_uri)))});
    }
  }
  // 工作表 -> _rels：外部超链接（rId1..N）+ 绘图部件（rId N+1）。顺序必须与
  // SheetXml 里分配 r:id 的顺序一致。
  for (int i = 0; i < wb->sheet_count(); ++i) {
    const Worksheet* ws = wb->SheetAt(i);
    if (ws == nullptr) continue;
    std::string drawing_uri;
    for (const SheetDrawing& sd : wb->drawings) {
      if (sd.sheet == ws->name()) drawing_uri = sd.part_uri;
    }
    std::vector<UnknownRel> rels;
    int rid = 0;
    for (const Hyperlink& link : ws->hyperlinks()) {
      if (!link.range.valid() || link.target.empty()) continue;
      UnknownRel rel;
      rel.id = "rId" + std::to_string(++rid);
      rel.type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
      rel.target = link.target;
      rel.external = true;
      rels.push_back(std::move(rel));
    }
    if (!drawing_uri.empty()) {
      UnknownRel drawing;
      drawing.id = "rId" + std::to_string(++rid);
      drawing.type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/drawing";
      drawing.target = drawing_uri;
      rels.push_back(std::move(drawing));
    }
    // 透视表只靠关系挂在工作表上（工作表 XML 里没有 r:id 引用），所以这条
    // 关系丢了就等于整张透视表丢了。
    for (size_t p = 0; p < wb->pivot_tables.size(); ++p) {
      if (wb->pivot_tables[p].sheet != ws->name()) continue;
      if (pivot_table_uris[p].empty()) continue;
      UnknownRel pivot;
      pivot.id = "rId" + std::to_string(++rid);
      pivot.type = kRelPivotTable;
      pivot.target = pivot_table_uris[p];
      rels.push_back(std::move(pivot));
    }
    if (rels.empty()) continue;
    const std::string sheet_uri = "/xl/worksheets/sheet" + std::to_string(i + 1) + ".xml";
    entries.push_back({PartNameFor(RelsPathFor(sheet_uri)),
                       StrBytes(PreservedRelsXml(rels, PartDirectory(sheet_uri)))});
  }
  ZipWriteToStream(out, entries);
}

void SaveToFile(const Workbook& book, const std::string& path, const SaveOptions& opts) {
  MemoryStream mem;
  Save(book, mem, opts);
  WriteFileAtomically(path, mem.data());
}

// ---- 导出 ---------------------------------------------------------------

namespace {

const CellStyle& StyleFor(const Worksheet& ws, const Cell* cell) {
  static const CellStyle kDefault{};
  if (cell == nullptr || cell->style_id < 0) return kDefault;
  const Workbook* owner = ws.owner();
  if (owner == nullptr) return kDefault;
  return owner->StyleAt(cell->style_id);
}

std::string DisplayOf(const Worksheet& ws, int row, int col) {
  const Cell* cell = ws.Find(row, col);
  if (cell == nullptr) return std::string();
  const Value value = ws.GetValue(row, col);
  const CellStyle& style = StyleFor(ws, cell);
  const Workbook* owner = ws.owner();
  return FormatValue(value, style.num_fmt, owner != nullptr && owner->date1904);
}

std::string CsvField(const std::string& text, char delimiter) {
  bool needs = false;
  for (char c : text) {
    if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
      needs = true;
      break;
    }
  }
  if (!needs) return text;
  std::string out = "\"";
  for (char c : text) {
    if (c == '"') out += "\"\"";
    else out.push_back(c);
  }
  out += '"';
  return out;
}

std::string HtmlEsc(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "<br/>"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

}  // namespace

std::string ToCsv(const Worksheet& ws, char delimiter) {
  const Range used = ws.UsedRange();
  std::string out;
  for (int row = used.row1; row <= used.row2; ++row) {
    for (int col = used.col1; col <= used.col2; ++col) {
      if (col != used.col1) out.push_back(delimiter);
      out += CsvField(DisplayOf(ws, row, col), delimiter);
    }
    out += "\r\n";
  }
  return out;
}

std::string ToHtml(const Workbook& book) {
  std::string out;
  out += "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"/><title>";
  out += HtmlEsc(book.sheet_count() > 0 ? book.SheetAt(0)->name() : std::string("Workbook"));
  out += "</title><style>table{border-collapse:collapse;margin:0 0 24px 0;font-family:Calibri,Arial,sans-serif;"
         "font-size:11pt}td,th{border:1px solid #ccc;padding:2px 6px;vertical-align:bottom}"
         "h2{font-size:12pt;margin:8px 0}</style></head><body>\n";
  for (int i = 0; i < book.sheet_count(); ++i) {
    const Worksheet* ws = book.SheetAt(i);
    if (ws == nullptr) continue;
    out += "<h2>" + HtmlEsc(ws->name()) + "</h2>\n<table>\n";
    const Range used = ws->UsedRange();
    for (int row = used.row1; row <= used.row2; ++row) {
      out += "<tr>";
      for (int col = used.col1; col <= used.col2; ++col) {
        // 被合并覆盖的单元格不再输出，避免表格错位。
        bool covered = false;
        int rowspan = 1;
        int colspan = 1;
        for (const Range& merge : ws->merges()) {
          if (row < merge.row1 || row > merge.row2 || col < merge.col1 || col > merge.col2) continue;
          if (row == merge.row1 && col == merge.col1) {
            rowspan = merge.row2 - merge.row1 + 1;
            colspan = merge.col2 - merge.col1 + 1;
          } else {
            covered = true;
          }
          break;
        }
        if (covered) continue;
        const Cell* cell = ws->Find(row, col);
        const CellStyle& style = StyleFor(*ws, cell);
        std::string attrs;
        if (rowspan > 1) attrs += " rowspan=\"" + std::to_string(rowspan) + "\"";
        if (colspan > 1) attrs += " colspan=\"" + std::to_string(colspan) + "\"";
        std::string css;
        if (style.halign == 2) css += "text-align:center;";
        else if (style.halign == 3) css += "text-align:right;";
        if (style.font.bold) css += "font-weight:bold;";
        if (style.font.italic) css += "font-style:italic;";
        if (style.fill >= 0) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(style.fill & 0xFFFFFF));
          css += std::string("background:") + buf + ";";
        }
        if (!css.empty()) attrs += " style=\"" + css + "\"";
        out += "<td" + attrs + ">" + HtmlEsc(DisplayOf(*ws, row, col)) + "</td>";
      }
      out += "</tr>\n";
    }
    out += "</table>\n";
  }
  out += "</body></html>\n";
  return out;
}

}  // namespace sheet
}  // namespace doccore
