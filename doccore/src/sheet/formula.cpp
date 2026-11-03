// DocCore - 公式解析与求值。Design refs: docs/02 §3.4.2
//
// 分三段：词法 -> 递归下降语法 -> AST 解释执行。函数表用静态数组注册，
// 名字查表是二分（表按字典序排好），未注册 -> #NAME?，注册但未实现 -> #NIMPL!。
#include "doccore/formula.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace doccore {
namespace sheet {
namespace {

// ---- 词法 ---------------------------------------------------------------

struct Token {
  enum class Kind {
    End,
    Number,
    String,
    ErrorLit,
    Ident,
    Op,
    LParen,
    RParen,
    Comma,
    Colon,
    Bang,
  };
  Kind kind = Kind::End;
  std::string text;
  double number = 0.0;
};

bool IsIdentStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' || c == '\\';
}

bool IsIdentChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == '$' || c == '\\';
}

char Upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string ToUpper(std::string s) {
  for (char& c : s) c = Upper(c);
  return s;
}

class Lexer {
 public:
  explicit Lexer(std::string_view text) : text_(text) {}

  bool Next(Token* out) {
    SkipSpace();
    out->text.clear();
    if (pos_ >= text_.size()) {
      out->kind = Token::Kind::End;
      return true;
    }
    const char c = text_[pos_];
    if ((c >= '0' && c <= '9') || (c == '.' && pos_ + 1 < text_.size() &&
                                   text_[pos_ + 1] >= '0' && text_[pos_ + 1] <= '9')) {
      return ReadNumber(out);
    }
    if (c == '"') return ReadString(out);
    if (c == '\'') return ReadQuotedName(out);
    if (c == '#') return ReadError(out);
    if (IsIdentStart(c)) {
      size_t begin = pos_;
      while (pos_ < text_.size() && IsIdentChar(text_[pos_])) ++pos_;
      out->kind = Token::Kind::Ident;
      out->text.assign(text_.substr(begin, pos_ - begin));
      return true;
    }
    ++pos_;
    switch (c) {
      case '(': out->kind = Token::Kind::LParen; return true;
      case ')': out->kind = Token::Kind::RParen; return true;
      case ',': out->kind = Token::Kind::Comma; return true;
      case ';': out->kind = Token::Kind::Comma; return true;  // 部分区域用分号分隔参数
      case ':': out->kind = Token::Kind::Colon; return true;
      case '!': out->kind = Token::Kind::Bang; return true;
      case '<':
        out->kind = Token::Kind::Op;
        if (pos_ < text_.size() && text_[pos_] == '=') {
          ++pos_;
          out->text = "<=";
        } else if (pos_ < text_.size() && text_[pos_] == '>') {
          ++pos_;
          out->text = "<>";
        } else {
          out->text = "<";
        }
        return true;
      case '>':
        out->kind = Token::Kind::Op;
        if (pos_ < text_.size() && text_[pos_] == '=') {
          ++pos_;
          out->text = ">=";
        } else {
          out->text = ">";
        }
        return true;
      case '=':
      case '+':
      case '-':
      case '*':
      case '/':
      case '^':
      case '&':
      case '%':
        out->kind = Token::Kind::Op;
        out->text.assign(1, c);
        return true;
      default:
        out->kind = Token::Kind::Op;  // 未知字符当运算符，语法阶段会报错
        out->text.assign(1, c);
        return true;
    }
  }

 private:
  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool ReadNumber(Token* out) {
    const size_t begin = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      const size_t save = pos_;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      } else {
        pos_ = save;
      }
    }
    const std::string literal(text_.substr(begin, pos_ - begin));
    out->kind = Token::Kind::Number;
    out->text = literal;
    out->number = std::strtod(literal.c_str(), nullptr);
    return true;
  }

  bool ReadString(Token* out) {
    ++pos_;  // 开引号
    std::string value;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '"') {
        if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '"') {
          value.push_back('"');
          pos_ += 2;
          continue;
        }
        ++pos_;
        out->kind = Token::Kind::String;
        out->text = std::move(value);
        return true;
      }
      value.push_back(c);
      ++pos_;
    }
    out->kind = Token::Kind::String;
    out->text = std::move(value);
    return true;  // 未闭合的字符串按到结尾处理，语法阶段不会因此崩
  }

  bool ReadQuotedName(Token* out) {
    ++pos_;
    std::string value;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '\'') {
        if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '\'') {
          value.push_back('\'');
          pos_ += 2;
          continue;
        }
        ++pos_;
        break;
      }
      value.push_back(c);
      ++pos_;
    }
    out->kind = Token::Kind::Ident;
    out->text = std::move(value);
    out->number = -1.0;  // 标记：这是带引号的表名，不是单元格引用
    return true;
  }

  bool ReadError(Token* out) {
    const size_t begin = pos_;
    ++pos_;  // '#'
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '!' || c == '?') {
        ++pos_;
        break;
      }
      if (c == ' ' || c == ',' || c == ')') break;
      ++pos_;
    }
    out->kind = Token::Kind::ErrorLit;
    out->text.assign(text_.substr(begin, pos_ - begin));
    return true;
  }

  std::string_view text_;
  size_t pos_ = 0;
};

// ---- A1 地址 ------------------------------------------------------------

// "$AB$12" -> {row=11, col=27, rabs, cabs}。允许只有列名（整列引用）。
bool ParseRefText(const std::string& raw, int* row, int* col, bool* rabs, bool* cabs,
                  bool* column_only) {
  size_t i = 0;
  bool cabs_local = false;
  if (i < raw.size() && raw[i] == '$') {
    cabs_local = true;
    ++i;
  }
  int c = 0;
  size_t letters = 0;
  while (i < raw.size() && ((raw[i] >= 'A' && raw[i] <= 'Z') || (raw[i] >= 'a' && raw[i] <= 'z'))) {
    c = c * 26 + (Upper(raw[i]) - 'A' + 1);
    ++i;
    ++letters;
    if (letters > 3) return false;
  }
  if (letters == 0 || c <= 0) return false;
  bool rabs_local = false;
  if (i < raw.size() && raw[i] == '$') {
    rabs_local = true;
    ++i;
  }
  if (i >= raw.size()) {
    if (rabs_local) return false;
    *row = 0;
    *col = c - 1;
    *rabs = false;
    *cabs = cabs_local;
    *column_only = true;
    return true;
  }
  int r = 0;
  size_t digits = 0;
  while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') {
    r = r * 10 + (raw[i] - '0');
    ++i;
    ++digits;
    if (digits > 7) return false;
  }
  if (digits == 0 || i != raw.size() || r < 1) return false;
  *row = r - 1;
  *col = c - 1;
  *rabs = rabs_local;
  *cabs = cabs_local;
  *column_only = false;
  return true;
}

// 纯数字行号（整行引用 "3"）：只由数字组成。
bool ParseRowText(const std::string& raw, int* row) {
  if (raw.empty() || raw.size() > 7) return false;
  int r = 0;
  for (char c : raw) {
    if (c < '0' || c > '9') return false;
    r = r * 10 + (c - '0');
  }
  if (r < 1) return false;
  *row = r - 1;
  return true;
}

}  // namespace

namespace {

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

std::shared_ptr<Node> NewNode(Node::Kind kind) {
  auto n = std::make_shared<Node>();
  n->kind = kind;
  return n;
}

std::shared_ptr<Node> NumNode(double v) {
  auto n = NewNode(Node::Kind::Number);
  n->number = v;
  return n;
}

class Parser {
 public:
  explicit Parser(std::string_view text) {
    Lexer lex(text);
    Token t;
    while (true) {
      lex.Next(&t);
      tokens_.push_back(t);
      if (t.kind == Token::Kind::End) break;
      if (tokens_.size() > 20000) {  // 防御：畸形输入不占满内存
        ok_ = false;
        error_ = "formula too long";
        break;
      }
    }
  }

