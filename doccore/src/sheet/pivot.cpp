// DocCore - 数据透视表：刷新（重算 + 回写目标区域）与建表。
//
// 这里的"深度"体现在三件事：
//   1. 用**工作表里的实时数据**重算，而不是缓存记录——缓存是上次刷新的快照，
//      刷新就是把快照换成现在。
//   2. 目标区域先清空再重写，所以数据变少时不会留下上次的残行残列。
//   3. 行/列方向的 item 组合按"首次出现顺序"枚举，并重建 rowItems/colItems
//      描述，写出后 Excel 才能把表头和数据对齐。
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "doccore/sheet.h"

namespace doccore {
namespace sheet {
namespace {

// 聚合累加器。Count 数非空格数，CountNumbers 只数数值——Excel 里这是两个
// 不同的函数（COUNTA 与 COUNT）。
struct Accum {
  double sum = 0.0;
  int count = 0;         // 非空
  int count_numbers = 0;
  double product = 1.0;
  double min_value = 0.0;
  double max_value = 0.0;
  bool has_min = false;
  // 方差用 Welford 的在线算法，避免大数值下的精度损失。
  int n = 0;
  double mean = 0.0;
  double m2 = 0.0;

  void Add(const Value& v) {
    if (v.kind == Value::Kind::Empty) return;
    ++count;
    double x = 0.0;
    bool numeric = false;
    switch (v.kind) {
      case Value::Kind::Number:
        x = v.number;
        numeric = true;
        break;
      case Value::Kind::Bool:
        x = v.boolean ? 1.0 : 0.0;
        numeric = true;
        break;
      case Value::Kind::Text:
        // 文本型数字也计入（Excel 在透视表里会这么算）。
        numeric = TryParseNumber(v.text, &x);
        break;
      default:
        break;
    }
    if (!numeric) return;
    ++count_numbers;
    sum += x;
    product *= x;
    if (!has_min || x < min_value) min_value = x;
    if (!has_min || x > max_value) max_value = x;
    has_min = true;
    ++n;
    const double delta = x - mean;
    mean += delta / n;
    m2 += delta * (x - mean);
  }

  static bool TryParseNumber(const std::string& text, double* out) {
    if (text.empty()) return false;
    try {
      size_t used = 0;
      const double v = std::stod(text, &used);
      // 允许尾部空白，但不接受 "12abc" 这种。
      while (used < text.size() && (text[used] == ' ' || text[used] == '\t')) ++used;
      if (used != text.size()) return false;
      *out = v;
      return true;
    } catch (...) {
      return false;
    }
  }

  double Result(PivotAggregate agg) const {
    switch (agg) {
      case PivotAggregate::Sum:
        return sum;
      case PivotAggregate::Count:
        return static_cast<double>(count);
      case PivotAggregate::CountNumbers:
        return static_cast<double>(count_numbers);
      case PivotAggregate::Average:
        return count_numbers > 0 ? sum / count_numbers : 0.0;
      case PivotAggregate::Max:
        return has_min ? max_value : 0.0;
      case PivotAggregate::Min:
        return has_min ? min_value : 0.0;
      case PivotAggregate::Product:
        return count_numbers > 0 ? product : 0.0;
      case PivotAggregate::StdDev:
        return n > 1 ? std::sqrt(m2 / (n - 1)) : 0.0;
      case PivotAggregate::StdDevP:
        return n > 0 ? std::sqrt(m2 / n) : 0.0;
      case PivotAggregate::Variance:
        return n > 1 ? m2 / (n - 1) : 0.0;
      case PivotAggregate::VarianceP:
        return n > 0 ? m2 / n : 0.0;
    }
    return 0.0;
  }
};

std::string ValueText(const Value& v) {
  switch (v.kind) {
    case Value::Kind::Number: {
      // 整数不显示小数尾巴，和 Excel 的默认显示一致。
      char buf[64];
      if (std::fabs(v.number - std::llround(v.number)) < 1e-9) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v.number)));
      } else {
        std::snprintf(buf, sizeof(buf), "%.10g", v.number);
      }
      return buf;
    }
    case Value::Kind::Bool:
      return v.boolean ? "TRUE" : "FALSE";
    case Value::Kind::Text:
      return v.text;
    case Value::Kind::Error:
      return "#ERR";
    case Value::Kind::Empty:
      break;
  }
  return std::string();
}

