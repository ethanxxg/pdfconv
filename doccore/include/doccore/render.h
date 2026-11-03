// DocCore - page rasterization and PNG encoding. Design refs: docs/02 §3.2, §3.5.1
#pragma once

#include <cstdint>
#include <vector>

#include "doccore/core.h"
#include "doccore/layout.h"

namespace doccore {

struct RasterOptions {
  int dpi = 96;
  Rgba background{255, 255, 255, 255};
};

struct RasterImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

// Fills paths and blits images. Text needs a glyph rasterizer, which is not
// part of this wave: DrawText ops are reported as DrawOpSkipped.
RasterImage RasterizePage(const LayoutPage& page, const RasterOptions& opt, WarningSink* warnings);

// 8-bit RGBA PNG. Uses stored deflate blocks, so files are valid but not small.
std::vector<uint8_t> EncodePng(const RasterImage& image);

}  // namespace doccore
