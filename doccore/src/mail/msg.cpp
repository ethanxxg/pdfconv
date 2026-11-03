// DocCore - MSG(OLE2/CFB) 读取与属性抽取。Design refs: docs/02 §3.4 / F-501.
#include "doccore/mail.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doccore/ole.h"

namespace doccore {
namespace mail {
namespace {

using doccore::Error;
using doccore::ErrorCode;

std::string CharsetFromCodepage(uint32_t cp) {
  switch (cp) {
    case 65001: return "utf-8";
    case 1200: return "utf-16le";
    case 20127: case 1252: return "windows-1252";
    case 1250: return "windows-1250";
    case 1251: return "windows-1251";
    case 1253: return "windows-1253";
    case 1254: return "windows-1254";
    case 1255: return "windows-1255";
    case 1256: return "windows-1256";
    case 28591: return "iso-8859-1";
    case 28605: return "iso-8859-15";
    default: return "windows-1252";  // 其余（含 CJK 多字节）暂按 1252，已知缺口
  }
}

MailMessage ParseMsg(const std::vector<uint8_t>& data, WarningSink* warnings) {
  (void)warnings;
  doccore::ole::File ole(data);
  if (!ole.Parse()) throw Error(ErrorCode::Corrupt, "mail: malformed OLE2/CFB container");

  MailMessage msg;
  msg.from_msg = true;

  // 收集所有属性流。
  std::vector<uint8_t> subject_u, subject_s, cp_bytes, sender_name, sender_email, body_u, body_s;
  // ole::File 只暴露按名读取，这里按属性 id/类型拼流名。
  auto top = [&](uint32_t id, uint16_t type) {
    char nm[32];
    std::snprintf(nm, sizeof(nm), "__substg1.0_%04X%04X", id, type);
    std::u16string u;
    for (const char* p = nm; *p; ++p) u.push_back(static_cast<char16_t>(*p));
    return ole.ReadTopStream(u);
  };

  cp_bytes = top(0x3FFD, 0x0003);
  uint32_t codepage = 1252;
  if (cp_bytes.size() >= 4) {
    codepage = static_cast<uint32_t>(cp_bytes[0]) |
               (static_cast<uint32_t>(cp_bytes[1]) << 8) |
               (static_cast<uint32_t>(cp_bytes[2]) << 16) |
               (static_cast<uint32_t>(cp_bytes[3]) << 24);
  }
  const std::string cs8 = CharsetFromCodepage(codepage);

  subject_u = top(0x0037, 0x001F);
  subject_s = top(0x0037, 0x001E);
  if (!subject_u.empty())
    msg.subject = DecodeCharset(std::string_view(reinterpret_cast<const char*>(subject_u.data()),
                                                 subject_u.size()), "utf-16le");
  else if (!subject_s.empty())
    msg.subject = DecodeCharset(std::string_view(reinterpret_cast<const char*>(subject_s.data()),
                                                 subject_s.size()), cs8);

  sender_name = top(0x0C1A, 0x001F);
  sender_email = top(0x0065, 0x001F);
  if (!sender_name.empty() || !sender_email.empty()) {
    MailAddress a;
    if (!sender_name.empty())
      a.name = DecodeCharset(std::string_view(reinterpret_cast<const char*>(sender_name.data()),
                                              sender_name.size()), "utf-16le");
    if (!sender_email.empty())
      a.email = DecodeCharset(std::string_view(reinterpret_cast<const char*>(sender_email.data()),
                                               sender_email.size()), "utf-16le");
    msg.from.push_back(std::move(a));
  }

  body_u = top(0x1000, 0x001F);
  body_s = top(0x1000, 0x001E);
  if (!body_u.empty())
    msg.text = DecodeCharset(std::string_view(reinterpret_cast<const char*>(body_u.data()),
                                              body_u.size()), "utf-16le");
  else if (!body_s.empty())
    msg.text = DecodeCharset(std::string_view(reinterpret_cast<const char*>(body_s.data()),
                                              body_s.size()), cs8);
  msg.body_kind = msg.text.empty() ? MailBodyKind::None : MailBodyKind::Text;
  return msg;
}

}  // namespace

// 供 mail.cpp 的 OpenMemory 在识别到 MSG 时调用。
MailMessage OpenMsgMemory(const std::vector<uint8_t>& data, WarningSink* warnings) {
  return ParseMsg(data, warnings);
}

}  // namespace mail
}  // namespace doccore
