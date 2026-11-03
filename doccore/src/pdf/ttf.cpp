#include "doccore/ttf.h"

#include <algorithm>
#include <cstring>

namespace doccore {

namespace {

uint32_t Tag(const char* s) {
  return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
         (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

void Need(bool ok, const char* what) {
  if (!ok) throw Error(ErrorCode::Corrupt, std::string("ttf: ") + what);
}

void PutBe16(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}
void PutBe32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 24));
  o.push_back(static_cast<uint8_t>(v >> 16));
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}
void PutLe16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void PutBe16At(std::vector<uint8_t>& o, size_t at, uint32_t v) {
  o[at] = static_cast<uint8_t>(v >> 8);
  o[at + 1] = static_cast<uint8_t>(v);
}

uint32_t Checksum(const uint8_t* data, size_t len) {
  uint32_t sum = 0;
  size_t i = 0;
  for (; i + 4 <= len; i += 4) sum += ReadBe32(data + i);
  if (i < len) {
    uint8_t tail[4] = {0, 0, 0, 0};
    for (size_t k = 0; i + k < len; ++k) tail[k] = data[i + k];
    sum += ReadBe32(tail);
  }
  return sum;
}

}  // namespace

TtfFont TtfFont::Load(std::vector<uint8_t> data) {
  TtfFont f;
  f.data_ = std::move(data);
  Need(f.data_.size() >= 12, "file too small");
  const uint8_t* d = f.data_.data();
  const uint32_t version = ReadBe32(d);
  Need(version == 0x00010000 || version == 0x74727565 /*true*/, "not a TrueType font");

  const uint32_t num_tables = ReadBe16(d + 4);
  Need(num_tables > 0 && num_tables < 512, "bad table count");
  Need(12 + num_tables * 16 <= f.data_.size(), "truncated table directory");
  for (uint32_t i = 0; i < num_tables; ++i) {
    const uint8_t* rec = d + 12 + i * 16;
    const uint32_t tag = ReadBe32(rec);
    const uint32_t off = ReadBe32(rec + 8);
    const uint32_t len = ReadBe32(rec + 12);
    Need(uint64_t(off) + len <= f.data_.size(), "table outside file");
    f.tables_[tag] = {off, len};
  }

  uint32_t head_len = 0;
  const uint8_t* head = f.TableData("head", &head_len);
  Need(head != nullptr && head_len >= 54, "missing head");
  f.units_per_em_ = static_cast<uint16_t>(ReadBe16(head + 18));
  f.x_min_ = static_cast<int16_t>(ReadBe16(head + 36));
  f.y_min_ = static_cast<int16_t>(ReadBe16(head + 38));
  f.x_max_ = static_cast<int16_t>(ReadBe16(head + 40));
  f.y_max_ = static_cast<int16_t>(ReadBe16(head + 42));
  f.loc_format_ = static_cast<int16_t>(ReadBe16(head + 50));
  Need(f.units_per_em_ > 0, "bad unitsPerEm");

  uint32_t maxp_len = 0;
  const uint8_t* maxp = f.TableData("maxp", &maxp_len);
  Need(maxp != nullptr && maxp_len >= 6, "missing maxp");
  f.num_glyphs_ = static_cast<uint16_t>(ReadBe16(maxp + 4));
  Need(f.num_glyphs_ > 0, "no glyphs");

  uint32_t hhea_len = 0;
  const uint8_t* hhea = f.TableData("hhea", &hhea_len);
  Need(hhea != nullptr && hhea_len >= 36, "missing hhea");
  f.ascender_ = static_cast<int16_t>(ReadBe16(hhea + 4));
  f.descender_ = static_cast<int16_t>(ReadBe16(hhea + 6));

  uint32_t os2_len = 0;
  const uint8_t* os2 = f.TableData("OS/2", &os2_len);
  if (os2 != nullptr && os2_len >= 90) {
    f.bold_ = (ReadBe16(os2 + 4) & 0x0020) != 0;  // fsSelection BOLD
    f.italic_angle_ = static_cast<int16_t>(ReadBe16(os2 + 4) & 0x0001 ? -12 : 0);
    f.cap_height_ = static_cast<int16_t>(ReadBe16(os2 + 88));
    if (f.cap_height_ == 0) f.cap_height_ = static_cast<int16_t>(f.ascender_ * 0.7);
  }

  f.ParseCmap();
  f.ParseName();
  return f;
}