// 一个字段取值的比较键：数值按数值排，其余按文本排。
struct SortKey {
  bool numeric = false;
  double number = 0.0;
  std::string text;
};

SortKey KeyOf(const Value& v) {
  SortKey k;
  switch (v.kind) {
    case Value::Kind::Number:
      k.numeric = true;
      k.number = v.number;
      break;
    case Value::Kind::Bool:
      k.numeric = true;
      k.number = v.boolean ? 1.0 : 0.0;
      break;
    case Value::Kind::Text:
      if (Accum::TryParseNumber(v.text, &k.number)) {
        k.numeric = true;
      } else {
        k.text = v.text;
      }
      break;
    default:
      break;
  }
  return k;
}

bool KeyLess(const SortKey& a, const SortKey& b) {
  if (a.numeric != b.numeric) return a.numeric;  // 数值排在文本前（Excel 的顺序）
  if (a.numeric) return a.number < b.number;
  return a.text < b.text;
}

bool KeyEqual(const SortKey& a, const SortKey& b) {
  if (a.numeric != b.numeric) return false;
  if (a.numeric) return std::fabs(a.number - b.number) < 1e-12;
  return a.text == b.text;
}

// 源数据的一行：每个字段一个 Value。
using RowValues = std::vector<Value>;

}  // namespace

const char* PivotAggregateName(PivotAggregate a) noexcept {
  switch (a) {
    case PivotAggregate::Sum: return "sum";
    case PivotAggregate::Count: return "count";
    case PivotAggregate::Average: return "average";
    case PivotAggregate::Max: return "max";
    case PivotAggregate::Min: return "min";
    case PivotAggregate::Product: return "product";
    case PivotAggregate::CountNumbers: return "countNums";
    case PivotAggregate::StdDev: return "stdDev";
    case PivotAggregate::StdDevP: return "stdDevp";
    case PivotAggregate::Variance: return "var";
    case PivotAggregate::VarianceP: return "varp";
  }
  return "sum";
}

PivotAggregate PivotAggregateFromName(std::string_view name) {
  if (name == "count") return PivotAggregate::Count;
  if (name == "average") return PivotAggregate::Average;
  if (name == "max") return PivotAggregate::Max;
  if (name == "min") return PivotAggregate::Min;
  if (name == "product") return PivotAggregate::Product;
  if (name == "countNums") return PivotAggregate::CountNumbers;
  if (name == "stdDev") return PivotAggregate::StdDev;
  if (name == "stdDevp") return PivotAggregate::StdDevP;
  if (name == "var") return PivotAggregate::Variance;
  if (name == "varp") return PivotAggregate::VarianceP;
  return PivotAggregate::Sum;
}

