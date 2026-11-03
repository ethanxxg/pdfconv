// DocCore - 电子表格 DOM 与数字格式。Design refs: docs/02 §3.4.1
#include "doccore/sheet.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "doccore/formula.h"

namespace doccore {
namespace sheet {

// ---- errors --------------------------------------------------------------

const char* CellErrorText(CellError code) noexcept {
  switch (code) {
    case CellError::Null: return "#NULL!";
    case CellError::DivZero: return "#DIV/0!";
    case CellError::Value: return "#VALUE!";
    case CellError::Ref: return "#REF!";
    case CellError::Name: return "#NAME?";
    case CellError::Num: return "#NUM!";
    case CellError::NotAvailable: return "#N/A";
    case CellError::NotImpl: return "#NIMPL!";
    case CellError::Circular: return "#CIRC!";
  }
  return "#VALUE!";
}

const char* ChartKindName(ChartKind kind) noexcept {
  switch (kind) {
    case ChartKind::Column: return "column";
    case ChartKind::Bar: return "bar";
    case ChartKind::Line: return "line";
    case ChartKind::Pie: return "pie";
    case ChartKind::Area: return "area";
    case ChartKind::Scatter: return "scatter";
    case ChartKind::Doughnut: return "doughnut";
    case ChartKind::Unknown: break;
  }
  return "unknown";
}

CellError CellErrorFromText(std::string_view text) {
  if (text == "#NULL!") return CellError::Null;
  if (text == "#DIV/0!") return CellError::DivZero;
  if (text == "#VALUE!") return CellError::Value;
  if (text == "#REF!") return CellError::Ref;
  if (text == "#NAME?") return CellError::Name;
  if (text == "#NUM!") return CellError::Num;
  if (text == "#N/A") return CellError::NotAvailable;
  if (text == "#NIMPL!") return CellError::NotImpl;
  if (text == "#CIRC!") return CellError::Circular;
  return CellError::Value;
}

// ---- Value ---------------------------------------------------------------

Value Value::Num(double v) {
  Value out;
  out.kind = Kind::Number;
  out.number = v;
  return out;
}

Value Value::Str(std::string v) {
  Value out;
  out.kind = Kind::Text;
  out.text = std::move(v);
  return out;
}

Value Value::Boolean(bool v) {
  Value out;
  out.kind = Kind::Bool;
  out.boolean = v;
  return out;
}

Value Value::Err(CellError e) {
  Value out;
  out.kind = Kind::Error;
  out.error = e;
  return out;
}

// ---- addresses -----------------------------------------------------------

std::string ColumnName(int col) {
  if (col < 0) return std::string();
  std::string out;
  int v = col + 1;
  while (v > 0) {
    const int rem = (v - 1) % 26;
    out.insert(out.begin(), static_cast<char>('A' + rem));
    v = (v - 1) / 26;
  }
  return out;
}

int ColumnFromName(std::string_view name) {
  if (name.empty() || name.size() > 3) return -1;
  int v = 0;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') {
      v = v * 26 + (c - 'A' + 1);
    } else if (c >= 'a' && c <= 'z') {
      v = v * 26 + (c - 'a' + 1);
    } else {
      return -1;
    }
  }
  return v - 1;
}

std::string CellName(int row, int col) {
  return ColumnName(col) + std::to_string(row + 1);
}

bool ParseCellName(std::string_view name, CellRef* out) {
  size_t i = 0;
  std::string letters;
  while (i < name.size() && ((name[i] >= 'A' && name[i] <= 'Z') ||
                             (name[i] >= 'a' && name[i] <= 'z'))) {
    letters.push_back(name[i]);
    ++i;
  }
  if (letters.empty() || i >= name.size()) return false;
  int col = ColumnFromName(letters);
  if (col < 0) return false;
  int row = 0;
  while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
    row = row * 10 + (name[i] - '0');
    ++i;
  }
  if (row <= 0 || i != name.size()) return false;
  out->row = row - 1;
  out->col = col;
  out->absolute_row = false;
  out->absolute_col = false;
  return true;
}

// ---- number formats ------------------------------------------------------

std::string_view BuiltinNumberFormat(int id) {
  switch (id) {
    case 0: return "General";
    case 1: return "0";
    case 2: return "0.00";
    case 3: return "#,##0";
    case 4: return "#,##0.00";
    case 9: return "0%";
    case 10: return "0.00%";
    case 11: return "0.00E+00";
    case 12: return "# ?/?";
    case 13: return "# ??/??";
    case 14: return "yyyy-mm-dd";
    case 15: return "d-mmm-yy";
    case 16: return "d-mmm";
    case 17: return "mmm-yy";
    case 18: return "h:mm AM/PM";
    case 19: return "h:mm:ss AM/PM";
    case 20: return "hh:mm";
    case 21: return "hh:mm:ss";
    case 22: return "yyyy-mm-dd hh:mm";
    case 37: return "#,##0 ;(#,##0)";
    case 38: return "#,##0 ;[Red](#,##0)";
    case 39: return "#,##0.00;(#,##0.00)";
    case 40: return "#,##0.00;[Red](#,##0.00)";
    case 44: return "accounting";
    case 45: return "mm:ss";
    case 46: return "[h]:mm:ss";
    case 47: return "mmss.0";
    case 48: return "##0.0E+0";
    case 49: return "@";
    default: return std::string_view();
  }
}