  NodePtr Parse() {
    if (!ok_) return nullptr;
    NodePtr n = ParseComparison();
    if (ok_ && cur().kind != Token::Kind::End) {
      Fail("unexpected token '" + cur().text + "'");
    }
    return ok_ ? n : nullptr;
  }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

 private:
  const Token& cur() const { return tokens_[index_]; }
  const Token& Peek(size_t ahead) const {
    const size_t at = index_ + ahead;
    return at < tokens_.size() ? tokens_[at] : tokens_.back();
  }
  void Advance() {
    if (index_ + 1 < tokens_.size()) ++index_;
  }
  bool IsOp(const char* s) const { return cur().kind == Token::Kind::Op && cur().text == s; }
  bool AcceptOp(const char* s) {
    if (!IsOp(s)) return false;
    Advance();
    return true;
  }
  bool Accept(Token::Kind kind) {
    if (cur().kind != kind) return false;
    Advance();
    return true;
  }
  void Fail(const std::string& message) {
    if (ok_) {
      ok_ = false;
      error_ = message;
    }
  }

  // 比较 < 连接 < 加减 < 乘除 < 幂 < 一元 < 百分号 < 初等量
  NodePtr ParseComparison() {
    NodePtr left = ParseConcat();
    while (ok()) {
      BinaryOp op;
      if (IsOp("=")) {
        op = BinaryOp::Eq;
      } else if (IsOp("<>")) {
        op = BinaryOp::Ne;
      } else if (IsOp("<")) {
        op = BinaryOp::Lt;
      } else if (IsOp("<=")) {
        op = BinaryOp::Le;
      } else if (IsOp(">")) {
        op = BinaryOp::Gt;
      } else if (IsOp(">=")) {
        op = BinaryOp::Ge;
      } else {
        break;
      }
      Advance();
      NodePtr right = ParseConcat();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Binary);
      n->binary = op;
      n->args = {left, right};
      left = n;
    }
    return left;
  }

  NodePtr ParseConcat() {
    NodePtr left = ParseAddSub();
    while (ok() && IsOp("&")) {
      Advance();
      NodePtr right = ParseAddSub();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Binary);
      n->binary = BinaryOp::Concat;
      n->args = {left, right};
      left = n;
    }
    return left;
  }

  NodePtr ParseAddSub() {
    NodePtr left = ParseMulDiv();
    while (ok() && (IsOp("+") || IsOp("-"))) {
      const BinaryOp op = IsOp("+") ? BinaryOp::Add : BinaryOp::Sub;
      Advance();
      NodePtr right = ParseMulDiv();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Binary);
      n->binary = op;
      n->args = {left, right};
      left = n;
    }
    return left;
  }

  NodePtr ParseMulDiv() {
    NodePtr left = ParsePow();
    while (ok() && (IsOp("*") || IsOp("/"))) {
      const BinaryOp op = IsOp("*") ? BinaryOp::Mul : BinaryOp::Div;
      Advance();
      NodePtr right = ParsePow();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Binary);
      n->binary = op;
      n->args = {left, right};
      left = n;
    }
    return left;
  }

  // Excel 的 '^' 左结合，且一元负号比它更紧（=-2^2 得 4）。
  NodePtr ParsePow() {
    NodePtr left = ParseUnary();
    while (ok() && IsOp("^")) {
      Advance();
      NodePtr right = ParseUnary();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Binary);
      n->binary = BinaryOp::Pow;
      n->args = {left, right};
      left = n;
    }
    return left;
  }

  NodePtr ParseUnary() {
    if (IsOp("-")) {
      Advance();
      NodePtr operand = ParseUnary();
      if (!ok()) return nullptr;
      auto n = NewNode(Node::Kind::Unary);
      n->unary = UnaryOp::Minus;
      n->args = {operand};
      return n;
    }
    if (IsOp("+")) {
      Advance();
      return ParseUnary();
    }
    return ParsePostfix();
  }

  NodePtr ParsePostfix() {
    NodePtr n = ParsePrimary();
    while (ok() && IsOp("%")) {
      Advance();
      auto pct = NewNode(Node::Kind::Unary);
      pct->unary = UnaryOp::Percent;
      pct->args = {n};
      n = pct;
    }
    return n;
  }

  NodePtr ParsePrimary() {
    if (!ok()) return nullptr;
    switch (cur().kind) {
      case Token::Kind::Number: {
        // "1:3" 整行引用：数字后面直接跟冒号。
        if (Peek(1).kind == Token::Kind::Colon && Peek(2).kind == Token::Kind::Number) {
          return ParseRowRange();
        }
        auto n = NumNode(cur().number);
        Advance();
        return n;
      }
      case Token::Kind::String: {
        // 可能只是 'Sheet 1'!A1 的表名部分。
        if (Peek(1).kind == Token::Kind::Bang) return ParseRefOrRange();
        auto n = NewNode(Node::Kind::Text);
        n->text = cur().text;
        Advance();
        return n;
      }
      case Token::Kind::ErrorLit: {
        auto n = NewNode(Node::Kind::Error);
        n->error = CellErrorFromText(cur().text);
        Advance();
        return n;
      }
      case Token::Kind::LParen: {
        Advance();
        NodePtr inner = ParseComparison();
        if (!ok()) return nullptr;
        if (!Accept(Token::Kind::RParen)) {
          Fail("missing ')'");
          return nullptr;
        }
        return inner;
      }
      case Token::Kind::Ident:
        return ParseRefOrRange();
      case Token::Kind::End:
        Fail("unexpected end of formula");
        return nullptr;
      default:
        Fail("unexpected token '" + cur().text + "'");
        return nullptr;
    }
  }

  NodePtr ParseRowRange() {
    int first = 0;
    int last = 0;
    if (!ParseRowText(cur().text, &first)) {
      Fail("bad row reference");
      return nullptr;
    }
    Advance();  // number
    Advance();  // ':'
    if (cur().kind != Token::Kind::Number || !ParseRowText(cur().text, &last)) {
      Fail("bad row reference");
      return nullptr;
    }
    Advance();
    auto n = NewNode(Node::Kind::RangeRef);
    n->row1 = std::min(first, last);
    n->row2 = std::max(first, last);
    n->col1 = 0;
    n->col2 = kMaxCols - 1;
    return n;
  }

  // 表名 + 引用/名称，可选 ":另一个引用"。
  NodePtr ParseRefOrRange() {
    std::string sheet;
    if ((cur().kind == Token::Kind::Ident || cur().kind == Token::Kind::String) &&
        Peek(1).kind == Token::Kind::Bang) {
      sheet = cur().text;
      index_ += 2;
    }
    if (cur().kind == Token::Kind::Ident && Peek(1).kind == Token::Kind::LParen) {
      if (!sheet.empty()) {
        Fail("unexpected sheet prefix before function call");
        return nullptr;
      }
      return ParseCall();
    }

    int mode = 0;  // 1 = 单元格，2 = 整列，3 = 整行
    int row = 0;
    int col = 0;
    bool rabs = false;
    bool cabs = false;
    std::string name;
    bool quoted_name = false;

    if (cur().kind == Token::Kind::Ident) {
      const bool quoted = cur().number < 0.0;
      const std::string ident = cur().text;
      Advance();
      bool column_only = false;
      if (!quoted && ParseRefText(ident, &row, &col, &rabs, &cabs, &column_only)) {
        // 裸 "Tax" 也可能是定义名称：只有后面跟冒号时才当整列引用。
        if (!column_only || cur().kind == Token::Kind::Colon) {
          mode = column_only ? 2 : 1;
        } else {
          name = ident;
        }
      } else {
        name = ident;
        quoted_name = quoted;
      }
    } else if (cur().kind == Token::Kind::Number) {
      // 只有 "1:3" 这种走到这里（左侧已在 ParsePrimary 处理），
      // 剩下的是 "A1:3" 之类的畸形写法。
      Fail("bad range reference");
      return nullptr;
    } else {
      Fail("expected cell reference");
      return nullptr;
    }

    if (mode == 0) {
      if (!quoted_name) {
        const std::string upper = ToUpper(name);
        if (upper == "TRUE" || upper == "FALSE") {
          auto literal = NewNode(Node::Kind::Bool);
          literal->boolean = upper == "TRUE";
          return literal;
        }
      }
      if (cur().kind == Token::Kind::Colon) {
        Fail("'" + name + "' is not a valid range endpoint");
        return nullptr;
      }
      auto n = NewNode(Node::Kind::Name);
      n->text = name;
      n->sheet = sheet;
      return n;
    }

    if (cur().kind != Token::Kind::Colon) {
      if (mode == 2) {
        auto n = NewNode(Node::Kind::RangeRef);
        n->sheet = sheet;
        n->row1 = 0;
        n->row2 = kMaxRows - 1;
        n->col1 = n->col2 = col;
        n->col1_abs = n->col2_abs = cabs;
        return n;
      }
      auto n = NewNode(Node::Kind::Ref);
      n->sheet = sheet;
      n->row1 = row;
      n->col1 = col;
      n->row1_abs = rabs;
      n->col1_abs = cabs;
      return n;
    }

    Advance();  // ':'
    if (cur().kind != Token::Kind::Ident) {
      Fail("expected range end");
      return nullptr;
    }
    const bool quoted = cur().number < 0.0;
    const std::string ident = cur().text;
    Advance();
    int row2 = 0;
    int col2 = 0;
    bool rabs2 = false;
    bool cabs2 = false;
    bool column_only2 = false;
    if (quoted || !ParseRefText(ident, &row2, &col2, &rabs2, &cabs2, &column_only2)) {
      Fail("'" + ident + "' is not a valid range endpoint");
      return nullptr;
    }
    const int mode2 = column_only2 ? 2 : 1;
    if (mode != mode2) {
      Fail("mixed range types are not supported");
      return nullptr;
    }
    auto n = NewNode(Node::Kind::RangeRef);
    n->sheet = sheet;
    if (mode == 2) {
      n->row1 = 0;
      n->row2 = kMaxRows - 1;
      n->col1 = std::min(col, col2);
      n->col2 = std::max(col, col2);
      n->col1_abs = cabs;
      n->col2_abs = cabs2;
    } else {
      n->row1 = std::min(row, row2);
      n->row2 = std::max(row, row2);
      n->col1 = std::min(col, col2);
      n->col2 = std::max(col, col2);
      n->row1_abs = rabs;
      n->row2_abs = rabs2;
      n->col1_abs = cabs;
      n->col2_abs = cabs2;
    }
    return n;
  }

  NodePtr ParseCall() {
    std::string name = ToUpper(cur().text);
    if (name.compare(0, 6, "_XLFN.") == 0) name.erase(0, 6);
    Advance();
    Advance();  // '('
    auto n = NewNode(Node::Kind::Call);
    n->text = name;
    if (Accept(Token::Kind::RParen)) return n;
    while (ok()) {
      if (cur().kind == Token::Kind::Comma || cur().kind == Token::Kind::RParen) {
        n->args.push_back(NumNode(0.0));  // 空参数按 0 处理（Excel 行为）
      } else {
        NodePtr arg = ParseComparison();
        if (!ok()) return nullptr;
        n->args.push_back(arg);
      }
      if (Accept(Token::Kind::Comma)) continue;
      if (Accept(Token::Kind::RParen)) break;
      Fail("expected ',' or ')' in argument list");
      return nullptr;
    }
    return n;
  }

  std::vector<Token> tokens_;
  size_t index_ = 0;
  bool ok_ = true;
  std::string error_;
};

}  // namespace

