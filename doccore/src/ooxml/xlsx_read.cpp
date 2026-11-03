// DocCore - XLSX -> sheet DOM reader. Design refs: docs/02 §3.4.1
//
// Part tree:
//   xl/workbook.xml        -> 工作表列表、定义名称、date1904
//   xl/sharedStrings.xml   -> 共享字符串池
//   xl/styles.xml          -> 字体/填充/边框/cellXfs -> CellStyle
//   xl/worksheets/*.xml    -> 单元格、合并、列宽行高、打印设置
//
// 共享公式（<f t="shared" si="n"/>）只写一次公式文本，其余单元格是空元素；
// 读的时候按相对偏移重写引用，否则从属单元格会指回主单元格的地址。
#include "doccore/xlsx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "doccore/opc.h"
#include "doccore/xml.h"

namespace doccore {
namespace sheet {

namespace {

using doccore::ErrorCode;
using doccore::WarningCode;
using doccore::XmlEvent;
using doccore::XmlEventType;
using doccore::XmlReader;

// ---- 事件流 -------------------------------------------------------------

class XmlWalker {
 public:
  explicit XmlWalker(std::string_view xml) : reader_(xml) {}

  bool Next(XmlEvent& ev) {
    const bool ok = reader_.Next(ev);
    if (ok) {
      if (ev.type == XmlEventType::StartElement) {
        ++open_;
      } else if (ev.type == XmlEventType::EndElement) {
        --open_;
      }
    }
    return ok;
  }

  int open() const noexcept { return open_; }

 private:
  XmlReader reader_;
  int open_ = 0;
};

template <class F>
void WalkChildren(XmlWalker& w, const XmlEvent& start, F on_child) {
  const int mine = w.open();
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) return;
    if (ev.type == XmlEventType::StartElement && w.open() == mine + 1) on_child(ev);
  }
  throw Error(ErrorCode::Corrupt, "xlsx: unterminated <" + std::string(start.name) + ">");
}

std::string ElemText(XmlWalker& w, const XmlEvent& /*start*/) {
  const int mine = w.open();
  std::string out;
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) break;
    if (ev.type == XmlEventType::Text) out.append(ev.text);
  }
  return out;
}

const std::string_view* Attr(const XmlEvent& ev, std::string_view name) {
  for (const XmlAttribute& a : ev.attributes) {
    if (a.name == name) return &a.value;
  }
  return nullptr;
}

std::string AttrOr(const XmlEvent& ev, std::string_view name, const std::string& fallback) {
  const std::string_view* v = Attr(ev, name);
  return v == nullptr ? fallback : std::string(*v);
}

int AttrInt(const XmlEvent& ev, std::string_view name, int fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr || v->empty()) return fallback;
  return std::atoi(std::string(*v).c_str());
}

double AttrDouble(const XmlEvent& ev, std::string_view name, double fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr || v->empty()) return fallback;
  return std::strtod(std::string(*v).c_str(), nullptr);
}

bool AttrBool(const XmlEvent& ev, std::string_view name, bool fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr) return fallback;
  return *v == "1" || *v == "true";
}

std::string_view LocalName(std::string_view name) {
  const size_t pos = name.rfind(':');
  return pos == std::string_view::npos ? name : name.substr(pos + 1);
}

bool NameIs(const XmlEvent& ev, std::string_view local) { return LocalName(ev.name) == local; }

std::string AsUtf8(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// "FF00FF00" / "00FF00" -> 0x00FF00；主题色等非 rgb 属性返回 -1。
int ParseColor(const std::string_view* rgb) {
  if (rgb == nullptr || rgb->size() < 6) return -1;
  const std::string hex(rgb->substr(rgb->size() - 6));
  char* end = nullptr;
  const long value = std::strtol(hex.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') return -1;
  return static_cast<int>(value & 0xFFFFFF);
}

// ---- 共享公式的相对引用平移 ---------------------------------------------

bool IsRefLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsRefDigit(char c) { return c >= '0' && c <= '9'; }

// 把公式文本里所有相对 A1 引用按 (drow, dcol) 平移；跳过字符串与引号表名。
std::string ShiftFormula(const std::string& text, int drow, int dcol) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"') {  // 字符串字面量原样复制
      out.push_back(c);
      ++i;
      while (i < text.size()) {
        out.push_back(text[i]);
        if (text[i] == '"') {
          if (i + 1 < text.size() && text[i + 1] == '"') {
            out.push_back(text[i + 1]);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '\'') {  // 引号表名
      out.push_back(c);
      ++i;
      while (i < text.size()) {
        out.push_back(text[i]);
        if (text[i] == '\'') {
          if (i + 1 < text.size() && text[i + 1] == '\'') {
            out.push_back(text[i + 1]);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    const bool col_abs = c == '$';
    const size_t start = i + (col_abs ? 1 : 0);
    if (start < text.size() && IsRefLetter(text[start])) {
      size_t j = start;
      int col = 0;
      while (j < text.size() && IsRefLetter(text[j]) && (j - start) < 3) {
        const char u = text[j] >= 'a' ? static_cast<char>(text[j] - 'a' + 'A') : text[j];
        col = col * 26 + (u - 'A' + 1);
        ++j;
      }
      const bool row_abs = j < text.size() && text[j] == '$';
      const size_t digit_start = j + (row_abs ? 1 : 0);
      size_t k = digit_start;
      int row = 0;
      while (k < text.size() && IsRefDigit(text[k])) {
        row = row * 10 + (text[k] - '0');
        ++k;
      }
      const bool trailing_ok =
          k >= text.size() || !(IsRefLetter(text[k]) || IsRefDigit(text[k]) || text[k] == '_' ||
                                text[k] == '(' || text[k] == '.');
      if (col > 0 && row > 0 && k > digit_start && trailing_ok) {
        const int new_col = col_abs ? col : col + dcol;
        const int new_row = row_abs ? row : row + drow;
        if (new_col < 1 || new_row < 1) {
          out += "#REF!";
        } else {
          if (col_abs) out.push_back('$');
          out += ColumnName(new_col - 1);
          if (row_abs) out.push_back('$');
          out += std::to_string(new_row);
        }
        i = k;
        continue;
      }
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

}  // namespace

// ---- 样式 ---------------------------------------------------------------

namespace {

struct StyleTables {
  std::vector<CellFont> fonts;
  std::vector<int> fills;
  std::vector<int> borders;
  std::map<int, std::string> custom_formats;
  std::vector<CellStyle> xfs;
  std::vector<CfDxf> dxfs;  // 条件格式引用的差异格式
};

CellFont ReadFont(XmlWalker& w, const XmlEvent& start) {
  CellFont font;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "sz") {
      font.size_pt = AttrDouble(child, "val", font.size_pt);
      ElemText(w, child);
    } else if (name == "name") {
      font.name = AttrOr(child, "val", font.name);
      ElemText(w, child);
    } else if (name == "color") {
      font.color = ParseColor(Attr(child, "rgb"));
      ElemText(w, child);
    } else if (name == "b") {
      font.bold = AttrBool(child, "val", true);
      ElemText(w, child);
    } else if (name == "i") {
      font.italic = AttrBool(child, "val", true);
      ElemText(w, child);
    } else if (name == "u") {
      font.underline = AttrOr(child, "val", "single") != "none";
      ElemText(w, child);
    } else if (name == "strike") {
      font.strike = AttrBool(child, "val", true);
      ElemText(w, child);
    }
  });
  return font;
}

int ReadFill(XmlWalker& w, const XmlEvent& start) {
  int color = -1;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    if (LocalName(child.name) != "patternFill") return;
    const std::string type = AttrOr(child, "patternType", "none");
    WalkChildren(w, child, [&](const XmlEvent& paint) {
      if (LocalName(paint.name) != "fgColor") return;
      if (type != "none" && type != "solid") return;
      const int parsed = ParseColor(Attr(paint, "rgb"));
      if (parsed >= 0) color = parsed;
    });
  });
  return color;
}

int ReadBorder(XmlWalker& w, const XmlEvent& start) {
  int mask = 0;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    int bit = -1;
    if (name == "top") bit = 1 << 0;
    else if (name == "right") bit = 1 << 1;
    else if (name == "bottom") bit = 1 << 2;
    else if (name == "left") bit = 1 << 3;
    if (bit < 0) return;
    const std::string style = AttrOr(child, "style", "");
    if (!style.empty() && style != "none") mask |= bit;
  });
  return mask;
}

// dxf（差异格式）：条件格式命中后叠加的格式。填充色在 bgColor 里（不是 fgColor），
// 这是 OOXML 的历史约定，Excel 写出的高亮底色都在 bgColor。
CfDxf ReadDxf(XmlWalker& w, const XmlEvent& start) {
  CfDxf dxf;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "font") {
      WalkChildren(w, child, [&](const XmlEvent& f) {
        const std::string_view fn = LocalName(f.name);
        if (fn == "color") dxf.font_color = ParseColor(Attr(f, "rgb"));
        else if (fn == "b") dxf.bold = AttrBool(f, "val", true);
      });
    } else if (name == "fill") {
      WalkChildren(w, child, [&](const XmlEvent& pf) {
        if (LocalName(pf.name) != "patternFill") return;
        WalkChildren(w, pf, [&](const XmlEvent& paint) {
          const std::string_view pn = LocalName(paint.name);
          if (pn == "bgColor") dxf.fill = ParseColor(Attr(paint, "rgb"));
          else if (pn == "fgColor" && dxf.fill < 0) dxf.fill = ParseColor(Attr(paint, "rgb"));
        });
      });
    }
  });
  return dxf;
}

