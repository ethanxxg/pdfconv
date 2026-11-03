#include "doccore/zip_write.h"

#include "doccore/deflate.h"
#include "doccore/zip.h"

namespace doccore {
namespace {

void Put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void Put64(std::vector<uint8_t>& out, uint64_t v) {
  Put32(out, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  Put32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

constexpr uint32_t kLocalSig = 0x04034B50;
constexpr uint32_t kCentralSig = 0x02014B50;
constexpr uint32_t kEocdSig = 0x06054B50;
constexpr uint32_t kEocd64Sig = 0x06064B50;
constexpr uint32_t kEocd64LocatorSig = 0x07064B50;
// Version 4.5 = ZIP64 support; this is what our reader expects for .rels-heavy
// packages, and classic tools accept it too.
constexpr uint16_t kVersionNeeded = 45;
constexpr uint16_t kGeneralFlags = 0x0800;  // UTF-8 names

struct WrittenEntry {
  std::string name;
  uint32_t crc = 0;
  uint64_t compressed = 0;
  uint64_t uncompressed = 0;
  uint16_t method = 0;
  uint64_t offset = 0;
};

}  // namespace

void ZipWriteToStream(Stream& out, const std::vector<ZipWriteEntry>& entries) {
  std::vector<uint8_t> body;
  std::vector<WrittenEntry> written;
  written.reserve(entries.size());

  for (const auto& e : entries) {
    std::string reason;
    if (!IsSafePartName(e.name, &reason)) {
      throw Error(ErrorCode::InvalidArgument, "zip write: unsafe entry name (" + reason + "): " + e.name);
    }
    WrittenEntry w;
    w.name = e.name;
    w.uncompressed = e.data.size();
    w.crc = Crc32(e.data.data(), e.data.size());
    w.offset = body.size();
    w.method = static_cast<uint16_t>(e.method);

    // Choose the encoding: deflate only when it actually shrinks the payload.
    const std::vector<uint8_t>* payload = &e.data;
    std::vector<uint8_t> compressed;
    if (e.method == ZipMethod::Deflate) {
      compressed = DeflateRaw(e.data.data(), e.data.size());
      if (compressed.size() < e.data.size()) {
        w.method = 8;
        payload = &compressed;
      } else {
        w.method = 0;
        payload = &e.data;
      }
    }
    w.compressed = payload->size();

    Put32(body, kLocalSig);
    Put16(body, kVersionNeeded);
    Put16(body, kGeneralFlags);
    Put16(body, w.method);
    Put16(body, 0);  // mod time
    Put16(body, 0x21);  // mod date (2000-01-01, fixed for reproducibility)
    Put32(body, w.crc);
    Put32(body, static_cast<uint32_t>(w.compressed > 0xFFFFFFFFull ? 0xFFFFFFFFull : w.compressed));
    Put32(body, static_cast<uint32_t>(w.uncompressed > 0xFFFFFFFFull ? 0xFFFFFFFFull : w.uncompressed));
    Put16(body, static_cast<uint16_t>(w.name.size()));
    Put16(body, 20);  // extra field: one zip64 (uncompressed + compressed) record
    body.insert(body.end(), w.name.begin(), w.name.end());
    Put16(body, 0x0001);
    Put16(body, 16);
    Put64(body, w.uncompressed);
    Put64(body, w.compressed);
    body.insert(body.end(), payload->begin(), payload->end());
    written.push_back(std::move(w));
  }

  const uint64_t central_start = body.size();
  for (const auto& w : written) {
    Put32(body, kCentralSig);
    Put16(body, kVersionNeeded << 8 | 0x14);  // version made by: MS-DOS, 4.5
    Put16(body, kVersionNeeded);
    Put16(body, kGeneralFlags);
    Put16(body, w.method);
    Put16(body, 0);
    Put16(body, 0x21);
    Put32(body, w.crc);
    Put32(body, static_cast<uint32_t>(w.compressed > 0xFFFFFFFFull ? 0xFFFFFFFFull : w.compressed));
    Put32(body, static_cast<uint32_t>(w.uncompressed > 0xFFFFFFFFull ? 0xFFFFFFFFull : w.uncompressed));
    Put16(body, static_cast<uint16_t>(w.name.size()));
    Put16(body, 32);  // extra: zip64 id+size (4) + uncompressed/compressed/offset/disk (28)
    Put16(body, 0);   // comment length
    Put16(body, 0);   // disk number start
    Put16(body, 0);   // internal attrs
    Put32(body, 0);   // external attrs
    Put32(body, static_cast<uint32_t>(w.offset > 0xFFFFFFFFull ? 0xFFFFFFFFull : w.offset));
    body.insert(body.end(), w.name.begin(), w.name.end());
    // ZIP64 extended information: uncompressed, compressed, offset, disk.
    Put16(body, 0x0001);
    Put16(body, 24);
    Put64(body, w.uncompressed);
    Put64(body, w.compressed);
    Put64(body, w.offset);
    Put32(body, 0);
  }
  const uint64_t central_size = body.size() - central_start;

  // ZIP64 end-of-central-directory + locator are only written when a classic
  // field would overflow, matching what readers (including ours) expect: the
  // plain EOCD stays the last record and points at real numbers.
  bool needs_zip64 = written.size() > 0xFFFFu || central_size > 0xFFFFFFFFull ||
                     central_start > 0xFFFFFFFFull;
  for (const auto& w : written) {
    if (w.offset > 0xFFFFFFFFull || w.uncompressed > 0xFFFFFFFFull ||
        w.compressed > 0xFFFFFFFFull) {
      needs_zip64 = true;
    }
  }
  if (needs_zip64) {
    const uint64_t zip64_eocd_offset = body.size();
    Put32(body, kEocd64Sig);
    Put64(body, 44);  // size of the remaining zip64 EOCD record
    Put16(body, kVersionNeeded << 8 | 0x14);
    Put16(body, kVersionNeeded);
    Put32(body, 0);  // disk number
    Put32(body, 0);  // disk with the central directory
    Put64(body, written.size());
    Put64(body, written.size());
    Put64(body, central_size);
    Put64(body, central_start);
    Put32(body, kEocd64LocatorSig);
    Put32(body, 0);  // disk with the zip64 EOCD record
    Put64(body, zip64_eocd_offset);
    Put32(body, 1);  // total number of disks
  }

  Put32(body, kEocdSig);
  Put16(body, 0);  // number of this disk
  Put16(body, 0);  // disk where the central directory starts
  Put16(body, static_cast<uint16_t>(needs_zip64 ? 0xFFFFu : written.size()));
  Put16(body, static_cast<uint16_t>(needs_zip64 ? 0xFFFFu : written.size()));
  Put32(body, static_cast<uint32_t>(needs_zip64 ? 0xFFFFFFFFull : central_size));
  Put32(body, static_cast<uint32_t>(needs_zip64 ? 0xFFFFFFFFull : central_start));
  Put16(body, 0);  // comment length

  out.Write(body.data(), body.size());
}

std::vector<uint8_t> ZipWriteToBytes(const std::vector<ZipWriteEntry>& entries) {
  MemoryStream mem;
  ZipWriteToStream(mem, entries);
  std::vector<uint8_t> out;
  const std::vector<uint8_t>& buf = mem.data();
  out.assign(buf.begin(), buf.end());
  return out;
}

}  // namespace doccore
