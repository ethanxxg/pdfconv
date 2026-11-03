// DocCore - PDF 读取实现。Design refs: docs/02 §3.5.2、§3.5.4
//
// 分层：
//   1. 词法 / 语法（PDF 对象）
//   2. 交叉引用（经典表 + xref 流 + /Prev 链，损坏时整文件扫描兜底）
//   3. 过滤器（Flate + PNG 预测、ASCIIHex、ASCII85、RunLength）
//   4. 页面树与资源继承
//   5. 字体（ToUnicode CMap、宽度表）
//   6. 内容流解释器 -> 文本片段 / 线 / 矩形 / 图像落位
#include "doccore/pdf_read.h"

#include "pdf_crypt.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doccore/deflate.h"
#include "doccore/stream.h"

namespace doccore {
namespace pdf {
namespace {

constexpr int kMaxFormDepth = 8;

void Warn(WarningSink* sink, WarningCode code, const std::string& message) {
  if (sink == nullptr) return;
  Warning w;
  w.code = static_cast<int>(code);
  w.module = "pdf_read";
  w.message = message;
  sink->Add(w);
}

[[noreturn]] void ThrowCorrupt(const std::string& what) {
  throw Error(ErrorCode::Corrupt, "pdf: " + what);
}

bool IsWhite(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

bool IsDelim(uint8_t c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
         c == '}' || c == '/' || c == '%';
}

bool IsRegular(uint8_t c) { return !IsWhite(c) && !IsDelim(c); }

int HexValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void AppendUtf8(std::string* out, uint32_t cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// ---- 字形名 -> Unicode（无 ToUnicode 时的兜底映射）-----------------------
// 覆盖常见 Adobe 字形名：连字、希腊字母、数学运算符、数学字体字形名（LaTeX
// Computer Modern / MathTime 等）。仅兜底用，名字表里没有的保持原样。
uint32_t GlyphNameToCodepoint(std::string_view name) {
  static const std::map<std::string, uint32_t, std::less<>> kMap = {
      // 连字
      {"fi", 0xFB01}, {"fl", 0xFB02}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
      {"Lslash", 0x0141}, {"lslash", 0x0142}, {"ring", 0x02DA},
      {"breve", 0x02D8}, {"caron", 0x02C7}, {"dotlessi", 0x0131},
      {"dotaccent", 0x00A8}, {"hungarumlaut", 0x02DD}, {"ogonek", 0x02DB},
      {"minus", 0x2212}, {"fraction", 0x2044}, {"cdot", 0x22C5},
      // 希腊小写
      {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
      {"epsilon", 0x03B5}, {"zeta", 0x03B6}, {"eta", 0x03B7}, {"theta", 0x03B8},
      {"iota", 0x03B9}, {"kappa", 0x03BA}, {"lambda", 0x03BB}, {"mu", 0x03BC},
      {"nu", 0x03BD}, {"xi", 0x03BE}, {"omicron", 0x03BF}, {"pi", 0x03C0},
      {"rho", 0x03C1}, {"sigma", 0x03C3}, {"tau", 0x03C4}, {"upsilon", 0x03C5},
      {"phi", 0x03C6}, {"chi", 0x03C7}, {"psi", 0x03C8}, {"omega", 0x03C9},
      {"varphi", 0x03D5}, {"vartheta", 0x03D1}, {"varpi", 0x03D6},
      {"varrho", 0x03F1}, {"varsigma", 0x03C2},
      // 希腊大写
      {"Alpha", 0x0391}, {"Beta", 0x0392}, {"Gamma", 0x0393}, {"Delta", 0x0394},
      {"Epsilon", 0x0395}, {"Zeta", 0x0396}, {"Eta", 0x0397}, {"Theta", 0x0398},
      {"Iota", 0x0399}, {"Kappa", 0x039A}, {"Lambda", 0x039B}, {"Mu", 0x039C},
      {"Nu", 0x039D}, {"Xi", 0x039E}, {"Omicron", 0x039F}, {"Pi", 0x03A0},
      {"Rho", 0x03A1}, {"Sigma", 0x03A3}, {"Tau", 0x03A4}, {"Upsilon", 0x03A5},
      {"Phi", 0x03A6}, {"Chi", 0x03A7}, {"Psi", 0x03A8}, {"Omega", 0x03A9},
      {"Phi1", 0x03A6},
      // 数学运算符（WGE 的 /Differences 直接用这些字形名）
      {"summation", 0x2211}, {"summationdisplay", 0x2211}, {"summationtext", 0x2211},
      {"integral", 0x222B}, {"product", 0x220F}, {"coproduct", 0x2210},
      {"nabla", 0x2207}, {"gradient", 0x2207}, {"partialdiff", 0x2202},
      {"infinity", 0x221E}, {"propto", 0x221D}, {"in", 0x2208},
      {"element", 0x2208}, {"notelement", 0x2209}, {"notin", 0x2209},
      {"subset", 0x2282}, {"superset", 0x2283}, {"subseteq", 0x2286},
      {"superseteq", 0x2287}, {"union", 0x222A}, {"intersection", 0x2229},
      {"negationslash", 0x2260}, {"notequal", 0x2260}, {"lessequal", 0x2264},
      {"greaterequal", 0x2265}, {"lessmuch", 0x226A}, {"greatermuch", 0x226B},
      {"equivalence", 0x2261}, {"approxequal", 0x2248}, {"approx", 0x2248},
      {"similar", 0x223C}, {"congruent", 0x2245}, {"arrowright", 0x2192},
      {"arrowleft", 0x2190}, {"arrowup", 0x2191}, {"arrowdown", 0x2193},
      {"plus", 0x002B}, {"minus", 0x2212}, {"multiply", 0x00D7},
      {"divide", 0x00F7}, {"pm", 0x00B1}, {"plusminus", 0x00B1},
      {"radical", 0x221A}, {"sqrt", 0x221A}, {"prime", 0x2032},
      {"degree", 0x00B0}, {"ellipsis", 0x2026}, {"periodcentered", 0x00B7},
      {"asteriskmath", 0x2217}, {"bardbl", 0x2225}, {"parallel", 0x2225},
      {"circlemultiply", 0x2297}, {"circleplus", 0x2295}, {"perpendicular", 0x22A5},
      {"emptyset", 0x2205}, {"existential", 0x2203}, {"universal", 0x2200},
      {"logicaland", 0x2227}, {"logicalor", 0x2228}, {"angle", 0x2220},
      {"tilde", 0x007E}, {"bar", 0x007C}, {"bullet", 0x2022},
      // 常用 ASCII 标点 / 括号（/Differences 里直接以字形名给出）
      {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022},
      {"numbersign", 0x0023}, {"dollar", 0x0024}, {"percent", 0x0025},
      {"ampersand", 0x0026}, {"quotesingle", 0x0027}, {"parenleft", 0x0028},
      {"parenright", 0x0029}, {"asterisk", 0x002A}, {"comma", 0x002C},
      {"hyphen", 0x002D}, {"period", 0x002E}, {"slash", 0x002F},
      {"colon", 0x003A}, {"semicolon", 0x003B}, {"less", 0x003C},
      {"equal", 0x003D}, {"greater", 0x003E}, {"question", 0x003F},
      {"at", 0x0040}, {"bracketleft", 0x005B}, {"backslash", 0x005C},
      {"bracketright", 0x005D}, {"asciicircum", 0x005E}, {"underscore", 0x005F},
      {"grave", 0x0060}, {"braceleft", 0x007B}, {"bar", 0x007C},
      {"braceright", 0x007D}, {"asciitilde", 0x007E}, {"quotesinglbase", 0x201A},
      {"quotedblbase", 0x201E}, {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
      {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"endash", 0x2013},
      {"emdash", 0x2014}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
  };
  auto it = kMap.find(name);
  return it == kMap.end() ? 0 : it->second;
}

// ---- 基编码：码 -> Unicode（仅高字节需要表；0x00-0x7F 全编码一致，直接透传）---

enum { kEncNone = 0, kEncStandard = 1, kEncWinAnsi = 2, kEncMacRoman = 3, kEncPdfDoc = 4 };

int EncodingNameToEnum(std::string_view name) {
  if (name == "StandardEncoding") return kEncStandard;
  if (name == "WinAnsiEncoding") return kEncWinAnsi;
  if (name == "MacRomanEncoding") return kEncMacRoman;
  if (name == "PDFDocEncoding") return kEncPdfDoc;
  return kEncNone;
}

// CP1252 (WinAnsi) 高字节 0x80-0xFF -> Unicode；0 表示未定义（回落 Latin-1）。
uint32_t WinAnsiHigh(uint32_t code) {
  static const uint16_t t[128] = {
      0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 0x80-0x87
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,      // 0x88-0x8F
      0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 0x90-0x97
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,  // 0x98-0x9F
      0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,  // 0xA0-0xA7
      0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,  // 0xA8-0xAF
      0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,  // 0xB0-0xB7
      0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,  // 0xB8-0xBF
      0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,  // 0xC0-0xC7
      0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,  // 0xC8-0xCF
      0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,  // 0xD0-0xD7
      0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,  // 0xD8-0xDF
      0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,  // 0xE0-0xE7
      0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,  // 0xE8-0xEF
      0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,  // 0xF0-0xF7
      0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,  // 0xF8-0xFF
  };
  return t[code - 0x80];
}

// Adobe StandardEncoding 高字节 0x80-0xFF -> Unicode。多数与 Latin-1 相同，
// 关键项：0xDE=fi、0xDF=fl、0xD7=× 等；其余回落 Latin-1。
uint32_t StandardHigh(uint32_t code) {
  static const uint16_t t[128] = {
      0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x87
      0, 0, 0, 0, 0, 0, 0, 0,  // 0x88-0x8F
      0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x97
      0, 0, 0, 0, 0, 0, 0, 0,  // 0x98-0x9F
      0x00A0, 0, 0, 0, 0, 0, 0, 0x00A7,  // 0xA0-0xA7
      0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,  // 0xA8-0xAF
      0x00B0, 0x00B1, 0, 0x00B4, 0x00B5, 0, 0, 0x00B7,  // 0xB0-0xB7
      0, 0, 0, 0, 0, 0, 0, 0x00BF,  // 0xB8-0xBF
      0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,  // 0xC0-0xC7
      0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,  // 0xC8-0xCF
      0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,  // 0xD0-0xD7
      0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0xFB01, 0xFB02,  // 0xD8-0xDF (fi/fl)
      0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,  // 0xE0-0xE7
      0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,  // 0xE8-0xEF
      0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,  // 0xF0-0xF7
      0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,  // 0xF8-0xFF
  };
  return t[code - 0x80];
}

uint32_t BaseEncodingCodepoint(int enc, uint32_t code) {
  if (code < 0x80) return code;  // ASCII 各编码一致，直接透传
  if (code > 0xFF) return code;
  switch (enc) {
    case kEncStandard: {
      const uint32_t v = StandardHigh(code);
      return v ? v : code;
    }
    case kEncWinAnsi: {
      const uint32_t v = WinAnsiHigh(code);
      return v ? v : code;
    }
    default:  // kEncMacRoman / kEncPdfDoc / kEncNone：回落 Latin-1
      return code;
  }
}

bool IsStandard14(const std::string& family) {
  // 基础 14 字体（PDF 规范），无 /Encoding 时默认 StandardEncoding。
  const char* kPrefix[] = {"Times", "Helvetica", "Courier"};
  for (const char* p : kPrefix) {
    if (family.rfind(p, 0) == 0) return true;
  }
  return false;
}

// ToUnicode 的 bfchar/bfrange 目标串按规范是 UTF-16BE（**不带** BOM）。
// 少数生成器会写成单字节 <41> 这种非规范形式，此时按码点直取。
std::string Utf16BeToUtf8(std::string_view bytes) {
  std::string out;
  if (bytes.size() == 1) {
    AppendUtf8(&out, static_cast<uint8_t>(bytes[0]));
    return out;
  }
  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    const uint32_t cp = (uint32_t(uint8_t(bytes[i])) << 8) | uint8_t(bytes[i + 1]);
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size()) {
      const uint32_t lo = (uint32_t(uint8_t(bytes[i + 2])) << 8) | uint8_t(bytes[i + 3]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        AppendUtf8(&out, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
        i += 2;
        continue;
      }
    }
    AppendUtf8(&out, cp);
  }
  return out;
}

// ---- 词法 ---------------------------------------------------------------

struct Token {
  enum class Kind {
    End,
    Number,
    Name,
    String,
    ArrayOpen,
    ArrayClose,
    DictOpen,
    DictClose,
    Keyword,
  };
  Kind kind = Kind::End;
  double number = 0.0;
  bool is_int = false;
  std::string text;
};

class Lexer {
 public:
  Lexer() = default;
  Lexer(const std::string* data, size_t pos = 0) : data_(data), pos_(pos) {}

  void Reset(const std::string* data, size_t pos) {
    data_ = data;
    pos_ = pos;
  }
  size_t pos() const noexcept { return pos_; }
  void set_pos(size_t pos) noexcept { pos_ = pos; }

  void SkipWhite() {
    if (data_ == nullptr) return;
    while (pos_ < data_->size()) {
      const uint8_t c = static_cast<uint8_t>((*data_)[pos_]);
      if (IsWhite(c)) {
        ++pos_;
      } else if (c == '%') {
        while (pos_ < data_->size() && (*data_)[pos_] != '\r' && (*data_)[pos_] != '\n') ++pos_;
      } else {
        break;
      }
    }
  }

  Token Next() {
    Token token;
    SkipWhite();
    if (data_ == nullptr || pos_ >= data_->size()) return token;
    const std::string& d = *data_;
    const uint8_t c = static_cast<uint8_t>(d[pos_]);

    switch (c) {
      case '[':
        ++pos_;
        token.kind = Token::Kind::ArrayOpen;
        return token;
      case ']':
        ++pos_;
        token.kind = Token::Kind::ArrayClose;
        return token;
      case '<':
        if (pos_ + 1 < d.size() && d[pos_ + 1] == '<') {
          pos_ += 2;
          token.kind = Token::Kind::DictOpen;
          return token;
        }
        ++pos_;
        token.kind = Token::Kind::String;
        token.text = ReadHexString();
        return token;
      case '>':
        if (pos_ + 1 < d.size() && d[pos_ + 1] == '>') {
          pos_ += 2;
          token.kind = Token::Kind::DictClose;
          return token;
        }
        ++pos_;
        return Next();
      case '(':
        ++pos_;
        token.kind = Token::Kind::String;
        token.text = ReadLiteralString();
        return token;
      case '/':
        ++pos_;
        token.kind = Token::Kind::Name;
        token.text = ReadName();
        return token;
      case ')':
      case '}':
      case '{':
        ++pos_;
        return Next();
      default:
        break;
    }

    const size_t start = pos_;
    while (pos_ < d.size() && IsRegular(static_cast<uint8_t>(d[pos_]))) ++pos_;
    if (pos_ == start) {
      ++pos_;
      return Next();
    }
    const std::string word = d.substr(start, pos_ - start);
    const bool numeric = !word.empty() && (std::isdigit(static_cast<unsigned char>(word[0])) ||
                                           word[0] == '-' || word[0] == '+' || word[0] == '.');
    if (numeric) {
      token.kind = Token::Kind::Number;
      token.number = std::strtod(word.c_str(), nullptr);
      token.is_int = word.find('.') == std::string::npos && word.find('e') == std::string::npos &&
                     word.find('E') == std::string::npos;
      return token;
    }
    token.kind = Token::Kind::Keyword;
    token.text = word;
    return token;
  }

 private:
  std::string ReadHexString() {
    std::string out;
    int high = -1;
    const std::string& d = *data_;
    while (pos_ < d.size()) {
      const uint8_t c = static_cast<uint8_t>(d[pos_++]);
      if (c == '>') break;
      const int v = HexValue(c);
      if (v < 0) continue;
      if (high < 0) {
        high = v;
      } else {
        out.push_back(static_cast<char>((high << 4) | v));
        high = -1;
      }
    }
    if (high >= 0) out.push_back(static_cast<char>(high << 4));
    return out;
  }

  std::string ReadLiteralString() {
    std::string out;
    int depth = 1;
    const std::string& d = *data_;
    while (pos_ < d.size()) {
      const uint8_t c = static_cast<uint8_t>(d[pos_++]);
      if (c == '\\') {
        if (pos_ >= d.size()) break;
        const uint8_t e = static_cast<uint8_t>(d[pos_++]);
        switch (e) {
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case '(': out.push_back('('); break;
          case ')': out.push_back(')'); break;
          case '\\': out.push_back('\\'); break;
          case '\r':
            if (pos_ < d.size() && d[pos_] == '\n') ++pos_;
            break;
          case '\n': break;
          default:
            if (e >= '0' && e <= '7') {
              int value = e - '0';
              for (int i = 0; i < 2 && pos_ < d.size(); ++i) {
                const uint8_t o = static_cast<uint8_t>(d[pos_]);
                if (o < '0' || o > '7') break;
                value = value * 8 + (o - '0');
                ++pos_;
              }
              out.push_back(static_cast<char>(value & 0xFF));
            } else {
              out.push_back(static_cast<char>(e));
            }
            break;
        }
        continue;
      }
      if (c == '(') {
        ++depth;
        out.push_back('(');
        continue;
      }
      if (c == ')') {
        if (--depth == 0) break;
        out.push_back(')');
        continue;
      }
      out.push_back(static_cast<char>(c));
    }
    return out;
  }

  std::string ReadName() {
    std::string out;
    const std::string& d = *data_;
    while (pos_ < d.size()) {
      const uint8_t c = static_cast<uint8_t>(d[pos_]);
      if (!IsRegular(c)) break;
      if (c == '#' && pos_ + 2 < d.size()) {
        const int hi = HexValue(static_cast<uint8_t>(d[pos_ + 1]));
        const int lo = HexValue(static_cast<uint8_t>(d[pos_ + 2]));
        if (hi >= 0 && lo >= 0) {
          out.push_back(static_cast<char>((hi << 4) | lo));
          pos_ += 3;
          continue;
        }
      }
      out.push_back(static_cast<char>(c));
      ++pos_;
    }
    return out;
  }

  const std::string* data_ = nullptr;
  size_t pos_ = 0;
};

// ---- 语法 ---------------------------------------------------------------

ObjectPtr MakeNull() { return std::make_shared<Object>(); }

ObjectPtr MakeNumber(double value, bool is_int) {
  auto o = std::make_shared<Object>();
  if (is_int) {
    o->kind = ObjKind::Int;
    o->int_value = static_cast<int64_t>(value);
  } else {
    o->kind = ObjKind::Real;
    o->real_value = value;
  }
  return o;
}

ObjectPtr MakeName(std::string name) {
  auto o = std::make_shared<Object>();
  o->kind = ObjKind::Name;
  o->text = std::move(name);
  return o;
}

ObjectPtr MakeString(std::string bytes) {
  auto o = std::make_shared<Object>();
  o->kind = ObjKind::String;
  o->text = std::move(bytes);
  return o;
}

ObjectPtr MakeBool(bool v) {
  auto o = std::make_shared<Object>();
  o->kind = ObjKind::Bool;
  o->bool_value = v;
  return o;
}

ObjectPtr MakeArray(std::vector<ObjectPtr> items) {
  auto o = std::make_shared<Object>();
  o->kind = ObjKind::Array;
  o->array = std::move(items);
  return o;
}

ObjectPtr MakeRef(int32_t num, int32_t gen) {
  auto o = std::make_shared<Object>();
  o->kind = ObjKind::Ref;
  o->ref_num = num;
  o->ref_gen = gen;
  return o;
}

class Parser {
 public:
  explicit Parser(const std::string* data, size_t pos = 0) : lex_(data, pos) {}

  Lexer& lexer() { return lex_; }
  size_t pos() const { return lex_.pos(); }
  void set_pos(size_t p) { lex_.set_pos(p); }

  ObjectPtr ParseObject(int depth = 0) {
    if (depth > 64) ThrowCorrupt("object nesting too deep");
    const Token token = lex_.Next();
    switch (token.kind) {
      case Token::Kind::End:
        return MakeNull();
      case Token::Kind::Number: {
        if (!token.is_int) return MakeNumber(token.number, false);
        const size_t save = lex_.pos();
        const Token second = lex_.Next();
        if (second.kind == Token::Kind::Number && second.is_int) {
          const size_t save2 = lex_.pos();
          const Token third = lex_.Next();
          if (third.kind == Token::Kind::Keyword && third.text == "R") {
            return MakeRef(static_cast<int32_t>(token.number), static_cast<int32_t>(second.number));
          }
          lex_.set_pos(save2);
        }
        lex_.set_pos(save);
        return MakeNumber(token.number, true);
      }
      case Token::Kind::Name:
        return MakeName(token.text);
      case Token::Kind::String:
        return MakeString(token.text);
      case Token::Kind::ArrayOpen: {
        std::vector<ObjectPtr> items;
        while (true) {
          const size_t save = lex_.pos();
          const Token next = lex_.Next();
          if (next.kind == Token::Kind::End || next.kind == Token::Kind::ArrayClose) break;
          lex_.set_pos(save);
          items.push_back(ParseObject(depth + 1));
        }
        return MakeArray(std::move(items));
      }
      case Token::Kind::DictOpen: {
        auto dict = std::make_shared<Dict>();
        while (true) {
          const size_t save = lex_.pos();
          const Token next = lex_.Next();
          if (next.kind == Token::Kind::End || next.kind == Token::Kind::DictClose) break;
          if (next.kind != Token::Kind::Name) {
            lex_.set_pos(save);
            ParseObject(depth + 1);
            continue;
          }
          const size_t value_pos = lex_.pos();
          const Token probe = lex_.Next();
          if (probe.kind == Token::Kind::DictClose || probe.kind == Token::Kind::End) break;
          lex_.set_pos(value_pos);
          (*dict)[next.text] = ParseObject(depth + 1);
        }
        auto o = std::make_shared<Object>();
        o->kind = ObjKind::Dict;
        o->dict = std::move(dict);
        return o;
      }
      case Token::Kind::Keyword:
        if (token.text == "true") return MakeBool(true);
        if (token.text == "false") return MakeBool(false);
        return MakeNull();
      default:
        return MakeNull();
    }
  }

 private:
  Lexer lex_;
};

// ---- 2D 矩阵 ------------------------------------------------------------

struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

  Mat Multiply(const Mat& m) const {
    Mat r;
    r.a = a * m.a + b * m.c;
    r.b = a * m.b + b * m.d;
    r.c = c * m.a + d * m.c;
    r.d = c * m.b + d * m.d;
    r.e = e * m.a + f * m.c + m.e;
    r.f = e * m.b + f * m.d + m.f;
    return r;
  }
  void Apply(double x, double y, double* ox, double* oy) const {
    *ox = a * x + c * y + e;
    *oy = b * x + d * y + f;
  }
  double scale_x() const { return std::sqrt(a * a + b * b); }
  double scale_y() const { return std::sqrt(c * c + d * d); }
};

int RgbFromComponents(const std::vector<double>& parts) {
  auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
  auto to255 = [&](double v) { return static_cast<int>(clamp01(v) * 255.0 + 0.5); };
  if (parts.size() == 1) {
    const int v = to255(parts[0]);
    return (v << 16) | (v << 8) | v;
  }
  if (parts.size() == 3) return (to255(parts[0]) << 16) | (to255(parts[1]) << 8) | to255(parts[2]);
  if (parts.size() == 4) {
    const double k = clamp01(parts[3]);
    const int r = to255((1.0 - clamp01(parts[0])) * (1.0 - k));
    const int g = to255((1.0 - clamp01(parts[1])) * (1.0 - k));
    const int b = to255((1.0 - clamp01(parts[2])) * (1.0 - k));
    return (r << 16) | (g << 8) | b;
  }
  return -1;
}

// ---- 过滤器 -------------------------------------------------------------

std::vector<uint8_t> ApplyPngPredictor(const std::vector<uint8_t>& in, int colors, int bpc,
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

std::vector<uint8_t> AsciiHexDecode(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  int high = -1;
  for (uint8_t c : in) {
    if (c == '>') break;
    const int v = HexValue(c);
    if (v < 0) continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<uint8_t>((high << 4) | v));
      high = -1;
    }
  }
  if (high >= 0) out.push_back(static_cast<uint8_t>(high << 4));
  return out;
}

std::vector<uint8_t> Ascii85Decode(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  uint32_t tuple = 0;
  int count = 0;
  for (uint8_t c : in) {
    if (c == '~') break;
    if (IsWhite(c)) continue;
    if (c == 'z' && count == 0) {
      out.insert(out.end(), {0, 0, 0, 0});
      continue;
    }
    if (c < '!' || c > 'u') continue;
    tuple = tuple * 85 + static_cast<uint32_t>(c - '!');
    if (++count == 5) {
      out.push_back(static_cast<uint8_t>((tuple >> 24) & 0xFF));
      out.push_back(static_cast<uint8_t>((tuple >> 16) & 0xFF));
      out.push_back(static_cast<uint8_t>((tuple >> 8) & 0xFF));
      out.push_back(static_cast<uint8_t>(tuple & 0xFF));
      tuple = 0;
      count = 0;
    }
  }
  if (count > 0) {
    for (int i = count; i < 5; ++i) tuple = tuple * 85 + 84;
    const uint8_t bytes[4] = {static_cast<uint8_t>((tuple >> 24) & 0xFF),
                              static_cast<uint8_t>((tuple >> 16) & 0xFF),
                              static_cast<uint8_t>((tuple >> 8) & 0xFF),
                              static_cast<uint8_t>(tuple & 0xFF)};
    for (int i = 0; i < count - 1; ++i) out.push_back(bytes[i]);
  }
  return out;
}

std::vector<uint8_t> RunLengthDecode(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  size_t i = 0;
  while (i < in.size()) {
    const uint8_t length = in[i++];
    if (length == 128) break;
    if (length < 128) {
      const size_t count = size_t(length) + 1;
      for (size_t k = 0; k < count && i < in.size(); ++k) out.push_back(in[i++]);
    } else {
      if (i >= in.size()) break;
      const uint8_t value = in[i++];
      for (size_t k = 0; k < size_t(257 - length); ++k) out.push_back(value);
    }
  }
  return out;
}

// ---- ToUnicode CMap -----------------------------------------------------

// 解析 bfchar / bfrange / codespacerange，得到码 -> UTF-8。
void ParseToUnicode(const std::string& cmap, std::map<uint32_t, std::string>* out,
                    int* bytes_per_code) {
  Lexer lex(&cmap, 0);
  std::string mode;
  uint32_t pending = 0;
  bool have_pending = false;
  int max_width = 0;

  auto code_of = [&](const std::string& bytes, int* width) {
    *width = static_cast<int>(bytes.size() >= 2 ? 2 : bytes.size());
    if (bytes.size() >= 2) {
      return (uint32_t(uint8_t(bytes[0])) << 8) | uint8_t(bytes[1]);
    }
    return bytes.empty() ? 0u : uint32_t(uint8_t(bytes[0]));
  };

  while (true) {
    const Token token = lex.Next();
    if (token.kind == Token::Kind::End) break;
    if (token.kind == Token::Kind::Keyword) {
      if (token.text == "beginbfchar") {
        mode = "bfchar";
      } else if (token.text == "beginbfrange") {
        mode = "bfrange";
      } else if (token.text == "begincodespacerange") {
        mode = "codespace";
      } else if (token.text == "endbfchar" || token.text == "endbfrange" ||
                 token.text == "endcodespacerange") {
        mode.clear();
      }
      have_pending = false;
      continue;
    }
    if (mode.empty()) continue;

    if (token.kind == Token::Kind::String) {
      int width = 1;
      const uint32_t value = code_of(token.text, &width);
      if (mode == "codespace") {
        max_width = std::max(max_width, width);
      } else if (mode == "bfchar") {
        if (!have_pending) {
          pending = value;
          have_pending = true;
          max_width = std::max(max_width, width);
        } else {
          (*out)[pending] = Utf16BeToUtf8(token.text);
          have_pending = false;
        }
      } else {  // bfrange: <lo> <hi> <dst> | <lo> <hi> [<dst>...]
        if (!have_pending) {
          pending = value;
          have_pending = true;
          max_width = std::max(max_width, width);
          continue;
        }
        const uint32_t lo = pending;
        const uint32_t hi = value;
        const size_t save = lex.pos();
        const Token dst = lex.Next();
        if (dst.kind == Token::Kind::ArrayOpen) {
          uint32_t code = lo;
          while (true) {
            const size_t inner = lex.pos();
            const Token item = lex.Next();
            if (item.kind == Token::Kind::ArrayClose || item.kind == Token::Kind::End) break;
            if (item.kind == Token::Kind::String) {
              (*out)[code] = Utf16BeToUtf8(item.text);
              ++code;
            } else {
              lex.set_pos(inner);
              lex.Next();
            }
          }
        } else if (dst.kind == Token::Kind::String) {
          const std::string base = dst.text;
          if (base.size() >= 2) {
            const uint32_t start = (uint32_t(uint8_t(base[0])) << 8) | uint8_t(base[1]);
            for (uint32_t c = lo; c <= hi && c - lo < 65536; ++c) {
              std::string one = base;
              one[0] = static_cast<char>((start + (c - lo)) >> 8);
              one[1] = static_cast<char>((start + (c - lo)) & 0xFF);
              (*out)[c] = Utf16BeToUtf8(one);
            }
          }
        } else {
          lex.set_pos(save);
        }
        have_pending = false;
      }
      continue;
    }
    if (token.kind == Token::Kind::ArrayOpen && mode == "bfrange" && have_pending) {
      uint32_t code = pending;
      while (true) {
        const size_t save = lex.pos();
        const Token item = lex.Next();
        if (item.kind == Token::Kind::ArrayClose || item.kind == Token::Kind::End) break;
        if (item.kind == Token::Kind::String) {
          (*out)[code] = Utf16BeToUtf8(item.text);
          ++code;
        } else {
          lex.set_pos(save);
          lex.Next();
        }
      }
      have_pending = false;
      continue;
    }
  }
  if (bytes_per_code != nullptr) *bytes_per_code = max_width >= 2 ? 2 : 1;
}

// /W 数组：[ c [w1 w2 ...] ] 或 [ cfirst clast w ]
void ParseCidWidths(const ObjectPtr& widths, std::map<uint32_t, double>* out) {
  if (widths == nullptr || widths->kind != ObjKind::Array) return;
  size_t i = 0;
  while (i < widths->array.size()) {
    const ObjectPtr& first = widths->array[i];
    if (first == nullptr || !first->is_number()) {
      ++i;
      continue;
    }
    const uint32_t cfirst = static_cast<uint32_t>(first->number());
    if (i + 1 >= widths->array.size()) break;
    const ObjectPtr& second = widths->array[i + 1];
    if (second != nullptr && second->kind == ObjKind::Array) {
      for (size_t k = 0; k < second->array.size(); ++k) {
        const ObjectPtr& v = second->array[k];
        if (v != nullptr && v->is_number()) {
          (*out)[cfirst + static_cast<uint32_t>(k)] = v->number();
        }
      }
      i += 2;
      continue;
    }
    if (second == nullptr || !second->is_number()) break;
    const uint32_t clast = static_cast<uint32_t>(second->number());
    if (i + 2 >= widths->array.size()) break;
    const ObjectPtr& third = widths->array[i + 2];
    const double value = third != nullptr && third->is_number() ? third->number() : 0.0;
    for (uint32_t c = cfirst; c <= clast && c - cfirst < 65536; ++c) (*out)[c] = value;
    i += 3;
  }
}

std::string FamilyFromBaseFont(const std::string& base) {
  std::string name = base;
  if (name.size() > 7 && name[6] == '+') name = name.substr(7);  // 去子集前缀
  const size_t comma = name.find(',');
  if (comma != std::string::npos) name = name.substr(0, comma);
  return name;
}

}  // namespace

// ---- Object / 字符串 ----------------------------------------------------

const ObjectPtr* Object::Find(std::string_view key) const {
  if (dict == nullptr) return nullptr;
  auto it = dict->find(std::string(key));
  return it == dict->end() ? nullptr : &it->second;
}

std::string PdfStringToUtf8(std::string_view bytes) {
  if (bytes.size() >= 2) {
    const uint8_t b0 = static_cast<uint8_t>(bytes[0]);
    const uint8_t b1 = static_cast<uint8_t>(bytes[1]);
    if (b0 == 0xFE && b1 == 0xFF) return Utf16BeToUtf8(bytes.substr(2));
  }
  static const char* kHigh[32] = {
      "\u2022", "\u2020", "\u2021", "\u2026", "\u2014", "\u2013", "\u0192", "\u2044",
      "\u2039", "\u203a", "\u2212", "\u2030", "\u201e", "\u201c", "\u201d", "\u2018",
      "\u2019", "\u201a", "\u2122", "\ufb01", "\ufb02", "\u0141", "\u0152", "\u0160",
      "\u0178", "\u017d", "\u0131", "\u0142", "\u0153", "\u0161", "\u017e", "\ufffd",
  };
  std::string out;
  out.reserve(bytes.size());
  for (char ch : bytes) {
    const uint8_t c = static_cast<uint8_t>(ch);
    if (c >= 0x80 && c <= 0x9F) {
      out += kHigh[c - 0x80];
    } else if (c < 0x80) {
      out.push_back(ch);
    } else {
      AppendUtf8(&out, c);
    }
  }
  return out;
}

// ---- FontDecoder --------------------------------------------------------

std::vector<std::pair<uint32_t, std::string>> FontDecoder::Decode(std::string_view bytes) const {
  std::vector<std::pair<uint32_t, std::string>> out;
  const size_t step = static_cast<size_t>(std::max(1, bytes_per_code));
  for (size_t i = 0; i + step <= bytes.size(); i += step) {
    uint32_t code = 0;
    for (size_t k = 0; k < step; ++k) code = (code << 8) | static_cast<uint8_t>(bytes[i + k]);
    std::string text;
    auto it = to_unicode.find(code);
    if (it != to_unicode.end()) {
      text = it->second;
    } else if (identity_unicode && step >= 2) {
      // 预定义 Uni*-UTF16/UCS2 CMap：码值即 Unicode。代理对要合成为一个码点。
      uint32_t cp = code;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 * step <= bytes.size()) {
        uint32_t low = 0;
        for (size_t k = 0; k < step; ++k) {
          low = (low << 8) | static_cast<uint8_t>(bytes[i + step + k]);
        }
        if (low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += step;
        }
      }
      AppendUtf8(&text, cp);
    } else if (!composite) {
      auto eit = enc_unicode.find(code);
      if (eit != enc_unicode.end()) {
        text = eit->second;  // 来自 /Encoding（基编码表 + /Differences）的映射
      } else {
        // 没有 ToUnicode、也没有编码表：单字节按 Latin-1 近似，至少不丢字。
        if (code < 0x80) {
          text.push_back(static_cast<char>(code));
        } else {
          AppendUtf8(&text, code);
        }
      }
    } else {
      continue;  // 复合字体的码解不出字形：丢码不丢行
    }
    // 有的 CMap 把码映到 U+0000（占位符字形）。剔掉它：NUL 进了文本流会把
    // 下游所有按 C 字符串处理文本的地方截断。
    if (text.find('\0') != std::string::npos) {
      std::string clean;
      clean.reserve(text.size());
      for (char c : text) {
        if (c != '\0') clean.push_back(c);
      }
      text.swap(clean);
    }
    // LaTeX/dvips 把 Times 的 fi/fl 连字放在 0xDE/0xDF，但部分 PDF 生成器把编码标成
    // MacRoman/WinAnsi（0xDE=Þ、0xDF=ß）。英文论文里几乎不会出现冰岛语/德文，这里还原成连字。
    if (text == "\xC3\x9E") {
      text = "fi";
    } else if (text == "\xC3\x9F") {
      text = "fl";
    }
    if (text.empty()) continue;
    out.emplace_back(code, text);
  }
  return out;
}

double FontDecoder::WidthOf(uint32_t code) const {
  auto it = widths.find(code);
  return it == widths.end() ? default_width : it->second;
}

// ---- 内容流解释器 -------------------------------------------------------

namespace {

struct GState {
  Mat ctm;
  double font_size = 12.0;
  double char_spacing = 0.0;
  double word_spacing = 0.0;
  double horizontal_scale = 1.0;
  double leading = 0.0;
  double rise = 0.0;
  std::string font_name;
  const FontDecoder* font = nullptr;
  int fill = 0x000000;
  int stroke = 0x000000;
  double line_width = 1.0;
};

struct Interp {
  PdfPage* page = nullptr;
  std::vector<PdfTextRun>* runs = nullptr;
  std::vector<PdfLine>* lines = nullptr;
  std::vector<PdfRect>* rects = nullptr;
  std::vector<PdfImagePlacement>* images = nullptr;
  const ResourceScope* scope = nullptr;
  const InterpHost* host = nullptr;
  Mat ctm;  // 进入这段内容流时的 CTM：Form 递归必须带着调用方的变换
  double ox = 0.0;
  double oy = 0.0;
  int depth = 0;