const uint8_t* TtfFont::TableData(const char tag[4], uint32_t* length) const {
  const auto it = tables_.find(Tag(tag));
  if (it == tables_.end()) return nullptr;
  if (length != nullptr) *length = it->second.second;
  return data_.data() + it->second.first;
}

void TtfFont::ParseName() {
  uint32_t len = 0;
  const uint8_t* name = TableData("name", &len);
  if (name == nullptr || len < 6) return;
  const uint32_t count = ReadBe16(name + 2);
  const uint32_t str_off = ReadBe16(name + 4);
  std::string best;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* rec = name + 6 + i * 12;
    if (6 + (i + 1) * 12 > len) break;
    const uint32_t platform = ReadBe16(rec);
    const uint32_t name_id = ReadBe16(rec + 6);
    const uint32_t slen = ReadBe16(rec + 8);
    const uint32_t soff = ReadBe16(rec + 10);
    if (name_id != 1) continue;
    if (str_off + soff + slen > len) continue;
    const uint8_t* s = name + str_off + soff;
    if (platform == 3 || platform == 0) {
      best = U16BeToUtf8(s, slen);
      break;
    }
    if (platform == 1 && best.empty()) best.assign(reinterpret_cast<const char*>(s), slen);
  }
  if (!best.empty()) family_ = best;
}

void TtfFont::ParseCmap() {
  uint32_t len = 0;
  const uint8_t* cmap = TableData("cmap", &len);
  if (cmap == nullptr || len < 4) return;
  const uint32_t n = ReadBe16(cmap + 2);
  for (uint32_t i = 0; i < n; ++i) {
    if (4 + (i + 1) * 8 > len) break;
    const uint8_t* rec = cmap + 4 + i * 8;
    const uint32_t platform = ReadBe16(rec);
    const uint32_t encoding = ReadBe16(rec + 2);
    const uint32_t off = ReadBe32(rec + 4);
    if (off + 2 > len) continue;
    const uint32_t format = ReadBe16(cmap + off);
    const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
    if (!unicode) continue;
    if (format == 4 && cmap4_ == nullptr) {
      cmap4_ = cmap + off;
      cmap4_len_ = len - off;
    } else if (format == 12 && cmap12_start_.empty()) {
      const uint32_t groups = ReadBe32(cmap + off + 12);
      if (off + 16 + groups * 12 > len) continue;
      for (uint32_t g = 0; g < groups; ++g) {
        const uint8_t* gr = cmap + off + 16 + g * 12;
        const uint32_t sc = ReadBe32(gr);
        const uint32_t ec = ReadBe32(gr + 4);
        const uint32_t sg = ReadBe32(gr + 8);
        cmap12_start_.push_back(sc);
        cmap12_end_.push_back(ec);
        cmap12_gid_.push_back(sg);
      }
    }
  }
}

