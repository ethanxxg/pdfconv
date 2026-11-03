// DocCore - 邮件 DOM 与 MIME/OLE2 解析。Design refs: docs/02 §3.4 / F-501.
#include "doccore/mail.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

#include "doccore/stream.h"

namespace doccore {
namespace mail {

namespace {

using doccore::Error;
using doccore::ErrorCode;

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string QuotedPrintableDecode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c != '=') {
      out.push_back(c);
      continue;
    }
    // 软换行：=\r\n 或 =\n
    if (i + 2 < in.size() && in[i + 1] == '\r' && in[i + 2] == '\n') { i += 2; continue; }
    if (i + 1 < in.size() && in[i + 1] == '\n') { i += 1; continue; }
    const int h1 = (i + 1 < in.size()) ? HexVal(in[i + 1]) : -1;
    const int h2 = (i + 2 < in.size()) ? HexVal(in[i + 2]) : -1;
    if (h1 >= 0 && h2 >= 0) {
      out.push_back(static_cast<char>((h1 << 4) | h2));
      i += 2;
    } else {
      out.push_back(c);  // 孤立 '=' 原样保留
    }
  }
  return out;
}

namespace {

// Windows-1252 高字节 0x80-0x9F 的特殊映射（其余 0xA0-0xFF 同 Latin-1）。
const uint16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

void DecodeSingleByte(std::string_view bytes, bool cp1252, std::string& out) {
  out.reserve(out.size() + bytes.size());
  for (char ch : bytes) {
    const uint8_t b = static_cast<uint8_t>(ch);
    if (b < 0x80) {
      out.push_back(static_cast<char>(b));
    } else if (cp1252 && b <= 0x9F) {
      AppendUtf8(out, kCp1252[b - 0x80]);
    } else {
      AppendUtf8(out, b);
    }
  }
}

void DecodeUtf16(std::string_view bytes, bool little, std::string& out) {
  size_t i = 0;
  // 处理 BOM
  if (bytes.size() >= 2) {
    const uint8_t b0 = static_cast<uint8_t>(bytes[0]);
    const uint8_t b1 = static_cast<uint8_t>(bytes[1]);
    if (b0 == 0xFF && b1 == 0xFE) { little = true; i = 2; }
    else if (b0 == 0xFE && b1 == 0xFF) { little = false; i = 2; }
  }
  for (; i + 1 < bytes.size(); i += 2) {
    const uint8_t lo = little ? static_cast<uint8_t>(bytes[i]) : static_cast<uint8_t>(bytes[i + 1]);
    const uint8_t hi = little ? static_cast<uint8_t>(bytes[i + 1]) : static_cast<uint8_t>(bytes[i]);
    uint32_t cp = (static_cast<uint32_t>(hi) << 8) | lo;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size()) {
      const uint8_t l2 = little ? static_cast<uint8_t>(bytes[i + 2]) : static_cast<uint8_t>(bytes[i + 3]);
      const uint8_t h2 = little ? static_cast<uint8_t>(bytes[i + 3]) : static_cast<uint8_t>(bytes[i + 2]);
      const uint32_t cp2 = (static_cast<uint32_t>(h2) << 8) | l2;
      if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
        i += 2;
      }
    }
    AppendUtf8(out, cp);
  }
}

}  // namespace

std::string DecodeCharset(std::string_view bytes, std::string_view charset) {
  // 取 charset 的第一个 token（去掉 ";..." 参数与引号）。
  std::string cs = ToLower(Trim(charset));
  const size_t sc = cs.find(';');
  if (sc != std::string::npos) cs = Trim(cs.substr(0, sc));
  if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"') cs = cs.substr(1, cs.size() - 2);

  std::string out;
  if (cs.find("utf-8") != std::string::npos || cs.find("utf8") != std::string::npos ||
      cs.empty() || cs == "us-ascii" || cs == "ascii" || cs == "default" || cs == "none" ||
      cs == "7bit" || cs == "8bit") {
    // UTF-8 原样；ASCII/未声明按 Latin-1（ASCII 子集不变）。
    if (cs.empty() || cs == "us-ascii" || cs == "ascii" || cs == "default" || cs == "none" ||
        cs == "7bit" || cs == "8bit") {
      DecodeSingleByte(bytes, /*cp1252=*/false, out);
    } else {
      out.assign(bytes.data(), bytes.size());
    }
    return out;
  }
  if (cs.find("utf-16le") != std::string::npos || cs == "utf-16" || cs == "ucs-2le") {
    DecodeUtf16(bytes, /*little=*/true, out);
    return out;
  }
  if (cs.find("utf-16be") != std::string::npos || cs == "ucs-2" || cs == "unicodefffe") {
    DecodeUtf16(bytes, /*little=*/false, out);
    return out;
  }
  if (cs.find("1252") != std::string::npos) {
    DecodeSingleByte(bytes, /*cp1252=*/true, out);
    return out;
  }
  // iso-8859-1 / latin-1 / 其余未知单字节 → Latin-1；多字节 CJK 目前退回 Latin-1（已知缺口）。
  DecodeSingleByte(bytes, /*cp1252=*/false, out);
  return out;
}