  void EmitLine(const GState& gs, double x1, double y1, double x2, double y2) {
    const double dx = std::abs(x2 - x1);
    const double dy = std::abs(y2 - y1);
    if (dx < 2.0 && dy < 2.0) return;  // 短于 2 pt 丢弃
    if (dx > 0.5 && dy > 0.5) return;  // 斜线不进表格检测
    PdfLine line;
    line.x1 = x1 - ox;
    line.y1 = y1 - oy;
    line.x2 = x2 - ox;
    line.y2 = y2 - oy;
    line.width_pt = gs.line_width;
    line.color = gs.stroke;
    lines->push_back(line);
  }

  void ShowText(const GState& gs, Mat* text_matrix, const std::string& bytes) {
    if (gs.font == nullptr) return;
    const auto glyphs = gs.font->Decode(bytes);
    PdfTextRun run;
    run.font_name = gs.font_name;
    run.family = gs.font->family;
    run.bold = gs.font->bold;
    run.italic = gs.font->italic;
    run.color = gs.fill;
    const Mat trm = text_matrix->Multiply(gs.ctm);
    run.size_pt = gs.font_size * trm.scale_y();
    if (run.size_pt <= 0.0) run.size_pt = gs.font_size;
    run.x = trm.e - ox;
    run.y = trm.f + gs.rise * trm.scale_y() - oy;

    double advance_total = 0.0;
    for (const auto& glyph : glyphs) {
      const double w0 = gs.font->WidthOf(glyph.first) / 1000.0;
      const bool is_space = glyph.first == 32;
      const double tx = (w0 * gs.font_size + gs.char_spacing +
                         (is_space ? gs.word_spacing : 0.0)) *
                        gs.horizontal_scale;
      run.text_utf8 += glyph.second;
      advance_total += tx;
      Mat move;
      move.e = tx;
      *text_matrix = move.Multiply(*text_matrix);
    }
    run.width = advance_total * trm.scale_x();
    if (!run.text_utf8.empty()) runs->push_back(std::move(run));
  }