ParseResult ParseFormula(std::string_view text) {
  ParseResult result;
  std::string_view body = text;
  if (!body.empty() && body[0] == '=') body.remove_prefix(1);
  Parser parser(body);
  result.root = parser.Parse();
  result.ok = parser.ok() && result.root != nullptr;
  if (!result.ok) {
    result.error = parser.ok() ? std::string("empty formula") : parser.error();
    result.root.reset();
  }
  return result;
}

// ---- 反打印 -------------------------------------------------------------

namespace {

void PrintNumber(double v, std::string* out) {
  if (v == static_cast<double>(static_cast<long long>(v)) && std::fabs(v) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    *out += buf;
    return;
  }
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  *out += buf;
}

std::string QuoteSheet(const std::string& sheet) {
  if (sheet.empty()) return std::string();
  bool needs = false;
  for (char c : sheet) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) needs = true;
  }
  if (!needs) return sheet + "!";
  std::string out = "'";
  for (char c : sheet) {
    if (c == '\'') out += "''";
    else out.push_back(c);
  }
  out += "'!";
  return out;
}

std::string AbsDollar(bool absolute) { return absolute ? "$" : std::string(); }

void PrintInto(const Node& node, std::string* out);

int PrecOf(const Node& node) {
  if (node.kind == Node::Kind::Unary) {
    return node.unary == UnaryOp::Percent ? 7 : 6;
  }
  if (node.kind != Node::Kind::Binary) return 8;
  switch (node.binary) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      return 1;
    case BinaryOp::Concat:
      return 2;
    case BinaryOp::Add:
    case BinaryOp::Sub:
      return 3;
    case BinaryOp::Mul:
    case BinaryOp::Div:
      return 4;
    case BinaryOp::Pow:
      return 5;
  }
  return 0;
}

const char* OpText(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Pow: return "^";
    case BinaryOp::Concat: return "&";
    case BinaryOp::Eq: return "=";
    case BinaryOp::Ne: return "<>";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
  }
  return "?";
}

void PrintInto(const Node& node, std::string* out) {
  switch (node.kind) {
    case Node::Kind::Number:
      PrintNumber(node.number, out);
      return;
    case Node::Kind::Text:
      *out += '"';
      for (char c : node.text) {
        if (c == '"') *out += '"';
        *out += c;
      }
      *out += '"';
      return;
    case Node::Kind::Bool:
      *out += node.boolean ? "TRUE" : "FALSE";
      return;
    case Node::Kind::Error:
      *out += CellErrorText(node.error);
      return;
    case Node::Kind::Ref:
      *out += QuoteSheet(node.sheet);
      *out += AbsDollar(node.col1_abs);
      *out += ColumnName(node.col1);
      *out += AbsDollar(node.row1_abs);
      *out += std::to_string(node.row1 + 1);
      return;
    case Node::Kind::RangeRef:
      *out += QuoteSheet(node.sheet);
      *out += AbsDollar(node.col1_abs);
      *out += ColumnName(node.col1);
      if (node.row2 != kMaxRows - 1 || node.row1 != 0) {
        *out += AbsDollar(node.row1_abs);
        *out += std::to_string(node.row1 + 1);
      }
      *out += ':';
      *out += AbsDollar(node.col2_abs);
      *out += ColumnName(node.col2);
      if (node.row2 != kMaxRows - 1 || node.row1 != 0) {
        *out += AbsDollar(node.row2_abs);
        *out += std::to_string(node.row2 + 1);
      }
      return;
    case Node::Kind::Name:
      *out += QuoteSheet(node.sheet);
      *out += node.text;
      return;
    case Node::Kind::Unary:
      if (node.unary == UnaryOp::Percent) {
        if (!node.args.empty()) PrintInto(*node.args[0], out);
        *out += '%';
        return;
      }
      *out += node.unary == UnaryOp::Minus ? "-" : "+";
      if (!node.args.empty()) PrintInto(*node.args[0], out);
      return;
    case Node::Kind::Binary: {
      if (node.args.size() < 2) return;
      // 只在必要时加括号：保留原文可读性，同时保证再解析得到同一棵树。
      const int prec = PrecOf(node);
      const bool strict_right = node.binary == BinaryOp::Sub || node.binary == BinaryOp::Div ||
                                node.binary == BinaryOp::Pow;
      const auto emit = [&](const Node& child, bool right_side) {
        const int child_prec = PrecOf(child);
        const bool parens =
            child_prec < prec || (right_side && strict_right && child_prec == prec);
        if (parens) *out += '(';
        PrintInto(child, out);
        if (parens) *out += ')';
      };
      emit(*node.args[0], false);
      *out += OpText(node.binary);
      emit(*node.args[1], true);
      return;
    }
    case Node::Kind::Call: {
      *out += node.text;
      *out += '(';
      for (size_t i = 0; i < node.args.size(); ++i) {
        if (i != 0) *out += ',';
        PrintInto(*node.args[i], out);
      }
      *out += ')';
      return;
    }
  }
}

}  // namespace

