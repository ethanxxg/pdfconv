// DocCore - 依赖图与重算。Design refs: docs/02 §3.4.2
//
// 步骤：解析 -> 建依赖 -> 拓扑排序（同时检测环）-> 按序求值。
// 环上的单元格互相污染，但不会传染给环外的单元格：环上标 #CIRC!，下游
// 只是把这个错误值当普通错误传播，和 Excel 的行为一致。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "doccore/formula.h"
#include "doccore/sheet.h"

namespace doccore {
namespace sheet {
namespace {

using NodePtr = std::shared_ptr<const Node>;

NodePtr AstOf(const Formula& formula) {
  return std::static_pointer_cast<const Node>(formula.ast);
}

void SetAst(Formula& formula, NodePtr node) {
  formula.ast = std::static_pointer_cast<const void>(node);
}

// (工作表下标, 行, 列)
using CellKey = std::tuple<int, int, int>;

int IndexOfSheet(const Workbook& wb, const Worksheet* ws) {
  for (int i = 0; i < wb.sheet_count(); ++i) {
    if (wb.SheetAt(i) == ws) return i;
  }
  return -1;
}

double NowSerial(bool date1904) {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto unix_seconds = duration_cast<seconds>(now.time_since_epoch()).count();
  const double unix_days = static_cast<double>(unix_seconds) / 86400.0;
  // 1970-01-01 的 Excel 序列数是 25569（1904 系统再减 1462）。
  return unix_days + 25569.0 - (date1904 ? 1462.0 : 0.0);
}

// 求值上下文：把 sheet 名字解析成工作表，并把"脏的公式单元格"当成循环引用。
class WorkbookEval : public EvalContext {
 public:
  WorkbookEval(Workbook& wb, Worksheet* current, double today)
      : wb_(wb), current_(current), today_(today) {}

  Value CellValue(std::string_view sheet, int row, int col) override {
    Worksheet* ws = Resolve(sheet);
    if (ws == nullptr) return Value::Err(CellError::Ref);
    const Cell* cell = ws->Find(row, col);
    if (cell == nullptr) return Value::Empty();
    if (cell->has_formula() && cell->dirty) {
      // 拓扑序保证依赖已经算完；还脏说明它在环上或者根本没排进这次重算。
      return Value::Err(CellError::Circular);
    }
    return ws->GetValue(row, col);
  }

  void ForEachInRange(std::string_view sheet, const Range& range,
                      const std::function<void(int, int, const Value&)>& fn) override {
    Worksheet* ws = Resolve(sheet);
    if (ws == nullptr) return;
    if (range.row1 < 0 || range.col1 < 0) return;
    auto& columns = ws->columns();
    auto col_it = columns.lower_bound(range.col1);
    for (; col_it != columns.end() && col_it->first <= range.col2; ++col_it) {
      auto& rows = col_it->second;
      auto row_it = rows.lower_bound(range.row1);
      for (; row_it != rows.end() && row_it->first <= range.row2; ++row_it) {
        if (row_it->second.empty()) continue;
        fn(row_it->first, col_it->first, ws->GetValue(row_it->first, col_it->first));
      }
    }
  }

  bool LookupName(std::string_view name, Range* range, std::string* sheet) override {
    for (const DefinedName& dn : wb_.names) {
      if (dn.name != name) continue;
      const ParseResult parsed = ParseFormula(dn.refers_to);
      if (!parsed.ok || parsed.root == nullptr) return false;
      const Node& node = *parsed.root;
      if (node.kind == Node::Kind::Ref) {
        range->row1 = range->row2 = node.row1;
        range->col1 = range->col2 = node.col1;
        *sheet = node.sheet;
        return true;
      }
      if (node.kind == Node::Kind::RangeRef) {
        *range = Range{node.row1, node.col1, node.row2, node.col2};
        *sheet = node.sheet;
        return true;
      }
      return false;
    }
    return false;
  }

  double TodaySerial() const override { return today_; }