  void Run(const std::string& content);
  // Form XObject 的 /Resources 挂在对象自己身上，解析后作为当前域的子域递归。
  void RunForm(const ObjectPtr& form, const Mat& ctm);
};

void Interp::Run(const std::string& content) {
  if (depth > kMaxFormDepth) return;
  GState gs;
  gs.ctm = ctm;
  std::vector<GState> stack;
  std::vector<double> numbers;
  std::vector<std::string> strings;
  Mat text_matrix;
  Mat line_matrix;

  // 路径累积：只关心水平/垂直线段与 re 矩形
  struct PathPoint {
    double x = 0, y = 0;
  };
  std::vector<PathPoint> points;
  PathPoint current;
  PathPoint start;
  bool has_start = false;

  Parser parser(&content, 0);
  Lexer& lex = parser.lexer();

  auto clear_operands = [&] {
    numbers.clear();
    strings.clear();
  };

  while (true) {
    const size_t before = lex.pos();
    const Token token = lex.Next();
    if (token.kind == Token::Kind::End) break;
    if (token.kind == Token::Kind::Number) {
      numbers.push_back(token.number);
      continue;
    }
    if (token.kind == Token::Kind::String) {
      strings.push_back(token.text);
      continue;
    }
    // /Name 也是操作数：Tf 的字体名、Do 的 XObject 名都走这里。
    if (token.kind == Token::Kind::Name) {
      strings.push_back(token.text);
      continue;
    }
    if (token.kind == Token::Kind::ArrayOpen) {
      // TJ 数组：把字符串和数字按顺序摊平，数字用 NaN 标记间距
      std::vector<std::string> parts;
      while (true) {
        const size_t save = lex.pos();
        const Token item = lex.Next();
        if (item.kind == Token::Kind::ArrayClose || item.kind == Token::Kind::End) break;
        if (item.kind == Token::Kind::String) {
          parts.push_back(item.text);
        } else if (item.kind == Token::Kind::Number) {
          // TJ 的间距调整：绝对值 >= 0.18 em 视为词间空隙，补一个空格
          if (item.number <= -180.0 && !parts.empty()) parts.back().push_back(' ');
        } else {
          lex.set_pos(save);
          lex.Next();
        }
      }
      // 交给 TJ 算子：数组里的字符串按顺序进 strings 队列
      for (const auto& part : parts) strings.push_back(part);
      continue;
    }
    if (token.kind == Token::Kind::DictOpen) {
      lex.set_pos(before);
      parser.ParseObject();
      continue;
    }
    if (token.kind != Token::Kind::Keyword) continue;

    const std::string& op = token.text;
    if (op == "q") {
      stack.push_back(gs);
    } else if (op == "Q") {
      if (!stack.empty()) {
        gs = stack.back();
        stack.pop_back();
      }
    } else if (op == "cm" && numbers.size() >= 6) {
      Mat m;
      m.a = numbers[numbers.size() - 6];
      m.b = numbers[numbers.size() - 5];
      m.c = numbers[numbers.size() - 4];
      m.d = numbers[numbers.size() - 3];
      m.e = numbers[numbers.size() - 2];
      m.f = numbers[numbers.size() - 1];
      gs.ctm = m.Multiply(gs.ctm);
    } else if (op == "BT") {
      text_matrix = Mat{};
      line_matrix = Mat{};
    } else if (op == "Tf") {
      if (!strings.empty()) {
        gs.font_name = strings.back();
        gs.font = scope == nullptr ? nullptr : scope->FindFont(gs.font_name);
      }
      if (!numbers.empty()) gs.font_size = numbers.back();
    } else if (op == "TL" && !numbers.empty()) {
      gs.leading = numbers.back();
    } else if (op == "Tc" && !numbers.empty()) {
      gs.char_spacing = numbers.back();
    } else if (op == "Tw" && !numbers.empty()) {
      gs.word_spacing = numbers.back();
    } else if (op == "Tz" && !numbers.empty()) {
      gs.horizontal_scale = numbers.back() / 100.0;
    } else if (op == "Ts" && !numbers.empty()) {
      gs.rise = numbers.back();
    } else if (op == "Tm" && numbers.size() >= 6) {
      Mat m;
      m.a = numbers[numbers.size() - 6];
      m.b = numbers[numbers.size() - 5];
      m.c = numbers[numbers.size() - 4];
      m.d = numbers[numbers.size() - 3];
      m.e = numbers[numbers.size() - 2];
      m.f = numbers[numbers.size() - 1];
      text_matrix = m;
      line_matrix = m;
    } else if ((op == "Td" || op == "TD") && numbers.size() >= 2) {
      const double tx = numbers[numbers.size() - 2];
      const double ty = numbers[numbers.size() - 1];
      if (op == "TD") gs.leading = -ty;
      Mat move;
      move.e = tx;
      move.f = ty;
      line_matrix = move.Multiply(line_matrix);
      text_matrix = line_matrix;
    } else if (op == "T*") {
      Mat move;
      move.f = -gs.leading;
      line_matrix = move.Multiply(line_matrix);
      text_matrix = line_matrix;
    } else if (op == "Tj" || op == "TJ") {
      for (const std::string& part : strings) ShowText(gs, &text_matrix, part);
    } else if (op == "'") {
      Mat move;
      move.f = -gs.leading;
      line_matrix = move.Multiply(line_matrix);
      text_matrix = line_matrix;
      for (const std::string& part : strings) ShowText(gs, &text_matrix, part);
    } else if (op == "\"") {
      if (numbers.size() >= 2) {
        gs.word_spacing = numbers[numbers.size() - 2];
        gs.char_spacing = numbers[numbers.size() - 1];
      }
      Mat move;
      move.f = -gs.leading;
      line_matrix = move.Multiply(line_matrix);
      text_matrix = line_matrix;
      for (const std::string& part : strings) ShowText(gs, &text_matrix, part);
    } else if (op == "g" && !numbers.empty()) {
      gs.fill = RgbFromComponents({numbers.back()});
    } else if (op == "rg" && numbers.size() >= 3) {
      gs.fill = RgbFromComponents({numbers[numbers.size() - 3], numbers[numbers.size() - 2],
                                   numbers[numbers.size() - 1]});
    } else if (op == "k" && numbers.size() >= 4) {
      gs.fill = RgbFromComponents({numbers[numbers.size() - 4], numbers[numbers.size() - 3],
                                   numbers[numbers.size() - 2], numbers[numbers.size() - 1]});
    } else if (op == "G" && !numbers.empty()) {
      gs.stroke = RgbFromComponents({numbers.back()});
    } else if (op == "RG" && numbers.size() >= 3) {
      gs.stroke = RgbFromComponents({numbers[numbers.size() - 3], numbers[numbers.size() - 2],
                                     numbers[numbers.size() - 1]});
    } else if (op == "K" && numbers.size() >= 4) {
      gs.stroke = RgbFromComponents({numbers[numbers.size() - 4], numbers[numbers.size() - 3],
                                     numbers[numbers.size() - 2], numbers[numbers.size() - 1]});
    } else if (op == "w" && !numbers.empty()) {
      gs.line_width = numbers.back();
    } else if (op == "m" && numbers.size() >= 2) {
      current.x = numbers[numbers.size() - 2];
      current.y = numbers[numbers.size() - 1];
      start = current;
      has_start = true;
      points.clear();
      double x = 0, y = 0;
      gs.ctm.Apply(current.x, current.y, &x, &y);
      points.push_back({x, y});
    } else if (op == "l" && numbers.size() >= 2) {
      double x = 0, y = 0;
      gs.ctm.Apply(numbers[numbers.size() - 2], numbers[numbers.size() - 1], &x, &y);
      if (has_start && !points.empty()) EmitLine(gs, points.back().x, points.back().y, x, y);
      points.push_back({x, y});
      current.x = numbers[numbers.size() - 2];
      current.y = numbers[numbers.size() - 1];
    } else if ((op == "c" || op == "v" || op == "y") && numbers.size() >= 2) {
      // 曲线：只记终点，起点到终点的直线段不参与表格检测
      double x = 0, y = 0;
      gs.ctm.Apply(numbers[numbers.size() - 2], numbers[numbers.size() - 1], &x, &y);
      points.push_back({x, y});
    } else if (op == "h") {
      // 闭合子路径：补上"当前点 -> 起点"这条边。表格框线大量依赖它——不补
      // 的话每个矩形的左边会丢，表格检测只能看到 3 条边。
      if (has_start && !points.empty()) {
        const double sx = points.front().x;
        const double sy = points.front().y;
        if (std::abs(points.back().x - sx) > 0.01 || std::abs(points.back().y - sy) > 0.01) {
          EmitLine(gs, points.back().x, points.back().y, sx, sy);
          points.push_back({sx, sy});
        }
      }
      current = start;
    } else if (op == "re" && numbers.size() >= 4) {
      const double x0 = numbers[numbers.size() - 4];
      const double y0 = numbers[numbers.size() - 3];
      const double w0 = numbers[numbers.size() - 2];
      const double h0 = numbers[numbers.size() - 1];
      double ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0, dx = 0, dy = 0;
      gs.ctm.Apply(x0, y0, &ax, &ay);
      gs.ctm.Apply(x0 + w0, y0, &bx, &by);
      gs.ctm.Apply(x0 + w0, y0 + h0, &cx, &cy);
      gs.ctm.Apply(x0, y0 + h0, &dx, &dy);
      PdfRect rect;
      rect.x = std::min(std::min(ax, bx), std::min(cx, dx)) - ox;
      rect.y = std::min(std::min(ay, by), std::min(cy, dy)) - oy;
      rect.w = std::max(std::abs(bx - ax), std::abs(cx - dx));
      rect.h = std::max(std::abs(cy - by), std::abs(dy - ay));
      rect.stroke = gs.stroke;
      rect.stroke_pt = gs.line_width;
      rects->push_back(rect);
      // re 的四条边也当线段用（很多表格只有描边矩形）
      EmitLine(gs, ax, ay, bx, by);
      EmitLine(gs, bx, by, cx, cy);
      EmitLine(gs, cx, cy, dx, dy);
      EmitLine(gs, dx, dy, ax, ay);
      current.x = x0;
      current.y = y0;
      start = current;
      has_start = true;
      points.clear();
      points.push_back({ax, ay});
      points.push_back({cx, cy});
    } else if (op == "f" || op == "F" || op == "f*" || op == "B" || op == "b" || op == "b*") {
      if (!rects->empty() && rects->back().fill < 0 && rects->back().stroke_pt >= 0.0) {
        rects->back().fill = gs.fill;
      }
      points.clear();
      has_start = false;
    } else if (op == "S" || op == "s" || op == "n") {
      if (op == "n" && !rects->empty() && rects->back().fill < 0) {
        // clip 路径不是可见矩形
        rects->pop_back();
      }
      points.clear();
      has_start = false;
    } else if (op == "Do" && !strings.empty()) {
      const std::string name = strings.back();
      const ObjectPtr* found = scope == nullptr ? nullptr : scope->FindXObject(name);
      const ObjectPtr obj = found == nullptr ? nullptr : *found;
      if (obj != nullptr && obj->is_dict()) {
        const ObjectPtr* subtype_ptr = obj->Find("Subtype");
        std::string subtype;
        if (subtype_ptr != nullptr && (*subtype_ptr)->kind == ObjKind::Name) {
          subtype = (*subtype_ptr)->text;
        }
        if (subtype == "Image") {
          PdfImagePlacement placement;
          placement.xobject_name = name;
          placement.xobject = obj;
          double ax = 0, ay = 0, bx = 0, by = 0;
          gs.ctm.Apply(0, 0, &ax, &ay);
          gs.ctm.Apply(1, 1, &bx, &by);
          placement.x = std::min(ax, bx) - ox;
          placement.y = std::min(ay, by) - oy;
          placement.w = std::abs(bx - ax);
          placement.h = std::abs(by - ay);
          auto int_of = [&](const char* key) {
            const ObjectPtr* v = obj->Find(key);
            return v != nullptr && (*v)->is_number() ? static_cast<int>((*v)->number()) : 0;
          };
          placement.pixel_width = int_of("Width");
          placement.pixel_height = int_of("Height");
          images->push_back(placement);
        } else if (subtype == "Form") {
          // Form XObject：/Matrix 串到 CTM，然后带着 Form 自己的资源递归执行。
          const Mat saved = gs.ctm;
          if (const ObjectPtr* matrix = obj->Find("Matrix")) {
            if ((*matrix) != nullptr && (*matrix)->kind == ObjKind::Array &&
                (*matrix)->array.size() == 6) {
              Mat m;
              m.a = (*matrix)->array[0]->number();
              m.b = (*matrix)->array[1]->number();
              m.c = (*matrix)->array[2]->number();
              m.d = (*matrix)->array[3]->number();
              m.e = (*matrix)->array[4]->number();
              m.f = (*matrix)->array[5]->number();
              gs.ctm = m.Multiply(gs.ctm);
            }
          }
          RunForm(obj, gs.ctm);
          gs.ctm = saved;
        }
      }
    }

    clear_operands();
  }
}

void Interp::RunForm(const ObjectPtr& form, const Mat& entry_ctm) {
  if (host == nullptr || form == nullptr || form->kind != ObjKind::Stream) return;
  if (depth >= kMaxFormDepth) return;

  ResourceScope child;
  child.parent = scope;
  if (const ObjectPtr* res = form->Find("Resources")) {
    ObjectPtr dict = host->Resolve(*res);
    if (dict != nullptr && dict->is_dict()) {
      if (const ObjectPtr* fonts = dict->Find("Font")) {
        ObjectPtr fd = host->Resolve(*fonts);
        if (fd != nullptr && fd->is_dict()) {
          for (const auto& kv : *fd->dict) {
            child.fonts[kv.first] = host->BuildFontDecoder(host->Resolve(kv.second));
          }
        }
      }
      if (const ObjectPtr* xo = dict->Find("XObject")) {
        ObjectPtr xd = host->Resolve(*xo);
        if (xd != nullptr && xd->is_dict()) {
          for (const auto& kv : *xd->dict) child.xobjects[kv.first] = host->Resolve(kv.second);
        }
      }
    }
  }

  const std::vector<uint8_t> bytes = host->StreamBytes(form);
  const std::string content(bytes.begin(), bytes.end());
  Interp sub;
  sub.page = page;
  sub.runs = runs;
  sub.lines = lines;
  sub.rects = rects;
  sub.images = images;
  sub.scope = &child;
  sub.host = host;
  sub.ctm = entry_ctm;
  sub.ox = ox;
  sub.oy = oy;
  sub.depth = depth + 1;
  sub.Run(content);
}

}  // namespace

// ---- PdfPage ------------------------------------------------------------

const FontDecoder* ResourceScope::FindFont(std::string_view name) const {
  for (const ResourceScope* s = this; s != nullptr; s = s->parent) {
    auto it = s->fonts.find(std::string(name));
    if (it != s->fonts.end()) return &it->second;
  }
  return nullptr;
}

const ObjectPtr* ResourceScope::FindXObject(std::string_view name) const {
  for (const ResourceScope* s = this; s != nullptr; s = s->parent) {
    auto it = s->xobjects.find(std::string(name));
    if (it != s->xobjects.end()) return &it->second;
  }
  return nullptr;
}

const ObjectPtr* PdfPage::FindXObject(std::string_view name) const {
  EnsureScope();
  return scope_root_->FindXObject(name);
}

void PdfPage::EnsureScope() const {
  if (scope_root_ != nullptr) return;
  auto scope = std::make_unique<ResourceScope>();
  scope->fonts = fonts_;
  scope->xobjects = xobjects_;
  scope_root_ = std::move(scope);
}

void PdfPage::EnsureParsed() const {
  EnsureScope();
  if (parsed_) return;
  parsed_ = true;
  if (content_.empty()) return;
  Interp interp;
  interp.page = const_cast<PdfPage*>(this);
  interp.runs = &runs_;
  interp.lines = &lines_;
  interp.rects = &rects_;
  interp.images = &images_;
  interp.scope = scope_root_.get();
  interp.host = host_;
  interp.ox = origin_x_;
  interp.oy = origin_y_;
  interp.Run(content_);
}

const std::vector<PdfTextRun>& PdfPage::text_runs() const {
  EnsureParsed();
  return runs_;
}

const std::vector<PdfLine>& PdfPage::lines() const {
  EnsureParsed();
  return lines_;
}

const std::vector<PdfRect>& PdfPage::rects() const {
  EnsureParsed();
  return rects_;
}

const std::vector<PdfImagePlacement>& PdfPage::images() const {
  EnsureParsed();
  return images_;
}

// ---- 注释 ---------------------------------------------------------------

const char* AnnotSubtypeName(AnnotSubtype s) noexcept {
  switch (s) {
    case AnnotSubtype::Text: return "Text";
    case AnnotSubtype::Link: return "Link";
    case AnnotSubtype::FreeText: return "FreeText";
    case AnnotSubtype::Line: return "Line";
    case AnnotSubtype::Square: return "Square";
    case AnnotSubtype::Circle: return "Circle";
    case AnnotSubtype::Polygon: return "Polygon";
    case AnnotSubtype::PolyLine: return "PolyLine";
    case AnnotSubtype::Highlight: return "Highlight";
    case AnnotSubtype::Underline: return "Underline";
    case AnnotSubtype::Squiggly: return "Squiggly";
    case AnnotSubtype::StrikeOut: return "StrikeOut";
    case AnnotSubtype::Stamp: return "Stamp";
    case AnnotSubtype::Caret: return "Caret";
    case AnnotSubtype::Ink: return "Ink";
    case AnnotSubtype::Popup: return "Popup";
    case AnnotSubtype::FileAttachment: return "FileAttachment";
    case AnnotSubtype::Sound: return "Sound";
    case AnnotSubtype::Movie: return "Movie";
    case AnnotSubtype::Widget: return "Widget";
    case AnnotSubtype::Screen: return "Screen";
    case AnnotSubtype::PrinterMark: return "PrinterMark";
    case AnnotSubtype::TrapNet: return "TrapNet";
    case AnnotSubtype::Watermark: return "Watermark";
    case AnnotSubtype::ThreeD: return "3D";
    case AnnotSubtype::Redact: return "Redact";
    case AnnotSubtype::Unknown: return "Unknown";
  }
  return "Unknown";
}

AnnotSubtype AnnotSubtypeFromName(std::string_view name) noexcept {
  if (name == "Text") return AnnotSubtype::Text;
  if (name == "Link") return AnnotSubtype::Link;
  if (name == "FreeText") return AnnotSubtype::FreeText;
  if (name == "Line") return AnnotSubtype::Line;
  if (name == "Square") return AnnotSubtype::Square;
  if (name == "Circle") return AnnotSubtype::Circle;
  if (name == "Polygon") return AnnotSubtype::Polygon;
  if (name == "PolyLine") return AnnotSubtype::PolyLine;
  if (name == "Highlight") return AnnotSubtype::Highlight;
  if (name == "Underline") return AnnotSubtype::Underline;
  if (name == "Squiggly") return AnnotSubtype::Squiggly;
  if (name == "StrikeOut") return AnnotSubtype::StrikeOut;
  if (name == "Stamp") return AnnotSubtype::Stamp;
  if (name == "Caret") return AnnotSubtype::Caret;
  if (name == "Ink") return AnnotSubtype::Ink;
  if (name == "Popup") return AnnotSubtype::Popup;
  if (name == "FileAttachment") return AnnotSubtype::FileAttachment;
  if (name == "Sound") return AnnotSubtype::Sound;
  if (name == "Movie") return AnnotSubtype::Movie;
  if (name == "Widget") return AnnotSubtype::Widget;
  if (name == "Screen") return AnnotSubtype::Screen;
  if (name == "PrinterMark") return AnnotSubtype::PrinterMark;
  if (name == "TrapNet") return AnnotSubtype::TrapNet;
  if (name == "Watermark") return AnnotSubtype::Watermark;
  if (name == "3D") return AnnotSubtype::ThreeD;
  if (name == "Redact") return AnnotSubtype::Redact;
  return AnnotSubtype::Unknown;
}

// 解析页面 /Annots。页面字典按对象号取，注释本身可能是直接字典或间接引用。
std::vector<PdfAnnotation> ParsePageAnnotations(const InterpHost& host, int page_obj, int page_index);

const std::vector<PdfAnnotation>& PdfPage::annotations() const {
  if (!annots_loaded_) {
    annots_loaded_ = true;
    if (host_ != nullptr) annots_ = ParsePageAnnotations(*host_, obj_num, index);
  }
  return annots_;
}

namespace {

// /Rect 之类的数字数组。
bool AnnotRect(const InterpHost& host, const ObjectPtr& dict, const char* key, double out[4]) {
  const ObjectPtr* v = dict->Find(key);
  if (v == nullptr) return false;
  const ObjectPtr arr = host.Resolve(*v);
  if (arr == nullptr || arr->kind != ObjKind::Array || arr->array.size() != 4) return false;
  for (int i = 0; i < 4; ++i) {
    const ObjectPtr n = host.Resolve(arr->array[static_cast<size_t>(i)]);
    if (n == nullptr || !n->is_number()) return false;
    out[i] = n->number();
  }
  return true;
}

std::string AnnotText(const InterpHost& host, const ObjectPtr& dict, const char* key) {
  const ObjectPtr* v = dict->Find(key);
  if (v == nullptr) return std::string();
  const ObjectPtr s = host.Resolve(*v);
  if (s == nullptr || s->kind != ObjKind::String) return std::string();
  return PdfStringToUtf8(s->text);
}

bool AnnotNumber(const InterpHost& host, const ObjectPtr& dict, const char* key, double* out) {
  const ObjectPtr* v = dict->Find(key);
  if (v == nullptr) return false;
  const ObjectPtr n = host.Resolve(*v);
  if (n == nullptr || !n->is_number()) return false;
  *out = n->number();
  return true;
}

std::string AnnotName(const InterpHost& host, const ObjectPtr& dict, const char* key) {
  const ObjectPtr* v = dict->Find(key);
  if (v == nullptr) return std::string();
  const ObjectPtr n = host.Resolve(*v);
  if (n == nullptr || n->kind != ObjKind::Name) return std::string();
  return n->text;
}

}  // namespace

std::vector<PdfAnnotation> ParsePageAnnotations(const InterpHost& host, int page_obj,
                                                int page_index) {
  std::vector<PdfAnnotation> out;
  const ObjectPtr page = host.ObjectAt(page_obj);
  if (page == nullptr || !page->is_dict()) return out;
  const ObjectPtr* annots_ptr = page->Find("Annots");
  if (annots_ptr == nullptr) return out;
  const ObjectPtr list = host.Resolve(*annots_ptr);
  if (list == nullptr || list->kind != ObjKind::Array) return out;

  for (const ObjectPtr& item : list->array) {
    const ObjectPtr dict = host.Resolve(item);
    if (dict == nullptr || !dict->is_dict()) continue;
    PdfAnnotation a;
    a.page = page_index;
    a.subtype_name = AnnotName(host, dict, "Subtype");
    a.subtype = AnnotSubtypeFromName(a.subtype_name);
    if (a.subtype_name.empty()) continue;  // 没有 /Subtype 的不是注释

    double rect[4] = {0, 0, 0, 0};
    if (AnnotRect(host, dict, "Rect", rect)) {
      // 规范化成左下角 + 宽高（原文的 x1/x2 顺序不保证）。
      const double x0 = std::min(rect[0], rect[2]);
      const double y0 = std::min(rect[1], rect[3]);
      a.x = x0;
      a.y = y0;
      a.w = std::abs(rect[2] - rect[0]);
      a.h = std::abs(rect[3] - rect[1]);
    }
    a.contents = AnnotText(host, dict, "Contents");
    a.author = AnnotText(host, dict, "T");
    a.subject = AnnotText(host, dict, "Subj");
    a.modified = AnnotText(host, dict, "M");
    a.unique_name = AnnotText(host, dict, "NM");
    a.icon = AnnotName(host, dict, "Name");

    double value = 0.0;
    if (AnnotNumber(host, dict, "CA", &value)) a.opacity = value;
    if (AnnotNumber(host, dict, "F", &value)) a.flags = static_cast<int>(value);
    if (AnnotNumber(host, dict, "Open", &value)) a.open = value != 0.0;

    // /C 是 0..1 的分量数组；1/3/4 分量都见过，只取前三个拼成 RGB。
    if (const ObjectPtr* c_ptr = dict->Find("C")) {
      const ObjectPtr c = host.Resolve(*c_ptr);
      if (c != nullptr && c->kind == ObjKind::Array && !c->array.empty()) {
        int rgb = 0;
        const size_t n = std::min<size_t>(3, c->array.size());
        for (size_t i = 0; i < n; ++i) {
          const ObjectPtr comp = host.Resolve(c->array[i]);
          double v = 0.0;
          if (comp != nullptr && comp->is_number()) v = comp->number();
          const int byte = static_cast<int>(v * 255.0 + 0.5);
          rgb = (rgb << 8) | (byte < 0 ? 0 : (byte > 255 ? 255 : byte));
        }
        if (n == 1) rgb = (rgb << 16) | (rgb << 8) | rgb;  // 灰度
        a.color = rgb & 0xFFFFFF;
      }
    }

    // /QuadPoints：8 的倍数，每 8 个数一个四边形。
    if (const ObjectPtr* q_ptr = dict->Find("QuadPoints")) {
      const ObjectPtr q = host.Resolve(*q_ptr);
      if (q != nullptr && q->kind == ObjKind::Array) {
        std::vector<double> nums;
        for (const ObjectPtr& one : q->array) {
          const ObjectPtr n = host.Resolve(one);
          if (n != nullptr && n->is_number()) nums.push_back(n->number());
        }
        for (size_t i = 0; i + 7 < nums.size(); i += 8) {
          PdfQuad quad;
          quad.x1 = nums[i + 0];
          quad.y1 = nums[i + 1];
          quad.x2 = nums[i + 2];
          quad.y2 = nums[i + 3];
          quad.x3 = nums[i + 4];
          quad.y3 = nums[i + 5];
          quad.x4 = nums[i + 6];
          quad.y4 = nums[i + 7];
          a.quads.push_back(quad);
        }
      }
    }

    if (const ObjectPtr* ap = dict->Find("AP")) {
      const ObjectPtr ap_dict = host.Resolve(*ap);
      a.has_appearance = ap_dict != nullptr && ap_dict->is_dict();
    }
    out.push_back(std::move(a));
  }
  return out;
}

std::string PdfPage::ExtractText() const {
  const auto& all = text_runs();
  if (all.empty()) return std::string();

  // 按视觉行分组：估每块字的纵向跨度（上伸 0.8、下伸 0.2），与当前行跨度重叠到
  // 自身高度 35% 以上即视为同一行。同基线只是其中一种情况；同一行里的上下标、
  // 异号字号（fi/fl 连字、尾部小号数字）也都能并回来；行距大于 1.0*size 时
  // 两块字不重叠，不会把相邻两行并成一行。
  std::vector<size_t> order(all.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (std::abs(all[a].y - all[b].y) > 0.5) return all[a].y > all[b].y;
    return all[a].x < all[b].x;
  });