namespace {

bool ContainsAny(const std::string& s, const char* chars) {
  for (char c : s) {
    for (const char* p = chars; *p != '\0'; ++p) {
      if (c == *p) return true;
    }
  }
  return false;
}

std::string UpperAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

// 去掉 [Red] / [$-409] 这类颜色与区域修饰，只保留可见字面量。
std::string StripModifiers(const std::string& pattern) {
  std::string out;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '[') {
      out.push_back(pattern[i]);
      continue;
    }
    const size_t close = pattern.find(']', i);
    if (close == std::string::npos) {
      out.push_back(pattern[i]);
      continue;
    }
    const std::string inner = pattern.substr(i + 1, close - i - 1);
    // [$-409] 里的货币符号要保留；其他修饰丢弃。
    if (!inner.empty() && inner[0] == '$') {
      const size_t dash = inner.find('-');
      if (dash != std::string::npos && dash > 1) out += inner.substr(1, dash - 1);
    }
    i = close;
  }
  return out;
}

}  // namespace

NumberFormat ParseNumberFormat(std::string_view pattern_view) {
  NumberFormat fmt;
  fmt.pattern = std::string(pattern_view);
  if (fmt.pattern.empty() || fmt.pattern == "General") return fmt;

  // 正/负/零分段：只用第一段决定显示，负号由值本身带出。
  std::string first = fmt.pattern;
  const size_t semi = first.find(';');
  if (semi != std::string::npos) {
    fmt.negative_parens = first.find('(', semi) != std::string::npos;
    first = first.substr(0, semi);
  }
  std::string body = StripModifiers(first);
  const std::string upper = UpperAscii(body);

  if (upper == "@") {
    fmt.kind = NumberFormat::Kind::Text;
    return fmt;
  }

  const bool has_year = upper.find("YY") != std::string::npos;
  const bool has_day = upper.find('D') != std::string::npos;
  const bool has_hour = upper.find('H') != std::string::npos;
  const bool has_second = upper.find('S') != std::string::npos;
  // 'M' 既可能是月也可能是分；只有和 y/d 一起出现才算月。
  const bool has_month = upper.find('M') != std::string::npos && (has_year || has_day);
  const bool has_minute = upper.find('M') != std::string::npos && !has_month && has_hour;

  if (has_year || has_day || has_month || has_hour || has_second || has_minute) {
    const bool date_part = has_year || has_day || has_month;
    const bool time_part = has_hour || has_second || has_minute;
    fmt.kind = date_part && time_part
                   ? NumberFormat::Kind::DateTime
                   : (date_part ? NumberFormat::Kind::Date : NumberFormat::Kind::Time);
    return fmt;
  }

  // 百分号
  const size_t percent = body.find('%');
  const bool is_percent = percent != std::string::npos;

  // 数字骨架：从第一个 0/#/? 到最后一个 0/#/?
  size_t start = std::string::npos;
  size_t end = std::string::npos;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '0' || c == '#' || c == '?') {
      if (start == std::string::npos) start = i;
      end = i;
    }
  }
  if (start == std::string::npos) {
    // 纯字面量（例如货币符号）：按通用格式显示并保留前后缀。
    fmt.kind = NumberFormat::Kind::General;
    fmt.prefix = body;
    return fmt;
  }
  const std::string skeleton = body.substr(start, end - start + 1);
  // 科学计数法（0.00E+00）不在 P0 子集内：保留原文，按通用格式显示。
  if (skeleton.find('E') != std::string::npos || skeleton.find('e') != std::string::npos) {
    fmt.kind = NumberFormat::Kind::General;
    return fmt;
  }
  const size_t dot = skeleton.find('.');
  fmt.decimals = dot == std::string::npos ? 0
                                         : static_cast<int>(skeleton.size() - dot - 1);
  fmt.thousands = skeleton.find(',') != std::string::npos;
  fmt.prefix = body.substr(0, start);
  fmt.suffix = body.substr(end + 1);
  if (is_percent) {
    fmt.kind = NumberFormat::Kind::Percent;
    fmt.suffix = "%";
    // 去掉格式串里原有的 '%' 以免重复
    const size_t pos = fmt.prefix.find('%');
    if (pos != std::string::npos) fmt.prefix.erase(pos, 1);
  } else if (!fmt.prefix.empty() || !fmt.suffix.empty()) {
    fmt.kind = NumberFormat::Kind::Currency;
  } else if (fmt.decimals == 0 && !fmt.thousands) {
    fmt.kind = NumberFormat::Kind::Integer;
  } else {
    fmt.kind = NumberFormat::Kind::Decimal;
  }
  return fmt;
}

// ---- date conversion -----------------------------------------------------