StyleTables ReadStyles(const std::string& xml) {
  StyleTables tables;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "fonts") {
      WalkChildren(w, ev, [&](const XmlEvent& child) {
        if (!NameIs(child, "font")) return;
        tables.fonts.push_back(ReadFont(w, child));
      });
    } else if (name == "fills") {
      WalkChildren(w, ev, [&](const XmlEvent& child) {
        if (!NameIs(child, "fill")) return;
        tables.fills.push_back(ReadFill(w, child));
      });
    } else if (name == "borders") {
      WalkChildren(w, ev, [&](const XmlEvent& child) {
        if (!NameIs(child, "border")) return;
        tables.borders.push_back(ReadBorder(w, child));
      });
    } else if (name == "numFmts") {
      WalkChildren(w, ev, [&](const XmlEvent& child) {
        if (!NameIs(child, "numFmt")) return;
        tables.custom_formats[AttrInt(child, "numFmtId", 0)] =
            AttrOr(child, "formatCode", std::string());
      });
    } else if (name == "cellXfs") {
      WalkChildren(w, ev, [&](const XmlEvent& xf) {
        if (!NameIs(xf, "xf")) return;
        CellStyle style;
        const int num_fmt_id = AttrInt(xf, "numFmtId", 0);
        style.num_fmt_id = num_fmt_id;
        const int font_id = AttrInt(xf, "fontId", 0);
        if (font_id >= 0 && static_cast<size_t>(font_id) < tables.fonts.size()) {
          style.font = tables.fonts[static_cast<size_t>(font_id)];
        }
        const int fill_id = AttrInt(xf, "fillId", 0);
        if (fill_id >= 0 && static_cast<size_t>(fill_id) < tables.fills.size()) {
          style.fill = tables.fills[static_cast<size_t>(fill_id)];
        }
        const int border_id = AttrInt(xf, "borderId", 0);
        if (border_id >= 0 && static_cast<size_t>(border_id) < tables.borders.size()) {
          style.border_mask = tables.borders[static_cast<size_t>(border_id)];
        }
        WalkChildren(w, xf, [&](const XmlEvent& child) {
          if (!NameIs(child, "alignment")) return;
          const std::string h = AttrOr(child, "horizontal", "");
          if (h == "left" || h == "centerContinuous") style.halign = 1;
          else if (h == "center") style.halign = 2;
          else if (h == "right") style.halign = 3;
          else if (h == "general") style.halign = 0;
          const std::string v = AttrOr(child, "vertical", "");
          if (v == "top") style.valign = 0;
          else if (v == "center") style.valign = 1;
          else if (v == "bottom") style.valign = 2;
          style.wrap = AttrBool(child, "wrapText", false);
        });
        tables.xfs.push_back(style);
      });
    } else if (name == "dxfs") {
      WalkChildren(w, ev, [&](const XmlEvent& dxf) {
        if (!NameIs(dxf, "dxf")) return;
        tables.dxfs.push_back(ReadDxf(w, dxf));
      });
    }
  }
  // 数字格式：自定义优先，其次内置表。
  for (CellStyle& style : tables.xfs) {
    auto it = tables.custom_formats.find(style.num_fmt_id);
    if (it != tables.custom_formats.end()) {
      style.num_fmt = ParseNumberFormat(it->second);
    } else {
      const std::string_view builtin = BuiltinNumberFormat(style.num_fmt_id);
      style.num_fmt = builtin.empty() ? NumberFormat{} : ParseNumberFormat(builtin);
    }
  }
  return tables;
}

}  // namespace

// ---- 工作表 -------------------------------------------------------------

