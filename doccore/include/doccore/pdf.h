// DocCore - PDF writer. Consumes Layout IR only (docs/02 §3.5.1).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doccore/core.h"
#include "doccore/layout.h"
#include "doccore/stream.h"
#include "doccore/ttf.h"

namespace doccore {

struct PdfSaveOptions {
  std::string title;
  bool compress = true;   // content streams use /FlateDecode (stored blocks for now)
  bool pdf_a = false;     // not implemented in this wave
};

class FontRegistry {
 public:
  // Loads a TTF/OTF (TrueType outlines) and indexes it by family name.
  void AddFontFile(const std::string& path);
  void AddFontBytes(std::vector<uint8_t> bytes);
  const TtfFont* Match(const FontKey& key) const;
  size_t size() const noexcept { return fonts_.size(); }

 private:
  std::vector<TtfFont> fonts_;
};

class PdfWriter {
 public:
  static void Write(const LayoutDocument& doc, Stream& out, const FontRegistry& fonts,
                    const PdfSaveOptions& options = {}, WarningSink* warnings = nullptr);

  static std::vector<uint8_t> ToBytes(const LayoutDocument& doc, const FontRegistry& fonts,
                                      const PdfSaveOptions& options = {},
                                      WarningSink* warnings = nullptr);
};

}  // namespace doccore