namespace {

// days since 1970-01-01 -> y/m/d（Howard Hinnant 的 civil_from_days）。
void CivilFromDays(int64_t z, int* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t dd = doy - (153 * mp + 2) / 5 + 1;
  const int64_t mm = mp < 10 ? mp + 3 : mp - 9;
  *y = static_cast<int>(yy + (mm <= 2 ? 1 : 0));
  *m = static_cast<int>(mm);
  *d = static_cast<int>(dd);
}

struct DateTime {
  int year = 1900;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

DateTime FromSerial(double serial, bool date1904) {
  DateTime dt;
  if (!(serial > -1e9 && serial < 1e9)) return dt;
  int64_t days = static_cast<int64_t>(std::floor(serial));
  double frac = serial - static_cast<double>(days);
  if (!date1904) {
    // Excel 的 1900 闰年 bug：序列 60 是不存在的 1900-02-29。
    if (days < 61) days += 1;
  }
  const int64_t unix_days = days - 25569 + (date1904 ? 1462 : 0);
  CivilFromDays(unix_days, &dt.year, &dt.month, &dt.day);
  int seconds = static_cast<int>(std::lround(frac * 86400.0));
  if (seconds >= 86400) seconds = 86399;
  dt.hour = seconds / 3600;
  dt.minute = (seconds % 3600) / 60;
  dt.second = seconds % 60;
  return dt;
}

std::string Pad2(int v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d", v);
  return buf;
}

const char* kMonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* kMonthLong[] = {"January", "February", "March",     "April",   "May",      "June",
                            "July",    "August",   "September", "October", "November", "December"};

// 日期格式串渲染：识别 y/m/d/h/s 的连续段，其余字符按字面量输出。
std::string RenderDate(const std::string& pattern, const DateTime& dt) {
  std::string out;
  const std::string upper = UpperAscii(pattern);
  size_t i = 0;
  int last_field = 0;  // 1=y 2=mo 3=d 4=h 5=mi 6=s
  while (i < upper.size()) {
    const char c = upper[i];
    if (c == '"') {  // 引号内的字面量
      ++i;
      while (i < upper.size() && upper[i] != '"') out.push_back(pattern[i++]);
      if (i < upper.size()) ++i;
      continue;
    }
    if (c == '\\' && i + 1 < upper.size()) {
      out.push_back(pattern[i + 1]);
      i += 2;
      continue;
    }
    if (c != 'Y' && c != 'M' && c != 'D' && c != 'H' && c != 'S' && c != 'A' && c != 'P') {
      out.push_back(pattern[i]);
      ++i;
      continue;
    }
    size_t run = 1;
    while (i + run < upper.size() && upper[i + run] == c) ++run;
    switch (c) {
      case 'Y':
        if (run <= 2) {
          out += Pad2(dt.year % 100);
        } else {
          out += std::to_string(dt.year);
        }
        last_field = 1;
        break;
      case 'M': {
        // 分钟还是月份：紧跟小时之后、且后面不是 d/y 时按分钟处理。
        const bool minute = last_field == 4;
        if (minute) {
          out += (run >= 2) ? Pad2(dt.minute) : std::to_string(dt.minute);
          last_field = 5;
        } else if (run >= 4) {
          out += kMonthLong[std::max(0, std::min(11, dt.month - 1))];
          last_field = 2;
        } else if (run == 3) {
          out += kMonthShort[std::max(0, std::min(11, dt.month - 1))];
          last_field = 2;
        } else if (run == 2) {
          out += Pad2(dt.month);
          last_field = 2;
        } else {
          out += std::to_string(dt.month);
          last_field = 2;
        }
        break;
      }
      case 'D':
        if (run >= 4) {
          out += kMonthLong[std::max(0, std::min(11, dt.month - 1))];  // dddd 近似为月名
        } else if (run == 3) {
          out += kMonthShort[std::max(0, std::min(11, dt.month - 1))];
        } else if (run == 2) {
          out += Pad2(dt.day);
        } else {
          out += std::to_string(dt.day);
        }
        last_field = 3;
        break;
      case 'H':
        out += (run >= 2) ? Pad2(dt.hour) : std::to_string(dt.hour);
        last_field = 4;
        break;
      case 'S':
        out += (run >= 2) ? Pad2(dt.second) : std::to_string(dt.second);
        last_field = 6;
        break;
      case 'A':
        if (upper.compare(i, 2, "AM") == 0) {
          out += dt.hour < 12 ? "AM" : "PM";
          i += 2;
          continue;
        }
        out.push_back(pattern[i]);
        break;
      default:
        out.push_back(pattern[i]);
        break;
    }
    i += run;
  }
  return out;
}

std::string GroupThousands(const std::string& digits) {
  std::string out;
  int count = 0;
  for (size_t i = digits.size(); i > 0; --i) {
    out.insert(out.begin(), digits[i - 1]);
    if (++count % 3 == 0 && i > 1) out.insert(out.begin(), ',');
  }
  return out;
}

}  // namespace

std::string FormatValue(const Value& value, const NumberFormat& fmt, bool date1904) {
  switch (value.kind) {
    case Value::Kind::Empty:
      return std::string();
    case Value::Kind::Bool:
      return value.boolean ? "TRUE" : "FALSE";
    case Value::Kind::Error:
      return CellErrorText(value.error);
    case Value::Kind::Text:
      return value.text;
    case Value::Kind::Number:
      break;
  }

  const double v = value.number;
  if (fmt.kind == NumberFormat::Kind::Text) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
  }
  if (fmt.is_date() || fmt.is_time()) {
    const DateTime dt = FromSerial(v, date1904);
    return RenderDate(fmt.pattern.empty() ? "yyyy-mm-dd" : fmt.pattern, dt);
  }
  if (fmt.kind == NumberFormat::Kind::General) {
    if (fmt.pattern.empty() || fmt.pattern == "General") {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.15g", v);
      return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return fmt.prefix + buf + fmt.suffix;
  }

  double scaled = v;
  if (fmt.kind == NumberFormat::Kind::Percent) scaled *= 100.0;
  const bool negative = scaled < 0;
  const double abs_value = std::fabs(scaled);
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%.*f", std::max(0, std::min(fmt.decimals, 12)), abs_value);
  std::string number = buf;
  if (fmt.thousands) {
    const size_t dot = number.find('.');
    const std::string int_part = number.substr(0, dot);
    const std::string frac_part = dot == std::string::npos ? std::string() : number.substr(dot);
    number = GroupThousands(int_part) + frac_part;
  }
  std::string out;
  if (negative) {
    if (fmt.negative_parens) {
      out = "(" + fmt.prefix + number + fmt.suffix + ")";
      return out;
    }
    out = "-";
  }
  out += fmt.prefix;
  out += number;
  out += fmt.suffix;
  return out;
}

std::string FormatValue(const Value& value, bool date1904) {
  NumberFormat general;
  return FormatValue(value, general, date1904);
}

// ---- Worksheet -----------------------------------------------------------

Cell* Worksheet::Find(int row, int col) {
  if (row < 0 || col < 0) return nullptr;
  auto c = columns_.find(col);
  if (c == columns_.end()) return nullptr;
  auto r = c->second.find(row);
  if (r == c->second.end()) return nullptr;
  return &r->second;
}

const Cell* Worksheet::Find(int row, int col) const {
  if (row < 0 || col < 0) return nullptr;
  auto c = columns_.find(col);
  if (c == columns_.end()) return nullptr;
  auto r = c->second.find(row);
  if (r == c->second.end()) return nullptr;
  return &r->second;
}

Cell& Worksheet::CellAt(int row, int col) { return columns_[col][row]; }

void Worksheet::SetNumber(int row, int col, double v) {
  Cell& cell = CellAt(row, col);
  cell.kind = CellKind::Number;
  cell.number = v;
  cell.cached = Value::Num(v);
  cell.dirty = false;
}

void Worksheet::SetText(int row, int col, std::string text) {
  Cell& cell = CellAt(row, col);
  cell.kind = CellKind::Text;
  cell.cached = Value::Str(text);
  cell.dirty = false;
  if (owner_ != nullptr) {
    cell.text_id = owner_->InternString(text);
  } else {
    // 没有工作簿时退化成"值里存文本"，保证独立使用也能取到内容。
    cell.text_id = -1;
  }
}

void Worksheet::SetBool(int row, int col, bool v) {
  Cell& cell = CellAt(row, col);
  cell.kind = CellKind::Bool;
  cell.boolean = v;
  cell.cached = Value::Boolean(v);
  cell.dirty = false;
}

void Worksheet::SetError(int row, int col, CellError e) {
  Cell& cell = CellAt(row, col);
  cell.kind = CellKind::Error;
  cell.error = e;
  cell.cached = Value::Err(e);
  cell.dirty = false;
}

int32_t Worksheet::SetFormula(int row, int col, std::string text) {
  Formula f;
  f.text = std::move(text);
  if (!f.text.empty() && f.text[0] == '=') f.text.erase(0, 1);
  const int32_t id = static_cast<int32_t>(formulas_.size());
  formulas_.push_back(std::move(f));
  Cell& cell = CellAt(row, col);
  cell.kind = CellKind::Formula;
  cell.formula_id = id;
  cell.dirty = true;
  cell.cached = Value::Empty();
  return id;
}

Value Worksheet::GetValue(int row, int col) const {
  const Cell* cell = Find(row, col);
  if (cell == nullptr) return Value::Empty();
  if (cell->formula_id >= 0) {
    if (!cell->dirty) return cell->cached;
    // 脏单元格：有缓存就用缓存（保存路径不重算），否则返回空。
    return cell->cached;
  }
  switch (cell->kind) {
    case CellKind::Empty:
      return Value::Empty();
    case CellKind::Number:
      return Value::Num(cell->number);
    case CellKind::Bool:
      return Value::Boolean(cell->boolean);
    case CellKind::Error:
      return Value::Err(cell->error);
    case CellKind::Text: {
      if (cell->text_id >= 0 && owner_ != nullptr) return Value::Str(owner_->StringAt(cell->text_id));
      return cell->cached.is_text() ? cell->cached : Value::Str(std::string());
    }
    case CellKind::Formula:
      break;
  }
  return Value::Empty();
}

const CellStyle& Worksheet::StyleOf(int row, int col) const {
  static const CellStyle kDefault{};
  const Cell* cell = Find(row, col);
  if (cell == nullptr || owner_ == nullptr) return kDefault;
  return owner_->StyleAt(cell->style_id);
}

Range Worksheet::UsedRange() const {
  Range r{};
  r.row1 = -1;
  r.col1 = -1;
  r.row2 = -1;
  r.col2 = -1;
  bool any = false;
  for (const auto& col : columns_) {
    for (const auto& row : col.second) {
      const Cell& cell = row.second;
      if (cell.empty()) continue;
      const int c = col.first;
      const int rr = row.first;
      if (!any) {
        r.row1 = r.row2 = rr;
        r.col1 = r.col2 = c;
        any = true;
      } else {
        r.row1 = std::min(r.row1, rr);
        r.row2 = std::max(r.row2, rr);
        r.col1 = std::min(r.col1, c);
        r.col2 = std::max(r.col2, c);
      }
    }
  }
  if (!any) {
    r.row1 = r.col1 = r.row2 = r.col2 = 0;
  }
  return r;
}

void Worksheet::Merge(int row1, int col1, int row2, int col2) {
  Range r;
  r.row1 = std::min(row1, row2);
  r.col1 = std::min(col1, col2);
  r.row2 = std::max(row1, row2);
  r.col2 = std::max(col1, col2);
  if (!r.valid()) return;
  merges_.push_back(r);
}

bool Worksheet::Merged(int row, int col) const {
  for (const Range& m : merges_) {
    if (row >= m.row1 && row <= m.row2 && col >= m.col1 && col <= m.col2) return true;
  }
  return false;
}

double Worksheet::ColumnWidth(int col) const {
  auto it = col_widths_.find(col);
  return it == col_widths_.end() ? 8.43 : it->second;  // Excel 默认 8.43 字符
}

void Worksheet::SetColumnWidth(int col, double chars) {
  col_widths_[col] = std::max(0.0, chars);
}

double Worksheet::RowHeight(int row) const {
  auto it = row_heights_.find(row);
  return it == row_heights_.end() ? 15.0 : it->second;  // Excel 默认 15 点
}

void Worksheet::SetRowHeight(int row, double pt) { row_heights_[row] = std::max(0.0, pt); }

// ---- Workbook ------------------------------------------------------------

Workbook Workbook::Create() {
  Workbook wb;
  wb.styles.push_back(CellStyle{});
  wb.AddSheet("Sheet1");
  return wb;
}

Workbook Workbook::Clone() const {
  Workbook out;
  out.sheets.reserve(sheets.size());
  for (const std::unique_ptr<Worksheet>& sheet : sheets) {
    auto copy = std::make_unique<Worksheet>(*sheet);
    copy->owner_ = &out;
    out.sheets.push_back(std::move(copy));
  }
  out.shared_strings = shared_strings;
  out.styles = styles;
  out.names = names;
  out.unknown_parts = unknown_parts;
  out.drawings = drawings;
  out.charts = charts;
  out.dxfs = dxfs;
  out.strict_validation = strict_validation;
  out.date1904 = date1904;
  out.active_sheet = active_sheet;
  return out;
}

Worksheet* Workbook::SheetAt(int index) {
  if (index < 0 || index >= sheet_count()) return nullptr;
  return sheets[static_cast<size_t>(index)].get();
}

const Worksheet* Workbook::SheetAt(int index) const {
  if (index < 0 || index >= sheet_count()) return nullptr;
  return sheets[static_cast<size_t>(index)].get();
}

Worksheet* Workbook::SheetByName(std::string_view name) {
  for (auto& s : sheets) {
    if (s->name() == name) return s.get();
  }
  return nullptr;
}

const Worksheet* Workbook::SheetByName(std::string_view name) const {
  return FindSheetByName(name);
}

const Worksheet* Workbook::FindSheetByName(std::string_view name) const {
  for (const auto& s : sheets) {
    if (s->name() == name) return s.get();
  }
  return nullptr;
}

Worksheet& Workbook::AddSheet(std::string name) {
  auto ws = std::make_unique<Worksheet>(std::move(name));
  ws->owner_ = this;
  sheets.push_back(std::move(ws));
  return *sheets.back();
}

void Workbook::RemoveSheet(int index) {
  if (index < 0 || index >= sheet_count()) return;
  sheets.erase(sheets.begin() + index);
}

int32_t Workbook::InternString(const std::string& text) {
  if (string_index_.size() != shared_strings.size()) {
    string_index_.clear();
    string_index_.reserve(shared_strings.size() * 2);
    for (size_t i = 0; i < shared_strings.size(); ++i) {
      string_index_.emplace(shared_strings[i], static_cast<int32_t>(i));
    }
  }
  auto it = string_index_.find(text);
  if (it != string_index_.end()) return it->second;
  const int32_t id = static_cast<int32_t>(shared_strings.size());
  shared_strings.push_back(text);
  string_index_.emplace(text, id);
  return id;
}

const std::string& Workbook::StringAt(int32_t id) const {
  static const std::string kEmpty;
  if (id < 0 || static_cast<size_t>(id) >= shared_strings.size()) return kEmpty;
  return shared_strings[static_cast<size_t>(id)];
}

const CellStyle& Workbook::StyleAt(int32_t id) const {
  if (id < 0 || static_cast<size_t>(id) >= styles.size()) return DefaultStyle();
  return styles[static_cast<size_t>(id)];
}

int32_t Workbook::AddStyle(CellStyle style) {
  for (size_t i = 0; i < styles.size(); ++i) {
    const CellStyle& s = styles[i];
    if (s.font.name == style.font.name && s.font.size_pt == style.font.size_pt &&
        s.font.bold == style.font.bold && s.font.italic == style.font.italic &&
        s.font.underline == style.font.underline && s.font.strike == style.font.strike &&
        s.font.color == style.font.color && s.num_fmt_id == style.num_fmt_id &&
        s.fill == style.fill && s.halign == style.halign && s.valign == style.valign &&
        s.wrap == style.wrap && s.border_mask == style.border_mask) {
      return static_cast<int32_t>(i);
    }
  }
  styles.push_back(std::move(style));
  return static_cast<int32_t>(styles.size() - 1);
}

const CellStyle& Workbook::DefaultStyle() const {
  static const CellStyle kDefault{};
  return styles.empty() ? kDefault : styles[0];
}

Value Workbook::ValueOf(const Worksheet& ws, int row, int col) const { return ws.GetValue(row, col); }

// ---- 条件格式 -------------------------------------------------------------

const char* CfOperatorName(CfOperator op) noexcept {
  switch (op) {
    case CfOperator::LessThan: return "lessThan";
    case CfOperator::LessEqual: return "lessThanOrEqual";
    case CfOperator::Equal: return "equal";
    case CfOperator::NotEqual: return "notEqual";
    case CfOperator::GreaterEqual: return "greaterThanOrEqual";
    case CfOperator::GreaterThan: return "greaterThan";
    case CfOperator::Between: return "between";
    case CfOperator::NotBetween: return "notBetween";
    case CfOperator::None: break;
  }
  return "equal";
}

namespace {

// 条件格式求值上下文：把单元格引用解析到工作簿。
class CfEvalContext : public EvalContext {
 public:
  CfEvalContext(const Workbook& book, const Worksheet& ws, int row, int col)
      : book_(book), ws_(ws), row_(row), col_(col) {}