namespace {

struct SharedFormula {
  std::string text;
  int row = 0;
  int col = 0;
};

Value ParseCellValue(const std::string& type, const std::string& raw, Workbook& wb) {
  if (type == "s") {
    const long index = std::strtol(raw.c_str(), nullptr, 10);
    return Value::Str(wb.StringAt(static_cast<int32_t>(index)));
  }
  if (type == "str" || type == "inlineStr") return Value::Str(raw);
  if (type == "b") return Value::Boolean(raw == "1" || raw == "true");
  if (type == "e") return Value::Err(CellErrorFromText(raw));
  if (raw.empty()) return Value::Empty();
  return Value::Num(std::strtod(raw.c_str(), nullptr));
}

void ApplyCellValue(Cell& cell, const std::string& type, const std::string& raw, Workbook& wb) {
  if (type == "s") {
    const long index = std::strtol(raw.c_str(), nullptr, 10);
    cell.kind = CellKind::Text;
    cell.text_id = static_cast<int32_t>(index);
    cell.cached = Value::Str(wb.StringAt(static_cast<int32_t>(index)));
    return;
  }
  if (type == "str") {
    cell.kind = CellKind::Text;
    cell.text_id = wb.InternString(raw);
    cell.cached = Value::Str(raw);
    return;
  }
  if (type == "b") {
    cell.kind = CellKind::Bool;
    cell.boolean = raw == "1" || raw == "true";
    cell.cached = Value::Boolean(cell.boolean);
    return;
  }
  if (type == "e") {
    cell.kind = CellKind::Error;
    cell.error = CellErrorFromText(raw);
    cell.cached = Value::Err(cell.error);
    return;
  }
  if (raw.empty()) {
    cell.kind = CellKind::Empty;
    cell.cached = Value::Empty();
    return;
  }
  cell.kind = CellKind::Number;
  cell.number = std::strtod(raw.c_str(), nullptr);
  cell.cached = Value::Num(cell.number);
}

void ReadSheetData(XmlWalker& w, const XmlEvent& start, Workbook& wb, Worksheet& ws) {
  std::map<int, SharedFormula> shared;
  int sequential_row = 0;
  WalkChildren(w, start, [&](const XmlEvent& row_ev) {
    if (!NameIs(row_ev, "row")) return;
    int row = AttrInt(row_ev, "r", sequential_row + 1) - 1;
    if (row < 0) row = 0;
    sequential_row = row + 1;
    const bool has_height = AttrBool(row_ev, "customHeight", false);
    const double height = AttrDouble(row_ev, "ht", 0.0);
    if (has_height && height > 0.0) ws.SetRowHeight(row, height);

    int sequential_col = -1;
    WalkChildren(w, row_ev, [&](const XmlEvent& cell_ev) {
      if (!NameIs(cell_ev, "c")) return;
      int col = sequential_col + 1;
      int cell_row = row;
      CellRef ref;
      const std::string_view* ref_attr = Attr(cell_ev, "r");
      if (ref_attr != nullptr && ParseCellName(*ref_attr, &ref)) {
        col = ref.col;
        cell_row = ref.row;  // 单元格自己的 r 属性优先
      }
      sequential_col = col;
      const int style_id = AttrInt(cell_ev, "s", -1);
      const std::string type = AttrOr(cell_ev, "t", "n");

      std::string formula_text;
      std::string shared_attr;
      int shared_index = -1;
      std::string value_text;
      std::string inline_text;

      WalkChildren(w, cell_ev, [&](const XmlEvent& child) {
        const std::string_view name = LocalName(child.name);
        if (name == "f") {
          formula_text = ElemText(w, child);
          shared_attr = AttrOr(child, "t", "");
          shared_index = AttrInt(child, "si", -1);
        } else if (name == "v") {
          value_text = ElemText(w, child);
        } else if (name == "is") {
          // 富文本内联串：把所有 <t> 拼起来。
          WalkChildren(w, child, [&](const XmlEvent& t) {
            if (LocalName(t.name) == "t") inline_text += ElemText(w, t);
          });
        }
      });

      Cell& cell = ws.CellAt(cell_row, col);
      cell.style_id = style_id;
      if (!formula_text.empty() || shared_index >= 0) {
        if (shared_attr == "shared" && shared_index >= 0) {
          if (!formula_text.empty()) {
            shared[shared_index] = SharedFormula{formula_text, row, col};
          } else {
            auto it = shared.find(shared_index);
            if (it != shared.end()) {
              formula_text = ShiftFormula(it->second.text, cell_row - it->second.row,
                                          col - it->second.col);
            }
          }
        }
        if (!formula_text.empty()) {
          const int32_t id = ws.SetFormula(cell_row, col, formula_text);
          cell.cached = ParseCellValue(type == "inlineStr" ? "str" : type, value_text, wb);
          cell.dirty = false;
          (void)id;
        }
      } else if (!inline_text.empty() || type == "inlineStr") {
        cell.kind = CellKind::Text;
        cell.text_id = wb.InternString(inline_text);
        cell.cached = Value::Str(inline_text);
      } else {
        ApplyCellValue(cell, type, value_text, wb);
      }
      if (cell.kind == CellKind::Empty && cell.formula_id < 0 && value_text.empty()) {
        // 只有样式没有值的单元格：保留样式，值仍是空。
        cell.kind = CellKind::Empty;
      }
    });
  });
}

CfType ParseCfType(std::string_view s) {
  if (s == "cellIs") return CfType::CellIs;
  if (s == "expression") return CfType::Expression;
  return CfType::Unknown;  // colorScale/dataBar/iconSet 等 P9 不渲染，按未知跳过
}

CfOperator ParseCfOperator(std::string_view s) {
  if (s == "lessThan") return CfOperator::LessThan;
  if (s == "lessThanOrEqual") return CfOperator::LessEqual;
  if (s == "equal") return CfOperator::Equal;
  if (s == "notEqual") return CfOperator::NotEqual;
  if (s == "greaterThanOrEqual") return CfOperator::GreaterEqual;
  if (s == "greaterThan") return CfOperator::GreaterThan;
  if (s == "between") return CfOperator::Between;
  if (s == "notBetween") return CfOperator::NotBetween;
  return CfOperator::None;
}

void ReadConditionalFormatting(XmlWalker& w, const XmlEvent& start, const Workbook& wb,
                               Worksheet& ws) {
  // sqref 形如 "A1:C5"，也可能是多段（"A1:C5 D1:F5"）；P9 只取第一段。
  std::string sqref = AttrOr(start, "sqref", "");
  const size_t sp = sqref.find(' ');
  if (sp != std::string::npos) sqref = sqref.substr(0, sp);
  ConditionalFormat cf;
  const size_t colon = sqref.find(':');
  CellRef a;
  CellRef b;
  if (colon != std::string::npos) {
    if (!ParseCellName(sqref.substr(0, colon), &a)) return;
    if (!ParseCellName(sqref.substr(colon + 1), &b)) return;
    cf.range = Range{a.row, a.col, b.row, b.col};
  } else {
    if (!ParseCellName(sqref, &a)) return;
    cf.range = Range{a.row, a.col, a.row, a.col};
  }
  WalkChildren(w, start, [&](const XmlEvent& rule) {
    if (!NameIs(rule, "cfRule")) return;
    ConditionalRule r;
    r.type = ParseCfType(AttrOr(rule, "type", "cellIs"));
    r.op = ParseCfOperator(AttrOr(rule, "operator", "equal"));
    r.priority = AttrInt(rule, "priority", 1);
    const int dxf_id = AttrInt(rule, "dxfId", -1);
    if (dxf_id >= 0 && static_cast<size_t>(dxf_id) < wb.dxfs.size()) {
      r.dxf_id = dxf_id;
      r.dxf = wb.dxfs[static_cast<size_t>(dxf_id)];
    }
    WalkChildren(w, rule, [&](const XmlEvent& f) {
      if (!NameIs(f, "formula")) return;
      const std::string text = ElemText(w, f);
      if (r.type == CfType::Expression) {
        r.formula = text;
      } else {
        // cellIs 的阈值写成数值公式；非数值操作数（如单元格引用）跳过。
        try {
          r.operands.push_back(std::stod(text));
        } catch (...) {
        }
      }
    });
    if (r.type != CfType::Unknown) cf.rules.push_back(r);
  });
  if (!cf.rules.empty() && cf.range.valid()) ws.conditional().push_back(cf);
}

// F-205 数据验证：解析 <dataValidation>（sqref 可能多段、type/operator/公式等）。
static ValidationType ParseValidationType(std::string_view s) {
  if (s == "list") return ValidationType::List;
  if (s == "whole") return ValidationType::Whole;
  if (s == "decimal") return ValidationType::Decimal;
  if (s == "date") return ValidationType::Date;
  if (s == "textLength") return ValidationType::TextLength;
  if (s == "custom") return ValidationType::Custom;
  return ValidationType::Unknown;
}

static ValidationOp ParseValidationOp(std::string_view s) {
  if (s == "between") return ValidationOp::Between;
  if (s == "notBetween") return ValidationOp::NotBetween;
  if (s == "equal") return ValidationOp::Equal;
  if (s == "notEqual") return ValidationOp::NotEqual;
  if (s == "lessThan") return ValidationOp::LessThan;
  if (s == "lessThanOrEqual") return ValidationOp::LessEqual;
  if (s == "greaterThan") return ValidationOp::GreaterThan;
  if (s == "greaterThanOrEqual") return ValidationOp::GreaterEqual;
  return ValidationOp::None;
}

// 解析 sqref（可能多段，空格分隔），把每段追加到 out。
static void ParseDvSqref(std::string_view sqref, std::vector<Range>& out) {
  size_t start = 0;
  while (start < sqref.size()) {
    while (start < sqref.size() && sqref[start] == ' ') ++start;
    if (start >= sqref.size()) break;
    size_t end = sqref.find(' ', start);
    if (end == std::string_view::npos) end = sqref.size();
    std::string_view token = sqref.substr(start, end - start);
    const size_t colon = token.find(':');
    CellRef a, b;
    if (colon == std::string_view::npos) {
      if (ParseCellName(token, &a)) out.push_back(Range{a.row, a.col, a.row, a.col});
    } else {
      if (ParseCellName(token.substr(0, colon), &a) &&
          ParseCellName(token.substr(colon + 1), &b)) {
        out.push_back(Range{a.row, a.col, b.row, b.col});
      }
    }
    start = end + 1;
  }
}

void ReadDataValidations(XmlWalker& w, const XmlEvent& start, const Workbook& wb,
                         Worksheet& ws) {
  (void)wb;  // 读取期原样保留 formula，列表取值在求值/写回时再解析。
  WalkChildren(w, start, [&](const XmlEvent& dv) {
    if (!NameIs(dv, "dataValidation")) return;
    DataValidation rule;
    rule.type = ParseValidationType(AttrOr(dv, "type", "list"));
    rule.op = ParseValidationOp(AttrOr(dv, "operator", "between"));
    ParseDvSqref(AttrOr(dv, "sqref", ""), rule.ranges);
    rule.allow_blank = AttrInt(dv, "allowBlank", 1) != 0;
    rule.show_error = AttrInt(dv, "showErrorMessage", 1) != 0;
    const std::string es = AttrOr(dv, "errorStyle", "stop");
    rule.error_style = (es == "warning") ? 2 : (es == "information") ? 3 : 1;
    rule.error_title = AttrOr(dv, "errorTitle", "");
    rule.error_message = AttrOr(dv, "error", "");
    rule.prompt_title = AttrOr(dv, "promptTitle", "");
    rule.prompt_message = AttrOr(dv, "prompt", "");
    WalkChildren(w, dv, [&](const XmlEvent& f) {
      if (!NameIs(f, "formula1") && !NameIs(f, "formula2")) return;
      const std::string text = ElemText(w, f);
      if (NameIs(f, "formula1"))
        rule.formula1 = text;
      else
        rule.formula2 = text;
    });
    if (!rule.ranges.empty()) ws.validations().push_back(rule);
  });
}

// 按本地名取属性，忽略命名空间前缀（r:id 的 prefix 未必是 "r"）。
static std::string AttrLocal(const XmlEvent& ev, std::string_view local,
                            const std::string& fallback) {
  for (const XmlAttribute& a : ev.attributes) {
    if (LocalName(a.name) == local) return std::string(a.value);
  }
  return fallback;
}

// 超链接：<hyperlinks><hyperlink ref="A1:B2" r:id="rId1" location="..."
// display="..." tooltip="..."/></hyperlinks>。r:id 只在外部链接时出现，指向
// 工作表 _rels 里的 External 关系；location 用于文档内跳转。
void ReadHyperlinks(XmlWalker& w, const XmlEvent& start, Worksheet& ws,
                    const std::map<std::string, std::string>* rid_target) {
  WalkChildren(w, start, [&](const XmlEvent& h) {
    if (!NameIs(h, "hyperlink")) return;
    const std::string ref = AttrOr(h, "ref", "");
    CellRef a, b;
    const size_t colon = ref.find(':');
    Hyperlink link;
    if (colon == std::string::npos) {
      if (!ParseCellName(ref, &a)) return;
      link.range = Range{a.row, a.col, a.row, a.col};
    } else {
      if (!ParseCellName(ref.substr(0, colon), &a)) return;
      if (!ParseCellName(ref.substr(colon + 1), &b)) return;
      link.range = Range{a.row, a.col, b.row, b.col};
    }
    if (!link.range.valid()) return;
    link.display = AttrOr(h, "display", "");
    link.tooltip = AttrOr(h, "tooltip", "");
    link.location = AttrOr(h, "location", "");
    const std::string rid = AttrLocal(h, "id", "");
    if (!rid.empty() && rid_target != nullptr) {
      const auto it = rid_target->find(rid);
      if (it != rid_target->end()) link.target = it->second;
    }
    if (link.empty()) return;
    ws.hyperlinks().push_back(std::move(link));
  });
}

void ReadWorksheetXml(const std::string& xml, Workbook& wb, Worksheet& ws,
                      const std::map<std::string, std::string>* rid_target = nullptr) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "sheetData") {
      ReadSheetData(w, ev, wb, ws);
    } else if (name == "hyperlinks") {
      ReadHyperlinks(w, ev, ws, rid_target);
    } else if (name == "conditionalFormatting") {
      ReadConditionalFormatting(w, ev, wb, ws);
    } else if (name == "dataValidations") {
      ReadDataValidations(w, ev, wb, ws);
    } else if (name == "mergeCells") {
      WalkChildren(w, ev, [&](const XmlEvent& child) {
        if (!NameIs(child, "mergeCell")) return;
        const std::string ref = AttrOr(child, "ref", "");
        const size_t colon = ref.find(':');
        CellRef a;
        CellRef b;
        if (colon == std::string::npos) return;
        if (!ParseCellName(ref.substr(0, colon), &a)) return;
        if (!ParseCellName(ref.substr(colon + 1), &b)) return;
        ws.Merge(a.row, a.col, b.row, b.col);
      });
    } else if (name == "cols") {
      WalkChildren(w, ev, [&](const XmlEvent& col) {
        if (!NameIs(col, "col")) return;
        const int min_col = AttrInt(col, "min", 0);
        const int max_col = AttrInt(col, "max", 0);
        const double width = AttrDouble(col, "width", 0.0);
        if (width <= 0.0) return;
        for (int c = min_col; c <= max_col && c < 16384; ++c) {
          ws.SetColumnWidth(c - 1, width);
        }
      });
    } else if (name == "sheetFormatPr") {
      // 默认行高/列宽不单独落地：没有 ht/width 的行列沿用 Worksheet 的默认值。
    } else if (name == "pageMargins") {
      PrintSetup& print = ws.print();
      print.margin_left = static_cast<int>(std::lround(AttrDouble(ev, "left", 0.7) * 1440.0));
      print.margin_right = static_cast<int>(std::lround(AttrDouble(ev, "right", 0.7) * 1440.0));
      print.margin_top = static_cast<int>(std::lround(AttrDouble(ev, "top", 0.75) * 1440.0));
      print.margin_bottom = static_cast<int>(std::lround(AttrDouble(ev, "bottom", 0.75) * 1440.0));
    } else if (name == "pageSetup") {
      PrintSetup& print = ws.print();
      print.landscape = AttrOr(ev, "orientation", "portrait") == "landscape";
      print.fit_to_width = AttrInt(ev, "fitToWidth", print.fit_to_width);
      print.fit_to_height = AttrInt(ev, "fitToHeight", print.fit_to_height);
      const int paper = AttrInt(ev, "paperSize", 0);
      if (paper == 9) {  // A4
        print.paper_width_twips = 11906;
        print.paper_height_twips = 16838;
      }
    } else if (name == "rowBreaks" || name == "colBreaks") {
      // 分页符由布局阶段重新计算，读入时忽略。
    }
  }
}

}  // namespace