std::string ToText(const Node& node) {
  std::string out;
  PrintInto(node, &out);
  return out;
}

// ---- 求值辅助 -----------------------------------------------------------

namespace {

Value NumV(double v) { return Value::Num(v); }
Value ErrV(CellError e) { return Value::Err(e); }

std::string TrimSpace(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

// 显示文本（General 格式）。
std::string Display(const Value& v) { return FormatValue(v); }

// 算术取值：空 -> 0，布尔 -> 1/0，文本能整体解析成数字就用，否则 #VALUE!。
bool AsNumber(const Value& v, double* out, Value* err) {
  switch (v.kind) {
    case Value::Kind::Empty:
      *out = 0.0;
      return true;
    case Value::Kind::Number:
      *out = v.number;
      return true;
    case Value::Kind::Bool:
      *out = v.boolean ? 1.0 : 0.0;
      return true;
    case Value::Kind::Error:
      *err = v;
      return false;
    case Value::Kind::Text: {
      const std::string s = TrimSpace(v.text);
      if (s.empty()) {
        *out = 0.0;
        return true;
      }
      double scale = 1.0;
      std::string body = s;
      if (body.size() > 1 && body.back() == '%') {
        scale = 0.01;
        body.pop_back();
      }
      char* end = nullptr;
      const double parsed = std::strtod(body.c_str(), &end);
      if (end == nullptr || *end != '\0') {
        *err = ErrV(CellError::Value);
        return false;
      }
      *out = parsed * scale;
      return true;
    }
  }
  *err = ErrV(CellError::Value);
  return false;
}

// 逻辑取值：空 -> false，数字 0 -> false，文本 "TRUE"/"FALSE" 可识别。
bool AsBool(const Value& v, bool* out) {
  switch (v.kind) {
    case Value::Kind::Empty:
      *out = false;
      return true;
    case Value::Kind::Bool:
      *out = v.boolean;
      return true;
    case Value::Kind::Number:
      *out = v.number != 0.0;
      return true;
    case Value::Kind::Text: {
      const std::string s = ToUpper(TrimSpace(v.text));
      if (s == "TRUE") {
        *out = true;
        return true;
      }
      if (s == "FALSE") {
        *out = false;
        return true;
      }
      *out = false;
      return true;
    }
    case Value::Kind::Error:
      return false;
  }
  return false;
}

// Excel 的比较次序：数字 < 文本 < 逻辑值。
int RankOf(const Value& v) {
  switch (v.kind) {
    case Value::Kind::Empty:
      return 0;
    case Value::Kind::Number:
      return 0;
    case Value::Kind::Text:
      return 1;
    case Value::Kind::Bool:
      return 2;
    case Value::Kind::Error:
      return 3;
  }
  return 0;
}

// 比较两个值；err 非空表示有一侧是错误值。
int CompareValues(const Value& a, const Value& b, Value* err) {
  const int ra = RankOf(a);
  const int rb = RankOf(b);
  if (ra != rb) return ra < rb ? -1 : 1;
  switch (a.kind) {
    case Value::Kind::Empty:
      return 0;
    case Value::Kind::Number: {
      if (a.number < b.number) return -1;
      if (a.number > b.number) return 1;
      return 0;
    }
    case Value::Kind::Text: {
      // Excel 的文本比较不区分大小写。
      const std::string x = ToUpper(a.text);
      const std::string y = ToUpper(b.text);
      if (x < y) return -1;
      if (x > y) return 1;
      return 0;
    }
    case Value::Kind::Bool: {
      const int x = a.boolean ? 1 : 0;
      const int y = b.boolean ? 1 : 0;
      return x == y ? 0 : (x < y ? -1 : 1);
    }
    case Value::Kind::Error:
      *err = a;
      return 0;
  }
  return 0;
}

Value Arith(BinaryOp op, const Value& a, const Value& b) {
  if (a.is_error()) return a;
  if (b.is_error()) return b;
  if (op == BinaryOp::Concat) return Value::Str(Display(a) + Display(b));
  if (op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Le ||
      op == BinaryOp::Gt || op == BinaryOp::Ge) {
    Value err;
    // 空单元格与数字比较时按 0，与文本比较时按空串。
    Value lhs = a;
    Value rhs = b;
    if (lhs.is_empty() && !rhs.is_empty() && rhs.kind != Value::Kind::Number) lhs = Value::Str("");
    if (rhs.is_empty() && !lhs.is_empty() && lhs.kind != Value::Kind::Number) rhs = Value::Str("");
    const int cmp = CompareValues(lhs, rhs, &err);
    if (err.is_error()) return err;
    switch (op) {
      case BinaryOp::Eq: return Value::Boolean(cmp == 0);
      case BinaryOp::Ne: return Value::Boolean(cmp != 0);
      case BinaryOp::Lt: return Value::Boolean(cmp < 0);
      case BinaryOp::Le: return Value::Boolean(cmp <= 0);
      case BinaryOp::Gt: return Value::Boolean(cmp > 0);
      default: return Value::Boolean(cmp >= 0);
    }
  }
  double x = 0.0;
  double y = 0.0;
  Value err;
  if (!AsNumber(a, &x, &err)) return err;
  if (!AsNumber(b, &y, &err)) return err;
  switch (op) {
    case BinaryOp::Add: return NumV(x + y);
    case BinaryOp::Sub: return NumV(x - y);
    case BinaryOp::Mul: return NumV(x * y);
    case BinaryOp::Div:
      if (y == 0.0) return ErrV(CellError::DivZero);
      return NumV(x / y);
    case BinaryOp::Pow: {
      const double r = std::pow(x, y);
      if (std::isnan(r) || std::isinf(r)) return ErrV(CellError::Num);
      return NumV(r);
    }
    default:
      return ErrV(CellError::Value);
  }
}

Value EvalNode(const Node& node, EvalContext& ctx, int depth);

// 区域展开：把参数里的区域按行优先展开成标量序列。
// first_error 一旦被写入，调用方应当立即返回它（Excel 的错误传播）。
void ForEachScalar(const std::vector<NodePtr>& args, EvalContext& ctx, int depth,
                   const std::function<void(const Value&)>& fn, Value* first_error) {
  for (const NodePtr& arg : args) {
    if (arg == nullptr) continue;
    if (arg->kind == Node::Kind::RangeRef) {
      Range r;
      r.row1 = arg->row1;
      r.col1 = arg->col1;
      r.row2 = arg->row2;
      r.col2 = arg->col2;
      ctx.ForEachInRange(arg->sheet, r, [&](int, int, const Value& v) {
        if (first_error->is_error()) return;
        if (v.is_error()) {
          *first_error = v;
          return;
        }
        fn(v);
      });
      continue;
    }
    if (arg->kind == Node::Kind::Name) {
      Range r;
      std::string sheet;
      if (ctx.LookupName(arg->text, &r, &sheet)) {
        ctx.ForEachInRange(sheet, r, [&](int, int, const Value& v) {
          if (first_error->is_error()) return;
          if (v.is_error()) {
            *first_error = v;
            return;
          }
          fn(v);
        });
        continue;
      }
      *first_error = ErrV(CellError::Name);
      return;
    }
    const Value v = EvalNode(*arg, ctx, depth + 1);
    if (v.is_error()) {
      *first_error = v;
      return;
    }
    fn(v);
  }
}

// 参数 -> 区域（INDEX/MATCH/SUMIF 用）。
bool ArgAsRange(const Node& node, EvalContext& ctx, Range* out, std::string* sheet) {
  if (node.kind == Node::Kind::RangeRef) {
    out->row1 = node.row1;
    out->col1 = node.col1;
    out->row2 = node.row2;
    out->col2 = node.col2;
    *sheet = node.sheet;
    return true;
  }
  if (node.kind == Node::Kind::Name) {
    if (ctx.LookupName(node.text, out, sheet)) return true;
    return false;
  }
  if (node.kind == Node::Kind::Ref) {
    out->row1 = out->row2 = node.row1;
    out->col1 = out->col2 = node.col1;
    *sheet = node.sheet;
    return true;
  }
  return false;
}

bool ArgNumber(const std::vector<NodePtr>& args, size_t index, EvalContext& ctx, int depth,
               double* out, Value* err) {
  if (index >= args.size()) {
    *out = 0.0;
    return true;
  }
  const Value v = EvalNode(*args[index], ctx, depth + 1);
  if (v.is_error()) {
    *err = v;
    return false;
  }
  return AsNumber(v, out, err);
}

std::string ArgText(const std::vector<NodePtr>& args, size_t index, EvalContext& ctx, int depth) {
  if (index >= args.size()) return std::string();
  return Display(EvalNode(*args[index], ctx, depth + 1));
}

}  // namespace

// ---- 条件匹配（COUNTIF / SUMIF） ----------------------------------------

namespace {

// 通配符匹配：'*' 任意串，'?' 单字符，'~' 转义。两边都已转大写。
bool WildMatch(const std::string& pattern, const std::string& text) {
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string::npos;
  size_t resume = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
      continue;
    }
    if (p < pattern.size() && pattern[p] == '~' && p + 1 < pattern.size() &&
        pattern[p + 1] == text[t]) {
      p += 2;
      ++t;
      continue;
    }
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      resume = t;
      continue;
    }
    if (star != std::string::npos) {
      p = star + 1;
      t = ++resume;
      continue;
    }
    return false;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

struct Criteria {
  enum class Op { Eq, Ne, Lt, Le, Gt, Ge };
  Op op = Op::Eq;
  Value operand;
  bool wildcard = false;
  std::string pattern;
};

Criteria ParseCriteria(const Value& v) {
  Criteria c;
  c.operand = v;
  if (!v.is_text()) return c;
  std::string s = v.text;
  size_t skip = 0;
  if (s.compare(0, 2, "<>") == 0) {
    c.op = Criteria::Op::Ne;
    skip = 2;
  } else if (s.compare(0, 2, "<=") == 0) {
    c.op = Criteria::Op::Le;
    skip = 2;
  } else if (s.compare(0, 2, ">=") == 0) {
    c.op = Criteria::Op::Ge;
    skip = 2;
  } else if (!s.empty() && s[0] == '<') {
    c.op = Criteria::Op::Lt;
    skip = 1;
  } else if (!s.empty() && s[0] == '>') {
    c.op = Criteria::Op::Gt;
    skip = 1;
  } else if (!s.empty() && s[0] == '=') {
    c.op = Criteria::Op::Eq;
    skip = 1;
  }
  const std::string rest = s.substr(skip);
  // 剩余部分能解析成数字就当数字条件，否则当文本条件。
  if (!rest.empty()) {
    char* end = nullptr;
    const double d = std::strtod(rest.c_str(), &end);
    if (end != nullptr && *end == '\0') {
      c.operand = NumV(d);
      return c;
    }
  }
  if (skip == 0 && rest.empty()) {
    c.operand = Value::Str(std::string());
    return c;
  }
  c.operand = Value::Str(rest);
  c.wildcard = rest.find('*') != std::string::npos || rest.find('?') != std::string::npos;
  if (c.wildcard) c.pattern = ToUpper(rest);
  return c;
}

bool MatchCriteria(const Value& cell, const Criteria& c) {
  if (c.operand.is_number()) {
    double d = 0.0;
    Value err;
    const bool numeric = !cell.is_empty() && cell.kind != Value::Kind::Bool &&
                         AsNumber(cell, &d, &err);
    if (!numeric) {
      // 数字条件遇到非数字：只有 "<>" 成立。
      return c.op == Criteria::Op::Ne;
    }
    const double target = c.operand.number;
    switch (c.op) {
      case Criteria::Op::Eq: return d == target;
      case Criteria::Op::Ne: return d != target;
      case Criteria::Op::Lt: return d < target;
      case Criteria::Op::Le: return d <= target;
      case Criteria::Op::Gt: return d > target;
      case Criteria::Op::Ge: return d >= target;
    }
    return false;
  }
  const std::string needle = c.operand.text;
  const std::string hay = cell.is_empty() ? std::string() : Display(cell);
  if (needle.empty()) {
    const bool empty = cell.is_empty() || hay.empty();
    return c.op == Criteria::Op::Eq ? empty : !empty;
  }
  if (c.wildcard) {
    const bool hit = WildMatch(c.pattern, ToUpper(hay));
    return c.op == Criteria::Op::Eq ? hit : !hit;
  }
  const int cmp = CompareValues(Value::Str(ToUpper(hay)), Value::Str(ToUpper(needle)), nullptr);
  switch (c.op) {
    case Criteria::Op::Eq: return cmp == 0;
    case Criteria::Op::Ne: return cmp != 0;
    case Criteria::Op::Lt: return cmp < 0;
    case Criteria::Op::Le: return cmp <= 0;
    case Criteria::Op::Gt: return cmp > 0;
    case Criteria::Op::Ge: return cmp >= 0;
  }
  return false;
}

// ---- 聚合 ---------------------------------------------------------------

Value FnSum(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double sum = 0.0;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  double d = 0.0;
                  Value e;
                  if (AsNumber(v, &d, &e)) sum += d;
                },
                &err);
  if (err.is_error()) return err;
  return NumV(sum);
}