// ---- 容器/类型探测 ------------------------------------------------------

MailFormat DetectFormat(std::string_view bytes) {
  // OLE2/CFB 复合文件魔数（MSG 用的就是它）。
  static const uint8_t kOle[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  if (bytes.size() >= 8 && std::memcmp(bytes.data(), kOle, 8) == 0) return MailFormat::Msg;
  // MIME：至少含一个 "Name:" 头（冒号前是 token 且行首不是空白）。
  size_t i = 0;
  int lines = 0;
  while (i < bytes.size() && lines < 64) {
    size_t eol = bytes.find('\n', i);
    const size_t end = (eol == std::string_view::npos) ? bytes.size() : eol;
    const std::string_view line = bytes.substr(i, end - i);
    if (line.empty() || (line.size() == 1 && line[0] == '\r')) break;  // 空行=头结束
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos && colon > 0) {
      bool token = true;
      for (size_t k = 0; k < colon; ++k) {
        const char c = line[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) { token = false; break; }
      }
      if (token) return MailFormat::Eml;
    }
    if (eol == std::string_view::npos) break;
    i = eol + 1;
    ++lines;
  }
  return MailFormat::Unknown;
}

std::string DetectMime(const std::vector<uint8_t>& b) {
  auto sig = [&](const char* s, size_t n) {
    return b.size() >= n && std::memcmp(b.data(), s, n) == 0;
  };
  if (sig("\x89PNG\r\n\x1a\n", 8)) return "image/png";
  if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
  if (sig("GIF87a", 6) || sig("GIF89a", 6)) return "image/gif";
  if (sig("%PDF-", 5)) return "application/pdf";
  if (sig("PK\x03\x04", 4) || sig("PK\x05\x06", 4) || sig("PK\x07\x08", 4)) return "application/zip";
  static const uint8_t kOle[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  if (b.size() >= 8 && std::memcmp(b.data(), kOle, 8) == 0) return "application/x-ole-storage";
  if (b.size() >= 2 && b[0] == 0x1F && b[1] == 0x8B) return "application/gzip";
  if (sig("{\\rtf", 5)) return "text/rtf";
  if (sig("BM", 2)) return "image/bmp";
  return "application/octet-stream";
}

// FRAGMENT2_MARKER

namespace {

struct Headers {
  std::vector<std::pair<std::string, std::string>> items;  // (小写名, 原值)
  std::string body;                                        // 头之后的原始字节
};

// 在第一个空行处切头/体；折行（以空格/制表符起始）并入上一条头。
Headers SplitMessage(std::string_view raw) {
  Headers h;
  size_t hdr_end = std::string_view::npos;
  size_t body_start = std::string_view::npos;
  for (size_t i = 0; i + 1 < raw.size(); ++i) {
    if (raw[i] != '\n') continue;
    size_t j = i + 1;
    if (j < raw.size() && raw[j] == '\r') ++j;
    if (j < raw.size() && raw[j] == '\n') {
      hdr_end = i;
      body_start = j + 1;
      break;
    }
  }
  const std::string_view hdr = (hdr_end == std::string_view::npos) ? raw : raw.substr(0, hdr_end);
  if (body_start != std::string_view::npos) h.body.assign(raw.substr(body_start));

  std::string cur_name, cur_val;
  bool have = false;
  size_t i = 0;
  while (i < hdr.size()) {
    const size_t eol = hdr.find('\n', i);
    const size_t end = (eol == std::string_view::npos) ? hdr.size() : eol;
    std::string line(hdr.substr(i, end - i));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    i = (eol == std::string_view::npos) ? hdr.size() : eol + 1;
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      if (have) { cur_val += ' '; cur_val += Trim(line); }
      continue;
    }
    if (have) h.items.emplace_back(cur_name, Trim(cur_val));
    const size_t colon = line.find(':');
    if (colon == std::string::npos) { have = false; continue; }
    cur_name = ToLower(Trim(line.substr(0, colon)));
    cur_val = Trim(line.substr(colon + 1));
    have = true;
  }
  if (have) h.items.emplace_back(cur_name, Trim(cur_val));
  return h;
}

std::string GetHeader(const Headers& h, const char* name) {
  for (const auto& kv : h.items) if (kv.first == name) return kv.second;
  return "";
}

// 从 "text/plain; charset=\"utf-8\"; name=\"a.txt\"" 取指定参数（大小写不敏感）。
std::string GetParam(const std::string& header_value, const char* param) {
  const std::string p = ToLower(std::string(param));
  const size_t first_semi = header_value.find(';');
  if (first_semi == std::string::npos) return "";
  size_t start = first_semi + 1;
  while (start <= header_value.size()) {
    const size_t semi = header_value.find(';', start);
    const std::string seg = header_value.substr(
        start, (semi == std::string::npos ? header_value.size() : semi) - start);
    const size_t eq = seg.find('=');
    if (eq != std::string::npos) {
      const std::string key = ToLower(Trim(seg.substr(0, eq)));
      if (key == p) {
        std::string val = Trim(seg.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
          val = val.substr(1, val.size() - 2);
        return val;
      }
    }
    if (semi == std::string::npos) break;
    start = semi + 1;
  }
  return "";
}

// RFC 2047 encoded-word：=?charset?B/Q?text?= ，其余文本原样。
std::string DecodeEncodedWords(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '=' && i + 1 < s.size() && s[i + 1] == '?') {
      const size_t q1 = s.find('?', i + 2);
      if (q1 == std::string::npos) { out.push_back(s[i++]); continue; }
      const size_t q2 = s.find('?', q1 + 1);
      if (q2 == std::string::npos) { out.push_back(s[i++]); continue; }
      const size_t q3 = s.find("?=", q2 + 1);
      if (q3 == std::string::npos) { out.push_back(s[i++]); continue; }
      const std::string charset = s.substr(i + 2, q1 - (i + 2));
      const char enc = s[q1 + 1];
      const std::string text = s.substr(q2 + 1, q3 - (q2 + 1));
      if (enc == 'B' || enc == 'b') {
        const std::vector<uint8_t> bytes = Base64Decode(text);
        out += DecodeCharset(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                              bytes.size()), charset);
      } else if (enc == 'Q' || enc == 'q') {
        std::string qp;
        for (char c : text) qp.push_back(c == '_' ? ' ' : c);
        out += DecodeCharset(QuotedPrintableDecode(qp), charset);
      } else {
        out.append(s, i, (q3 + 2) - i);
      }
      i = q3 + 2;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;  // 吸收相邻编码词间空白
    } else {
      out.push_back(s[i++]);
    }
  }
  return out;
}