// ---- 工作簿 -------------------------------------------------------------

namespace {

struct SheetRef {
  std::string name;
  std::string rid;
};

struct ParsedWorkbook {
  std::vector<SheetRef> sheets;
  std::vector<DefinedName> names;
  bool date1904 = false;
  std::string active_sheet;
};

ParsedWorkbook ReadWorkbookXml(const std::string& xml) {
  ParsedWorkbook out;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "workbookPr") {
      out.date1904 = AttrBool(ev, "date1904", false);
    } else if (name == "workbookView") {
      const int active = AttrInt(ev, "activeTab", 0);
      (void)active;
    } else if (name == "sheets") {
      WalkChildren(w, ev, [&](const XmlEvent& sheet) {
        if (!NameIs(sheet, "sheet")) return;
        SheetRef ref;
        ref.name = AttrOr(sheet, "name", "Sheet");
        for (const XmlAttribute& a : sheet.attributes) {
          if (LocalName(a.name) == "id") ref.rid = std::string(a.value);
        }
        out.sheets.push_back(ref);
      });
    } else if (name == "definedNames") {
      WalkChildren(w, ev, [&](const XmlEvent& dn) {
        if (!NameIs(dn, "definedName")) return;
        DefinedName item;
        item.name = AttrOr(dn, "name", "");
        item.refers_to = ElemText(w, dn);
        if (!item.name.empty()) out.names.push_back(item);
      });
    }
  }
  return out;
}

void ReadSharedStrings(const std::string& xml, Workbook& wb) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement || !NameIs(ev, "si")) continue;
    std::string text;
    WalkChildren(w, ev, [&](const XmlEvent& child) {
      const std::string_view name = LocalName(child.name);
      if (name == "t") {
        text += ElemText(w, child);
      } else if (name == "r") {
        WalkChildren(w, child, [&](const XmlEvent& run) {
          if (LocalName(run.name) == "t") text += ElemText(w, run);
        });
      }
    });
    wb.shared_strings.push_back(text);
  }
}