  Value CellValue(std::string_view sheet, int row, int col) override {
    const Worksheet* target = &ws_;
    if (!sheet.empty()) {
      const Worksheet* named = book_.SheetByName(sheet);
      if (named != nullptr) target = named;
    }
    return book_.ValueOf(*target, row, col);
  }

  void ForEachInRange(std::string_view sheet, const Range& range,
                      const std::function<void(int row, int col, const Value&)>& fn) override {
    const Worksheet* target = &ws_;
    if (!sheet.empty()) {
      const Worksheet* named = book_.SheetByName(sheet);
      if (named != nullptr) target = named;
    }
    for (int r = range.row1; r <= range.row2; ++r) {
      for (int c = range.col1; c <= range.col2; ++c) {
        fn(r, c, book_.ValueOf(*target, r, c));
      }
    }
  }

  bool LookupName(std::string_view name, Range* range, std::string* sheet) override {
    for (const DefinedName& dn : book_.names) {
      if (dn.name != name) continue;
      // "Sheet1!$A$1:$B$3" -> 解析出区域；跨表引用这里只取区域，跨表名忽略。
      const size_t bang = dn.refers_to.find('!');
      std::string_view ref = dn.refers_to;
      if (bang != std::string::npos) ref = dn.refers_to.substr(bang + 1);
      CellRef a;
      CellRef b;
      const size_t colon = ref.find(':');
      if (colon == std::string_view::npos) {
        if (!ParseCellName(ref, &a)) return false;
        *range = Range{a.row, a.col, a.row, a.col};
      } else {
        if (!ParseCellName(ref.substr(0, colon), &a)) return false;
        if (!ParseCellName(ref.substr(colon + 1), &b)) return false;
        *range = Range{a.row, a.col, b.row, b.col};
      }
      if (sheet != nullptr) *sheet = "";
      return true;
    }
    return false;
  }