 private:
  Worksheet* Resolve(std::string_view sheet) {
    if (sheet.empty()) return current_;
    for (auto& s : wb_.sheets) {
      if (s->name() == sheet) return s.get();
    }
    return nullptr;
  }

  Workbook& wb_;
  Worksheet* current_;
  double today_;
};

// 收集一个表达式直接依赖的单元格。
void CollectDeps(const Node& node, Worksheet* current, Workbook& wb, std::vector<CellKey>* out) {
  switch (node.kind) {
    case Node::Kind::Ref: {
      Worksheet* ws = current;
      if (!node.sheet.empty()) {
        ws = wb.SheetByName(node.sheet);
        if (ws == nullptr) return;
      }
      out->emplace_back(IndexOfSheet(wb, ws), node.row1, node.col1);
      return;
    }
    case Node::Kind::RangeRef: {
      Worksheet* ws = current;
      if (!node.sheet.empty()) {
        ws = wb.SheetByName(node.sheet);
        if (ws == nullptr) return;
      }
      const int ws_index = IndexOfSheet(wb, ws);
      auto& columns = ws->columns();
      auto col_it = columns.lower_bound(node.col1);
      for (; col_it != columns.end() && col_it->first <= node.col2; ++col_it) {
        auto& rows = col_it->second;
        auto row_it = rows.lower_bound(node.row1);
        for (; row_it != rows.end() && row_it->first <= node.row2; ++row_it) {
          if (row_it->second.empty()) continue;
          out->emplace_back(ws_index, row_it->first, col_it->first);
        }
      }
      return;
    }
    case Node::Kind::Name: {
      Range range{};
      std::string sheet;
      WorkbookEval probe(wb, current, 0.0);
      if (!probe.LookupName(node.text, &range, &sheet)) return;
      Worksheet* ws = sheet.empty() ? current : wb.SheetByName(sheet);
      if (ws == nullptr) return;
      const int ws_index = IndexOfSheet(wb, ws);
      auto& columns = ws->columns();
      auto col_it = columns.lower_bound(range.col1);
      for (; col_it != columns.end() && col_it->first <= range.col2; ++col_it) {
        auto& rows = col_it->second;
        auto row_it = rows.lower_bound(range.row1);
        for (; row_it != rows.end() && row_it->first <= range.row2; ++row_it) {
          if (row_it->second.empty()) continue;
          out->emplace_back(ws_index, row_it->first, col_it->first);
        }
      }
      return;
    }
    case Node::Kind::Unary:
    case Node::Kind::Binary:
    case Node::Kind::Call:
      for (const NodePtr& arg : node.args) {
        if (arg != nullptr) CollectDeps(*arg, current, wb, out);
      }
      return;
    default:
      return;
  }
}

struct Entry {
  Worksheet* ws = nullptr;
  int ws_index = -1;
  int row = 0;
  int col = 0;
  int32_t formula_id = -1;
};

int RecalcImpl(Workbook& wb, Worksheet* only, CancelToken* cancel) {
  const double today = NowSerial(wb.date1904);

  // 1. 收集要重算的公式单元格，顺手把 AST 解析出来。
  std::vector<Entry> entries;
  for (int i = 0; i < wb.sheet_count(); ++i) {
    Worksheet* ws = wb.SheetAt(i);
    if (ws == nullptr) continue;
    if (only != nullptr && ws != only) continue;
    for (auto& col : ws->columns()) {
      for (auto& row : col.second) {
        Cell& cell = row.second;
        if (!cell.has_formula()) continue;
        if (cell.formula_id >= static_cast<int32_t>(ws->formulas().size())) continue;
        Formula& formula = ws->formulas()[static_cast<size_t>(cell.formula_id)];
        if (formula.ast == nullptr) {
          ParseResult parsed = ParseFormula(formula.text);
          if (!parsed.ok || parsed.root == nullptr) {
            cell.cached = Value::Err(CellError::Name);
            cell.dirty = false;
            continue;
          }
          SetAst(formula, parsed.root);
        }
        Entry e;
        e.ws = ws;
        e.ws_index = i;
        e.row = row.first;
        e.col = col.first;
        e.formula_id = cell.formula_id;
        entries.push_back(e);
      }
    }
  }
  if (entries.empty()) return 0;

  std::map<CellKey, int> index_of;
  for (size_t i = 0; i < entries.size(); ++i) {
    index_of.emplace(CellKey(entries[i].ws_index, entries[i].row, entries[i].col),
                     static_cast<int>(i));
  }

  // 2. 建依赖边（只保留指向公式单元格的边）。
  std::vector<std::vector<int>> deps(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    const Formula& formula = e.ws->formulas()[static_cast<size_t>(e.formula_id)];
    NodePtr ast = AstOf(formula);
    if (ast == nullptr) continue;
    std::vector<CellKey> raw;
    CollectDeps(*ast, e.ws, wb, &raw);
    for (const CellKey& key : raw) {
      auto it = index_of.find(key);
      if (it == index_of.end()) continue;
      if (it->second == static_cast<int>(i)) continue;  // 自引用由求值阶段兜住
      deps[i].push_back(it->second);
    }
    std::sort(deps[i].begin(), deps[i].end());
    deps[i].erase(std::unique(deps[i].begin(), deps[i].end()), deps[i].end());
  }

  // 3. 迭代式 DFS：白 -> 灰 -> 黑，遇到灰边就是环。
  const size_t n = entries.size();
  std::vector<uint8_t> color(n, 0);
  std::vector<bool> cyclic(n, false);
  std::vector<int> order;
  order.reserve(n);
  std::vector<std::pair<int, size_t>> stack;
  for (size_t start = 0; start < n; ++start) {
    if (color[start] != 0) continue;
    color[start] = 1;
    stack.clear();
    stack.emplace_back(static_cast<int>(start), 0);
    while (!stack.empty()) {
      const size_t top = stack.size() - 1;
      const int node = stack[top].first;
      if (cancel != nullptr && (order.size() & 0xFF) == 0) cancel->ThrowIfCancelled();
      if (stack[top].second < deps[static_cast<size_t>(node)].size()) {
        const int dep = deps[static_cast<size_t>(node)][stack[top].second++];
        if (color[static_cast<size_t>(dep)] == 0) {
          color[static_cast<size_t>(dep)] = 1;
          stack.emplace_back(dep, 0);
        } else if (color[static_cast<size_t>(dep)] == 1) {
          // 环：把栈上从 dep 到 node 的节点都标成循环。
          for (size_t k = stack.size(); k > 0; --k) {
            const int member = stack[k - 1].first;
            cyclic[static_cast<size_t>(member)] = true;
            if (member == dep) break;
          }
        }
      } else {
        color[static_cast<size_t>(node)] = 2;
        order.push_back(node);
        stack.pop_back();
      }
    }
  }

  // 4. 按拓扑序求值。
  int computed = 0;
  for (int node : order) {
    Entry& e = entries[static_cast<size_t>(node)];
    Cell* cell = e.ws->Find(e.row, e.col);
    if (cell == nullptr) continue;
    if (cyclic[static_cast<size_t>(node)]) {
      cell->cached = Value::Err(CellError::Circular);
      cell->dirty = false;
      ++computed;
      continue;
    }
    const Formula& formula = e.ws->formulas()[static_cast<size_t>(e.formula_id)];
    NodePtr ast = AstOf(formula);
    if (ast == nullptr) continue;
    WorkbookEval ctx(wb, e.ws, today);
    cell->cached = Evaluate(*ast, ctx);
    cell->dirty = false;
    ++computed;
    if (cancel != nullptr && (computed & 0xFF) == 0) cancel->ThrowIfCancelled();
  }
  return computed;
}

}  // namespace

int Workbook::Recalculate(CancelToken* cancel) { return RecalcImpl(*this, nullptr, cancel); }

int Workbook::RecalculateSheet(Worksheet& sheet, CancelToken* cancel) {
  return RecalcImpl(*this, &sheet, cancel);
}

}  // namespace sheet
}  // namespace doccore