Value FnAverage(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double sum = 0.0;
  int count = 0;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  double d = 0.0;
                  Value e;
                  if (v.is_number() && AsNumber(v, &d, &e)) {
                    sum += d;
                    ++count;
                  }
                },
                &err);
  if (err.is_error()) return err;
  if (count == 0) return ErrV(CellError::DivZero);
  return NumV(sum / count);
}

Value FnCount(const std::vector<NodePtr>& args, EvalContext& ctx) {
  int count = 0;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  if (v.is_number()) ++count;
                },
                &err);
  if (err.is_error()) return err;
  return NumV(count);
}

Value FnCountA(const std::vector<NodePtr>& args, EvalContext& ctx) {
  int count = 0;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  if (!v.is_empty()) ++count;
                },
                &err);
  if (err.is_error()) return err;
  return NumV(count);
}

Value FnMinMax(const std::vector<NodePtr>& args, EvalContext& ctx, bool want_max) {
  double best = 0.0;
  bool any = false;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  double d = 0.0;
                  Value e;
                  if (!v.is_number() || !AsNumber(v, &d, &e)) return;
                  if (!any) {
                    best = d;
                    any = true;
                  } else {
                    best = want_max ? std::max(best, d) : std::min(best, d);
                  }
                },
                &err);
  if (err.is_error()) return err;
  return NumV(any ? best : 0.0);
}

