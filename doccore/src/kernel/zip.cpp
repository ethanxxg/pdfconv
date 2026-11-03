#include "doccore/zip.h"

#include <cstring>

#include "doccore/deflate.h"

namespace doccore {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kEocd64LocatorSig = 0x07064b50;
constexpr uint32_t kEocd64Sig = 0x06064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;

uint64_t ReadLe64(const uint8_t* p) {
  return uint64_t(ReadLe32(p)) | (uint64_t(ReadLe32(p + 4)) << 32);
}

void ReadExact(Stream& in, uint8_t* dst, int64_t bytes, const char* what) {
  const int64_t got = in.Read(dst, bytes);
  if (got != bytes) throw Error(ErrorCode::Corrupt, std::string("zip: truncated ") + what);
}

}  // namespace

bool IsSafePartName(std::string_view name, std::string* reason) {
  auto fail = [&](const char* why) {
    if (reason != nullptr) *reason = why;
    return false;
  };
  if (name.empty()) return fail("empty name");
  if (name.find('\\') != std::string_view::npos) return fail("backslash in name");
  if (name[0] == '/' || name[0] == '\\') return fail("absolute path");
  if (name.size() >= 2 && name[1] == ':') return fail("drive letter");
  if (name.find('\0') != std::string_view::npos) return fail("nul byte in name");
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string_view::npos) end = name.size();
    const std::string_view seg = name.substr(start, end - start);
    if (seg == "..") return fail("parent segment");
    if (end == name.size()) break;
    start = end + 1;
  }
  return true;
}

ZipArchive ZipArchive::Open(Stream& in, const ZipLimits& limits, WarningSink* warnings) {
  ZipArchive z;
  z.in_ = &in;
  z.limits_ = limits;
  z.warnings_ = warnings;

  const int64_t size = in.Size();
  if (size < 22) throw Error(ErrorCode::Corrupt, "zip: file too small");

  // Locate EOCD by scanning back over the comment field (max 64 KB + 22).
  const int64_t scan = size < 65557 ? size : 65557;
  std::vector<uint8_t> tail(static_cast<size_t>(scan));
  in.Seek(size - scan);
  ReadExact(in, tail.data(), scan, "end of file");

  int64_t eocd_pos = -1;
  for (int64_t i = scan - 22; i >= 0; --i) {
    if (ReadLe32(tail.data() + i) == kEocdSig) {
      eocd_pos = i;
      break;
    }
  }
  if (eocd_pos < 0) throw Error(ErrorCode::Corrupt, "zip: end of central directory not found");

  const uint8_t* e = tail.data() + eocd_pos;
  uint64_t entry_count = ReadLe16(e + 10);
  uint64_t cd_size = ReadLe32(e + 12);
  uint64_t cd_offset = ReadLe32(e + 16);

  if (entry_count == 0xFFFF || cd_size == 0xFFFFFFFFull || cd_offset == 0xFFFFFFFFull) {
    // ZIP64: the locator sits immediately before the EOCD.
    const int64_t locator_abs = size - scan + eocd_pos - 20;
    if (locator_abs < 0) throw Error(ErrorCode::Corrupt, "zip: zip64 locator missing");
    uint8_t loc[20];
    in.Seek(locator_abs);
    ReadExact(in, loc, 20, "zip64 locator");
    if (ReadLe32(loc) != kEocd64LocatorSig) throw Error(ErrorCode::Corrupt, "zip: bad zip64 locator");
    const uint64_t eocd64_off = ReadLe64(loc + 8);
    uint8_t e64[56];
    in.Seek(static_cast<int64_t>(eocd64_off));
    ReadExact(in, e64, 56, "zip64 eocd");
    if (ReadLe32(e64) != kEocd64Sig) throw Error(ErrorCode::Corrupt, "zip: bad zip64 eocd");
    entry_count = ReadLe64(e64 + 32);
    cd_size = ReadLe64(e64 + 40);
    cd_offset = ReadLe64(e64 + 48);
    z.zip64_ = true;
  }

  if (entry_count > limits.max_entries) {
    throw Error(ErrorCode::LimitExceeded, "zip: too many entries");
  }
  if (cd_size > (64ull << 20)) throw Error(ErrorCode::LimitExceeded, "zip: central directory too large");

  std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
  in.Seek(static_cast<int64_t>(cd_offset));
  ReadExact(in, cd.data(), static_cast<int64_t>(cd_size), "central directory");

  size_t p = 0;
  z.entries_.reserve(static_cast<size_t>(entry_count));
  for (uint64_t i = 0; i < entry_count; ++i) {
    if (p + 46 > cd.size()) throw Error(ErrorCode::Corrupt, "zip: truncated central entry");
    if (ReadLe32(cd.data() + p) != kCentralSig) throw Error(ErrorCode::Corrupt, "zip: bad central signature");
    ZipEntry ent;
    ent.method = static_cast<uint16_t>(ReadLe16(cd.data() + p + 10));
    ent.crc = ReadLe32(cd.data() + p + 16);
    ent.compressed = ReadLe32(cd.data() + p + 20);
    ent.uncompressed = ReadLe32(cd.data() + p + 24);
    const uint32_t name_len = ReadLe16(cd.data() + p + 28);
    const uint32_t extra_len = ReadLe16(cd.data() + p + 30);
    const uint32_t comment_len = ReadLe16(cd.data() + p + 32);
    ent.local_offset = ReadLe32(cd.data() + p + 42);
    if (p + 46 + name_len + extra_len + comment_len > cd.size()) {
      throw Error(ErrorCode::Corrupt, "zip: central entry overflow");
    }
    ent.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), name_len);

    // ZIP64 extended information (0x0001) carries the fields that overflowed.
    size_t ep = p + 46 + name_len;
    const size_t ep_end = ep + extra_len;
    while (ep + 4 <= ep_end) {
      const uint16_t tag = static_cast<uint16_t>(ReadLe16(cd.data() + ep));
      const uint16_t len = static_cast<uint16_t>(ReadLe16(cd.data() + ep + 2));
      if (ep + 4 + len > ep_end) break;
      if (tag == 0x0001) {
        size_t fp = ep + 4;
        if (ent.uncompressed == 0xFFFFFFFFull && fp + 8 <= ep + 4 + len) {
          ent.uncompressed = ReadLe64(cd.data() + fp);
          fp += 8;
        }
        if (ent.compressed == 0xFFFFFFFFull && fp + 8 <= ep + 4 + len) {
          ent.compressed = ReadLe64(cd.data() + fp);
          fp += 8;
        }
        if (ent.local_offset == 0xFFFFFFFFull && fp + 8 <= ep + 4 + len) {
          ent.local_offset = ReadLe64(cd.data() + fp);
        }
      }
      ep += 4 + len;
    }

    std::string reason;
    if (!IsSafePartName(ent.name, &reason)) {
      throw Error(ErrorCode::Corrupt, "zip: unsafe entry name (" + reason + ")", ent.name);
    }
    z.entries_.push_back(std::move(ent));
    p += 46 + name_len + extra_len + comment_len;
  }
  return z;
}