std::vector<MailAddress> ParseAddressList(const std::string& value) {
  std::vector<MailAddress> out;
  bool inq = false, inb = false;
  std::string cur;
  auto flush = [&]() {
    const std::string t = Trim(cur);
    cur.clear();
    if (t.empty()) return;
    MailAddress a;
    const size_t lt = t.rfind('<');
    const size_t gt = (lt == std::string::npos) ? std::string::npos : t.find('>', lt);
    if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
      std::string name = Trim(t.substr(0, lt));
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);  // 去掉显示名外层引号
      a.name = DecodeEncodedWords(name);
      a.email = Trim(t.substr(lt + 1, gt - (lt + 1)));
    } else {
      a.email = t;
    }
    if (!a.email.empty() || !a.name.empty()) out.push_back(a);
  };
  for (char c : value) {
    if (c == '"') inq = !inq;
    if (!inq) {
      if (c == '<') inb = true;
      else if (c == '>') inb = false;
      else if (c == ',' && !inb) { flush(); continue; }
    }
    cur.push_back(c);
  }
  flush();
  return out;
}

void ParsePart(std::string_view raw, MailMessage& msg, WarningSink* warnings);

void ParseLeafOrMultipart(std::string_view raw, MailMessage& msg, WarningSink* warnings) {
  const Headers h = SplitMessage(raw);
  std::string ctype = GetHeader(h, "content-type");
  if (ctype.empty()) ctype = "text/plain; charset=us-ascii";
  const std::string cte = ToLower(GetHeader(h, "content-transfer-encoding"));
  const size_t sc = ctype.find(';');
  const std::string mtype = ToLower(Trim(ctype.substr(0, sc == std::string::npos ? ctype.size() : sc)));

  if (mtype.rfind("multipart/", 0) == 0) {
    const std::string boundary = GetParam(ctype, "boundary");
    if (boundary.empty()) return;
    const std::string delim = "--" + boundary;
    const std::string_view body = h.body;  // string_view，substr 返回视图而非临时 string
    const size_t first = body.find(delim);
    if (first == std::string::npos) return;
    size_t seg = first + delim.size();
    while (true) {
      const size_t next = body.find(delim, seg);
      const std::string_view part =
          body.substr(seg, (next == std::string::npos ? body.size() : next) - seg);
      size_t a = 0, b = part.size();
      while (a < b && (part[a] == '\r' || part[a] == '\n')) ++a;
      while (b > a && (part[b - 1] == '\r' || part[b - 1] == '\n')) --b;
      if (b > a) ParsePart(part.substr(a, b - a), msg, warnings);
      if (next == std::string::npos) break;
      seg = next + delim.size();
      if (seg + 1 < body.size() && body[seg] == '-' && body[seg + 1] == '-') break;  // 收尾 --
      if (seg >= body.size()) break;
    }
    return;
  }

  // 叶子：先按传输编码解码。
  std::vector<uint8_t> data;
  if (cte == "base64") {
    data = Base64Decode(h.body);
  } else if (cte == "quoted-printable") {
    const std::string d = QuotedPrintableDecode(h.body);
    data.assign(d.begin(), d.end());
  } else {
    data.assign(h.body.begin(), h.body.end());
  }
  std::string charset = GetParam(ctype, "charset");
  if (charset.empty()) charset = "us-ascii";
  const std::string_view dv(reinterpret_cast<const char*>(data.data()), data.size());

  const std::string cd = GetHeader(h, "content-disposition");
  const bool is_attachment =
      cd.find("attachment") != std::string::npos || !GetParam(cd, "filename").empty();
  if (!is_attachment && mtype == "text/plain") {
    if (msg.text.empty()) msg.text = DecodeCharset(dv, charset);
  } else if (!is_attachment && mtype == "text/html") {
    if (msg.html.empty()) msg.html = DecodeCharset(dv, charset);
  } else {
    MailAttachment att;
    att.name = GetParam(cd, "filename");
    if (att.name.empty()) att.name = GetParam(ctype, "name");
    att.mime = mtype.empty() ? "application/octet-stream" : mtype;
    att.detected = DetectMime(data);
    att.bytes = std::move(data);
    msg.attachments.push_back(std::move(att));
  }
}