Value FnMin(const std::vector<NodePtr>& args, EvalContext& ctx) { return FnMinMax(args, ctx, false); }
Value FnMax(const std::vector<NodePtr>& args, EvalContext& ctx) { return FnMinMax(args, ctx, true); }

Value FnCountBlank(const std::vector<NodePtr>& args, EvalContext& ctx) {
  int count = 0;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  if (v.is_empty()) ++count;
                },
                &err);
  if (err.is_error()) return err;
  return NumV(count);
}

// ---- 逻辑 ---------------------------------------------------------------

Value FnIf(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value cond = EvalNode(*args[0], ctx, 1);
  if (cond.is_error()) return cond;
  bool truth = false;
  if (!AsBool(cond, &truth)) return ErrV(CellError::Value);
  if (truth) return args.size() > 1 ? EvalNode(*args[1], ctx, 1) : Value::Boolean(true);
  return args.size() > 2 ? EvalNode(*args[2], ctx, 1) : Value::Boolean(false);
}

Value FnIfError(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  if (!v.is_error()) return v;
  return args.size() > 1 ? EvalNode(*args[1], ctx, 1) : Value::Empty();
}

Value FnIfNa(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  if (!(v.is_error() && v.error == CellError::NotAvailable)) return v;
  return args.size() > 1 ? EvalNode(*args[1], ctx, 1) : Value::Empty();
}

Value FnAnd(const std::vector<NodePtr>& args, EvalContext& ctx) {
  bool all = true;
  bool any = false;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  bool b = false;
                  if (AsBool(v, &b)) {
                    any = true;
                    if (!b) all = false;
                  }
                },
                &err);
  if (err.is_error()) return err;
  if (!any) return ErrV(CellError::Value);
  return Value::Boolean(all);
}

Value FnOr(const std::vector<NodePtr>& args, EvalContext& ctx) {
  bool any_true = false;
  bool any = false;
  Value err;
  ForEachScalar(args, ctx, 0,
                [&](const Value& v) {
                  bool b = false;
                  if (AsBool(v, &b)) {
                    any = true;
                    if (b) any_true = true;
                  }
                },
                &err);
  if (err.is_error()) return err;
  if (!any) return ErrV(CellError::Value);
  return Value::Boolean(any_true);
}

Value FnNot(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  if (v.is_error()) return v;
  bool b = false;
  if (!AsBool(v, &b)) return ErrV(CellError::Value);
  return Value::Boolean(!b);
}

// ---- 数学 ---------------------------------------------------------------

double RoundDigits(double x, int digits, int mode) {
  // mode: -1 向下取位（toward zero），0 四舍五入（远离零），1 向上取位（away from zero）
  const double f = std::pow(10.0, static_cast<double>(digits));
  const double scaled = x * f;
  double r = 0.0;
  if (mode == 0) {
    r = scaled >= 0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
  } else if (mode > 0) {
    r = scaled >= 0 ? std::ceil(scaled) : std::floor(scaled);
  } else {
    r = scaled >= 0 ? std::floor(scaled) : std::ceil(scaled);
  }
  return r / f;
}

Value FnRound(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  double digits = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &digits, &err)) return err;
  return NumV(RoundDigits(x, static_cast<int>(digits), 0));
}

Value FnRoundUp(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  double digits = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &digits, &err)) return err;
  return NumV(RoundDigits(x, static_cast<int>(digits), 1));
}

Value FnRoundDown(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  double digits = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &digits, &err)) return err;
  return NumV(RoundDigits(x, static_cast<int>(digits), -1));
}

Value FnInt(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  return NumV(std::floor(x));
}

Value FnTrunc(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  double digits = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &digits, &err)) return err;
  return NumV(RoundDigits(x, static_cast<int>(digits), -1));
}

Value FnAbs(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  return NumV(std::fabs(x));
}

Value FnSign(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  return NumV(x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0));
}

Value FnMod(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double a = 0.0;
  double b = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &a, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &b, &err)) return err;
  if (b == 0.0) return ErrV(CellError::DivZero);
  return NumV(a - b * std::floor(a / b));
}

Value FnSqrt(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double x = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &x, &err)) return err;
  if (x < 0.0) return ErrV(CellError::Num);
  return NumV(std::sqrt(x));
}

Value FnPower(const std::vector<NodePtr>& args, EvalContext& ctx) {
  double a = 0.0;
  double b = 0.0;
  Value err;
  if (!ArgNumber(args, 0, ctx, 1, &a, &err)) return err;
  if (!ArgNumber(args, 1, ctx, 1, &b, &err)) return err;
  return Arith(BinaryOp::Pow, NumV(a), NumV(b));
}

Value FnPi(const std::vector<NodePtr>&, EvalContext&) { return NumV(3.14159265358979323846); }

// ---- 文本 ---------------------------------------------------------------

Value FnLen(const std::vector<NodePtr>& args, EvalContext& ctx) {
  return NumV(static_cast<double>(ArgText(args, 0, ctx, 1).size()));
}

Value FnLeft(const std::vector<NodePtr>& args, EvalContext& ctx) {
  const std::string s = ArgText(args, 0, ctx, 1);
  double n = 1.0;
  Value err;
  if (!ArgNumber(args, 1, ctx, 1, &n, &err)) return err;
  if (n < 0) return ErrV(CellError::Value);
  const size_t count = std::min(s.size(), static_cast<size_t>(n));
  return Value::Str(s.substr(0, count));
}

Value FnRight(const std::vector<NodePtr>& args, EvalContext& ctx) {
  const std::string s = ArgText(args, 0, ctx, 1);
  double n = 1.0;
  Value err;
  if (!ArgNumber(args, 1, ctx, 1, &n, &err)) return err;
  if (n < 0) return ErrV(CellError::Value);
  const size_t count = std::min(s.size(), static_cast<size_t>(n));
  return Value::Str(s.substr(s.size() - count));
}

Value FnMid(const std::vector<NodePtr>& args, EvalContext& ctx) {
  const std::string s = ArgText(args, 0, ctx, 1);
  double start = 1.0;
  double n = 1.0;
  Value err;
  if (!ArgNumber(args, 1, ctx, 1, &start, &err)) return err;
  if (!ArgNumber(args, 2, ctx, 1, &n, &err)) return err;
  if (start < 1.0 || n < 0.0) return ErrV(CellError::Value);
  const size_t begin = static_cast<size_t>(start) - 1;
  if (begin >= s.size()) return Value::Str(std::string());
  const size_t count = std::min(s.size() - begin, static_cast<size_t>(n));
  return Value::Str(s.substr(begin, count));
}

Value FnTrim(const std::vector<NodePtr>& args, EvalContext& ctx) {
  const std::string s = ArgText(args, 0, ctx, 1);
  std::string out;
  bool pending_space = false;
  for (char c : s) {
    if (c == ' ') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
  }
  return Value::Str(out);
}

Value FnUpper(const std::vector<NodePtr>& args, EvalContext& ctx) {
  std::string s = ArgText(args, 0, ctx, 1);
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return Value::Str(s);
}

Value FnLower(const std::vector<NodePtr>& args, EvalContext& ctx) {
  std::string s = ArgText(args, 0, ctx, 1);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return Value::Str(s);
}

Value FnConcatenate(const std::vector<NodePtr>& args, EvalContext& ctx) {
  std::string out;
  Value err;
  ForEachScalar(args, ctx, 0, [&](const Value& v) { out += Display(v); }, &err);
  if (err.is_error()) return err;
  return Value::Str(out);
}

Value FnExact(const std::vector<NodePtr>& args, EvalContext& ctx) {
  return Value::Boolean(ArgText(args, 0, ctx, 1) == ArgText(args, 1, ctx, 1));
}