  struct Line {
    std::vector<size_t> runs;
    double lo = 0.0;  // 跨度下沿（PDF 坐标，左下原点）
    double hi = 0.0;  // 跨度上沿
  };
  std::vector<Line> lines;
  auto span_of = [](const PdfTextRun& r) {
    const double size = r.size_pt > 0.0 ? r.size_pt : 10.0;
    return std::make_pair(r.y - 0.2 * size, r.y + 0.8 * size);
  };
  for (size_t idx : order) {
    const PdfTextRun& run = all[idx];
    const std::pair<double, double> span = span_of(run);
    const double height = span.second - span.first;
    Line* cur = lines.empty() ? nullptr : &lines.back();
    if (cur != nullptr && height > 0.0) {
      const double overlap = std::min(cur->hi, span.second) - std::max(cur->lo, span.first);
      if (overlap >= 0.35 * height) {
        cur->lo = std::min(cur->lo, span.first);
        cur->hi = std::max(cur->hi, span.second);
        cur->runs.push_back(idx);
        continue;
      }
    }
    Line fresh;
    fresh.lo = span.first;
    fresh.hi = span.second;
    fresh.runs.push_back(idx);
    lines.push_back(std::move(fresh));
  }

  std::string out;
  bool first_line = true;
  for (const Line& line : lines) {
    std::vector<size_t> by_x = line.runs;
    std::sort(by_x.begin(), by_x.end(),
              [&](size_t a, size_t b) { return all[a].x < all[b].x; });
    if (!first_line) out += '\n';
    first_line = false;
    bool first_run = true;
    double last_right = 0.0;
    double last_size = 10.0;
    for (size_t idx : by_x) {
      const PdfTextRun& run = all[idx];
      if (first_run) {
        out += run.text_utf8;
        first_run = false;
      } else {
        const double gap = run.x - last_right;
        if (gap > std::max(1.5, last_size * 0.25)) out += ' ';
        out += run.text_utf8;
      }
      last_right = run.right();
      last_size = run.size_pt;
    }
  }
  return out;
}

bool PdfPage::looks_scanned() const {
  const auto& all = images();
  if (all.empty()) return false;
  const double page_area = size.width * size.height;
  if (page_area <= 0.0) return false;
  double biggest = 0.0;
  for (const PdfImagePlacement& image : all) {
    biggest = std::max(biggest, image.w * image.h);
  }
  return biggest >= page_area * 0.8;
}

// ---- PdfFile ------------------------------------------------------------

struct PdfFile::Impl : InterpHost {
  std::string data;
  std::string version = "1.7";
  bool encrypted = false;
  std::string title;
  WarningSink* warnings = nullptr;
  std::map<int32_t, size_t> offsets;
  mutable std::map<int32_t, ObjectPtr> cache;
  ObjectPtr trailer;
  size_t startxref = 0;  // 最后一个 xref 段的偏移；增量更新写 /Prev 用
  std::unique_ptr<SecurityHandler> security;  // 非空表示文档已解密
  int encrypt_obj_num = 0;                    // /Encrypt 字典本身不能被解密
  PdfPermissions perms;                       // 未加密时保持全 true 的默认值
  std::vector<std::unique_ptr<PdfPage>> pages;