void ParsePart(std::string_view raw, MailMessage& msg, WarningSink* warnings) {
  ParseLeafOrMultipart(raw, msg, warnings);
}

MailMessage ParseEml(std::string_view raw, WarningSink* warnings) {
  MailMessage msg;
  const Headers h = SplitMessage(raw);
  msg.headers = h.items;
  msg.subject = DecodeEncodedWords(GetHeader(h, "subject"));
  msg.from = ParseAddressList(GetHeader(h, "from"));
  msg.to = ParseAddressList(GetHeader(h, "to"));
  msg.cc = ParseAddressList(GetHeader(h, "cc"));
  msg.bcc = ParseAddressList(GetHeader(h, "bcc"));
  msg.date = GetHeader(h, "date");
  msg.message_id = GetHeader(h, "message-id");
  ParsePart(raw, msg, warnings);
  if (!msg.html.empty())
    msg.body_kind = MailBodyKind::Html;
  else if (!msg.text.empty())
    msg.body_kind = MailBodyKind::Text;
  else
    msg.body_kind = MailBodyKind::None;
  return msg;
}

}  // namespace

MailMessage OpenMemory(std::string_view bytes, WarningSink* warnings, CancelToken* cancel) {
  if (cancel != nullptr) cancel->ThrowIfCancelled();
  const MailFormat fmt = DetectFormat(bytes);
  if (fmt == MailFormat::Eml) return ParseEml(bytes, warnings);
  if (fmt == MailFormat::Msg) {
    const std::vector<uint8_t> data(bytes.begin(), bytes.end());
    return OpenMsgMemory(data, warnings);
  }
  throw Error(ErrorCode::Unsupported, "mail: unrecognized container (not EML/MSG)");
}

MailMessage Open(const OpenOptions& opts) {
  if (opts.path.empty()) throw Error(ErrorCode::InvalidArgument, "mail: empty path");
  FileStream in(opts.path, "rb");
  const std::vector<uint8_t> data = ReadAll(in, 256ll << 20);
  return OpenMemory(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()),
                    opts.warnings, opts.cancel);
}

}  // namespace mail
}  // namespace doccore
