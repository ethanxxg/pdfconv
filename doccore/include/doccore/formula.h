// DocCore - 公式解析与求值（docs/02 §3.4.2）。
//
// 解析：递归下降，优先级从低到高为 比较 < 连接 < 加减 < 乘除 < 幂 < 一元 < 百分号 < 区域。
// 求值：AST 解释执行；函数表编译期注册，未注册的名字求值为 #NAME?，P0 之外
// 已注册但未实现的返回 #NIMPL!。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/sheet.h"

namespace doccore {
namespace sheet {

enum class BinaryOp {
  Add, Sub, Mul, Div, Pow, Concat, Eq, Ne, Lt, Le, Gt, Ge,
};

enum class UnaryOp { Plus, Minus, Percent };

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
  enum class Kind {
    Number,
    Text,
    Bool,
    Error,
    Ref,       // A1 / Sheet1!A1
    RangeRef,  // A1:B3 / Sheet1!A1:B3
    Name,      // 定义名称或函数名的裸标识符
    Unary,
    Binary,
    Call,
  };

  Kind kind = Kind::Number;
  double number = 0.0;
  std::string text;  // Text 的字面量 / Name 的名字 / Call 的函数名
  bool boolean = false;
  CellError error = CellError::Value;

  // Ref / RangeRef
  std::string sheet;   // 空 = 当前表
  int row1 = 0, col1 = 0, row2 = 0, col2 = 0;
  bool row1_abs = false, col1_abs = false, row2_abs = false, col2_abs = false;

  UnaryOp unary = UnaryOp::Plus;
  BinaryOp binary = BinaryOp::Add;
  std::vector<NodePtr> args;  // Unary 用 1 个，Binary 用 2 个，Call 用 n 个
};

struct ParseResult {
  NodePtr root;
  bool ok = false;
  std::string error;  // 解析失败原因（英文，进日志/告警）
};

// text 不含前导 '='。失败时 ok=false，root 为空。
ParseResult ParseFormula(std::string_view text);

// 把 AST 还原成公式文本（保存用；不保证与原文逐字相同）。
std::string ToText(const Node& node);

// ---- 求值 ---------------------------------------------------------------

// 求值上下文由调用方（工作簿）提供：解析单元格与区域、提供易变函数的时间戳。
class EvalContext {
 public:
  virtual ~EvalContext() = default;
  // sheet 为空表示"当前表"。
  virtual Value CellValue(std::string_view sheet, int row, int col) = 0;
  // 遍历区域内每个非空单元格；区域可能很大，所以用回调而不是物化成数组。
  virtual void ForEachInRange(std::string_view sheet, const Range& range,
                              const std::function<void(int row, int col, const Value&)>& fn) = 0;
  // 定义名称指向的区域；找不到返回 false。
  virtual bool LookupName(std::string_view name, Range* range, std::string* sheet) {
    (void)name;
    (void)range;
    (void)sheet;
    return false;
  }
  // TODAY()/NOW() 用的序列数（同一批次内保持不变）。
  virtual double TodaySerial() const = 0;
};

// 求值一个表达式。参数按需传递：区域只有在函数需要时才展开。
Value Evaluate(const Node& node, EvalContext& ctx);

// 已注册函数个数（测试用）。
size_t FunctionCount();

}  // namespace sheet
}  // namespace doccore