bool IsConsumedPart(const std::string& uri, const std::set<std::string>& sheet_parts,
                    const std::string& workbook_part, const std::string& strings_part,
                    const std::string& styles_part) {
  if (uri == workbook_part || uri == strings_part || uri == styles_part) return true;
  if (sheet_parts.count(uri) != 0) return true;
  // calcChain 由公式重算替代，保存时不写回。
  if (uri == "/xl/calcChain.xml") return true;
  return false;
}

// ---- 数据透视表（F-206）--------------------------------------------------

// "A1:C9" -> 区域（0 基）。解析不了返回 false。
bool ParseA1Range(std::string_view text, Range* out) {
  std::string clean;
  clean.reserve(text.size());
  for (char c : text) {
    if (c == '$' || c == ' ' || c == '\t') continue;
    clean.push_back(c);
  }
  const size_t colon = clean.find(':');
  const std::string first = clean.substr(0, colon);
  const std::string second = colon == std::string::npos ? first : clean.substr(colon + 1);
  CellRef a;
  CellRef b;
  if (!ParseCellName(first, &a) || !ParseCellName(second, &b)) return false;
  out->row1 = std::min(a.row, b.row);
  out->col1 = std::min(a.col, b.col);
  out->row2 = std::max(a.row, b.row);
  out->col2 = std::max(a.col, b.col);
  return true;
}

// cacheField/@name + 共享项。共享项的顺序就是 records 里 <x v="N"/> 的下标。
void ParseCacheField(XmlWalker& w, const XmlEvent& start, PivotCacheField& out) {
  out.name = AttrOr(start, "name", std::string());
  WalkChildren(w, start, [&](const XmlEvent& c) {
    if (LocalName(c.name) != "sharedItems") return;
    out.has_numbers = AttrBool(c, "containsNumber", false);
    out.has_strings = AttrBool(c, "containsString", false);
    if (Attr(c, "minValue") != nullptr) {
      out.min_value = AttrDouble(c, "minValue", 0.0);
      out.has_numbers = true;
    }
    if (Attr(c, "maxValue") != nullptr) {
      out.max_value = AttrDouble(c, "maxValue", 0.0);
    }
    WalkChildren(w, c, [&](const XmlEvent& item) {
      const std::string_view n = LocalName(item.name);
      if (n == "s" || n == "n" || n == "d") {
        out.shared_items.push_back(AttrOr(item, "v", std::string()));
        if (n == "n") out.has_numbers = true;
        if (n == "s") out.has_strings = true;
      } else if (n == "m") {
        // 缺失项：占一个下标，值为空。
        out.shared_items.emplace_back();
      }
      // 其余（b 布尔、e 错误）不进共享项，刷新时会重算。
    });
  });
}

void ParsePivotCacheDefinition(std::string_view xml, PivotTable& out) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "pivotCacheDefinition") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "cacheSource") {
        WalkChildren(w, c, [&](const XmlEvent& src) {
          if (LocalName(src.name) != "worksheetSource") return;
          out.source_sheet = AttrOr(src, "sheet", out.source_sheet);
          ParseA1Range(AttrOr(src, "ref", std::string()), &out.source);
        });
      } else if (n == "cacheFields") {
        // 缓存定义才是字段表的权威来源。ParsePivotTableDefinition 已经按
        // pivotFields 的个数补过一轮占位项（那里面只有轴信息、没有字段名），
        // 这里必须整体替换而不是追加，否则字段数会翻倍。
        std::vector<PivotCacheField> fields;
        WalkChildren(w, c, [&](const XmlEvent& f) {
          if (LocalName(f.name) != "cacheField") return;
          PivotCacheField field;
          ParseCacheField(w, f, field);
          fields.push_back(std::move(field));
        });
        if (!fields.empty()) out.fields.swap(fields);
      }
    });
  }
}

// pivotField/@axis -> 轴。
PivotAxis AxisFromName(std::string_view v) {
  if (v == "axisRow") return PivotAxis::Row;
  if (v == "axisCol") return PivotAxis::Column;
  if (v == "axisPage") return PivotAxis::Page;
  if (v == "axisValues") return PivotAxis::Data;
  return PivotAxis::None;
}

void ParsePivotItems(XmlWalker& w, const XmlEvent& start, std::vector<PivotItem>* out) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    if (LocalName(c.name) != "i") return;
    PivotItem item;
    item.grand = AttrOr(c, "t", std::string()) == "grand";
    WalkChildren(w, c, [&](const XmlEvent& x) {
      if (LocalName(x.name) != "x") return;
      item.indices.push_back(AttrInt(x, "v", 0));
    });
    out->push_back(std::move(item));
  });
}

void ParsePivotTableDefinition(std::string_view xml, PivotTable& out) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "pivotTableDefinition") continue;
    out.name = AttrOr(ev, "name", out.name);
    out.cache_id = AttrInt(ev, "cacheId", out.cache_id);
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "location") {
        Range ref;
        if (ParseA1Range(AttrOr(c, "ref", std::string()), &ref)) {
          out.destination.row = ref.row1;
          out.destination.col = ref.col1;
        }
      } else if (n == "pivotFields") {
        WalkChildren(w, c, [&](const XmlEvent& f) {
          if (LocalName(f.name) != "pivotField") return;
          PivotField pf;
          pf.axis = AxisFromName(AttrOr(f, "axis", std::string()));
          pf.show_all = AttrBool(f, "showAll", true);
          pf.data_field = AttrBool(f, "dataField", false);
          out.pivot_fields.push_back(std::move(pf));
        });
      } else if (n == "rowFields" || n == "colFields" || n == "pageFields") {
        std::vector<int>* target = n == "rowFields"    ? &out.row_fields
                                   : n == "colFields"  ? &out.column_fields
                                                       : &out.page_fields;
        WalkChildren(w, c, [&](const XmlEvent& f) {
          if (LocalName(f.name) != "field") return;
          target->push_back(AttrInt(f, "x", -1));
        });
      } else if (n == "dataFields") {
        WalkChildren(w, c, [&](const XmlEvent& f) {
          if (LocalName(f.name) != "dataField") return;
          PivotDataField df;
          df.field_index = AttrInt(f, "fld", -1);
          df.name = AttrOr(f, "name", std::string());
          df.aggregate = PivotAggregateFromName(AttrOr(f, "subtotal", std::string()));
          df.number_format = AttrOr(f, "numFmtId", std::string());
          out.data_fields.push_back(std::move(df));
        });
      } else if (n == "rowItems") {
        ParsePivotItems(w, c, &out.row_items);
      } else if (n == "colItems") {
        ParsePivotItems(w, c, &out.column_items);
      } else if (n == "pivotTableStyleInfo") {
        out.style_name = AttrOr(c, "name", out.style_name);
      }
    });
    // 字段名以缓存定义为准；定义里没有的才按 pivotFields 的顺序补。
    if (out.fields.size() < out.pivot_fields.size()) {
      out.fields.resize(out.pivot_fields.size());
    }
    for (size_t i = 0; i < out.pivot_fields.size(); ++i) {
      if (out.fields[i].name.empty()) out.fields[i].name = out.pivot_fields[i].name;
      out.pivot_fields[i].name = out.fields[i].name;
    }
    break;
  }
}