Value FnValue(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  if (v.is_error()) return v;
  if (v.is_number()) return v;
  double d = 0.0;
  Value err;
  if (!AsNumber(v, &d, &err)) return err;
  return NumV(d);
}

Value FnText(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  if (v.is_error()) return v;
  const std::string pattern = ArgText(args, 1, ctx, 1);
  if (pattern.empty()) return Value::Str(Display(v));
  return Value::Str(FormatValue(v, ParseNumberFormat(pattern)));
}

// ---- 信息 ---------------------------------------------------------------

Value FnIsBlank(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  return Value::Boolean(EvalNode(*args[0], ctx, 1).is_empty());
}

Value FnIsNumber(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  return Value::Boolean(EvalNode(*args[0], ctx, 1).is_number());
}

Value FnIsText(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  return Value::Boolean(EvalNode(*args[0], ctx, 1).is_text());
}

Value FnIsError(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  return Value::Boolean(EvalNode(*args[0], ctx, 1).is_error());
}

Value FnIsNa(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.empty()) return ErrV(CellError::Value);
  const Value v = EvalNode(*args[0], ctx, 1);
  return Value::Boolean(v.is_error() && v.error == CellError::NotAvailable);
}

// ---- 日期 ---------------------------------------------------------------

Value FnToday(const std::vector<NodePtr>&, EvalContext& ctx) {
  return NumV(std::floor(ctx.TodaySerial()));
}

Value FnNow(const std::vector<NodePtr>&, EvalContext& ctx) { return NumV(ctx.TodaySerial()); }

// ---- 条件聚合 -----------------------------------------------------------

Value FnCountIf(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 2) return ErrV(CellError::Value);
  Range range{};
  std::string sheet;
  if (!ArgAsRange(*args[0], ctx, &range, &sheet)) return ErrV(CellError::Value);
  const Value needle = EvalNode(*args[1], ctx, 1);
  if (needle.is_error()) return needle;
  const Criteria criteria = ParseCriteria(needle);
  int count = 0;
  ctx.ForEachInRange(sheet, range, [&](int, int, const Value& v) {
    if (MatchCriteria(v, criteria)) ++count;
  });
  return NumV(count);
}

Value FnSumIf(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 2) return ErrV(CellError::Value);
  Range range{};
  std::string sheet;
  if (!ArgAsRange(*args[0], ctx, &range, &sheet)) return ErrV(CellError::Value);
  const Value needle = EvalNode(*args[1], ctx, 1);
  if (needle.is_error()) return needle;
  const Criteria criteria = ParseCriteria(needle);
  Range sum_range = range;
  std::string sum_sheet = sheet;
  if (args.size() > 2) {
    if (!ArgAsRange(*args[2], ctx, &sum_range, &sum_sheet)) return ErrV(CellError::Value);
  }
  double sum = 0.0;
  bool failed = false;
  Value failure;
  ctx.ForEachInRange(sheet, range, [&](int row, int col, const Value& v) {
    if (failed || !MatchCriteria(v, criteria)) return;
    const int target_row = sum_range.row1 + (row - range.row1);
    const int target_col = sum_range.col1 + (col - range.col1);
    if (target_row > sum_range.row2 || target_col > sum_range.col2) return;
    const Value target = ctx.CellValue(sum_sheet, target_row, target_col);
    if (target.is_error()) {
      failed = true;
      failure = target;
      return;
    }
    double d = 0.0;
    Value e;
    if (AsNumber(target, &d, &e)) sum += d;
  });
  if (failed) return failure;
  return NumV(sum);
}

Value FnAverageIf(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 2) return ErrV(CellError::Value);
  Range range{};
  std::string sheet;
  if (!ArgAsRange(*args[0], ctx, &range, &sheet)) return ErrV(CellError::Value);
  const Value needle = EvalNode(*args[1], ctx, 1);
  if (needle.is_error()) return needle;
  const Criteria criteria = ParseCriteria(needle);
  Range value_range = range;
  std::string value_sheet = sheet;
  if (args.size() > 2) {
    if (!ArgAsRange(*args[2], ctx, &value_range, &value_sheet)) return ErrV(CellError::Value);
  }
  double sum = 0.0;
  int count = 0;
  ctx.ForEachInRange(sheet, range, [&](int row, int col, const Value& v) {
    if (!MatchCriteria(v, criteria)) return;
    const int target_row = value_range.row1 + (row - range.row1);
    const int target_col = value_range.col1 + (col - range.col1);
    if (target_row > value_range.row2 || target_col > value_range.col2) return;
    const Value target = ctx.CellValue(value_sheet, target_row, target_col);
    double d = 0.0;
    Value e;
    if (target.is_number() && AsNumber(target, &d, &e)) {
      sum += d;
      ++count;
    }
  });
  if (count == 0) return ErrV(CellError::DivZero);
  return NumV(sum / count);
}

// ---- 查找 ---------------------------------------------------------------

Value FnVLookup(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 3) return ErrV(CellError::Value);
  const Value needle = EvalNode(*args[0], ctx, 1);
  if (needle.is_error()) return needle;
  Range table{};
  std::string sheet;
  if (!ArgAsRange(*args[1], ctx, &table, &sheet)) return ErrV(CellError::Value);
  double index = 1.0;
  Value err;
  if (!ArgNumber(args, 2, ctx, 1, &index, &err)) return err;
  const int col_index = static_cast<int>(index);
  if (col_index < 1) return ErrV(CellError::Value);
  bool approximate = true;
  if (args.size() > 3) {
    const Value flag = EvalNode(*args[3], ctx, 1);
    if (flag.is_error()) return flag;
    bool b = true;
    if (AsBool(flag, &b)) approximate = b;
  }
  const int target_col = table.col1 + col_index - 1;
  if (target_col > table.col2) return ErrV(CellError::Ref);

  int found_row = -1;
  for (int row = table.row1; row <= table.row2; ++row) {
    const Value key = ctx.CellValue(sheet, row, table.col1);
    if (key.is_error()) return key;
    const int cmp = CompareValues(key, needle, nullptr);
    if (cmp == 0) {
      found_row = row;
      break;
    }
    if (approximate && cmp < 0) found_row = row;  // 最后一个小于查找值的行
  }
  if (found_row < 0) return ErrV(CellError::NotAvailable);
  const Value result = ctx.CellValue(sheet, found_row, target_col);
  if (result.is_empty()) return NumV(0.0);
  return result;
}

Value FnMatch(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 2) return ErrV(CellError::Value);
  const Value needle = EvalNode(*args[0], ctx, 1);
  if (needle.is_error()) return needle;
  Range range{};
  std::string sheet;
  if (!ArgAsRange(*args[1], ctx, &range, &sheet)) return ErrV(CellError::Value);
  double type = 1.0;
  Value err;
  if (!ArgNumber(args, 2, ctx, 1, &type, &err)) return err;
  const bool single_row = range.row1 == range.row2;
  const int count = single_row ? (range.col2 - range.col1 + 1) : (range.row2 - range.row1 + 1);

  int best = -1;
  for (int i = 0; i < count; ++i) {
    const int row = single_row ? range.row1 : range.row1 + i;
    const int col = single_row ? range.col1 + i : range.col1;
    const Value key = ctx.CellValue(sheet, row, col);
    if (key.is_error()) return key;
    const int cmp = CompareValues(key, needle, nullptr);
    if (cmp == 0) return NumV(i + 1);
    if (type > 0 && cmp < 0) best = i;       // 升序：最后一个 <= 查找值
    if (type < 0 && cmp > 0) best = i;       // 降序：最后一个 >= 查找值
  }
  if (type == 0 || best < 0) return ErrV(CellError::NotAvailable);
  return NumV(best + 1);
}

