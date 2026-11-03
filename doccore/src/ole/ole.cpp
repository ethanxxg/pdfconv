// DocCore - OLE2 / CFB 读取实现。Design refs: docs/02 §3.4 / F-501、F-111.
#include "doccore/ole.h"

#include <cstring>

namespace doccore {
namespace ole {

namespace {

constexpr uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr uint32_t kFatSect = 0xFFFFFFFD;
constexpr uint32_t kFreeSect = 0xFFFFFFFF;

uint16_t Rd16(const std::vector<uint8_t>& d, size_t off) {
  return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}
uint32_t Rd32(const std::vector<uint8_t>& d, size_t off) {
  return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
         (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

}  // namespace

bool LooksLikeOle(const uint8_t* data, size_t size) {
  static const uint8_t kSig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  return data != nullptr && size >= 512 && std::memcmp(data, kSig, 8) == 0;
}

bool File::Parse() {
  dir_.clear();
  fat_.clear();
  minifat_.clear();
  mini_stream_.clear();
  message_.clear();
  if (!LooksLikeOle(data_.data(), data_.size())) {
    message_ = "not an OLE2/CFB container";
    return false;
  }
  sector_size_ = 1u << Rd16(data_, 0x1E);
  mini_size_ = 1u << Rd16(data_, 0x20);
  mini_cutoff_ = Rd32(data_, 0x38);
  const uint32_t n_fat = Rd32(data_, 0x2C);
  first_dir_ = Rd32(data_, 0x30);
  first_minifat_ = Rd32(data_, 0x3C);
  const uint32_t n_minifat = Rd32(data_, 0x40);
  const uint32_t first_difat = Rd32(data_, 0x44);
  const uint32_t n_difat = Rd32(data_, 0x48);
  if (sector_size_ == 0 || sector_size_ > (1u << 20)) {
    message_ = "bad sector shift";
    return false;
  }

  // 收集 FAT 扇区 ID：头部 DIFAT[0..108] + DIFAT 链。
  std::vector<uint32_t> fat_sectors;
  for (int i = 0; i < 109; ++i) {
    const uint32_t s = Rd32(data_, 0x4C + i * 4);
    if (s < kFreeSect) fat_sectors.push_back(s);
  }
  uint32_t difat = first_difat;
  for (uint32_t k = 0; k < n_difat && difat < kFreeSect; ++k) {
    const size_t base = SectorOffset(difat);
    if (base + sector_size_ > data_.size()) break;
    const uint32_t per = sector_size_ / 4;
    for (uint32_t i = 0; i + 1 < per; ++i) {
      const uint32_t s = Rd32(data_, base + i * 4);
      if (s < kFreeSect) fat_sectors.push_back(s);
    }
    difat = Rd32(data_, base + (per - 1) * 4);
  }
  (void)n_fat;

  // 组装 FAT。
  const uint32_t per_sector = sector_size_ / 4;
  for (uint32_t fs : fat_sectors) {
    const size_t base = SectorOffset(fs);
    if (base + sector_size_ > data_.size()) {
      message_ = "FAT sector out of range";
      return false;
    }
    for (uint32_t i = 0; i < per_sector; ++i) fat_.push_back(Rd32(data_, base + i * 4));
  }

  // MiniFAT。
  if (first_minifat_ < kFreeSect) {
    const std::vector<uint8_t> raw = ReadChain(first_minifat_, UINT64_MAX, false);
    for (size_t i = 0; i + 4 <= raw.size(); i += 4) minifat_.push_back(Rd32(raw, i));
  }
  (void)n_minifat;

  // 目录（线性扫描，不依赖红黑树）。
  const std::vector<uint8_t> dirraw = ReadChain(first_dir_, UINT64_MAX, false);
  for (size_t off = 0; off + 128 <= dirraw.size(); off += 128) {
    Entry e;
    const uint16_t nlen = Rd16(dirraw, off + 64);
    const size_t units = (nlen >= 2) ? (nlen / 2 - 1) : 0;
    for (size_t i = 0; i < units && (off + i * 2 + 1) < dirraw.size(); ++i) {
      e.name.push_back(static_cast<char16_t>(Rd16(dirraw, off + i * 2)));
    }
    e.type = static_cast<EntryType>(dirraw[off + 66]);
    e.start = Rd32(dirraw, off + 116);
    e.size = static_cast<uint64_t>(Rd32(dirraw, off + 120));  // v3：低 4 字节
    if (e.type != EntryType::Unallocated) dir_.push_back(std::move(e));
  }

  // mini stream = root 条目的流。
  for (const Entry& e : dir_) {
    if (e.IsRoot()) {
      mini_stream_ = ReadChain(e.start, e.size, false);
      break;
    }
  }
  return true;
}

std::vector<uint8_t> File::ReadChain(uint32_t start, uint64_t size, bool mini) const {
  std::vector<uint8_t> out;
  if (start >= kFreeSect) return out;
  const uint32_t unit = mini ? mini_size_ : sector_size_;
  const std::vector<uint32_t>& table = mini ? minifat_ : fat_;
  uint32_t cur = start;
  uint32_t guard = 0;
  const uint32_t max_units = static_cast<uint32_t>(table.size()) + 1;
  while (cur < kFreeSect) {
    if (++guard > max_units + 1) break;  // 防环
    size_t src;
    if (mini) {
      src = static_cast<size_t>(cur) * mini_size_;
      if (src + mini_size_ > mini_stream_.size()) break;
      out.insert(out.end(), mini_stream_.begin() + src,
                 mini_stream_.begin() + src + mini_size_);
    } else {
      src = SectorOffset(cur);
      if (src + sector_size_ > data_.size()) break;
      out.insert(out.end(), data_.begin() + src, data_.begin() + src + sector_size_);
    }
    if (size != UINT64_MAX && out.size() >= size) {
      out.resize(size);
      break;
    }
    if (cur >= table.size()) break;
    cur = table[cur];
    (void)unit;
  }
  if (size != UINT64_MAX && out.size() > size) out.resize(size);
  return out;
}

int File::IndexOf(const std::u16string& name) const {
  for (size_t i = 0; i < dir_.size(); ++i) {
    if (dir_[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

std::vector<uint8_t> File::ReadTopStream(const std::u16string& name) const {
  for (const Entry& e : dir_) {
    if (e.IsStream() && e.name == name) {
      return ReadChain(e.start, e.size, /*mini=*/e.size < mini_cutoff_);
    }
  }
  return {};
}

std::vector<uint8_t> File::ReadTopStream(const std::string& name) const {
  std::u16string u;
  for (char ch : name) u.push_back(static_cast<char16_t>(static_cast<unsigned char>(ch)));
  return ReadTopStream(u);
}

bool File::HasTopStream(const std::string& name) const {
  return IndexOf(std::u16string(name.begin(), name.end())) >= 0;
}

}  // namespace ole
}  // namespace doccore