  double TodaySerial() const override {
    (void)row_;
    (void)col_;
    return 46231.0;  // 固定值：条件格式求值不应引入"今天"的不确定性
  }

 private:
  const Workbook& book_;
  const Worksheet& ws_;
  int row_;
  int col_;
};

// 把 AST 里的相对引用整体平移 (dr,dc)：条件格式的公式以区域左上角为基准书写，
// 作用到区域内其它单元格时相对引用要跟着偏移。
NodePtr ShiftAst(const NodePtr& node, int dr, int dc) {
  if (node == nullptr) return nullptr;
  Node copy = *node;
  if (copy.kind == Node::Kind::Ref) {
    if (!copy.row1_abs) copy.row1 += dr;
    if (!copy.col1_abs) copy.col1 += dc;
  } else if (copy.kind == Node::Kind::RangeRef) {
    if (!copy.row1_abs) copy.row1 += dr;
    if (!copy.col1_abs) copy.col1 += dc;
    if (!copy.row2_abs) copy.row2 += dr;
    if (!copy.col2_abs) copy.col2 += dc;
  } else {
    for (NodePtr& arg : copy.args) arg = ShiftAst(arg, dr, dc);
  }
  return std::make_shared<const Node>(std::move(copy));
}

bool CompareOperands(CfOperator op, double lhs, const std::vector<double>& ops) {
  if (ops.empty()) return false;
  const double a = ops[0];
  switch (op) {
    case CfOperator::LessThan: return lhs < a;
    case CfOperator::LessEqual: return lhs <= a;
    case CfOperator::Equal: return std::fabs(lhs - a) < 1e-9;
    case CfOperator::NotEqual: return std::fabs(lhs - a) >= 1e-9;
    case CfOperator::GreaterEqual: return lhs >= a;
    case CfOperator::GreaterThan: return lhs > a;
    case CfOperator::Between:
      return ops.size() >= 2 && lhs >= std::min(a, ops[1]) && lhs <= std::max(a, ops[1]);
    case CfOperator::NotBetween:
      return ops.size() >= 2 && !(lhs >= std::min(a, ops[1]) && lhs <= std::max(a, ops[1]));
    case CfOperator::None: break;
  }
  return false;
}

bool RuleMatches(const Workbook& book, const Worksheet& ws, const ConditionalFormat& cf,
                 const ConditionalRule& rule, int row, int col) {
  if (rule.type == CfType::CellIs) {
    const Value v = ws.GetValue(row, col);
    // cellIs 的阈值是数值操作数（P9 只做数值比较）；文本单元格不命中。
    if (v.kind != Value::Kind::Number) return false;
    return CompareOperands(rule.op, v.number, rule.operands);
  }
  if (rule.type == CfType::Expression) {
    if (rule.formula.empty()) return false;
    ParseResult parsed = ParseFormula(rule.formula);
    if (!parsed.ok || parsed.root == nullptr) return false;
    const int dr = row - cf.range.row1;
    const int dc = col - cf.range.col1;
    NodePtr shifted = ShiftAst(parsed.root, dr, dc);
    CfEvalContext ctx(book, ws, row, col);
    const Value result = Evaluate(*shifted, ctx);
    if (result.kind == Value::Kind::Bool) return result.boolean;
    if (result.kind == Value::Kind::Number) return result.number != 0.0;
    return false;
  }
  return false;
}

}  // namespace

const CfDxf* MatchedConditionalDxf(const Workbook& book, const Worksheet& ws, int row, int col) {
  const std::vector<ConditionalFormat>& formats = ws.conditional();
  if (formats.empty()) return nullptr;
  const CfDxf* best = nullptr;
  int best_priority = 0;
  for (const ConditionalFormat& cf : formats) {
    if (!cf.range.valid()) continue;
    if (row < cf.range.row1 || row > cf.range.row2 || col < cf.range.col1 || col > cf.range.col2) {
      continue;
    }
    for (const ConditionalRule& rule : cf.rules) {
      if (!RuleMatches(book, ws, cf, rule, row, col)) continue;
      if (best == nullptr || rule.priority < best_priority) {
        best = &rule.dxf;
        best_priority = rule.priority;
      }
    }
  }
  return best;
}

// ---- 数据验证（F-205）---------------------------------------------------

const char* ValidationTypeName(ValidationType t) noexcept {
  switch (t) {
    case ValidationType::List: return "list";
    case ValidationType::Whole: return "whole";
    case ValidationType::Decimal: return "decimal";
    case ValidationType::Date: return "date";
    case ValidationType::TextLength: return "textLength";
    case ValidationType::Custom: return "custom";
    default: return "unknown";
  }
}

const char* ValidationOpName(ValidationOp op) noexcept {
  switch (op) {
    case ValidationOp::Between: return "between";
    case ValidationOp::NotBetween: return "notBetween";
    case ValidationOp::Equal: return "equal";
    case ValidationOp::NotEqual: return "notEqual";
    case ValidationOp::LessThan: return "lessThan";
    case ValidationOp::LessEqual: return "lessEqual";
    case ValidationOp::GreaterThan: return "greaterThan";
    case ValidationOp::GreaterEqual: return "greaterEqual";
    default: return "none";
  }
}

// 解析带可选 '$' 的单元格地址（如 $B$3 / B3），用于数据验证的 sqref 与区域引用。
static bool ParseCellRefDv(std::string_view s, int* row, int* col) {
  size_t i = 0;
  if (i < s.size() && s[i] == '$') ++i;
  std::string letters;
  while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) {
    letters.push_back(static_cast<char>(s[i]));
    ++i;
  }
  if (letters.empty()) return false;
  int c = ColumnFromName(letters);
  if (c < 0) return false;
  if (i < s.size() && s[i] == '$') ++i;
  int r = 0;
  bool any = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    r = r * 10 + (s[i] - '0');
    ++i;
    any = true;
  }
  if (!any || i != s.size()) return false;
  *col = c;
  *row = r - 1;
  return true;
}

