// DocCore - OLE2 / CFB（复合文档二进制）读取。Design refs: docs/02 §3.4 / F-501、F-111.
//
// 只做"打开容器、按名字读顶层流"。MSG 的属性流与 DOC 的 WordDocument/Table
// 流都走这里；写入侧不在本模块（MSG 夹具由 Python 脚本生成）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace doccore {
namespace ole {

// 目录项类型（[MS-CFB] 2.6.1）。
enum class EntryType : uint8_t {
  Unallocated = 0,
  Storage = 1,
  Stream = 2,
  Root = 5,
};

struct Entry {
  std::u16string name;  // UTF-16LE 名字（不含结尾 NUL）
  EntryType type = EntryType::Unallocated;
  uint32_t start = 0;   // 首扇区（流）或 mini 流首扇区
  uint64_t size = 0;    // 字节数
  bool IsStream() const { return type == EntryType::Stream; }
  bool IsStorage() const { return type == EntryType::Storage; }
  bool IsRoot() const { return type == EntryType::Root; }
};

class File {
 public:
  File() = default;
  explicit File(std::vector<uint8_t> data) : data_(std::move(data)) {}

  // 解析容器结构（FAT/DIFAT/MiniFAT/目录/mini stream）。失败返回 false，
  // 失败原因可从 message() 取（仅用于诊断，不参与错误码映射）。
  bool Parse();

  const std::vector<Entry>& entries() const { return dir_; }
  const std::string& message() const { return message_; }

  // 按 UTF-16 名字读顶层流；找不到返回空 vector。
  std::vector<uint8_t> ReadTopStream(const std::u16string& name) const;
  // 按 UTF-8 名字读顶层流（便利重载）。
  std::vector<uint8_t> ReadTopStream(const std::string& name) const;

  // 是否存在该顶层流。
  bool HasTopStream(const std::string& name) const;

  // 目录项索引（未找到返回 -1）。
  int IndexOf(const std::u16string& name) const;

 private:
  size_t SectorOffset(uint32_t sid) const {
    return 512 + static_cast<size_t>(sid) * sector_size_;
  }
  std::vector<uint8_t> ReadChain(uint32_t start, uint64_t size, bool mini) const;

  std::vector<uint8_t> data_;
  uint32_t sector_size_ = 512;
  uint32_t mini_size_ = 64;
  uint32_t mini_cutoff_ = 4096;
  uint32_t first_dir_ = 0;
  uint32_t first_minifat_ = 0;
  std::vector<uint32_t> fat_;
  std::vector<uint32_t> minifat_;
  std::vector<uint8_t> mini_stream_;
  std::vector<Entry> dir_;
  std::string message_;
};

// 是否是 OLE2/CFB 签名（D0 CF 11 E0 A1 B1 1A E1）。
bool LooksLikeOle(const uint8_t* data, size_t size);

}  // namespace ole
}  // namespace doccore