  size_t OffsetOf(int32_t num) const {
    auto it = offsets.find(num);
    return it == offsets.end() ? 0 : it->second;
  }

  ObjectPtr ResolveRef(int32_t num) const {
    auto hit = cache.find(num);
    if (hit != cache.end()) return hit->second;
    const size_t offset = OffsetOf(num);
    if (offset == 0 || offset >= data.size()) return nullptr;
    int gen = 0;
    ObjectPtr obj = ParseIndirectAt(offset, &gen);
    cache[num] = obj;
    // 解密在拿到对象号之后才能做（一次性密钥依赖对象号与代号）。
    if (obj != nullptr && security != nullptr && num != encrypt_obj_num) {
      DecryptObject(obj, num, gen);
    }
    return obj;
  }

  // 递归解密一个对象里的所有字符串与流。间接引用不进去（解到它时自己会解）。
  void DecryptObject(const ObjectPtr& obj, int num, int gen) const {
    if (obj == nullptr || security == nullptr) return;
    switch (obj->kind) {
      case ObjKind::String: {
        const std::vector<uint8_t> raw(obj->text.begin(), obj->text.end());
        const std::vector<uint8_t> plain = security->Decrypt(raw, num, gen, false);
        obj->text.assign(plain.begin(), plain.end());
        break;
      }
      case ObjKind::Array:
        for (const ObjectPtr& item : obj->array) DecryptObject(item, num, gen);
        break;
      case ObjKind::Dict:
        if (obj->dict != nullptr) {
          for (auto& kv : *obj->dict) DecryptObject(kv.second, num, gen);
        }
        break;
      case ObjKind::Stream: {
        // 交叉引用流永远不加密；/EncryptMetadata false 时元数据流也不加密。
        bool skip = false;
        if (obj->dict != nullptr) {
          auto type = obj->dict->find("Type");
          if (type != obj->dict->end() && type->second != nullptr &&
              type->second->kind == ObjKind::Name) {
            if (type->second->text == "XRef") skip = true;
            if (type->second->text == "Metadata" && !security->encrypt_metadata()) skip = true;
          }
        }
        if (!skip) obj->raw = security->Decrypt(obj->raw, num, gen, true);
        if (obj->dict != nullptr) {
          for (auto& kv : *obj->dict) DecryptObject(kv.second, num, gen);
        }
        break;
      }
      default:
        break;
    }
  }