// 解析 formula1 里的区域引用（可能带表名前缀：'My Sheet'!$A$1:$A$3 或 Sheet1!$A$1:$A$3
// 或 $A$1:$A$3）。返回 false 表示这不是区域引用（应视为内联列表）。
static bool ParseSheetRangeDv(std::string_view formula, std::string* out_sheet,
                              Range* out_range) {
  std::string_view body = formula;
  if (formula.size() >= 2 && formula.front() == '"' && formula.back() == '"') {
    return false;  // 内联列表
  }
  // 去掉首尾空白
  size_t a = 0, b = formula.size();
  while (a < b && (formula[a] == ' ' || formula[a] == '\t')) ++a;
  while (b > a && (formula[b - 1] == ' ' || formula[b - 1] == '\t')) --b;
  body = formula.substr(a, b - a);
  if (body.empty()) return false;
  // 表名前缀
  size_t bang = std::string_view::npos;
  if (body.front() == '\'') {
    size_t close = body.find('\'', 1);
    if (close == std::string_view::npos) return false;
    if (close + 1 < body.size() && body[close + 1] == '!') bang = close + 1;
  } else {
    bang = body.find('!');
  }
  std::string_view ref = (bang == std::string_view::npos) ? body : body.substr(bang + 1);
  if (out_sheet != nullptr) {
    if (bang != std::string_view::npos) {
      std::string_view sn = (body.front() == '\'') ? body.substr(1, bang - 2)
                                                   : body.substr(0, bang);
      *out_sheet = std::string(sn);
    } else {
      out_sheet->clear();
    }
  }
  size_t colon = ref.find(':');
  int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
  if (colon == std::string_view::npos) {
    if (!ParseCellRefDv(ref, &r1, &c1)) return false;
    r2 = r1;
    c2 = c1;
  } else {
    if (!ParseCellRefDv(ref.substr(0, colon), &r1, &c1)) return false;
    if (!ParseCellRefDv(ref.substr(colon + 1), &r2, &c2)) return false;
  }
  out_range->row1 = r1;
  out_range->col1 = c1;
  out_range->row2 = r2;
  out_range->col2 = c2;
  return true;
}