int PivotFieldIndex(const PivotTable& table, std::string_view name) {
  for (size_t i = 0; i < table.fields.size(); ++i) {
    if (table.fields[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

namespace {

// 目标区域的当前占用范围：清空时要用。没有内容时返回空。
Range UsedAround(const Worksheet& ws, const CellRef& dest, int max_row, int max_col) {
  Range r;
  r.row1 = dest.row;
  r.col1 = dest.col;
  r.row2 = dest.row - 1;
  r.col2 = dest.col - 1;
  for (const auto& col : ws.columns()) {
    if (col.first < dest.col || col.first > max_col) continue;
    for (const auto& row : col.second) {
      if (row.first < dest.row || row.first > max_row) continue;
      if (row.first > r.row2) r.row2 = row.first;
      if (col.first > r.col2) r.col2 = col.first;
    }
  }
  return r;
}

}  // namespace

int RefreshPivot(Workbook& book, PivotTable& table) {
  if (!table.source.valid()) {
    throw Error(ErrorCode::InvalidArgument, "pivot: source range is empty");
  }
  const Worksheet* src = book.SheetByName(table.source_sheet);
  if (src == nullptr) {
    throw Error(ErrorCode::InvalidArgument, "pivot: source sheet not found: " + table.source_sheet);
  }
  Worksheet* dst = book.SheetByName(table.sheet);
  if (dst == nullptr) {
    throw Error(ErrorCode::InvalidArgument, "pivot: target sheet not found: " + table.sheet);
  }

  const int first_data_row = table.source.row1 + 1;

  // ---- 1. 字段名（表头行）---------------------------------------------
  // 已有字段表就按位置对齐名字；没有则从表头读。
  const int field_count = table.source.col2 - table.source.col1 + 1;
  if (static_cast<int>(table.fields.size()) != field_count) {
    table.fields.assign(static_cast<size_t>(field_count), PivotCacheField{});
    for (int c = 0; c < field_count; ++c) {
      const Value v = src->GetValue(table.source.row1, table.source.col1 + c);
      std::string name = ValueText(v);
      if (name.empty()) name = "字段" + std::to_string(c + 1);
      table.fields[static_cast<size_t>(c)].name = name;
    }
  }
  if (table.pivot_fields.size() != table.fields.size()) {
    table.pivot_fields.assign(table.fields.size(), PivotField{});
    for (size_t i = 0; i < table.fields.size(); ++i) table.pivot_fields[i].name = table.fields[i].name;
  }
  for (size_t i = 0; i < table.fields.size(); ++i) {
    table.pivot_fields[i].axis = PivotAxis::None;
    table.pivot_fields[i].data_field = false;
  }
  for (int idx : table.row_fields) {
    if (idx >= 0 && idx < static_cast<int>(table.pivot_fields.size())) {
      table.pivot_fields[static_cast<size_t>(idx)].axis = PivotAxis::Row;
    }
  }
  for (int idx : table.column_fields) {
    if (idx >= 0 && idx < static_cast<int>(table.pivot_fields.size())) {
      table.pivot_fields[static_cast<size_t>(idx)].axis = PivotAxis::Column;
    }
  }
  for (int idx : table.page_fields) {
    if (idx >= 0 && idx < static_cast<int>(table.pivot_fields.size())) {
      table.pivot_fields[static_cast<size_t>(idx)].axis = PivotAxis::Page;
    }
  }
  for (const PivotDataField& df : table.data_fields) {
    if (df.field_index >= 0 && df.field_index < static_cast<int>(table.pivot_fields.size())) {
      table.pivot_fields[static_cast<size_t>(df.field_index)].data_field = true;
    }
  }
  if (table.data_fields.empty()) {
    // 没指定数据字段时，用第一个非行/列/页字段求和（Excel 的默认行为）。
    for (size_t i = 0; i < table.pivot_fields.size(); ++i) {
      if (table.pivot_fields[i].axis != PivotAxis::None) continue;
      PivotDataField df;
      df.field_index = static_cast<int>(i);
      df.name = "求和项:" + table.fields[i].name;
      table.data_fields.push_back(df);
      table.pivot_fields[i].data_field = true;
      break;
    }
  }

  // ---- 2. 扫一遍源数据，收集每行的字段值 --------------------------------
  std::vector<RowValues> rows;
  rows.reserve(static_cast<size_t>(std::max(0, table.source.row2 - first_data_row + 1)));
  for (int r = first_data_row; r <= table.source.row2; ++r) {
    RowValues values(static_cast<size_t>(field_count));
    bool any = false;
    for (int c = 0; c < field_count; ++c) {
      values[static_cast<size_t>(c)] = src->GetValue(r, table.source.col1 + c);
      if (values[static_cast<size_t>(c)].kind != Value::Kind::Empty) any = true;
    }
    if (!any) continue;  // 整行空：跳过（Excel 也不把空行算进记录数）
    rows.push_back(std::move(values));
  }

  // ---- 3. 重建共享项（字符串字段去重排序，数值字段记 min/max）----------
  for (int c = 0; c < field_count; ++c) {
    PivotCacheField& f = table.fields[static_cast<size_t>(c)];
    f.shared_items.clear();
    f.has_numbers = false;
    f.has_strings = false;
    f.min_value = 0;
    f.max_value = 0;
    std::vector<SortKey> keys;
    for (const RowValues& row : rows) {
      const Value& v = row[static_cast<size_t>(c)];
      if (v.kind == Value::Kind::Empty) continue;
      keys.push_back(KeyOf(v));
    }
    std::sort(keys.begin(), keys.end(), KeyLess);
    keys.erase(std::unique(keys.begin(), keys.end(), KeyEqual), keys.end());
    for (const SortKey& k : keys) {
      if (k.numeric) {
        f.has_numbers = true;
      } else {
        f.has_strings = true;
      }
    }
    if (f.has_numbers) {
      bool first = true;
      for (const SortKey& k : keys) {
        if (!k.numeric) continue;
        if (first) {
          f.min_value = k.number;
          f.max_value = k.number;
          first = false;
        } else {
          f.min_value = std::min(f.min_value, k.number);
          f.max_value = std::max(f.max_value, k.number);
        }
      }
    }
    // 行/列/页字段必须有完整的 item 表（表头要按下标取值）；纯数值的度量
    // 字段不需要。混合类型时统一按文本 item 存，取值时再转回数值。
    const PivotAxis axis = table.pivot_fields[static_cast<size_t>(c)].axis;
    const bool needs_items = axis != PivotAxis::None || f.has_strings;
    if (needs_items) {
      f.shared_items.reserve(keys.size());
      for (const SortKey& k : keys) f.shared_items.push_back(k.numeric ? ValueText(Value::Num(k.number))
                                                                      : k.text);
    }
  }

  // ---- 4. 行/列组合 ------------------------------------------------------
  auto item_index = [&](int field, const Value& v) -> int {
    const PivotCacheField& f = table.fields[static_cast<size_t>(field)];
    if (v.kind == Value::Kind::Empty) return -1;
    const SortKey k = KeyOf(v);
    const std::string text = k.numeric ? ValueText(Value::Num(k.number)) : k.text;
    for (size_t i = 0; i < f.shared_items.size(); ++i) {
      if (f.shared_items[i] == text) return static_cast<int>(i);
    }
    return -1;
  };

  // 组合的键是各字段 item 下标的串，值保留"按 item 顺序"的展示顺序。
  std::vector<std::vector<int>> row_combos;
  std::vector<std::vector<int>> col_combos;
  {
    std::map<std::string, size_t> seen_row;
    std::map<std::string, size_t> seen_col;
    for (const RowValues& row : rows) {
      std::vector<int> rk;
      std::string rs;
      for (int f : table.row_fields) {
        const int idx = item_index(f, row[static_cast<size_t>(f)]);
        rk.push_back(idx);
        rs += std::to_string(idx) + ",";
      }
      if (seen_row.find(rs) == seen_row.end()) {
        seen_row[rs] = row_combos.size();
        row_combos.push_back(rk);
      }
      std::vector<int> ck;
      std::string cs;
      for (int f : table.column_fields) {
        const int idx = item_index(f, row[static_cast<size_t>(f)]);
        ck.push_back(idx);
        cs += std::to_string(idx) + ",";
      }
      if (seen_col.find(cs) == seen_col.end()) {
        seen_col[cs] = col_combos.size();
        col_combos.push_back(ck);
      }
    }
    // 按 item 下标排序，得到与共享项一致的稳定顺序。
    std::sort(row_combos.begin(), row_combos.end());
    std::sort(col_combos.begin(), col_combos.end());
  }

  // ---- 5. 聚合 ----------------------------------------------------------
  // 键：行组合下标 * col_combos.size() + 列组合下标；再乘数据字段数。
  const size_t ncols = std::max<size_t>(1, col_combos.size());
  const size_t ndata = std::max<size_t>(1, table.data_fields.size());
  std::vector<Accum> cells(row_combos.size() * ncols * ndata);
  std::vector<Accum> row_totals(row_combos.size() * ndata);
  std::vector<Accum> col_totals(ncols * ndata);
  std::vector<Accum> grand(ndata);

  auto combo_pos = [](const std::vector<std::vector<int>>& list, const std::vector<int>& key) -> size_t {
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i] == key) return i;
    }
    return 0;
  };

  for (const RowValues& row : rows) {
    std::vector<int> rk;
    for (int f : table.row_fields) rk.push_back(item_index(f, row[static_cast<size_t>(f)]));
    std::vector<int> ck;
    for (int f : table.column_fields) ck.push_back(item_index(f, row[static_cast<size_t>(f)]));
    const size_t ri = combo_pos(row_combos, rk);
    const size_t ci = combo_pos(col_combos, ck);
    for (size_t d = 0; d < table.data_fields.size(); ++d) {
      const int fi = table.data_fields[d].field_index;
      if (fi < 0) continue;
      const Value& v = row[static_cast<size_t>(fi)];
      cells[(ri * ncols + ci) * ndata + d].Add(v);
      row_totals[ri * ndata + d].Add(v);
      col_totals[ci * ndata + d].Add(v);
      grand[d].Add(v);
    }
  }

  // ---- 6. 清空旧结果并写出 ----------------------------------------------
  const int nrowf = static_cast<int>(table.row_fields.size());
  const int ncolf = static_cast<int>(table.column_fields.size());
  const int data_count = static_cast<int>(table.data_fields.size());
  // 需要的列数：行标签列 + 列组合 * 数据字段 + （列总计）
  const int label_cols = std::max(1, nrowf);
  const int total_cols = label_cols + static_cast<int>(ncols) * std::max(1, data_count) +
                         (table.show_column_grand_totals ? std::max(1, data_count) : 0);
  const int total_rows = 2 + static_cast<int>(row_combos.size()) +
                         (table.show_row_grand_totals ? 1 : 0);

  // 旧结果可能比新结果大，先按"已知占用范围"清掉，否则数据变少时会留下
  // 上次的残行残列。
  {
    const Range old = UsedAround(*dst, table.destination, table.destination.row + total_rows + 64,
                                 table.destination.col + total_cols + 64);
    for (int r = old.row1; r <= old.row2; ++r) {
      for (int c = old.col1; c <= old.col2; ++c) {
        Cell* cell = dst->Find(r, c);
        if (cell == nullptr) continue;
        *cell = Cell{};  // 空值 + 无公式 + 无样式
      }
    }
  }

  int written = 0;
  const int r0 = table.destination.row;
  const int c0 = table.destination.col;
  auto put = [&](int r, int c, const std::string& text) {
    if (text.empty()) return;
    dst->SetText(r, c, text);
    ++written;
  };
  auto put_num = [&](int r, int c, double v) {
    dst->SetNumber(r, c, v);
    ++written;
  };

  // 表头两行：第一行写字段名，第二行写列组合标签。
  for (int i = 0; i < nrowf; ++i) {
    put(r0, c0 + i, table.fields[static_cast<size_t>(table.row_fields[static_cast<size_t>(i)])].name);
  }
  if (nrowf == 0) put(r0, c0, "值");
  // 列方向第一级字段名放在数据区第一列（Excel 的"列标签"位置）。
  {
    std::string col_head;
    for (size_t i = 0; i < table.column_fields.size(); ++i) {
      if (i != 0) col_head += " / ";
      col_head += table.fields[static_cast<size_t>(table.column_fields[i])].name;
    }
    if (col_head.empty() && data_count > 0) col_head = table.data_fields[0].name;
    put(r0, c0 + label_cols, col_head);
  }
  // 数据字段名：每个列组合一组；第二行写列组合标签（多级用 " / " 连接）。
  for (size_t ci = 0; ci < ncols; ++ci) {
    std::string combo_label;
    if (!col_combos.empty()) {
      const std::vector<int>& combo = col_combos[ci];
      for (size_t k = 0; k < combo.size(); ++k) {
        if (k != 0) combo_label += " / ";
        const int fi = table.column_fields[k];
        const PivotCacheField& f = table.fields[static_cast<size_t>(fi)];
        if (combo[k] >= 0 && combo[k] < static_cast<int>(f.shared_items.size())) {
          combo_label += f.shared_items[static_cast<size_t>(combo[k])];
        }
      }
    }
    for (int d = 0; d < data_count; ++d) {
      const int c = c0 + label_cols + static_cast<int>(ci) * std::max(1, data_count) + d;
      put(r0, c, table.data_fields[static_cast<size_t>(d)].name);
      put(r0 + 1, c, combo_label);
    }
  }
  if (table.show_column_grand_totals) {
    for (int d = 0; d < data_count; ++d) {
      const int c = c0 + label_cols + static_cast<int>(ncols) * std::max(1, data_count) + d;
      put(r0, c, "总计");
      put(r0 + 1, c, "总计");
    }
  }

  // 数据行。
  for (size_t ri = 0; ri < row_combos.size(); ++ri) {
    const int r = r0 + 2 + static_cast<int>(ri);
    const std::vector<int>& combo = row_combos[ri];
    for (int k = 0; k < nrowf; ++k) {
      const int fi = table.row_fields[static_cast<size_t>(k)];
      const PivotCacheField& f = table.fields[static_cast<size_t>(fi)];
      const int idx = k < static_cast<int>(combo.size()) ? combo[static_cast<size_t>(k)] : -1;
      if (idx >= 0 && idx < static_cast<int>(f.shared_items.size())) {
        put(r, c0 + k, f.shared_items[static_cast<size_t>(idx)]);
      } else if (idx >= 0) {
        put(r, c0 + k, "(空白)");
      }
    }
    if (nrowf == 0) put(r, c0, "总计");
    for (size_t ci = 0; ci < ncols; ++ci) {
      for (int d = 0; d < data_count; ++d) {
        const int c = c0 + label_cols + static_cast<int>(ci) * std::max(1, data_count) + d;
        const Accum& a = cells[(ri * ncols + ci) * ndata + static_cast<size_t>(d)];
        put_num(r, c, a.Result(table.data_fields[static_cast<size_t>(d)].aggregate));
      }
    }
    if (table.show_column_grand_totals) {
      for (int d = 0; d < data_count; ++d) {
        const int c = c0 + label_cols + static_cast<int>(ncols) * std::max(1, data_count) + d;
        put_num(r, c, row_totals[ri * ndata + static_cast<size_t>(d)]
                          .Result(table.data_fields[static_cast<size_t>(d)].aggregate));
      }
    }
  }

  // 行总计。
  if (table.show_row_grand_totals) {
    const int r = r0 + 2 + static_cast<int>(row_combos.size());
    put(r, c0, "总计");
    for (size_t ci = 0; ci < ncols; ++ci) {
      for (int d = 0; d < data_count; ++d) {
        const int c = c0 + label_cols + static_cast<int>(ci) * std::max(1, data_count) + d;
        put_num(r, c, col_totals[ci * ndata + static_cast<size_t>(d)]
                          .Result(table.data_fields[static_cast<size_t>(d)].aggregate));
      }
    }
    if (table.show_column_grand_totals) {
      for (int d = 0; d < data_count; ++d) {
        const int c = c0 + label_cols + static_cast<int>(ncols) * std::max(1, data_count) + d;
        put_num(r, c, grand[static_cast<size_t>(d)]
                          .Result(table.data_fields[static_cast<size_t>(d)].aggregate));
      }
    }
  }

  // ---- 7. 布局描述（rowItems / colItems）--------------------------------
  table.row_items.clear();
  for (size_t ri = 0; ri < row_combos.size(); ++ri) {
    PivotItem item;
    item.indices = row_combos[ri];
    table.row_items.push_back(std::move(item));
  }
  if (table.show_row_grand_totals) {
    PivotItem grand_item;
    grand_item.grand = true;
    table.row_items.push_back(std::move(grand_item));
  }
  table.column_items.clear();
  for (size_t ci = 0; ci < col_combos.size(); ++ci) {
    PivotItem item;
    item.indices = col_combos[ci];
    table.column_items.push_back(std::move(item));
  }
  if (table.show_column_grand_totals) {
    PivotItem grand_item;
    grand_item.grand = true;
    table.column_items.push_back(std::move(grand_item));
  }
  table.dirty = true;  // 保存时按 DOM 重新生成部件
  return written;
}

std::vector<std::vector<Value>> PivotSourceRows(const Workbook& book, const PivotTable& table) {
  std::vector<std::vector<Value>> out;
  if (!table.source.valid()) return out;
  const Worksheet* src = book.SheetByName(table.source_sheet);
  if (src == nullptr) return out;
  const int field_count = table.source.col2 - table.source.col1 + 1;
  for (int r = table.source.row1 + 1; r <= table.source.row2; ++r) {
    std::vector<Value> row(static_cast<size_t>(field_count));
    bool any = false;
    for (int c = 0; c < field_count; ++c) {
      row[static_cast<size_t>(c)] = src->GetValue(r, table.source.col1 + c);
      if (row[static_cast<size_t>(c)].kind != Value::Kind::Empty) any = true;
    }
    if (!any) continue;
    out.push_back(std::move(row));
  }
  return out;
}

PivotTable& AddPivotTable(Worksheet& ws, const Range& source, const CellRef& destination,
                          const std::vector<std::string>& row_fields,
                          const std::vector<std::string>& column_fields,
                          const std::vector<std::string>& data_fields, const std::string& name) {
  return AddPivotTable(ws, ws, source, destination, row_fields, column_fields, data_fields, name);
}

PivotTable& AddPivotTable(Worksheet& ws, Worksheet& source_ws, const Range& source,
                          const CellRef& destination,
                          const std::vector<std::string>& row_fields,
                          const std::vector<std::string>& column_fields,
                          const std::vector<std::string>& data_fields, const std::string& name) {
  Workbook* book = ws.owner();
  if (book == nullptr) {
    throw Error(ErrorCode::InvalidArgument, "pivot: worksheet is not attached to a workbook");
  }
  if (source_ws.owner() != book) {
    throw Error(ErrorCode::InvalidArgument, "pivot: source worksheet is in another workbook");
  }
  if (!source.valid()) {
    throw Error(ErrorCode::InvalidArgument, "pivot: source range is empty");
  }
  PivotTable table;
  table.sheet = ws.name();
  table.source = source;
  table.destination = destination;
  if (!name.empty()) {
    table.name = name;
  } else {
    table.name = "PivotTable" + std::to_string(book->pivot_tables.size() + 1);
  }
  table.cache_id = static_cast<int>(book->pivot_tables.size()) + 1;

  // 字段表先按源表表头建好，后面的名字才能解析。
  const int field_count = source.col2 - source.col1 + 1;
  table.fields.resize(static_cast<size_t>(field_count));
  table.pivot_fields.resize(static_cast<size_t>(field_count));
  for (int c = 0; c < field_count; ++c) {
    const Value v = source_ws.GetValue(source.row1, source.col1 + c);
    const std::string text = ValueText(v);
    table.fields[static_cast<size_t>(c)].name =
        text.empty() ? "字段" + std::to_string(c + 1) : text;
    table.pivot_fields[static_cast<size_t>(c)].name = table.fields[static_cast<size_t>(c)].name;
  }
  table.source_sheet = source_ws.name();

  auto resolve = [&](const std::vector<std::string>& names, const char* what) {
    std::vector<int> out;
    for (const std::string& n : names) {
      const int idx = PivotFieldIndex(table, n);
      if (idx < 0) {
        throw Error(ErrorCode::InvalidArgument,
                    std::string("pivot: ") + what + " field not found: " + n);
      }
      out.push_back(idx);
    }
    return out;
  };
  table.row_fields = resolve(row_fields, "row");
  table.column_fields = resolve(column_fields, "column");
  for (const std::string& n : data_fields) {
    const int idx = PivotFieldIndex(table, n);
    if (idx < 0) {
      throw Error(ErrorCode::InvalidArgument, "pivot: data field not found: " + n);
    }
    PivotDataField df;
    df.field_index = idx;
    df.name = "求和项:" + n;
    table.data_fields.push_back(std::move(df));
  }

  book->pivot_tables.push_back(std::move(table));
  PivotTable& stored = book->pivot_tables.back();
  RefreshPivot(*book, stored);
  return stored;
}

}  // namespace sheet
}  // namespace doccore