  ObjectPtr ResolveObj(const ObjectPtr& obj) const {
    ObjectPtr current = obj;
    for (int hop = 0; hop < 8 && current != nullptr; ++hop) {
      if (current->kind != ObjKind::Ref) return current;
      current = ResolveRef(current->ref_num);
    }
    return current;
  }

  ObjectPtr ObjectAt(int32_t num) const override { return ResolveRef(num); }

  ObjectPtr ParseIndirectAt(size_t offset, int* out_gen = nullptr) const {
    if (offset >= data.size()) return nullptr;
    Parser parser(&data, offset);
    const Token num = parser.lexer().Next();
    const Token gen = parser.lexer().Next();
    const Token kw = parser.lexer().Next();
    if (num.kind != Token::Kind::Number || kw.kind != Token::Kind::Keyword || kw.text != "obj") {
      (void)gen;
      return nullptr;
    }
    if (out_gen != nullptr) *out_gen = gen.kind == Token::Kind::Number ? static_cast<int>(gen.number) : 0;
    ObjectPtr obj = parser.ParseObject();
    if (obj == nullptr) return nullptr;
    const size_t after_dict = parser.pos();
    Parser probe(&data, after_dict);
    const Token maybe_stream = probe.lexer().Next();
    if (maybe_stream.kind == Token::Kind::Keyword && maybe_stream.text == "stream") {
      size_t start = probe.lexer().pos();
      if (start < data.size() && data[start] == '\r') ++start;
      if (start < data.size() && data[start] == '\n') ++start;
      size_t length = 0;
      bool have_length = false;
      if (const ObjectPtr* len = obj->Find("Length")) {
        const ObjectPtr resolved = ResolveObj(*len);
        if (resolved != nullptr && resolved->is_number()) {
          length = static_cast<size_t>(resolved->number());
          have_length = true;
        }
      }
      if (!have_length || start + length > data.size()) {
        const size_t end = data.find("endstream", start);
        length = end == std::string::npos ? (data.size() - start) : (end - start);
        while (length > 0) {
          const char c = data[start + length - 1];
          if (c != '\n' && c != '\r') break;
          --length;
        }
      }
      obj->kind = ObjKind::Stream;
      obj->raw.assign(data.begin() + static_cast<long>(start),
                      data.begin() + static_cast<long>(start + length));
    }
    return obj;
  }

  std::vector<uint8_t> DecodeStreamBytes(const ObjectPtr& stream) const {
    if (stream == nullptr || stream->kind != ObjKind::Stream) return {};
    std::vector<uint8_t> bytes = stream->raw;
    std::vector<std::string> filters;
    if (const ObjectPtr* f = stream->Find("Filter")) {
      const ObjectPtr resolved = ResolveObj(*f);
      if (resolved != nullptr && resolved->kind == ObjKind::Name) {
        filters.push_back(resolved->text);
      } else if (resolved != nullptr && resolved->kind == ObjKind::Array) {
        for (const ObjectPtr& item : resolved->array) {
          const ObjectPtr one = ResolveObj(item);
          if (one != nullptr && one->kind == ObjKind::Name) filters.push_back(one->text);
        }
      }
    }
    for (const std::string& filter : filters) {
      if (filter == "FlateDecode" || filter == "Fl") {
        const size_t cap = std::max<size_t>(bytes.size() * 100 + (1 << 20), size_t(1) << 20);
        try {
          bytes = ZlibDecompress(bytes.data(), bytes.size(), cap);
        } catch (const Error&) {
          bytes = InflateRaw(bytes.data(), bytes.size(), cap);
        }
        if (const ObjectPtr* parms = stream->Find("DecodeParms")) {
          const ObjectPtr p = ResolveObj(*parms);
          if (p != nullptr && p->is_dict()) {
            auto int_of = [&](const char* key, int fallback) {
              const ObjectPtr* v = p->Find(key);
              if (v == nullptr) return fallback;
              const ObjectPtr r = ResolveObj(*v);
              return r != nullptr && r->is_number() ? static_cast<int>(r->number()) : fallback;
            };
            if (int_of("Predictor", 1) >= 10) {
              bytes = ApplyPngPredictor(bytes, int_of("Colors", 1), int_of("BitsPerComponent", 8),
                                        int_of("Columns", 1));
            }
          }
        }
      } else if (filter == "ASCIIHexDecode" || filter == "AHx") {
        bytes = AsciiHexDecode(bytes);
      } else if (filter == "ASCII85Decode" || filter == "A85") {
        bytes = Ascii85Decode(bytes);
      } else if (filter == "RunLengthDecode" || filter == "RL") {
        bytes = RunLengthDecode(bytes);
      } else {
        break;  // DCTDecode / JPXDecode / CCITTFaxDecode：图像压缩，保留原始
      }
    }
    return bytes;
  }

  // ---- InterpHost：内容流解释器经由这里访问文件层 -----------------------

  std::vector<uint8_t> StreamBytes(const ObjectPtr& stream) const override {
    return DecodeStreamBytes(stream);
  }
  FontDecoder BuildFontDecoder(const ObjectPtr& font) const override {
    return MakeFontDecoder(font);
  }
  ObjectPtr Resolve(const ObjectPtr& obj) const override { return ResolveObj(obj); }

  // ---- 交叉引用 ----

  void ParseXref() {
    const size_t sx = data.rfind("startxref");
    if (sx != std::string::npos) {
      Parser parser(&data, sx + 9);
      const Token offset_token = parser.lexer().Next();
      if (offset_token.kind == Token::Kind::Number) {
        startxref = static_cast<size_t>(offset_token.number);
        if (ReadXrefChain(startxref, 0)) return;
      }
    }
    startxref = 0;  // 没有可用 xref：走全量扫描，增量更新也就不写 /Prev
    ScanAllObjects();
  }

