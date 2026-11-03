// DocCore - TrueType parsing and subsetting. Enough of the format to embed a
// subset as a PDF CIDFontType2 (docs/02 §3.5.1).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "doccore/core.h"

namespace doccore {

class TtfFont {
 public:
  struct SubsetResult {
    std::vector<uint8_t> bytes;
    std::unordered_map<uint16_t, uint16_t> old_to_new;  // old gid -> subset gid
    std::vector<uint16_t> new_to_old;
  };

  static TtfFont Load(std::vector<uint8_t> data);

  const std::string& family() const noexcept { return family_; }
  uint16_t units_per_em() const noexcept { return units_per_em_; }
  uint16_t num_glyphs() const noexcept { return num_glyphs_; }
  int16_t x_min() const noexcept { return x_min_; }
  int16_t y_min() const noexcept { return y_min_; }
  int16_t x_max() const noexcept { return x_max_; }
  int16_t y_max() const noexcept { return y_max_; }
  int16_t ascender() const noexcept { return ascender_; }
  int16_t descender() const noexcept { return descender_; }
  int16_t cap_height() const noexcept { return cap_height_; }
  int italic_angle() const noexcept { return italic_angle_; }
  bool is_bold() const noexcept { return bold_; }

  uint16_t GlyphForCodepoint(char32_t cp) const;
  uint16_t AdvanceWidth(uint16_t gid) const;
  int16_t LeftSideBearing(uint16_t gid) const;

  SubsetResult Subset(const std::vector<uint16_t>& used_gids) const;

 private:
  const uint8_t* TableData(const char tag[4], uint32_t* length = nullptr) const;
  void ParseCmap();
  void ParseName();

  std::vector<uint8_t> data_;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> tables_;  // tag -> (offset, len)
  std::string family_ = "Unknown";
  uint16_t units_per_em_ = 1000;
  uint16_t num_glyphs_ = 0;
  int16_t x_min_ = 0, y_min_ = 0, x_max_ = 0, y_max_ = 0;
  int16_t ascender_ = 800, descender_ = -200, cap_height_ = 700;
  int italic_angle_ = 0;
  bool bold_ = false;
  int16_t loc_format_ = 0;

  // cmap: BMP map plus a sparse map for astral codepoints.
  std::vector<std::pair<uint32_t, uint32_t>> cmap_segments_;  // (startCode, offsetInfo) format 4
  const uint8_t* cmap4_ = nullptr;
  uint32_t cmap4_len_ = 0;
  std::vector<std::pair<uint32_t, uint32_t>> cmap12_groups_;  // (startChar, startGlyph, count) flattened
  std::vector<uint32_t> cmap12_start_;
  std::vector<uint32_t> cmap12_end_;
  std::vector<uint32_t> cmap12_gid_;
};

}  // namespace doccore