// 工作表 -> pivotTable 部件 -> pivotCacheDefinition 部件。
void ReadPivotTables(Package& pkg, Workbook& wb, Part* sheet_part, const std::string& sheet_name,
                     WarningSink* warnings) {
  if (sheet_part == nullptr) return;
  for (const Relationship& rel : sheet_part->relationships().items()) {
    if (rel.external()) continue;
    if (rel.type.find("/pivotTable") == std::string::npos) continue;
    Part* table_part = pkg.Find(rel.resolved);
    if (table_part == nullptr) continue;
    PivotTable table;
    table.sheet = sheet_name;
    table.source_sheet = sheet_name;
    table.part_uri = table_part->uri();
    try {
      ParsePivotTableDefinition(AsUtf8(table_part->content()), table);
    } catch (const Error& e) {
      if (warnings != nullptr) {
        Warning warning;
        warning.code = static_cast<int>(WarningCode::UnknownPartKept);
        warning.module = "xlsx";
        warning.path = table_part->uri();
        warning.message = std::string("pivot table kept as-is: ") + e.what();
        warnings->Add(warning);
      }
      continue;
    }
    // 缓存定义里有字段名与源区域，缺了它这张表就没法刷新。
    for (const Relationship& cache_rel : table_part->relationships().items()) {
      if (cache_rel.external()) continue;
      if (cache_rel.type.find("/pivotCacheDefinition") == std::string::npos) continue;
      if (Part* cache = pkg.Find(cache_rel.resolved)) {
        table.cache_uri = cache->uri();
        ParsePivotCacheDefinition(AsUtf8(cache->content()), table);
        break;
      }
    }
    if (table.source_sheet.empty()) table.source_sheet = sheet_name;
    wb.pivot_tables.push_back(std::move(table));
  }
}

// ---- 图表（F-205）--------------------------------------------------------

// "Sheet1!$B$2:$B$5" / "Sheet1!$B$2" -> 区域。解析不了返回 false。
struct A1Ref {
  std::string sheet;
  int col1 = -1, row1 = -1, col2 = -1, row2 = -1;  // 0 基
};

// xdr:from/to 锚点（EMU 偏移 + 行列号），换成点坐标时按列宽/行高折算。
struct ChartAnchor {
  int from_col = 0, from_row = 0, to_col = 0, to_row = 0;
  double from_col_off_emu = 0, from_row_off_emu = 0;
  double ext_cx_emu = 0, ext_cy_emu = 0;  // oneCellAnchor 用的绝对尺寸
};

bool ParseRangeRef(std::string_view formula, A1Ref* out) {
  std::string_view s = formula;
  const size_t bang = s.rfind('!');
  if (bang != std::string_view::npos) {
    std::string sheet(s.substr(0, bang));
    // Excel 引用带空格的表名时加单引号：'My Sheet'!$A$1。
    if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') {
      sheet = sheet.substr(1, sheet.size() - 2);
    }
    out->sheet = sheet;
    s = s.substr(bang + 1);
  }
  if (s.empty()) return false;
  auto parse_cell = [](std::string_view cell, int* col, int* row) {
    size_t i = 0;
    if (!cell.empty() && cell[0] == '$') i = 1;
    int c = 0;
    while (i < cell.size() && std::isalpha(static_cast<unsigned char>(cell[i]))) {
      c = c * 26 + (std::toupper(static_cast<unsigned char>(cell[i])) - 'A' + 1);
      ++i;
    }
    if (c == 0) return false;
    if (i < cell.size() && cell[i] == '$') ++i;
    int r = 0;
    bool any = false;
    while (i < cell.size() && std::isdigit(static_cast<unsigned char>(cell[i]))) {
      r = r * 10 + (cell[i] - '0');
      any = true;
      ++i;
    }
    if (!any) return false;
    *col = c - 1;  // 0 基
    *row = r - 1;
    return true;
  };
  const size_t colon = s.find(':');
  if (colon == std::string_view::npos) {
    if (!parse_cell(s, &out->col1, &out->row1)) return false;
    out->col2 = out->col1;
    out->row2 = out->row1;
    return true;
  }
  if (!parse_cell(s.substr(0, colon), &out->col1, &out->row1)) return false;
  if (!parse_cell(s.substr(colon + 1), &out->col2, &out->row2)) return false;
  if (out->col1 > out->col2) std::swap(out->col1, out->col2);
  if (out->row1 > out->row2) std::swap(out->row1, out->row2);
  return true;
}

// 单元格网格 -> 点。与 sheet_layout 的换算一致：列宽是字符数（×char_width_pt），
// 行高本来就是点；未设置的行高按 15pt（OOXML 默认 defaultRowHeight）。
class SheetMap {
 public:
  static constexpr double kCharWidthPt = 5.25;
  static constexpr double kDefaultRowPt = 15.0;

  SheetMap(const Workbook& wb, const Worksheet& ws) : ws_(ws), wb_(wb) {}

  double ColLeft(int col) const {
    double x = 0.0;
    for (int c = 0; c < col && c < 16384; ++c) x += ws_.ColumnWidth(c) * kCharWidthPt;
    return x;
  }
  double RowTop(int row) const {
    double y = 0.0;
    for (int r = 0; r < row && r < 1048576; ++r) y += ws_.RowHeight(r);
    return y;
  }
  double ColWidthPt(int col) const { return ws_.ColumnWidth(col) * kCharWidthPt; }
  double RowHeightPt(int row) const { return ws_.RowHeight(row); }

  // 缓存缺失时按区域实取值。
  std::vector<double> Numbers(const A1Ref& range) const {
    std::vector<double> out;
    const Worksheet* ws = SheetOf(range);
    if (ws == nullptr) return out;
    for (int r = range.row1; r <= range.row2; ++r) {
      for (int c = range.col1; c <= range.col2; ++c) {
        const Value v = wb_.ValueOf(*ws, r, c);
        if (v.kind == Value::Kind::Number) out.push_back(v.number);
        else if (v.kind == Value::Kind::Bool) out.push_back(v.boolean ? 1.0 : 0.0);
        else if (!out.empty() || true) out.push_back(0.0);  // 空位补 0，保持点数对齐
      }
    }
    return out;
  }

  std::vector<std::string> Labels(const A1Ref& range) const {
    std::vector<std::string> out;
    const Worksheet* ws = SheetOf(range);
    if (ws == nullptr) return out;
    for (int r = range.row1; r <= range.row2; ++r) {
      for (int c = range.col1; c <= range.col2; ++c) {
        const Value v = wb_.ValueOf(*ws, r, c);
        if (v.kind == Value::Kind::Text) out.push_back(v.text);
        else if (v.kind == Value::Kind::Number) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%g", v.number);
          out.emplace_back(buf);
        } else {
          out.emplace_back();
        }
      }
    }
    return out;
  }

 private:
  const Worksheet* SheetOf(const A1Ref& range) const {
    if (!range.sheet.empty()) {
      const Worksheet* ws = const_cast<Workbook&>(wb_).SheetByName(range.sheet);
      if (ws != nullptr) return ws;
    }
    return &ws_;
  }

  const Worksheet& ws_;
  const Workbook& wb_;
};

std::string ChartPlainText(XmlWalker& w, const XmlEvent& /*start*/) {
  // c:title 里只取 a:t 的文本（run 属性忽略）。
  const int mine = w.open();
  std::string out;
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) break;
    if (ev.type == XmlEventType::StartElement && LocalName(ev.name) == "t") out += ElemText(w, ev);
  }
  return out;
}

