// DocCore - 邮件 DOM 与读写入口（中性命名 mail）。Design refs: docs/02 §3.4 / F-501。
//
// 只读侧：解析 EML（MIME，RFC 2045-2049/5322）与 MSG（OLE2/CFB 复合文件），
// 抽出主题、收发地址、纯文本/HTML 正文与附件原始字节。不连接 SMTP/IMAP，
// 打开 HTML 正文不执行脚本、不请求远程图片（业务规则，docs/01 F-501）。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace mail {

struct MailAddress {
  std::string name;    // 显示名（RFC 2047 解码后；可能为空）
  std::string email;   // 地址
};

// 附件：name/mime 来自邮件头声明；bytes 始终是原始字节（不按声明类型解释）；
// detected 是按内容魔数探测出的实际类型——扩展名与内容不符时给出真实类型。
struct MailAttachment {
  std::string name;
  std::string mime;
  std::string detected;
  std::vector<uint8_t> bytes;
};

enum class MailBodyKind { Text, Html, None };

struct MailMessage {
  std::string subject;                 // RFC 2047 解码后的主题
  std::vector<MailAddress> from;
  std::vector<MailAddress> to, cc, bcc;
  std::string date;                    // 原始 Date 头
  std::string message_id;
  std::string text;                    // text/plain 正文（UTF-8）
  std::string html;                    // text/html 正文（UTF-8）
  MailBodyKind body_kind = MailBodyKind::None;  // 首选正文类型
  std::vector<MailAttachment> attachments;
  std::vector<std::pair<std::string, std::string>> headers;  // 全部原始头（小写名, 原值）
  bool from_msg = false;               // true = MSG(OLE2)，false = EML(MIME)
};

// 容器探测：EML(MIME) 或 MSG(OLE2/CFB)。
enum class MailFormat { Eml, Msg, Unknown };
MailFormat DetectFormat(std::string_view bytes);

struct OpenOptions {
  std::string path;  // 文件路径（与 memory 二选一）
  CancelToken* cancel = nullptr;
  WarningSink* warnings = nullptr;
};

// 打开邮件：按 DetectFormat 识别 EML/MSG。抛 Error(Io/Corrupt/Unsupported)。
MailMessage Open(const OpenOptions& opts);
MailMessage OpenMemory(std::string_view bytes, WarningSink* warnings = nullptr,
                       CancelToken* cancel = nullptr);

// MSG(OLE2/CFB) 解析入口（由 mail.cpp 识别到 MSG 后调用；也可单独使用）。
MailMessage OpenMsgMemory(const std::vector<uint8_t>& data, WarningSink* warnings = nullptr);

// 按魔数探测实际 MIME 类型（识别不了返回 "application/octet-stream"）。
std::string DetectMime(const std::vector<uint8_t>& bytes);

// 把给定字符集的字节解码为 UTF-8（供独立校验/复用）。未知字符集按 Latin-1。
std::string DecodeCharset(std::string_view bytes, std::string_view charset);

// Quoted-Printable 解码（供独立校验/复用）。base64 在 doccore/core.h 里。
std::string QuotedPrintableDecode(std::string_view text);

}  // namespace mail
}  // namespace doccore
