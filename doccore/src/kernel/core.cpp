#include "doccore/core.h"

namespace doccore {

const char* ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::NeedPassword: return "NeedPassword";
    case ErrorCode::WrongPassword: return "WrongPassword";
    case ErrorCode::Corrupt: return "Corrupt";
    case ErrorCode::Io: return "Io";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::License: return "License";
    case ErrorCode::Formula: return "Formula";
  }
  return "Unknown";
}

// docs/01 §6.2: the single mapping used by the CLI. C ABI returns raw codes.
int ErrorCodeToExitCode(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return 0;
    case ErrorCode::InvalidArgument: return 2;
    case ErrorCode::Unsupported: return 3;
    case ErrorCode::NeedPassword: return 3;
    case ErrorCode::WrongPassword: return 3;
    case ErrorCode::Corrupt: return 3;
    case ErrorCode::LimitExceeded: return 5;
    case ErrorCode::Cancelled: return 6;
    case ErrorCode::License: return 4;
    case ErrorCode::Formula: return 1;
    case ErrorCode::Io: return 1;
    case ErrorCode::Unknown: return 1;
  }
  return 1;
}

const char* WarningCodeName(WarningCode code) noexcept {
  switch (code) {
    case WarningCode::FontSubstituted: return "FontSubstituted";
    case WarningCode::UnknownPartKept: return "UnknownPartKept";
    case WarningCode::MacroPreserved: return "MacroPreserved";
    case WarningCode::FieldNotStabilized: return "FieldNotStabilized";
    case WarningCode::ChartPlaceholder: return "ChartPlaceholder";
    case WarningCode::NumberFormatFallback: return "NumberFormatFallback";
    case WarningCode::DrawOpSkipped: return "DrawOpSkipped";
    case WarningCode::PathDegraded: return "PathDegraded";
    case WarningCode::ImageSkipped: return "ImageSkipped";
    case WarningCode::HeaderFooterUnsplit: return "HeaderFooterUnsplit";
    case WarningCode::NoTextLayer: return "NoTextLayer";
    case WarningCode::RebuildHeuristic: return "RebuildHeuristic";
    case WarningCode::ColumnWidthApproximated: return "ColumnWidthApproximated";
    case WarningCode::PermissionRestricted: return "PermissionRestricted";
    case WarningCode::ExternalEntityRejected: return "ExternalEntityRejected";
    case WarningCode::LimitApproaching: return "LimitApproaching";
    case WarningCode::LicenseClockRollback: return "LicenseClockRollback";
    case WarningCode::ValidationFailed: return "ValidationFailed";
    case WarningCode::RtfControlSkipped: return "RtfControlSkipped";
    case WarningCode::AnnotSkipped: return "AnnotSkipped";
  }
  return "UnknownWarning";
}

const char* FormatName(Format f) noexcept {
  switch (f) {
    case Format::Unknown: return "unknown";
    case Format::Docx: return "docx";
    case Format::Docm: return "docm";
    case Format::Dotx: return "dotx";
    case Format::Pdf: return "pdf";
    case Format::Html: return "html";
    case Format::Text: return "txt";
    case Format::Png: return "png";
    case Format::Xlsx: return "xlsx";
    case Format::Xlsm: return "xlsm";
    case Format::Csv: return "csv";
    case Format::Pptx: return "pptx";
    case Format::Eml: return "eml";
    case Format::Msg: return "msg";
  }
  return "unknown";
}

Format FormatFromName(std::string_view name) {
  if (name == "docx") return Format::Docx;
  if (name == "docm") return Format::Docm;
  if (name == "dotx") return Format::Dotx;
  if (name == "pdf") return Format::Pdf;
  if (name == "html") return Format::Html;
  if (name == "txt" || name == "text") return Format::Text;
  if (name == "png") return Format::Png;
  if (name == "xlsx") return Format::Xlsx;
  if (name == "xlsm") return Format::Xlsm;
  if (name == "csv") return Format::Csv;
  if (name == "pptx") return Format::Pptx;
  if (name == "eml") return Format::Eml;
  if (name == "msg") return Format::Msg;
  return Format::Unknown;
}

// ---- text helpers -------------------------------------------------------

std::u32string Utf8ToU32(std::string_view in) {
  std::u32string out;
  out.reserve(in.size());
  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    char32_t cp = 0;
    size_t len = 0;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + len > n) {
      out.push_back(0xFFFD);
      break;
    }
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(in[i + k]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += len;
  }
  return out;
}

std::string U32ToUtf8(std::u32string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char32_t cp : in) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
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
  return out;
}

std::u16string U32ToU16(std::u32string_view in) {
  std::u16string out;
  out.reserve(in.size());
  for (char32_t cp : in) {
    if (cp < 0x10000) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      const char32_t v = cp - 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
    }
  }
  return out;
}

std::string U16BeToUtf8(const uint8_t* data, size_t bytes) {
  std::u32string tmp;
  tmp.reserve(bytes / 2);
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    const uint32_t u = ReadBe16(data + i);
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes) {
      const uint32_t lo = ReadBe16(data + i + 2);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        tmp.push_back(static_cast<char32_t>(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)));
        i += 2;
        continue;
      }
    }
    tmp.push_back(static_cast<char32_t>(u));
  }
  return U32ToUtf8(tmp);
}

std::string Base64Encode(const uint8_t* data, size_t size) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back(kAlphabet[v & 0x3F]);
  }
  const size_t rest = size - i;
  if (rest == 1) {
    const uint32_t v = uint32_t(data[i]) << 16;
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out += "==";
  } else if (rest == 2) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::vector<uint8_t> Base64Decode(std::string_view text) {
  static int8_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) table[i] = -1;
    const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(k[i])] = static_cast<int8_t>(i);
    init = true;
  }
  std::vector<uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  int buf = 0, bits = 0;
  for (char c : text) {
    if (c == '=') break;
    const int8_t v = table[static_cast<unsigned char>(c)];
    if (v < 0) continue;  // 跳过空白与非法字符
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace doccore