// 一个 <c:ser>：名字/分类/数值都优先取缓存，缓存缺失按 <c:f> 引用实取。
ChartSeries ParseSeries(XmlWalker& w, const XmlEvent& ser, const SheetMap& map) {
  ChartSeries series;
  WalkChildren(w, ser, [&](const XmlEvent& child) {
    const std::string_view cn = LocalName(child.name);
    if (cn == "tx") {
      std::string cached;
      std::string formula;
      WalkChildren(w, child, [&](const XmlEvent& tx) {
        const std::string_view tn = LocalName(tx.name);
        if (tn == "strRef") {
          WalkChildren(w, tx, [&](const XmlEvent& f) {
            const std::string_view fn = LocalName(f.name);
            if (fn == "f") {
              formula = ElemText(w, f);
            } else if (fn == "strCache") {
              WalkChildren(w, f, [&](const XmlEvent& c) {
                if (LocalName(c.name) == "v") cached = ElemText(w, c);
              });
            }
          });
        } else if (tn == "v") {
          cached = ElemText(w, tx);
        }
      });
      if (!cached.empty()) {
        series.name = cached;
      } else if (!formula.empty()) {
        A1Ref ref;
        if (ParseRangeRef(formula, &ref)) {
          const std::vector<std::string> labels = map.Labels(ref);
          if (!labels.empty()) series.name = labels.front();
        }
      }
      return;
    }
    if (cn != "cat" && cn != "val" && cn != "yVal" && cn != "xVal") return;
    A1Ref range;
    std::vector<std::string> text_cache;
    std::vector<double> num_cache;
    WalkChildren(w, child, [&](const XmlEvent& ref) {
      const std::string_view rn = LocalName(ref.name);
      if (rn != "strRef" && rn != "numRef") return;
      WalkChildren(w, ref, [&](const XmlEvent& f) {
        const std::string_view fn = LocalName(f.name);
        if (fn == "f") {
          const std::string formula = ElemText(w, f);
          A1Ref parsed;
          if (ParseRangeRef(formula, &parsed)) range = parsed;
        } else if (fn == "strCache" || fn == "numCache") {
          WalkChildren(w, f, [&](const XmlEvent& c) {
            if (LocalName(c.name) != "v") return;
            const std::string text = ElemText(w, c);
            text_cache.push_back(text);
            num_cache.push_back(std::strtod(text.c_str(), nullptr));
          });
        }
      });
    });
    if (cn == "cat") {
      if (!text_cache.empty()) {
        series.categories = text_cache;
      } else if (range.row1 >= 0) {
        series.categories = map.Labels(range);
      }
      return;
    }
    if (!num_cache.empty()) {
      series.values = num_cache;
    } else if (range.row1 >= 0) {
      series.values = map.Numbers(range);
    }
  });
  return series;
}

// <c:chartSpace> -> <c:chart> -> <c:plotArea> -> *Chart -> <c:ser>。
// *Chart 的子元素必须在本层读完：交回外层循环时它们已经被 WalkChildren 吃掉，
// 结果就是一张空表（早期版本在这里漏掉全部系列）。
Chart ParseChartXml(const std::string& xml, const SheetMap& map) {
  Chart chart;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement || !NameIs(ev, "chart")) continue;
    WalkChildren(w, ev, [&](const XmlEvent& node) {
      const std::string_view name = LocalName(node.name);
      if (name == "title") {
        chart.title = ChartPlainText(w, node);
      } else if (name == "legend") {
        chart.legend = true;
      } else if (name == "plotArea") {
        WalkChildren(w, node, [&](const XmlEvent& group) {
          const std::string_view gn = LocalName(group.name);
          ChartKind kind = ChartKind::Unknown;
          if (gn == "barChart") kind = ChartKind::Column;
          else if (gn == "lineChart") kind = ChartKind::Line;
          else if (gn == "pieChart") kind = ChartKind::Pie;
          else if (gn == "areaChart") kind = ChartKind::Area;
          else if (gn == "scatterChart") kind = ChartKind::Scatter;
          else if (gn == "doughnutChart") kind = ChartKind::Doughnut;
          if (kind == ChartKind::Unknown) return;  // catAx/valAx/layout 等
          chart.kind = kind;
          WalkChildren(w, group, [&](const XmlEvent& item) {
            const std::string_view in = LocalName(item.name);
            if (in == "barDir") {
              // 值在 val 属性里；老实现写成元素文本，Excel 两者都见过。
              const std::string dir = AttrOr(item, "val", ElemText(w, item));
              if (dir == "bar") chart.kind = ChartKind::Bar;
            } else if (in == "grouping") {
              const std::string g = AttrOr(item, "val", ElemText(w, item));
              chart.stacked = g == "stacked" || g == "percentStacked";
            } else if (in == "smooth") {
              chart.smooth = AttrBool(item, "val", chart.smooth);
              ElemText(w, item);
            } else if (in == "ser") {
              chart.series.push_back(ParseSeries(w, item, map));
            }
          });
        });
      }
    });
  }
  return chart;
}


// xdr:from / xdr:to -> 工作表点坐标矩形。
bool ParseDrawingAnchors(const std::string& xml, std::vector<ChartAnchor>* anchors,
                         std::vector<std::string>* chart_rids) {
  struct Mark {
    int col = 0, row = 0;
    double col_off = 0, row_off = 0;  // EMU
  };
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name != "twoCellAnchor" && name != "oneCellAnchor" && name != "absoluteAnchor") continue;
    Mark from, to;
    double ext_cx = 0, ext_cy = 0;
    std::string rid;
    Mark* current = nullptr;
    // from/to 是容器，真正的 col/row/colOff/rowOff 在它们的子元素里。
    auto read_mark = [&](const XmlEvent& container, Mark* mark) {
      WalkChildren(w, container, [&](const XmlEvent& p) {
        const std::string_view pn = LocalName(p.name);
        if (pn != "col" && pn != "row" && pn != "colOff" && pn != "rowOff") return;
        // 值可能是 val 属性（Excel）也可能是元素文本（openpyxl），两者都读。
        const std::string text = ElemText(w, p);
        const bool numeric =
            !text.empty() && text.find_first_not_of(" \t\r\n-+.0123456789eE") == std::string::npos;
        const char* raw = numeric ? text.c_str() : nullptr;
        if (pn == "col") {
          mark->col = raw != nullptr ? std::atoi(raw) : AttrInt(p, "val", mark->col);
        } else if (pn == "row") {
          mark->row = raw != nullptr ? std::atoi(raw) : AttrInt(p, "val", mark->row);
        } else if (pn == "colOff") {
          mark->col_off = raw != nullptr ? std::strtod(raw, nullptr) : AttrDouble(p, "val", mark->col_off);
        } else {
          mark->row_off = raw != nullptr ? std::strtod(raw, nullptr) : AttrDouble(p, "val", mark->row_off);
        }
      });
    };
    WalkChildren(w, ev, [&](const XmlEvent& child) {
      const std::string_view cn = LocalName(child.name);
      if (cn == "from") {
        current = &from;
        read_mark(child, &from);
      } else if (cn == "to") {
        current = &to;
        read_mark(child, &to);
      } else if (cn == "ext") {
        ext_cx = AttrDouble(child, "cx", 0.0);
        ext_cy = AttrDouble(child, "cy", 0.0);
      } else if (cn == "graphicFrame") {
        // c:chart 在 graphicFrame/a:graphic/a:graphicData 里，要再下两级。
        WalkChildren(w, child, [&](const XmlEvent& gf) {
          if (LocalName(gf.name) != "graphic") return;
          WalkChildren(w, gf, [&](const XmlEvent& g) {
            if (LocalName(g.name) != "graphicData") return;
            WalkChildren(w, g, [&](const XmlEvent& gd) {
              if (LocalName(gd.name) != "chart") return;
              for (const XmlAttribute& a : gd.attributes) {
                if (LocalName(a.name) == "id") rid = std::string(a.value);
              }
            });
          });
        });
      }
    });
    if (rid.empty()) continue;
    ChartAnchor anchor;
    anchor.from_col = from.col;
    anchor.from_row = from.row;
    anchor.to_col = to.col;
    anchor.to_row = to.row;
    anchor.from_col_off_emu = from.col_off;
    anchor.from_row_off_emu = from.row_off;
    anchor.ext_cx_emu = ext_cx;
    anchor.ext_cy_emu = ext_cy;
    anchors->push_back(anchor);
    chart_rids->push_back(rid);
  }
  return true;
}