  bool ReadXrefChain(size_t offset, int depth) {
    if (depth > 32 || offset == 0 || offset >= data.size()) return false;
    Parser parser(&data, offset);
    const Token first = parser.lexer().Next();
    if (!(first.kind == Token::Kind::Keyword && first.text == "xref")) {
      return ReadXrefStream(offset, depth);
    }
    // 经典表
    bool saw_trailer = false;
    while (true) {
      const size_t save = parser.pos();
      const Token start = parser.lexer().Next();
      if (start.kind == Token::Kind::Keyword && start.text == "trailer") {
        saw_trailer = true;  // 关键字已经被吃掉，下面直接读字典
        break;
      }
      if (start.kind != Token::Kind::Number) {
        parser.set_pos(save);
        break;
      }
      const Token count = parser.lexer().Next();
      if (count.kind != Token::Kind::Number) break;
      const int64_t first_num = static_cast<int64_t>(start.number);
      const int64_t entries = static_cast<int64_t>(count.number);
      for (int64_t i = 0; i < entries && i < 2000000; ++i) {
        const Token off = parser.lexer().Next();
        const Token gen = parser.lexer().Next();
        const Token type = parser.lexer().Next();
        (void)gen;
        if (off.kind != Token::Kind::Number) break;
        if (type.kind == Token::Kind::Keyword && type.text == "n") {
          const int32_t num = static_cast<int32_t>(first_num + i);
          if (offsets.find(num) == offsets.end()) offsets[num] = static_cast<size_t>(off.number);
        }
      }
    }
    if (!saw_trailer) {
      const Token trailer_kw = parser.lexer().Next();
      if (!(trailer_kw.kind == Token::Kind::Keyword && trailer_kw.text == "trailer")) return false;
    }
    {
      ObjectPtr dict = parser.ParseObject();
      if (dict != nullptr && dict->is_dict()) {
        if (trailer == nullptr) trailer = dict;
        if (const ObjectPtr* prev = dict->Find("Prev")) {
          const ObjectPtr p = ResolveObj(*prev);
          if (p != nullptr && p->is_number()) {
            ReadXrefChain(static_cast<size_t>(p->number()), depth + 1);
          }
        }
        if (const ObjectPtr* xstm = dict->Find("XRefStm")) {
          const ObjectPtr p = ResolveObj(*xstm);
          if (p != nullptr && p->is_number()) {
            ReadXrefStream(static_cast<size_t>(p->number()), depth + 1);
          }
        }
      }
    }
    return true;
  }

  bool ReadXrefStream(size_t offset, int depth) {
    ObjectPtr stream = ParseIndirectAt(offset);
    if (stream == nullptr || stream->kind != ObjKind::Stream || stream->dict == nullptr) {
      return false;
    }
    const ObjectPtr* type_ptr = stream->Find("Type");
    if (type_ptr == nullptr) return false;
    const ObjectPtr type = ResolveObj(*type_ptr);
    if (type == nullptr || type->kind != ObjKind::Name || type->text != "XRef") return false;
    if (trailer == nullptr) trailer = stream;

    const std::vector<uint8_t> bytes = DecodeStreamBytes(stream);
    auto int_of = [&](const char* key, int64_t fallback) {
      const ObjectPtr* v = stream->Find(key);
      if (v == nullptr) return fallback;
      const ObjectPtr r = ResolveObj(*v);
      return r != nullptr && r->is_number() ? static_cast<int64_t>(r->number()) : fallback;
    };
    std::vector<int64_t> w;
    if (const ObjectPtr* wv = stream->Find("W")) {
      const ObjectPtr r = ResolveObj(*wv);
      if (r != nullptr && r->kind == ObjKind::Array) {
        for (const ObjectPtr& item : r->array) {
          const ObjectPtr n = ResolveObj(item);
          w.push_back(n != nullptr && n->is_number() ? static_cast<int64_t>(n->number()) : 0);
        }
      }
    }
    if (w.size() < 3) return false;

    std::vector<std::pair<int64_t, int64_t>> ranges;
    if (const ObjectPtr* index = stream->Find("Index")) {
      const ObjectPtr r = ResolveObj(*index);
      if (r != nullptr && r->kind == ObjKind::Array) {
        for (size_t i = 0; i + 1 < r->array.size(); i += 2) {
          const ObjectPtr a = ResolveObj(r->array[i]);
          const ObjectPtr b = ResolveObj(r->array[i + 1]);
          if (a != nullptr && b != nullptr && a->is_number() && b->is_number()) {
            ranges.emplace_back(static_cast<int64_t>(a->number()),
                                static_cast<int64_t>(b->number()));
          }
        }
      }
    }
    if (ranges.empty()) ranges.emplace_back(0, int_of("Size", 0));

    size_t pos = 0;
    auto read_field = [&](int64_t width, bool* ok) -> int64_t {
      if (width == 0) return 0;
      int64_t value = 0;
      for (int64_t i = 0; i < width; ++i) {
        if (pos >= bytes.size()) {
          *ok = false;
          return 0;
        }
        value = (value << 8) | bytes[pos++];
      }
      return value;
    };
    for (const auto& range : ranges) {
      for (int64_t i = 0; i < range.second; ++i) {
        bool ok = true;
        const int64_t type_field = read_field(w[0], &ok);
        const int64_t entry_type = w[0] == 0 ? 1 : type_field;
        const int64_t offset_field = read_field(w[1], &ok);
        read_field(w[2], &ok);
        if (!ok) break;
        const int32_t num = static_cast<int32_t>(range.first + i);
        if (entry_type == 1 && offsets.find(num) == offsets.end()) {
          offsets[num] = static_cast<size_t>(offset_field);
        }
      }
    }
    if (const ObjectPtr* prev = stream->Find("Prev")) {
      const ObjectPtr p = ResolveObj(*prev);
      if (p != nullptr && p->is_number()) ReadXrefChain(static_cast<size_t>(p->number()), depth + 1);
    }
    return true;
  }

  void ScanAllObjects() {
    size_t pos = 0;
    while (pos < data.size()) {
      const size_t at = data.find("obj", pos);
      if (at == std::string::npos) break;
      pos = at + 3;
      size_t cursor = at;
      while (cursor > 0 && IsWhite(static_cast<uint8_t>(data[cursor - 1]))) --cursor;
      const size_t gen_end = cursor;
      while (cursor > 0 && std::isdigit(static_cast<unsigned char>(data[cursor - 1]))) --cursor;
      if (cursor == gen_end) continue;
      while (cursor > 0 && IsWhite(static_cast<uint8_t>(data[cursor - 1]))) --cursor;
      const size_t num_end = cursor;
      while (cursor > 0 && std::isdigit(static_cast<unsigned char>(data[cursor - 1]))) --cursor;
      if (cursor == num_end) continue;
      const int32_t num = std::atoi(data.substr(cursor, num_end - cursor).c_str());
      if (num <= 0) continue;
      offsets[num] = cursor;
    }
    const size_t tr = data.rfind("trailer");
    if (tr != std::string::npos) {
      Parser parser(&data, tr + 7);
      ObjectPtr dict = parser.ParseObject();
      if (dict != nullptr && dict->is_dict()) trailer = dict;
    }
    if (trailer == nullptr) {
      for (const auto& kv : offsets) {
        ObjectPtr obj = ResolveRef(kv.first);
        if (obj == nullptr || !obj->is_dict()) continue;
        const ObjectPtr* type = obj->Find("Type");
        if (type == nullptr) continue;
        const ObjectPtr t = ResolveObj(*type);
        if (t != nullptr && t->kind == ObjKind::Name && t->text == "XRef") {
          trailer = obj;
          break;
        }
      }
    }
  }

  // ---- 字体 ----

  FontDecoder MakeFontDecoder(const ObjectPtr& font) const {
    FontDecoder decoder;
    if (font == nullptr || !font->is_dict()) return decoder;
    auto name_of = [&](const char* key) -> std::string {
      const ObjectPtr* v = font->Find(key);
      if (v == nullptr) return {};
      const ObjectPtr r = ResolveObj(*v);
      return r != nullptr && r->kind == ObjKind::Name ? r->text : std::string();
    };
    std::string base;
    if (const ObjectPtr* bf = font->Find("BaseFont")) {
      const ObjectPtr r = ResolveObj(*bf);
      if (r != nullptr && r->kind == ObjKind::Name) base = r->text;
    }
    decoder.family = FamilyFromBaseFont(base);
    std::string lowered = decoder.family;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    decoder.bold = lowered.find("bold") != std::string::npos ||
                   lowered.find("black") != std::string::npos ||
                   lowered.find("heavy") != std::string::npos;
    decoder.italic = lowered.find("italic") != std::string::npos ||
                     lowered.find("oblique") != std::string::npos;

    decoder.composite = name_of("Subtype") == "Type0";
    decoder.bytes_per_code = decoder.composite ? 2 : 1;

    // 预定义 Unicode CMap：UniGB-UCS2-H / UniGB-UTF16-H / UniCNS-* / UniJIS-* /
    // UniKS-*。这类字体往往不带 ToUnicode（码值本身就是 Unicode），
    // 认出来才能还原中文，否则整段文字会变成空串。
    const std::string encoding = name_of("Encoding");
    if (encoding.rfind("Uni", 0) == 0 &&
        (encoding.find("UCS2") != std::string::npos ||
         encoding.find("UTF16") != std::string::npos)) {
      decoder.identity_unicode = true;
    }

    if (decoder.composite) {
      if (const ObjectPtr* desc = font->Find("DescendantFonts")) {
        const ObjectPtr list = ResolveObj(*desc);
        if (list != nullptr && list->kind == ObjKind::Array && !list->array.empty()) {
          const ObjectPtr cid = ResolveObj(list->array[0]);
          if (cid != nullptr && cid->is_dict()) {
            if (const ObjectPtr* dw = cid->Find("DW")) {
              const ObjectPtr r = ResolveObj(*dw);
              if (r != nullptr && r->is_number()) decoder.default_width = r->number();
            }
            if (const ObjectPtr* w = cid->Find("W")) {
              ParseCidWidths(ResolveObj(*w), &decoder.widths);
            }
          }
        }
      }
    } else {
      double first = 0.0;
      if (const ObjectPtr* fc = font->Find("FirstChar")) {
        const ObjectPtr r = ResolveObj(*fc);
        if (r != nullptr && r->is_number()) first = r->number();
      }
      if (const ObjectPtr* widths = font->Find("Widths")) {
        const ObjectPtr list = ResolveObj(*widths);
        if (list != nullptr && list->kind == ObjKind::Array) {
          for (size_t i = 0; i < list->array.size(); ++i) {
            const ObjectPtr v = ResolveObj(list->array[i]);
            if (v != nullptr && v->is_number()) {
              decoder.widths[static_cast<uint32_t>(first) + static_cast<uint32_t>(i)] =
                  v->number();
            }
          }
        }
      }
    }

    if (const ObjectPtr* tu = font->Find("ToUnicode")) {
      const ObjectPtr stream = ResolveObj(*tu);
      if (stream != nullptr && stream->kind == ObjKind::Stream) {
        const std::vector<uint8_t> bytes = DecodeStreamBytes(stream);
        const std::string cmap(bytes.begin(), bytes.end());
        int code_bytes = decoder.bytes_per_code;
        ParseToUnicode(cmap, &decoder.to_unicode, &code_bytes);
        decoder.bytes_per_code = decoder.composite ? code_bytes : 1;
      }
    }

    // 无 ToUnicode 时，用 /Encoding（基编码表 + /Differences）兜底码 -> Unicode。
    // 这对 LaTeX 生成的论文（Times / CMSY / MathTime 等非嵌入字体）尤其关键：否则
    // 连字与数学符号会被错解成 Latin-1（如 fi 变 Þ，∑/λ/∈ 变乱码）。
    int base_enc = kEncNone;
    std::map<uint32_t, std::string> enc_map;
    if (const ObjectPtr* enc_ptr = font->Find("Encoding")) {
      const ObjectPtr enc = ResolveObj(*enc_ptr);
      if (enc != nullptr) {
        if (enc->kind == ObjKind::Name) {
          base_enc = EncodingNameToEnum(enc->text);
        } else if (enc->kind == ObjKind::Dict) {
          if (const ObjectPtr* be = enc->Find("BaseEncoding")) {
            const ObjectPtr b = ResolveObj(*be);
            if (b != nullptr && b->kind == ObjKind::Name) base_enc = EncodingNameToEnum(b->text);
          }
          if (base_enc == kEncNone) base_enc = kEncStandard;  // 无 BaseEncoding 时默认标准编码
          if (const ObjectPtr* diff = enc->Find("Differences")) {
            const ObjectPtr d = ResolveObj(*diff);
            if (d != nullptr && d->kind == ObjKind::Array) {
              int32_t cur = 0;
              for (const ObjectPtr& item : d->array) {
                const ObjectPtr r = ResolveObj(item);
                if (r == nullptr) continue;
                if (r->kind == ObjKind::Int) {
                  cur = static_cast<int32_t>(r->number());
                } else if (r->kind == ObjKind::Name) {
                  uint32_t cp = GlyphNameToCodepoint(r->text);
                  std::string u;
                  if (cp != 0) {
                    AppendUtf8(&u, cp);
                  } else if (r->text.size() == 1) {
                    u.push_back(r->text[0]);  // 单字符字形名（如 /w）直接取字符
                  }
                  if (!u.empty()) enc_map[static_cast<uint32_t>(cur)] = u;
                  ++cur;
                }
              }
            }
          }
        }
      }
    } else if (IsStandard14(decoder.family)) {
      base_enc = kEncStandard;  // 基础 14 字体缺省编码 = StandardEncoding（连字在 0xDE/0xDF）
    }

    // 基编码表里、未被 /Differences 覆盖的高字节码，填进 enc_map（ASCII 段透传，不必存）。
    for (uint32_t code = 0x80; code <= 0xFF; ++code) {
      if (enc_map.find(code) != enc_map.end()) continue;
      if (base_enc == kEncNone) continue;
      const uint32_t cp = BaseEncodingCodepoint(base_enc, code);
      if (cp != 0 && cp != code) {
        std::string u;
        AppendUtf8(&u, cp);
        enc_map[code] = u;
      }
    }
    decoder.enc_unicode = std::move(enc_map);
    return decoder;
  }