uint16_t TtfFont::GlyphForCodepoint(char32_t cp) const {
  const uint32_t u = static_cast<uint32_t>(cp);
  if (u > 0xFFFF) {
    for (size_t i = 0; i < cmap12_start_.size(); ++i) {
      if (u >= cmap12_start_[i] && u <= cmap12_end_[i]) {
        return static_cast<uint16_t>(cmap12_gid_[i] + (u - cmap12_start_[i]));
      }
    }
    return 0;
  }
  if (cmap4_ == nullptr) {
    for (size_t i = 0; i < cmap12_start_.size(); ++i) {
      if (u >= cmap12_start_[i] && u <= cmap12_end_[i]) {
        return static_cast<uint16_t>(cmap12_gid_[i] + (u - cmap12_start_[i]));
      }
    }
    return 0;
  }
  const uint16_t seg_count = static_cast<uint16_t>(ReadBe16(cmap4_ + 6) / 2);
  const uint8_t* end_codes = cmap4_ + 14;
  const uint8_t* start_codes = end_codes + seg_count * 2 + 2;
  const uint8_t* id_deltas = start_codes + seg_count * 2;
  const uint8_t* id_range_offsets = id_deltas + seg_count * 2;
  for (uint16_t i = 0; i < seg_count; ++i) {
    const uint32_t end = ReadBe16(end_codes + i * 2);
    if (u > end) continue;
    const uint32_t start = ReadBe16(start_codes + i * 2);
    if (u < start) return 0;
    const uint16_t delta = static_cast<uint16_t>(ReadBe16(id_deltas + i * 2));
    const uint16_t range_off = static_cast<uint16_t>(ReadBe16(id_range_offsets + i * 2));
    if (range_off == 0) {
      return static_cast<uint16_t>((u + delta) & 0xFFFF);
    }
    const uint8_t* glyph_index_addr = id_range_offsets + i * 2 + range_off + (u - start) * 2;
    if (glyph_index_addr + 2 > cmap4_ + cmap4_len_) return 0;
    const uint16_t gi = static_cast<uint16_t>(ReadBe16(glyph_index_addr));
    if (gi == 0) return 0;
    return static_cast<uint16_t>((gi + delta) & 0xFFFF);
  }
  return 0;
}

uint16_t TtfFont::AdvanceWidth(uint16_t gid) const {
  uint32_t hhea_len = 0;
  const uint8_t* hhea = TableData("hhea", &hhea_len);
  uint32_t hmtx_len = 0;
  const uint8_t* hmtx = TableData("hmtx", &hmtx_len);
  if (hhea == nullptr || hmtx == nullptr || hhea_len < 36) return 0;
  const uint16_t num_h = static_cast<uint16_t>(ReadBe16(hhea + 34));
  if (num_h == 0) return 0;
  const uint16_t idx = gid < num_h ? gid : static_cast<uint16_t>(num_h - 1);
  if (static_cast<uint32_t>(idx) * 4 + 2 > hmtx_len) return 0;
  return static_cast<uint16_t>(ReadBe16(hmtx + idx * 4));
}

int16_t TtfFont::LeftSideBearing(uint16_t gid) const {
  uint32_t hhea_len = 0;
  const uint8_t* hhea = TableData("hhea", &hhea_len);
  uint32_t hmtx_len = 0;
  const uint8_t* hmtx = TableData("hmtx", &hmtx_len);
  if (hhea == nullptr || hmtx == nullptr || hhea_len < 36) return 0;
  const uint16_t num_h = static_cast<uint16_t>(ReadBe16(hhea + 34));
  if (gid < num_h) {
    if (static_cast<uint32_t>(gid) * 4 + 4 > hmtx_len) return 0;
    return static_cast<int16_t>(ReadBe16(hmtx + gid * 4 + 2));
  }
  const uint32_t extra = static_cast<uint32_t>(gid - num_h) * 2;
  const uint32_t base = static_cast<uint32_t>(num_h) * 4 + extra;
  if (base + 2 > hmtx_len) return 0;
  return static_cast<int16_t>(ReadBe16(hmtx + base));
}