Workbook ReadPackage(Package& pkg, WarningSink* warnings, CancelToken* cancel) {
  if (pkg.kind() != PackageKind::Excel) {
    throw Error(ErrorCode::Unsupported, "xlsx: package is not a spreadsheet");
  }
  Part* main = pkg.main_part();
  if (main == nullptr) throw Error(ErrorCode::Corrupt, "xlsx: no workbook part");
  const std::string workbook_part = main->uri();

  Workbook wb;
  wb.date1904 = false;

  // 关系：rId -> Part-URI
  std::map<std::string, std::string> rid_target;
  std::string strings_part;
  std::string styles_part;
  for (const Relationship& rel : main->relationships().items()) {
    if (rel.external()) continue;
    rid_target[rel.id] = rel.resolved;
    if (rel.type.find("/sharedStrings") != std::string::npos) strings_part = rel.resolved;
    else if (rel.type.find("/styles") != std::string::npos) styles_part = rel.resolved;
  }
  if (strings_part.empty()) strings_part = "/xl/sharedStrings.xml";
  if (styles_part.empty()) styles_part = "/xl/styles.xml";

  const std::string workbook_xml = AsUtf8(main->content());
  const ParsedWorkbook parsed = ReadWorkbookXml(workbook_xml);
  wb.date1904 = parsed.date1904;
  wb.names = parsed.names;

  if (Part* strings = pkg.Find(strings_part)) ReadSharedStrings(AsUtf8(strings->content()), wb);

  if (Part* styles = pkg.Find(styles_part)) {
    const StyleTables tables = ReadStyles(AsUtf8(styles->content()));
    wb.styles = tables.xfs;
    wb.dxfs = tables.dxfs;
  }
  if (wb.styles.empty()) wb.styles.push_back(CellStyle{});

  std::set<std::string> sheet_parts;
  std::vector<std::pair<std::string, std::string>> order;  // (name, part uri)
  for (const SheetRef& ref : parsed.sheets) {
    auto it = rid_target.find(ref.rid);
    if (it == rid_target.end()) continue;
    sheet_parts.insert(it->second);
    order.emplace_back(ref.name, it->second);
  }

  for (size_t i = 0; i < order.size(); ++i) {
    if (cancel != nullptr) cancel->ThrowIfCancelled();
    Worksheet& ws = wb.AddSheet(order[i].first);
    Part* part = pkg.Find(order[i].second);
    if (part == nullptr) {
      if (warnings != nullptr) {
        Warning warning;
        warning.code = static_cast<int>(WarningCode::UnknownPartKept);
        warning.module = "xlsx";
        warning.path = order[i].second;
        warning.message = "worksheet part missing; sheet kept empty";
        warnings->Add(warning);
      }
      continue;
    }
    // 工作表自带的 External 关系（超链接的 r:id 指向这里）。
    std::map<std::string, std::string> sheet_rid;
    for (const Relationship& rel : part->relationships().items()) {
      if (!rel.external()) continue;
      sheet_rid[rel.id] = rel.resolved;
    }
    ReadWorksheetXml(AsUtf8(part->content()), wb, ws,
                     sheet_rid.empty() ? nullptr : &sheet_rid);
    // 透视表：工作表 -> pivotTable -> pivotCacheDefinition 三级关系。
    // 部件本身仍留在 unknown_parts 里（字节级往返），只有被刷新过的才会在
    // 保存时按 DOM 重新生成。
    ReadPivotTables(pkg, wb, part, ws.name(), warnings);
  }
  if (wb.sheets.empty()) wb.AddSheet("Sheet1");
  wb.active_sheet = wb.sheets.front()->name();

  // ---- 图表：worksheet -> drawing -> chart 三级关系 ----------------------
  for (size_t i = 0; i < order.size(); ++i) {
    Part* sheet_part = pkg.Find(order[i].second);
    if (sheet_part == nullptr) continue;
    std::string drawing_part;
    for (const Relationship& rel : sheet_part->relationships().items()) {
      if (rel.external()) continue;
      if (rel.type.find("/drawing") != std::string::npos) drawing_part = rel.resolved;
    }
    if (drawing_part.empty()) continue;
    Part* drawing = pkg.Find(drawing_part);
    if (drawing == nullptr) continue;

    std::vector<ChartAnchor> anchors;
    std::vector<std::string> rids;
    ParseDrawingAnchors(AsUtf8(drawing->content()), &anchors, &rids);

    std::map<std::string, std::string> drawing_rid;
    for (const Relationship& rel : drawing->relationships().items()) {
      if (rel.external()) continue;
      drawing_rid[rel.id] = rel.resolved;
    }
    const Worksheet& ws = *wb.sheets[i];
    for (size_t k = 0; k < anchors.size() && k < rids.size(); ++k) {
      auto rit = drawing_rid.find(rids[k]);
      if (rit == drawing_rid.end()) continue;
      Part* chart_part = pkg.Find(rit->second);
      if (chart_part == nullptr) continue;
      SheetMap map(wb, ws);
      Chart chart = ParseChartXml(AsUtf8(chart_part->content()), map);
      if (chart.kind == ChartKind::Unknown || chart.series.empty()) {
        // 不认识的图型保留原始部件；写回时按未知部件原样输出。
        if (warnings != nullptr) {
          Warning warning;
          warning.code = static_cast<int>(WarningCode::UnknownPartKept);
          warning.module = "xlsx";
          warning.path = rit->second;
          warning.message = "chart kind not modelled; part kept verbatim";
          warnings->Add(warning);
        }
        continue;
      }
      const ChartAnchor& a = anchors[k];
      const double kEmuPerPt = 12700.0;
      chart.x = map.ColLeft(a.from_col) + a.from_col_off_emu / kEmuPerPt;
      chart.y = map.RowTop(a.from_row) + a.from_row_off_emu / kEmuPerPt;
      if (a.to_col > a.from_col || a.to_row > a.from_row) {
        // twoCellAnchor 的 to 是右下单元格的左/上边界，矩形就到那里。
        chart.w = map.ColLeft(a.to_col) - chart.x;
        chart.h = map.RowTop(a.to_row) - chart.y;
      } else {
        chart.w = a.ext_cx_emu / kEmuPerPt;
        chart.h = a.ext_cy_emu / kEmuPerPt;
      }
      if (chart.w < 12.0) chart.w = 288.0;   // 锚点异常时给个 4 英寸的默认值
      if (chart.h < 12.0) chart.h = 144.0;
      chart.has_anchor = true;
      chart.sheet = ws.name();
      chart.part_uri = rit->second;
      // 画到这张表上的图表：记下表与绘图部件的对应关系，写回时补 drawing 关系。
      bool known = false;
      for (const SheetDrawing& sd : wb.drawings) {
        if (sd.part_uri == drawing_part) known = true;
      }
      if (!known) wb.drawings.push_back({ws.name(), drawing_part});
      wb.charts.push_back(std::move(chart));
    }
  }

  // 未识别的部件原样保留（docs/02 §3.1.3 第 6 条）。图表与绘图部件也走这条路：
  // 它们已经解析成 DOM（wb.charts），但 XML 里还有 DOM 装不下的东西（坐标轴
  // 格式、数据标签、调色板、锚点微调），原样写回才不会在保存时把图表弄丢。
  for (const std::unique_ptr<Part>& part : pkg.parts()) {
    if (IsConsumedPart(part->uri(), sheet_parts, workbook_part, strings_part, styles_part)) continue;
    UnknownPart unknown;
    unknown.uri = part->uri();
    unknown.mime = part->mime();
    unknown.bytes = part->content();
    for (const Relationship& rel : part->relationships().items()) {
      UnknownRel keep;
      keep.id = rel.id;
      keep.type = rel.type;
      keep.external = rel.external();
      keep.target = rel.external() ? rel.target : rel.resolved;
      unknown.rels.push_back(std::move(keep));
    }
    wb.unknown_parts.push_back(std::move(unknown));
  }
  return wb;
}

}  // namespace

Workbook Open(const OpenOptions& opts) {
  if (opts.path.empty()) throw Error(ErrorCode::InvalidArgument, "xlsx: empty path");
  FileStream in(opts.path, "rb");
  auto pkg = Package::Open(in, opts.warnings);
  return ReadPackage(*pkg, opts.warnings, opts.cancel);
}

Workbook OpenMemory(std::string_view bytes, WarningSink* warnings, CancelToken* cancel) {
  std::vector<uint8_t> data(bytes.begin(), bytes.end());
  MemoryStream mem(std::move(data));
  auto pkg = Package::Open(mem, warnings);
  return ReadPackage(*pkg, warnings, cancel);
}

}  // namespace sheet
}  // namespace doccore