Value FnIndex(const std::vector<NodePtr>& args, EvalContext& ctx) {
  if (args.size() < 2) return ErrV(CellError::Value);
  Range range{};
  std::string sheet;
  if (!ArgAsRange(*args[0], ctx, &range, &sheet)) return ErrV(CellError::Value);
  double row_index = 1.0;
  double col_index = 0.0;
  Value err;
  if (!ArgNumber(args, 1, ctx, 1, &row_index, &err)) return err;
  if (args.size() > 2) {
    if (!ArgNumber(args, 2, ctx, 1, &col_index, &err)) return err;
  }
  const int rows = range.row2 - range.row1 + 1;
  const int cols = range.col2 - range.col1 + 1;
  int row = static_cast<int>(row_index);
  int col = static_cast<int>(col_index);
  if (row == 0 && col == 0) return ErrV(CellError::Value);
  // 单行/单列时可以只给一个下标。
  if (col == 0 && rows == 1) {
    col = row;
    row = 1;
  }
  if (row == 0) row = 1;
  if (col == 0) col = 1;
  if (row < 1 || col < 1 || row > rows || col > cols) return ErrV(CellError::Ref);
  return ctx.CellValue(sheet, range.row1 + row - 1, range.col1 + col - 1);
}

// ---- 函数表 -------------------------------------------------------------

using FunctionImpl = Value (*)(const std::vector<NodePtr>&, EvalContext&);

struct FunctionEntry {
  const char* name;
  FunctionImpl impl;
};

const FunctionEntry kFunctionTable[] = {
    {"ABS", FnAbs},
    {"AND", FnAnd},
    {"AVERAGE", FnAverage},
    {"AVERAGEIF", FnAverageIf},
    {"CONCATENATE", FnConcatenate},
    {"COUNT", FnCount},
    {"COUNTA", FnCountA},
    {"COUNTBLANK", FnCountBlank},
    {"COUNTIF", FnCountIf},
    {"EXACT", FnExact},
    {"IF", FnIf},
    {"IFERROR", FnIfError},
    {"IFNA", FnIfNa},
    {"INDEX", FnIndex},
    {"INT", FnInt},
    {"ISBLANK", FnIsBlank},
    {"ISERROR", FnIsError},
    {"ISNA", FnIsNa},
    {"ISNUMBER", FnIsNumber},
    {"ISTEXT", FnIsText},
    {"LEFT", FnLeft},
    {"LEN", FnLen},
    {"LOWER", FnLower},
    {"MATCH", FnMatch},
    {"MAX", FnMax},
    {"MID", FnMid},
    {"MIN", FnMin},
    {"MOD", FnMod},
    {"NOT", FnNot},
    {"NOW", FnNow},
    {"OR", FnOr},
    {"PI", FnPi},
    {"POWER", FnPower},
    {"RIGHT", FnRight},
    {"ROUND", FnRound},
    {"ROUNDDOWN", FnRoundDown},
    {"ROUNDUP", FnRoundUp},
    {"SIGN", FnSign},
    {"SQRT", FnSqrt},
    {"SUM", FnSum},
    {"SUMIF", FnSumIf},
    {"TEXT", FnText},
    {"TODAY", FnToday},
    {"TRIM", FnTrim},
    {"TRUNC", FnTrunc},
    {"UPPER", FnUpper},
    {"VALUE", FnValue},
    {"VLOOKUP", FnVLookup},
};

// 认识但 P0 阶段不实现的函数：返回 #NIMPL! 而不是 #NAME?，
// 这样用户能区分"拼错了"和"还没做"。
const char* const kKnownUnimplemented[] = {
    "CEILING",   "CHOOSE",    "COLUMN",   "COLUMNS",  "CONCAT",   "DATE",
    "DATEVALUE", "DAY",       "DATEDIF",  "FIND",     "FLOOR",    "HLOOKUP",
    "HOUR",      "INDIRECT",  "LARGE",    "LOOKUP",   "MEDIAN",   "MINUTE",
    "MONTH",     "OFFSET",    "RANK",     "REPLACE",  "ROW",      "ROWS",
    "SEARCH",    "SECOND",    "SMALL",    "STDEV",    "SUBTOTAL", "SUMPRODUCT",
    "TEXTJOIN",  "TIME",      "VAR",      "WEEKDAY",  "XLOOKUP",
    "YEAR",      "COUNTIFS",  "SUMIFS",   "IFS",      "SWITCH",
};

const std::map<std::string, FunctionImpl>& FunctionMap() {
  static const std::map<std::string, FunctionImpl> kMap = [] {
    std::map<std::string, FunctionImpl> m;
    for (const FunctionEntry& e : kFunctionTable) m.emplace(e.name, e.impl);
    return m;
  }();
  return kMap;
}

const std::set<std::string>& KnownUnimplemented() {
  static const std::set<std::string> kSet = [] {
    std::set<std::string> s;
    for (const char* name : kKnownUnimplemented) s.insert(name);
    return s;
  }();
  return kSet;
}

}  // namespace

// ---- 求值入口 -----------------------------------------------------------

namespace {

// 递归深度保护：畸形/超长公式不能把栈打穿。
thread_local int g_depth = 0;

struct DepthGuard {
  DepthGuard() { ++g_depth; }
  ~DepthGuard() { --g_depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
};

Value EvalNode(const Node& node, EvalContext& ctx, int depth) {
  (void)depth;
  if (g_depth > 200) return ErrV(CellError::Num);
  DepthGuard guard;
  switch (node.kind) {
    case Node::Kind::Number:
      return NumV(node.number);
    case Node::Kind::Text:
      return Value::Str(node.text);
    case Node::Kind::Bool:
      return Value::Boolean(node.boolean);
    case Node::Kind::Error:
      return ErrV(node.error);
    case Node::Kind::Ref:
      return ctx.CellValue(node.sheet, node.row1, node.col1);
    case Node::Kind::RangeRef: {
      if (node.row1 == node.row2 && node.col1 == node.col2) {
        return ctx.CellValue(node.sheet, node.row1, node.col1);
      }
      return ErrV(CellError::Value);
    }
    case Node::Kind::Name: {
      Range range{};
      std::string sheet;
      if (!ctx.LookupName(node.text, &range, &sheet)) return ErrV(CellError::Name);
      if (range.row1 == range.row2 && range.col1 == range.col2) {
        return ctx.CellValue(sheet, range.row1, range.col1);
      }
      return ErrV(CellError::Value);
    }
    case Node::Kind::Unary: {
      if (node.args.empty()) return ErrV(CellError::Value);
      const Value v = EvalNode(*node.args[0], ctx, 0);
      if (v.is_error()) return v;
      double d = 0.0;
      Value err;
      if (!AsNumber(v, &d, &err)) return err;
      if (node.unary == UnaryOp::Minus) return NumV(-d);
      if (node.unary == UnaryOp::Percent) return NumV(d / 100.0);
      return NumV(d);
    }
    case Node::Kind::Binary: {
      if (node.args.size() < 2) return ErrV(CellError::Value);
      const Value a = EvalNode(*node.args[0], ctx, 0);
      const Value b = EvalNode(*node.args[1], ctx, 0);
      return Arith(node.binary, a, b);
    }
    case Node::Kind::Call: {
      const auto& map = FunctionMap();
      auto it = map.find(node.text);
      if (it != map.end()) return it->second(node.args, ctx);
      if (KnownUnimplemented().count(node.text) != 0) return ErrV(CellError::NotImpl);
      return ErrV(CellError::Name);
    }
  }
  return ErrV(CellError::Value);
}

}  // namespace

Value Evaluate(const Node& node, EvalContext& ctx) { return EvalNode(node, ctx, 0); }

size_t FunctionCount() { return FunctionMap().size(); }

}  // namespace sheet
}  // namespace doccore