static bool CiEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

// 把运行期值转成用于列表比对的文本（与 Excel 的列表比对口径一致）。
static std::string DvValueText(const Value& v) {
  switch (v.kind) {
    case Value::Kind::Text: return v.text;
    case Value::Kind::Number: return FormatValue(v);
    case Value::Kind::Bool: return v.boolean ? "TRUE" : "FALSE";
    case Value::Kind::Empty: return std::string();
    default: return std::string();
  }
}

// 直接按 Value 落盘（绕过验证），供 WriteCell 在允许/软失败时调用。
static void SetCellValueRaw(Worksheet& ws, int row, int col, const Value& v) {
  Cell& cell = ws.CellAt(row, col);
  switch (v.kind) {
    case Value::Kind::Number:
      cell.kind = CellKind::Number;
      cell.number = v.number;
      cell.cached = v;
      cell.dirty = false;
      break;
    case Value::Kind::Text:
      cell.kind = CellKind::Text;
      cell.cached = v;
      cell.dirty = false;
      if (ws.owner() != nullptr) cell.text_id = ws.owner()->InternString(v.text);
      else cell.text_id = -1;
      break;
    case Value::Kind::Bool:
      cell.kind = CellKind::Bool;
      cell.boolean = v.boolean;
      cell.cached = v;
      cell.dirty = false;
      break;
    case Value::Kind::Error:
      cell.kind = CellKind::Error;
      cell.error = v.error;
      cell.cached = v;
      cell.dirty = false;
      break;
    default:
      cell.kind = CellKind::Empty;
      cell.cached = Value::Empty();
      cell.dirty = false;
      cell.text_id = -1;
      break;
  }
}