const ZipEntry* ZipArchive::Find(std::string_view name) const {
  // Later entries win: a duplicate name overrides the earlier one (docs/02 §3.1.3).
  const ZipEntry* found = nullptr;
  for (const auto& e : entries_) {
    if (e.name == name) found = &e;
  }
  return found;
}

std::vector<uint8_t> ZipArchive::Read(std::string_view name) {
  const ZipEntry* ent = Find(name);
  if (ent == nullptr) throw Error(ErrorCode::Corrupt, "zip: entry not found", std::string(name));
  return Read(*ent);
}

std::vector<uint8_t> ZipArchive::Read(const ZipEntry& entry) {
  if (entry.method != 0 && entry.method != 8) {
    throw Error(ErrorCode::Unsupported, "zip: unsupported compression method", entry.name);
  }
  if (entry.uncompressed > limits_.max_part_bytes) {
    throw Error(ErrorCode::LimitExceeded, "zip: entry exceeds per-part limit", entry.name);
  }
  // Ratio guard: a bomb is about a large declared expansion, not about small
  // files that happen to compress well. Applying it to tiny entries would
  // reject legitimate highly-repetitive data, so it starts at 1 MiB.
  if (entry.uncompressed > (1ull << 20) && entry.compressed > 0 &&
      entry.uncompressed / entry.compressed > limits_.max_ratio) {
    throw Error(ErrorCode::LimitExceeded, "zip: suspicious compression ratio", entry.name);
  }
  if (total_uncompressed_ + entry.uncompressed > limits_.max_total_uncompressed) {
    throw Error(ErrorCode::LimitExceeded, "zip: total uncompressed budget exhausted", entry.name);
  }

  uint8_t hdr[30];
  in_->Seek(static_cast<int64_t>(entry.local_offset));
  ReadExact(*in_, hdr, 30, "local header");
  if (ReadLe32(hdr) != kLocalSig) throw Error(ErrorCode::Corrupt, "zip: bad local signature", entry.name);
  const uint32_t name_len = ReadLe16(hdr + 26);
  const uint32_t extra_len = ReadLe16(hdr + 28);
  const int64_t data_off = static_cast<int64_t>(entry.local_offset) + 30 + name_len + extra_len;

  if (entry.compressed > (64ull << 20)) {
    throw Error(ErrorCode::LimitExceeded, "zip: compressed entry too large for in-memory read", entry.name);
  }
  std::vector<uint8_t> raw(static_cast<size_t>(entry.compressed));
  if (!raw.empty()) {
    in_->Seek(data_off);
    ReadExact(*in_, raw.data(), static_cast<int64_t>(raw.size()), "entry data");
  }

  std::vector<uint8_t> out;
  if (entry.method == 0) {
    out = std::move(raw);
  } else {
    out = InflateRaw(raw.data(), raw.size(), entry.uncompressed + 1);
  }
  if (out.size() != entry.uncompressed) {
    throw Error(ErrorCode::Corrupt, "zip: uncompressed size mismatch", entry.name);
  }
  if (Crc32(out.data(), out.size()) != entry.crc) {
    throw Error(ErrorCode::Corrupt, "zip: crc mismatch", entry.name);
  }
  total_uncompressed_ += out.size();
  return out;
}

}  // namespace doccore
