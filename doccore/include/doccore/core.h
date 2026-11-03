// DocCore - core types: error codes, warnings, cancellation, text encoding.
// Design refs: docs/02 §3.1.1
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace doccore {

// Error codes 0..11. Must stay in sync with docs/01 §6.2 mapping table.
enum class ErrorCode : int {
  Ok = 0,
  Unknown = 1,
  InvalidArgument = 2,
  Unsupported = 3,
  NeedPassword = 4,
  WrongPassword = 5,
  Corrupt = 6,
  Io = 7,
  LimitExceeded = 8,
  Cancelled = 9,
  License = 10,
  Formula = 11,
};

const char* ErrorCodeName(ErrorCode code) noexcept;
int ErrorCodeToExitCode(ErrorCode code) noexcept;

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, std::string message, std::string path = {})
      : std::runtime_error(std::move(message)), code_(code), path_(std::move(path)) {}

  ErrorCode code() const noexcept { return code_; }
  const std::string& path() const noexcept { return path_; }

 private:
  ErrorCode code_;
  std::string path_;
};

// Warning codes 1001..1020, never reused once published.
enum class WarningCode : int {
  FontSubstituted = 1001,
  UnknownPartKept = 1002,
  MacroPreserved = 1003,
  FieldNotStabilized = 1004,
  ChartPlaceholder = 1005,
  NumberFormatFallback = 1006,
  DrawOpSkipped = 1007,
  PathDegraded = 1008,
  ImageSkipped = 1009,
  HeaderFooterUnsplit = 1010,
  NoTextLayer = 1011,
  RebuildHeuristic = 1012,
  ColumnWidthApproximated = 1013,
  PermissionRestricted = 1014,
  ExternalEntityRejected = 1015,
  LimitApproaching = 1016,
  LicenseClockRollback = 1017,
  ValidationFailed = 1018,  // F-205 数据验证未通过（列表外的值被写入或拒绝）
  RtfControlSkipped = 1019,  // F-111 RTF 控制字未识别，已跳过（已识别文本保留）
  AnnotSkipped = 1020,       // PDF 注释无法写入（页号越界、无对象号等），已跳过
};

const char* WarningCodeName(WarningCode code) noexcept;

struct Warning {
  int code = 0;
  std::string module;
  std::string path;
  std::string message;
};

class WarningSink {
 public:
  virtual ~WarningSink() = default;
  virtual void Add(const Warning& w) = 0;
};

class WarningList : public WarningSink {
 public:
  void Add(const Warning& w) override { items_.push_back(w); }
  const std::vector<Warning>& items() const noexcept { return items_; }
  bool empty() const noexcept { return items_.empty(); }
  void Clear() { items_.clear(); }

 private:
  std::vector<Warning> items_;
};

class CancelToken {
 public:
  void RequestCancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
  bool IsCancelled() const noexcept { return flag_.load(std::memory_order_relaxed); }
  void ThrowIfCancelled() const {
    if (IsCancelled()) throw Error(ErrorCode::Cancelled, "operation cancelled");
  }

 private:
  std::atomic<bool> flag_{false};
};

enum class Format {
  Unknown = 0,
  Docx,
  Docm,
  Dotx,
  Pdf,
  Html,
  Text,
  Png,
  Xlsx,
  Xlsm,
  Csv,
  Pptx,
  Eml,
  Msg,
};

const char* FormatName(Format f) noexcept;
Format FormatFromName(std::string_view name);

// ---- text helpers -------------------------------------------------------

// Decodes UTF-8, replacing malformed sequences with U+FFFD.
std::u32string Utf8ToU32(std::string_view in);
std::string U32ToUtf8(std::u32string_view in);
std::u16string U32ToU16(std::u32string_view in);
std::string U16BeToUtf8(const uint8_t* data, size_t bytes);

// ---- base64 -------------------------------------------------------------
//
// 标准字母表 + `=` 补位，换行不插入。解码容忍任意空白与非法字符（邮件与
// OOXML 里都见过带换行的 base64），遇 `=` 即停。
std::string Base64Encode(const uint8_t* data, size_t size);
std::vector<uint8_t> Base64Decode(std::string_view text);

inline uint32_t ReadBe16(const uint8_t* p) {
  return (uint32_t(p[0]) << 8) | uint32_t(p[1]);
}
inline uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t ReadLe16(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
}
inline uint32_t ReadLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace doccore