const DataValidation* ValidationAt(const Worksheet& ws, int row, int col) {
  for (const DataValidation& dv : ws.validations()) {
    for (const Range& r : dv.ranges) {
      if (row >= r.row1 && row <= r.row2 && col >= r.col1 && col <= r.col2) {
        return &dv;
      }
    }
  }
  return nullptr;
}

std::vector<std::string> ListValues(const Workbook& book, const Worksheet& ws,
                                    const DataValidation& dv) {
  std::vector<std::string> out;
  std::string f = dv.formula1;
  // 内联列表：OOXML 形如 "Apple,Banana,Cherry"（外层引号）。
  if (f.size() >= 2 && f.front() == '"' && f.back() == '"') {
    std::string inner = f.substr(1, f.size() - 2);
    std::string cur;
    for (char c : inner) {
      if (c == ',') {
        out.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    out.push_back(cur);
    return out;
  }
  // 区域引用：把区域内每个非空单元格的文本值收集为合法取值。
  std::string sheet_name;
  Range rng;
  if (!ParseSheetRangeDv(f, &sheet_name, &rng)) return out;
  const Worksheet* target = &ws;
  if (!sheet_name.empty()) {
    const Worksheet* s = book.SheetByName(sheet_name);
    if (s != nullptr) target = s;
  }
  for (int r = rng.row1; r <= rng.row2; ++r) {
    for (int c = rng.col1; c <= rng.col2; ++c) {
      Value v = book.ValueOf(*target, r, c);
      if (v.kind == Value::Kind::Empty) continue;
      out.push_back(DvValueText(v));
    }
  }
  return out;
}

ValidationResult Worksheet::WriteCell(int row, int col, const Value& v) {
  ValidationResult res;
  const DataValidation* dv = ValidationAt(*this, row, col);
  bool violation = false;
  if (dv != nullptr && dv->type == ValidationType::List) {
    bool is_blank = (v.kind == Value::Kind::Empty);
    if (is_blank && dv->allow_blank) {
      // 允许空白，视为合法。
    } else {
      std::vector<std::string> allowed = ListValues(*owner_, *this, *dv);
      std::string candidate = DvValueText(v);
      bool ok = false;
      for (const std::string& a : allowed) {
        if (CiEquals(a, candidate)) {
          ok = true;
          break;
        }
      }
      if (!ok) violation = true;
    }
  }

  if (violation && owner_ != nullptr && owner_->strict_validation) {
    res.rejected = true;
    Warning w;
    w.code = static_cast<int>(WarningCode::ValidationFailed);
    w.module = "sheet";
    w.path = name_;
    w.message = "数据验证（列表）未通过：单元格 " + CellName(row, col) +
                " 的值不在允许列表中，严格模式已拒绝写入。";
    owner_->write_warnings_.Add(w);
    return res;  // written 保持 false
  }

  SetCellValueRaw(*this, row, col, v);
  res.written = true;
  if (violation) {
    res.warned = true;
    if (owner_ != nullptr) {
      Warning w;
      w.code = static_cast<int>(WarningCode::ValidationFailed);
      w.module = "sheet";
      w.path = name_;
      w.message = "数据验证（列表）未通过：单元格 " + CellName(row, col) +
                  " 的值不在允许列表中，已按默认模式写入并产生警告。";
      owner_->write_warnings_.Add(w);
    }
  }
  return res;
}

}  // namespace sheet
}  // namespace doccore