  // ---- 页面树 ----

  struct PageNode {
    ObjectPtr resources;
    ObjectPtr media_box;
    int rotate = 0;
  };

  void CollectPages(const ObjectPtr& node, const PageNode& inherited, int depth,
                    std::set<const Object*>* seen, int node_obj = 0) {
    if (node == nullptr || depth > 64 || !node->is_dict()) return;
    if (seen->count(node.get()) != 0) return;
    seen->insert(node.get());

    PageNode current = inherited;
    if (const ObjectPtr* res = node->Find("Resources")) current.resources = ResolveObj(*res);
    if (const ObjectPtr* box = node->Find("MediaBox")) current.media_box = ResolveObj(*box);
    if (const ObjectPtr* rot = node->Find("Rotate")) {
      const ObjectPtr r = ResolveObj(*rot);
      if (r != nullptr && r->is_number()) current.rotate = static_cast<int>(r->number());
    }
    std::string type;
    if (const ObjectPtr* type_ptr = node->Find("Type")) {
      const ObjectPtr t = ResolveObj(*type_ptr);
      if (t != nullptr && t->kind == ObjKind::Name) type = t->text;
    }

    if (type == "Page") {
      auto page = std::make_unique<PdfPage>();
      page->index = static_cast<int>(pages.size());
      page->obj_num = node_obj;
      page->rotation = ((current.rotate % 360) + 360) % 360;

      double w = 612.0;
      double h = 792.0;
      double ox = 0.0;
      double oy = 0.0;
      if (current.media_box != nullptr && current.media_box->kind == ObjKind::Array &&
          current.media_box->array.size() == 4) {
        double v[4] = {0, 0, 612, 792};
        for (int i = 0; i < 4; ++i) {
          const ObjectPtr n = ResolveObj(current.media_box->array[static_cast<size_t>(i)]);
          if (n != nullptr && n->is_number()) v[i] = n->number();
        }
        w = std::abs(v[2] - v[0]);
        h = std::abs(v[3] - v[1]);
        ox = std::min(v[0], v[2]);
        oy = std::min(v[1], v[3]);
      }
      page->SetOrigin(ox, oy);
      if (page->rotation == 90 || page->rotation == 270) std::swap(w, h);
      page->size.width = w;
      page->size.height = h;

      if (const ObjectPtr* contents = node->Find("Contents")) {
        const ObjectPtr resolved = ResolveObj(*contents);
        if (resolved != nullptr && resolved->kind == ObjKind::Stream) {
          const std::vector<uint8_t> bytes = DecodeStreamBytes(resolved);
          page->SetContent(std::string(bytes.begin(), bytes.end()));
        } else if (resolved != nullptr && resolved->kind == ObjKind::Array) {
          std::string merged;
          for (const ObjectPtr& item : resolved->array) {
            const ObjectPtr one = ResolveObj(item);
            if (one == nullptr) continue;
            const std::vector<uint8_t> bytes = DecodeStreamBytes(one);
            merged.append(bytes.begin(), bytes.end());
            merged.push_back('\n');
          }
          page->SetContent(std::move(merged));
        }
      }

      if (current.resources != nullptr && current.resources->is_dict()) {
        if (const ObjectPtr* fonts = current.resources->Find("Font")) {
          const ObjectPtr dict = ResolveObj(*fonts);
          if (dict != nullptr && dict->is_dict()) {
            for (const auto& kv : *dict->dict) {
              page->AddFont(kv.first, MakeFontDecoder(ResolveObj(kv.second)));
            }
          }
        }
        if (const ObjectPtr* xobjs = current.resources->Find("XObject")) {
          const ObjectPtr dict = ResolveObj(*xobjs);
          if (dict != nullptr && dict->is_dict()) {
            for (const auto& kv : *dict->dict) page->AddXObject(kv.first, ResolveObj(kv.second));
          }
        }
      }
      page->SetHost(this);  // Form XObject 的 /Resources 是间接引用
      pages.push_back(std::move(page));
      return;
    }

    if (const ObjectPtr* kids = node->Find("Kids")) {
      const ObjectPtr list = ResolveObj(*kids);
      if (list != nullptr && list->kind == ObjKind::Array) {
        for (const ObjectPtr& kid : list->array) {
          const int kid_obj = kid != nullptr && kid->kind == ObjKind::Ref ? kid->ref_num : 0;
          CollectPages(ResolveObj(kid), current, depth + 1, seen, kid_obj);
        }
      }
    }
  }

  void BuildPages() {
    if (trailer == nullptr) ThrowCorrupt("no trailer");
    ObjectPtr root;
    if (const ObjectPtr* root_ptr = trailer->Find("Root")) root = ResolveObj(*root_ptr);
    if (root == nullptr) {
      for (const auto& kv : offsets) {
        ObjectPtr obj = ResolveRef(kv.first);
        if (obj == nullptr || !obj->is_dict()) continue;
        const ObjectPtr* type = obj->Find("Type");
        if (type == nullptr) continue;
        const ObjectPtr t = ResolveObj(*type);
        if (t != nullptr && t->kind == ObjKind::Name && t->text == "Catalog") {
          root = obj;
          break;
        }
      }
    }
    if (root == nullptr) ThrowCorrupt("no document catalog");

    if (const ObjectPtr* info = trailer->Find("Info")) {
      const ObjectPtr dict = ResolveObj(*info);
      if (dict != nullptr && dict->is_dict()) {
        if (const ObjectPtr* t = dict->Find("Title")) {
          const ObjectPtr value = ResolveObj(*t);
          if (value != nullptr && value->kind == ObjKind::String) {
            title = PdfStringToUtf8(value->text);
          }
        }
      }
    }

    ObjectPtr pages_node;
    if (const ObjectPtr* p = root->Find("Pages")) pages_node = ResolveObj(*p);
    if (pages_node == nullptr) ThrowCorrupt("no page tree");
    std::set<const Object*> seen;
    CollectPages(pages_node, PageNode{}, 0, &seen);
    if (pages.empty()) ThrowCorrupt("document has no pages");
  }
};

PdfFile::PdfFile() : impl_(std::make_unique<Impl>()) {}
PdfFile::~PdfFile() = default;
PdfFile::PdfFile(PdfFile&&) noexcept = default;
PdfFile& PdfFile::operator=(PdfFile&&) noexcept = default;

const std::string& PdfFile::raw_bytes() const noexcept { return impl_->data; }
ObjectPtr PdfFile::trailer() const noexcept { return impl_->trailer; }
size_t PdfFile::startxref_offset() const noexcept { return impl_->startxref; }

PdfFile PdfFile::OpenPath(const std::string& path, const PdfOpenOptions& opts) {
  FileStream in(path, "rb");
  const int64_t size = in.Size();
  if (size <= 0) throw Error(ErrorCode::Corrupt, "pdf: empty file", path);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  int64_t read = 0;
  while (read < size) {
    const int64_t got = in.Read(bytes.data() + read, size - read);
    if (got <= 0) break;
    read += got;
  }
  bytes.resize(static_cast<size_t>(read));
  return OpenMemory(std::move(bytes), opts);
}

PdfFile PdfFile::OpenMemory(std::vector<uint8_t> bytes, const PdfOpenOptions& opts) {
  if (opts.cancel != nullptr) opts.cancel->ThrowIfCancelled();
  if (bytes.size() < 8) throw Error(ErrorCode::Corrupt, "pdf: file too small");
  const std::string head(bytes.begin(), bytes.begin() + std::min<size_t>(5, bytes.size()));
  if (head != "%PDF-") throw Error(ErrorCode::Corrupt, "pdf: bad header");

  PdfFile file;
  Impl& impl = *file.impl_;
  impl.warnings = opts.warnings;
  impl.data.assign(bytes.begin(), bytes.end());
  // 版本号
  const size_t eol = impl.data.find_first_of("\r\n", 5);
  impl.version = impl.data.substr(5, (eol == std::string::npos ? 8 : eol - 5));
  while (!impl.version.empty() && impl.version.back() == ' ') impl.version.pop_back();

  impl.ParseXref();
  if (impl.trailer == nullptr) {
    throw Error(ErrorCode::Corrupt, "pdf: no trailer found");
  }
  if (const ObjectPtr* enc = impl.trailer->Find("Encrypt")) {
    impl.encrypted = true;
    // /Encrypt 可能是直接字典，也可能是间接引用（后者要记住对象号，不能解它）。
    ObjectPtr encrypt_dict;
    if ((*enc) != nullptr && (*enc)->kind == ObjKind::Ref) {
      impl.encrypt_obj_num = (*enc)->ref_num;
      encrypt_dict = impl.ResolveRef((*enc)->ref_num);
    } else {
      encrypt_dict = *enc;
    }
    // /ID 的第一项参与密钥派生。
    std::vector<uint8_t> id0;
    if (const ObjectPtr* id = impl.trailer->Find("ID")) {
      const ObjectPtr arr = impl.ResolveObj(*id);
      if (arr != nullptr && arr->kind == ObjKind::Array && !arr->array.empty()) {
        const ObjectPtr first = impl.ResolveObj(arr->array[0]);
        if (first != nullptr && first->kind == ObjKind::String) {
          id0.assign(first->text.begin(), first->text.end());
        }
      }
    }
    auto handler = std::make_unique<SecurityHandler>();
    // 空口令先试一次：很多 PDF 只设了所有者口令，用空用户口令就能打开。
    if (handler->Init(encrypt_dict, file, opts.password, id0)) {
      impl.security = std::move(handler);
      // 权限位：1 基的位号 3..12（Table 22）。位 7/8 是保留位，必须为 1。
      const int raw = impl.security->permissions();
      auto bit = [raw](int n) { return ((raw >> (n - 1)) & 1) != 0; };
      impl.perms.raw = raw;
      impl.perms.print = bit(3);
      impl.perms.modify = bit(4);
      impl.perms.copy = bit(5);
      impl.perms.annotate = bit(6);
      impl.perms.fill_forms = bit(9);
      impl.perms.extract_accessibility = bit(10);
      impl.perms.assemble = bit(11);
      impl.perms.print_high_quality = bit(12);
    } else if (opts.password.empty()) {
      throw Error(ErrorCode::NeedPassword, "pdf: document is encrypted");
    } else {
      throw Error(ErrorCode::NeedPassword, "pdf: wrong password");
    }
  }
  impl.BuildPages();
  return file;
}

int PdfFile::page_count() const noexcept {
  return impl_ == nullptr ? 0 : static_cast<int>(impl_->pages.size());
}

const PdfPage& PdfFile::page(int index) const {
  if (impl_ == nullptr || index < 0 || index >= page_count()) {
    throw Error(ErrorCode::InvalidArgument, "pdf: page index out of range");
  }
  return *impl_->pages[static_cast<size_t>(index)];
}

const PdfPage* PdfFile::page_at(int index) const {
  if (impl_ == nullptr || index < 0 || index >= page_count()) return nullptr;
  return impl_->pages[static_cast<size_t>(index)].get();
}

const std::string& PdfFile::version() const noexcept {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->version;
}

bool PdfFile::encrypted() const noexcept { return impl_ != nullptr && impl_->encrypted; }

const PdfPermissions& PdfFile::permissions() const noexcept {
  static const PdfPermissions kAll;  // 默认构造就是"全都可以"
  return impl_ == nullptr ? kAll : impl_->perms;
}

const std::string& PdfFile::title() const noexcept {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->title;
}

size_t PdfFile::object_count() const noexcept {
  return impl_ == nullptr ? 0 : impl_->offsets.size();
}

ObjectPtr PdfFile::Resolve(const ObjectPtr& obj) const {
  if (impl_ == nullptr) return obj;
  return impl_->ResolveObj(obj);
}

std::string PdfFile::ExtractText() const {
  std::string out;
  for (int i = 0; i < page_count(); ++i) {
    if (i != 0) out += '\f';
    out += page(i).ExtractText();
  }
  return out;
}

}  // namespace pdf
}  // namespace doccore
