// DocCore - ZIP container writer (OOXML packages, PDF-less containers).
//
// Complements ZipArchive (reader): both live in the kernel so that every
// family's writer (docx/xlsx/pptx) shares one implementation. Entry names go
// through the same IsSafePartName() guard as the reader, so a writer can never
// emit a package that the reader would refuse (docs/02 §3.1.3).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/stream.h"

namespace doccore {

// How an entry is stored. Stored is a safe fallback for tiny or incompressible
// payloads; Deflate uses the real encoder in deflate.cpp.
enum class ZipMethod { Stored = 0, Deflate = 8 };

struct ZipWriteEntry {
  std::string name;
  std::vector<uint8_t> data;
  ZipMethod method = ZipMethod::Deflate;
};

// Serializes a ZIP container. Entries are written in insertion order; the
// directory is always ZIP64-capable (a zip64 EOCD record is emitted when any
// field would overflow the 32-bit classic records).
std::vector<uint8_t> ZipWriteToBytes(const std::vector<ZipWriteEntry>& entries);

// Same, straight into a stream.
void ZipWriteToStream(Stream& out, const std::vector<ZipWriteEntry>& entries);

}  // namespace doccore
