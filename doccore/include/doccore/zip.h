// DocCore - ZIP container reader with the limits and path defense of
// docs/02 §3.1.3 (ZIP64, entry name sanitizing, ratio/size budgets).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/stream.h"

namespace doccore {

struct ZipEntry {
  std::string name;
  uint16_t method = 0;      // 0 stored, 8 deflate
  uint32_t crc = 0;
  uint64_t compressed = 0;
  uint64_t uncompressed = 0;
  uint64_t local_offset = 0;
};

struct ZipLimits {
  uint64_t max_total_uncompressed = 2ull << 30;  // docs/01 F-010 default: 2 GB
  uint64_t max_entries = 100000;
  uint32_t max_ratio = 100;
  uint64_t max_part_bytes = 256ull << 20;        // single part ceiling for in-memory use
};

// Rejects absolute paths, drive letters, backslash mixing and ".." segments.
bool IsSafePartName(std::string_view name, std::string* reason = nullptr);

class ZipArchive {
 public:
  static ZipArchive Open(Stream& in, const ZipLimits& limits = {}, WarningSink* warnings = nullptr);

  const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
  const ZipEntry* Find(std::string_view name) const;
  bool Has(std::string_view name) const { return Find(name) != nullptr; }

  // Decompresses one entry. Enforces per-entry and cumulative budgets.
  std::vector<uint8_t> Read(std::string_view name);
  std::vector<uint8_t> Read(const ZipEntry& entry);

  bool used_zip64() const noexcept { return zip64_; }

 private:
  Stream* in_ = nullptr;
  ZipLimits limits_;
  WarningSink* warnings_ = nullptr;
  std::vector<ZipEntry> entries_;
  uint64_t total_uncompressed_ = 0;
  bool zip64_ = false;
};

}  // namespace doccore