TtfFont::SubsetResult TtfFont::Subset(const std::vector<uint16_t>& used_gids) const {
  // 1. Close over composite glyph components.
  std::vector<bool> keep(num_glyphs_, false);
  keep[0] = true;
  for (uint16_t g : used_gids) {
    if (g < num_glyphs_) keep[g] = true;
  }

  uint32_t loca_len = 0;
  const uint8_t* loca = TableData("loca", &loca_len);
  uint32_t glyf_len = 0;
  const uint8_t* glyf = TableData("glyf", &glyf_len);
  Need(loca != nullptr && glyf != nullptr, "missing loca/glyf");

  auto glyph_span = [&](uint16_t gid, uint32_t* off, uint32_t* len) {
    uint32_t a = 0, b = 0;
    if (loc_format_ == 0) {
      if (static_cast<uint32_t>(gid) * 2 + 4 > loca_len) return false;
      a = ReadBe16(loca + gid * 2) * 2u;
      b = ReadBe16(loca + gid * 2 + 2) * 2u;
    } else {
      if (static_cast<uint32_t>(gid) * 4 + 8 > loca_len) return false;
      a = ReadBe32(loca + gid * 4);
      b = ReadBe32(loca + gid * 4 + 4);
    }
    if (b < a || b > glyf_len) return false;
    *off = a;
    *len = b - a;
    return true;
  };

  bool grew = true;
  while (grew) {
    grew = false;
    for (uint16_t g = 0; g < num_glyphs_; ++g) {
      if (!keep[g]) continue;
      uint32_t off = 0, len = 0;
      if (!glyph_span(g, &off, &len) || len < 10) continue;
      const int16_t contours = static_cast<int16_t>(ReadBe16(glyf + off));
      if (contours >= 0) continue;
      // composite: walk components
      size_t p = off + 10;
      for (;;) {
        if (p + 4 > off + len) break;
        const uint16_t flags = static_cast<uint16_t>(ReadBe16(glyf + p));
        const uint16_t comp = static_cast<uint16_t>(ReadBe16(glyf + p + 2));
        if (comp < num_glyphs_ && !keep[comp]) {
          keep[comp] = true;
          grew = true;
        }
        p += 4;
        p += (flags & 0x0001) ? 4 : 2;  // ARG_1_AND_2_ARE_WORDS
        if (flags & 0x0008) {
          p += 2;  // WE_HAVE_A_SCALE
        } else if (flags & 0x0040) {
          p += 4;  // WE_HAVE_AN_X_AND_Y_SCALE
        } else if (flags & 0x0080) {
          p += 8;  // WE_HAVE_A_TWO_BY_TWO
        }
        if ((flags & 0x0020) == 0) break;  // MORE_COMPONENTS
      }
    }
  }

  // 2. New gid order: ascending old gid, .notdef stays 0.
  std::vector<uint16_t> old_gids;
  for (uint16_t g = 0; g < num_glyphs_; ++g) {
    if (keep[g]) old_gids.push_back(g);
  }
  SubsetResult res;
  res.new_to_old = old_gids;
  for (uint16_t i = 0; i < old_gids.size(); ++i) {
    res.old_to_new[old_gids[i]] = static_cast<uint16_t>(i);
  }

  // 3. glyf + loca
  std::vector<uint8_t> new_glyf;
  std::vector<uint32_t> offsets;
  offsets.push_back(0);
  for (uint16_t g : old_gids) {
    uint32_t off = 0, len = 0;
    if (glyph_span(g, &off, &len) && len > 0) {
      new_glyf.insert(new_glyf.end(), glyf + off, glyf + off + len);
      while (new_glyf.size() % 4 != 0) new_glyf.push_back(0);
    }
    offsets.push_back(static_cast<uint32_t>(new_glyf.size()));
  }
  const bool short_loca = offsets.back() < 0x20000;
  std::vector<uint8_t> new_loca;
  for (uint32_t o : offsets) {
    if (short_loca) {
      PutBe16(new_loca, o / 2);
    } else {
      PutBe32(new_loca, o);
    }
  }

  // 4. hmtx / hhea / maxp / head / post
  const uint16_t new_count = static_cast<uint16_t>(old_gids.size());
  std::vector<uint8_t> new_hmtx;
  for (uint16_t g : old_gids) {
    PutBe16(new_hmtx, AdvanceWidth(g));
    PutBe16(new_hmtx, static_cast<uint16_t>(LeftSideBearing(g)));
  }

  std::vector<uint8_t> new_hhea;
  {
    uint32_t len = 0;
    const uint8_t* src = TableData("hhea", &len);
    Need(src != nullptr, "missing hhea");
    new_hhea.assign(src, src + len);
    if (new_hhea.size() >= 36) PutBe16At(new_hhea, 34, new_count);
  }
  std::vector<uint8_t> new_maxp;
  {
    uint32_t len = 0;
    const uint8_t* src = TableData("maxp", &len);
    Need(src != nullptr, "missing maxp");
    new_maxp.assign(src, src + len);
    if (new_maxp.size() >= 6) PutBe16At(new_maxp, 4, new_count);
  }
  std::vector<uint8_t> new_head;
  {
    uint32_t len = 0;
    const uint8_t* src = TableData("head", &len);
    Need(src != nullptr, "missing head");
    new_head.assign(src, src + len);
    if (new_head.size() >= 54) {
      PutBe16At(new_head, 50, short_loca ? 0 : 1);
      for (int i = 8; i < 12; ++i) new_head[i] = 0;  // clear checkSumAdjustment
    }
  }
  std::vector<uint8_t> new_post;
  PutBe32(new_post, 0x00030000);
  PutBe32(new_post, 0);  // italicAngle
  PutBe16(new_post, 0);  // underlinePosition
  PutBe16(new_post, 0);  // underlineThickness
  PutBe32(new_post, 0);  // isFixedPitch
  PutBe32(new_post, 0);
  PutBe32(new_post, 0);
  PutBe32(new_post, 0);

  struct OutTable {
    char tag[4];
    std::vector<uint8_t> data;
  };
  std::vector<OutTable> out_tables = {
      {{'h', 'e', 'a', 'd'}, new_head}, {{'h', 'h', 'e', 'a'}, new_hhea},
      {{'m', 'a', 'x', 'p'}, new_maxp}, {{'h', 'm', 't', 'x'}, new_hmtx},
      {{'l', 'o', 'c', 'a'}, new_loca}, {{'g', 'l', 'y', 'f'}, new_glyf},
      {{'p', 'o', 's', 't'}, new_post}};
  std::sort(out_tables.begin(), out_tables.end(),
            [](const OutTable& a, const OutTable& b) { return std::memcmp(a.tag, b.tag, 4) < 0; });

  // 5. Assemble the font.
  std::vector<uint8_t> out;
  PutBe32(out, 0x00010000);
  PutBe16(out, static_cast<uint32_t>(out_tables.size()));
  PutBe16(out, 0);
  PutBe16(out, 0);
  PutBe16(out, 0);
  const size_t dir_start = out.size();
  out.resize(out.size() + out_tables.size() * 16, 0);

  size_t head_dir_off = 0;
  for (size_t i = 0; i < out_tables.size(); ++i) {
    while (out.size() % 4 != 0) out.push_back(0);
    const uint32_t off = static_cast<uint32_t>(out.size());
    const uint32_t len = static_cast<uint32_t>(out_tables[i].data.size());
    out.insert(out.end(), out_tables[i].data.begin(), out_tables[i].data.end());
    uint8_t* rec = out.data() + dir_start + i * 16;
    std::memcpy(rec, out_tables[i].tag, 4);
    const uint32_t cs = Checksum(out_tables[i].data.data(), len);
    rec[4] = static_cast<uint8_t>(cs >> 24);
    rec[5] = static_cast<uint8_t>(cs >> 16);
    rec[6] = static_cast<uint8_t>(cs >> 8);
    rec[7] = static_cast<uint8_t>(cs);
    rec[8] = static_cast<uint8_t>(off >> 24);
    rec[9] = static_cast<uint8_t>(off >> 16);
    rec[10] = static_cast<uint8_t>(off >> 8);
    rec[11] = static_cast<uint8_t>(off);
    rec[12] = static_cast<uint8_t>(len >> 24);
    rec[13] = static_cast<uint8_t>(len >> 16);
    rec[14] = static_cast<uint8_t>(len >> 8);
    rec[15] = static_cast<uint8_t>(len);
    if (std::memcmp(out_tables[i].tag, "head", 4) == 0) head_dir_off = dir_start + i * 16 + 8;
  }
  while (out.size() % 4 != 0) out.push_back(0);

  // checkSumAdjustment = 0xB1B0AFBA - checksum(entire font with field zeroed)
  if (head_dir_off != 0) {
    const uint32_t off = ReadBe32(out.data() + head_dir_off);
    const uint32_t total = Checksum(out.data(), out.size());
    const uint32_t adj = 0xB1B0AFBAu - total;
    out[off + 8] = static_cast<uint8_t>(adj >> 24);
    out[off + 9] = static_cast<uint8_t>(adj >> 16);
    out[off + 10] = static_cast<uint8_t>(adj >> 8);
    out[off + 11] = static_cast<uint8_t>(adj);
  }

  res.bytes = std::move(out);
  return res;
}

}  // namespace doccore
