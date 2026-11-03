#include "doccore/pdf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "doccore/deflate.h"

namespace doccore {

void FontRegistry::AddFontFile(const std::string& path) {
  FileStream in(path, "rb");
  auto bytes = ReadAll(in, 256ull << 20);
  fonts_.push_back(TtfFont::Load(std::move(bytes)));
}

void FontRegistry::AddFontBytes(std::vector<uint8_t> bytes) {
  fonts_.push_back(TtfFont::Load(std::move(bytes)));
}

const TtfFont* FontRegistry::Match(const FontKey& key) const {
  for (const auto& f : fonts_) {
    if (f.family() == key.family) return &f;
  }
  if (!fonts_.empty()) return &fonts_.front();
  return nullptr;
}

namespace {

std::string Fmt(double v) {
  if (std::fabs(v) < 0.00005) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

void Append(std::vector<uint8_t>& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
}

std::string HexOf(const std::vector<uint16_t>& values) {
  static const char* kHex = "0123456789ABCDEF";
  std::string s;
  s.reserve(values.size() * 4 + 2);
  s.push_back('<');
  for (uint16_t v : values) {
    s.push_back(kHex[(v >> 12) & 0xF]);
    s.push_back(kHex[(v >> 8) & 0xF]);
    s.push_back(kHex[(v >> 4) & 0xF]);
    s.push_back(kHex[v & 0xF]);
  }
  s.push_back('>');
  return s;
}

std::string U16Hex(uint32_t cp) {
  static const char* kHex = "0123456789ABCDEF";
  std::string s;
  s.push_back('<');
  for (int shift = 12; shift >= 0; shift -= 4) s.push_back(kHex[(cp >> shift) & 0xF]);
  s.push_back('>');
  return s;
}

std::string EscapeText(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// PDF 文本字符串：ASCII 走字面量（转义括号/反斜杠），非 ASCII 走 UTF-16BE +
// BOM 的十六进制字符串（PDF 32000-1 §7.9.2），这样书签名/URI 里的中文也能被
// 阅读器正确还原。
std::string PdfTextString(const std::string& utf8) {
  bool ascii = true;
  for (unsigned char c : utf8) {
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) return "(" + EscapeText(utf8) + ")";
  static const char* kHex = "0123456789ABCDEF";
  std::string s = "<FEFF";
  auto push_u16 = [&](uint16_t u) {
    s.push_back(kHex[(u >> 12) & 0xF]);
    s.push_back(kHex[(u >> 8) & 0xF]);
    s.push_back(kHex[(u >> 4) & 0xF]);
    s.push_back(kHex[u & 0xF]);
  };
  for (char32_t cp : Utf8ToU32(utf8)) {
    if (cp > 0xFFFF) {
      const uint32_t v = cp - 0x10000;
      push_u16(static_cast<uint16_t>(0xD800 + (v >> 10)));
      push_u16(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
    } else {
      push_u16(static_cast<uint16_t>(cp));
    }
  }
  s.push_back('>');
  return s;
}

// PDF 名字对象（/Name）的转义：常规字符原样，其余按 #XX 转义
// （PDF 32000-1 §7.3.5）。
std::string PdfName(const std::string& in) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() + 4);
  for (unsigned char c : in) {
    const bool regular = c > 0x20 && c < 0x7F && std::strchr("()<>[]{}/%#", c) == nullptr;
    if (regular) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('#');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

struct FontUse {
  const TtfFont* font = nullptr;
  FontKey key;
  std::vector<uint32_t> codepoints;                       // sorted unique
  std::map<uint32_t, uint16_t> cp_to_new_gid;
  std::vector<uint16_t> new_gids;                         // sorted by new gid
  std::string base_font;
  int type0_obj = 0, cid_obj = 0, desc_obj = 0, file_obj = 0, tounicode_obj = 0;
};

class Builder {
 public:
  int Reserve() {
    objects_.emplace_back();
    return static_cast<int>(objects_.size());
  }

  void Set(int obj, std::vector<uint8_t> body) { objects_[static_cast<size_t>(obj - 1)] = std::move(body); }

  void Set(int obj, const std::string& body) {
    objects_[static_cast<size_t>(obj - 1)].assign(body.begin(), body.end());
  }

  int Add(std::vector<uint8_t> body) {
    objects_.push_back(std::move(body));
    return static_cast<int>(objects_.size());
  }

  int AddStream(const std::string& dict_extra, const std::vector<uint8_t>& data, bool compress) {
    std::vector<uint8_t> payload = compress ? ZlibStoreCompress(data.data(), data.size()) : data;
    std::string dict = "<< ";
    if (!dict_extra.empty()) dict += dict_extra + " ";
    if (compress) dict += "/Filter /FlateDecode ";
    dict += "/Length " + std::to_string(payload.size()) + " >>";
    std::vector<uint8_t> body;
    Append(body, dict);
    Append(body, "\nstream\n");
    body.insert(body.end(), payload.begin(), payload.end());
    Append(body, "\nendstream");
    return Add(std::move(body));
  }

  std::vector<uint8_t> Finish(int root_obj, const std::string& info) {
    std::vector<uint8_t> out;
    Append(out, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    std::vector<uint64_t> offsets(objects_.size() + 2, 0);
    for (size_t i = 0; i < objects_.size(); ++i) {
      offsets[i + 1] = out.size();
      Append(out, std::to_string(i + 1) + " 0 obj\n");
      out.insert(out.end(), objects_[i].begin(), objects_[i].end());
      Append(out, "\nendobj\n");
    }
    // xref stream (PDF 1.5), W = [1 4 2]
    const int xref_obj = static_cast<int>(objects_.size()) + 1;
    const uint64_t xref_offset = out.size();
    std::vector<uint8_t> rows;
    rows.reserve(static_cast<size_t>(xref_obj) * 7);
    auto row = [&](uint8_t type, uint32_t off, uint16_t gen) {
      rows.push_back(type);
      rows.push_back(static_cast<uint8_t>(off >> 24));
      rows.push_back(static_cast<uint8_t>(off >> 16));
      rows.push_back(static_cast<uint8_t>(off >> 8));
      rows.push_back(static_cast<uint8_t>(off));
      rows.push_back(static_cast<uint8_t>(gen >> 8));
      rows.push_back(static_cast<uint8_t>(gen));
    };
    row(0, 0, 0xFFFF);  // object 0: free
    for (int i = 1; i <= xref_obj; ++i) {
      if (i == xref_obj) {
        row(1, static_cast<uint32_t>(xref_offset), 0);
      } else {
        row(1, static_cast<uint32_t>(offsets[static_cast<size_t>(i)]), 0);
      }
    }
    std::vector<uint8_t> payload = ZlibStoreCompress(rows.data(), rows.size());
    Append(out, std::to_string(xref_obj) + " 0 obj\n<< /Type /XRef /Size " +
                   std::to_string(xref_obj + 1) + " /W [1 4 2] /Root " + std::to_string(root_obj) +
                   " 0 R" + (info.empty() ? "" : " /Info " + info) +
                   " /Filter /FlateDecode /Length " + std::to_string(payload.size()) +
                   " >>\nstream\n");
    out.insert(out.end(), payload.begin(), payload.end());
    Append(out, "\nendstream\nendobj\nstartxref\n" + std::to_string(xref_offset) + "\n%%EOF\n");
    return out;
  }

 private:
  std::vector<std::vector<uint8_t>> objects_;
};

std::string MakeBaseFont(const std::string& family, bool bold, bool italic) {
  std::string name;
  for (char c : family) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) name.push_back(c);
  }
  if (name.empty()) name = "DocCore";
  if (bold) name += "-Bold";
  if (italic) name += "-Italic";
  return "DCAAA+" + name;  // 6 uppercase letters + '+', per PDF subset naming
}

std::string ToUnicodeCMap(const FontUse& fu) {
  std::string s;
  s += "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n";
  s += "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n";
  s += "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n";
  s += "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
  std::vector<std::pair<uint16_t, uint32_t>> pairs;
  for (const auto& kv : fu.cp_to_new_gid) pairs.emplace_back(kv.second, kv.first);
  std::sort(pairs.begin(), pairs.end());
  for (size_t i = 0; i < pairs.size(); i += 100) {
    const size_t n = std::min<size_t>(100, pairs.size() - i);
    s += std::to_string(n) + " beginbfchar\n";
    for (size_t k = 0; k < n; ++k) {
      s += U16Hex(pairs[i + k].first) + " " + U16Hex(pairs[i + k].second) + "\n";
    }
    s += "endbfchar\n";
  }
  s += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  return s;
}

}  // namespace

void PdfWriter::Write(const LayoutDocument& doc, Stream& out, const FontRegistry& fonts,
                      const PdfSaveOptions& options, WarningSink* warnings) {
  const auto bytes = ToBytes(doc, fonts, options, warnings);
  if (!bytes.empty()) out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
}

std::vector<uint8_t> PdfWriter::ToBytes(const LayoutDocument& doc, const FontRegistry& fonts,
                                        const PdfSaveOptions& options, WarningSink* warnings) {
  if (options.pdf_a) {
    throw Error(ErrorCode::Unsupported, "pdf: PDF/A output is not implemented yet");
  }

  // 1. Collect codepoints per font key.
  std::vector<FontUse> uses;
  auto find_use = [&](const FontKey& key) -> FontUse& {
    for (auto& u : uses) {
      if (u.key == key) return u;
    }
    FontUse u;
    u.key = key;
    u.font = fonts.Match(key);
    uses.push_back(std::move(u));
    return uses.back();
  };
  for (const auto& page : doc.pages) {
    for (const auto& op : page.ops) {
      const auto* t = std::get_if<DrawText>(&op);
      if (t == nullptr) continue;
      FontUse& u = find_use(t->font);
      for (char32_t cp : Utf8ToU32(t->text_utf8)) {
        if (cp == '\n' || cp == '\r') continue;
        u.codepoints.push_back(static_cast<uint32_t>(cp));
      }
    }
  }
  for (auto& u : uses) {
    std::sort(u.codepoints.begin(), u.codepoints.end());
    u.codepoints.erase(std::unique(u.codepoints.begin(), u.codepoints.end()), u.codepoints.end());
    if (u.font == nullptr) {
      throw Error(ErrorCode::InvalidArgument, "pdf: no font available for family '" + u.key.family + "'");
    }
    u.base_font = MakeBaseFont(u.font->family(), u.key.bold, u.key.italic);
  }

  Builder b;
  const int catalog_obj = b.Reserve();
  const int pages_obj = b.Reserve();

  // 2. Font objects (subset + ToUnicode + descriptor).
  for (auto& u : uses) {
    std::vector<uint16_t> old_gids;
    std::set<uint16_t> uniq;
    for (uint32_t cp : u.codepoints) {
      const uint16_t gid = u.font->GlyphForCodepoint(static_cast<char32_t>(cp));
      if (gid == 0) {
        if (warnings != nullptr) {
          warnings->Add({static_cast<int>(WarningCode::FontSubstituted), "pdf", "",
                         "glyph missing for U+" + std::to_string(cp)});
        }
        continue;
      }
      uniq.insert(gid);
    }
    old_gids.assign(uniq.begin(), uniq.end());
    auto subset = u.font->Subset(old_gids);
    u.new_gids = subset.new_to_old;
    for (uint32_t cp : u.codepoints) {
      const uint16_t gid = u.font->GlyphForCodepoint(static_cast<char32_t>(cp));
      const auto it = subset.old_to_new.find(gid);
      if (it != subset.old_to_new.end()) u.cp_to_new_gid[cp] = it->second;
    }

    u.file_obj = b.AddStream("/Length1 " + std::to_string(subset.bytes.size()), subset.bytes, false);
    const std::string cmap_text = ToUnicodeCMap(u);
    u.tounicode_obj = b.AddStream("", std::vector<uint8_t>(cmap_text.begin(), cmap_text.end()), true);

    std::string bbox = "[" + Fmt(u.font->x_min() * 1000.0 / u.font->units_per_em()) + " " +
                       Fmt(u.font->y_min() * 1000.0 / u.font->units_per_em()) + " " +
                       Fmt(u.font->x_max() * 1000.0 / u.font->units_per_em()) + " " +
                       Fmt(u.font->y_max() * 1000.0 / u.font->units_per_em()) + "]";
    std::string desc = "<< /Type /FontDescriptor /FontName /" + u.base_font +
                       " /Flags " + std::to_string(u.font->is_bold() ? 262176 : 32) + " /FontBBox " + bbox +
                       " /ItalicAngle " + std::to_string(u.font->italic_angle()) +
                       " /Ascent " + Fmt(u.font->ascender() * 1000.0 / u.font->units_per_em()) +
                       " /Descent " + Fmt(u.font->descender() * 1000.0 / u.font->units_per_em()) +
                       " /CapHeight " + Fmt(u.font->cap_height() * 1000.0 / u.font->units_per_em()) +
                       " /StemV 80 /FontFile2 " + std::to_string(u.file_obj) + " 0 R >>";
    u.desc_obj = b.Add(std::vector<uint8_t>(desc.begin(), desc.end()));

    std::string w = "[";
    for (size_t i = 0; i < u.new_gids.size(); ++i) {
      const uint16_t gid = u.new_gids[i];
      const double adv = u.font->AdvanceWidth(gid) * 1000.0 / u.font->units_per_em();
      w += std::to_string(i) + " [" + Fmt(adv) + "] ";
    }
    w += "]";
    std::string cid = "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + u.base_font +
                      " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>" +
                      " /FontDescriptor " + std::to_string(u.desc_obj) + " 0 R /W " + w +
                      " /CIDToGIDMap /Identity >>";
    u.cid_obj = b.Add(std::vector<uint8_t>(cid.begin(), cid.end()));

    std::string t0 = "<< /Type /Font /Subtype /Type0 /BaseFont /" + u.base_font +
                     " /Encoding /Identity-H /DescendantFonts [" + std::to_string(u.cid_obj) +
                     " 0 R] /ToUnicode " + std::to_string(u.tounicode_obj) + " 0 R >>";
    u.type0_obj = b.Add(std::vector<uint8_t>(t0.begin(), t0.end()));
  }

  // 3. Pages.
  std::vector<int> page_objs;
  for (const auto& page : doc.pages) {
    std::string content;
    std::vector<std::pair<int, int>> images;  // (image obj, smask obj)
    content.reserve(1024);

    for (const auto& op : page.ops) {
      if (const auto* t = std::get_if<DrawText>(&op)) {
        size_t idx = 0;
        for (; idx < uses.size(); ++idx) {
          if (uses[idx].key == t->font) break;
        }
        if (idx == uses.size()) continue;
        const FontUse& u = uses[idx];
        const double y = page.height_pt - t->y;
        std::vector<uint16_t> gids;
        for (char32_t cp : Utf8ToU32(t->text_utf8)) {
          const auto it = u.cp_to_new_gid.find(static_cast<uint32_t>(cp));
          if (it != u.cp_to_new_gid.end()) gids.push_back(it->second);
        }
        if (gids.empty()) continue;
        content += "BT\n/F" + std::to_string(idx + 1) + " " + Fmt(t->size_pt) + " Tf\n";
        content += Fmt(t->color.r / 255.0) + " " + Fmt(t->color.g / 255.0) + " " +
                   Fmt(t->color.b / 255.0) + " rg\n";
        if (t->rotate_deg == 0.0) {
          content += "1 0 0 1 " + Fmt(t->x) + " " + Fmt(y) + " Tm\n";
        } else {
          // 文本空间到用户空间的旋转矩阵：PDF 用户空间 y 向上，逆时针为正，
          // 与 IR 里"页面上看到的逆时针"一致（两边都是同一视觉方向）。
          const double rad = t->rotate_deg * 3.14159265358979323846 / 180.0;
          const double c = std::cos(rad);
          const double s = std::sin(rad);
          content += Fmt(c) + " " + Fmt(s) + " " + Fmt(-s) + " " + Fmt(c) + " " + Fmt(t->x) + " " +
                     Fmt(y) + " Tm\n";
        }
        content += HexOf(gids) + " Tj\nET\n";
      } else if (const auto* p = std::get_if<DrawPath>(&op)) {
        std::string path;
        for (const auto& v : p->verbs) {
          switch (v.kind) {
            case PathVerbKind::MoveTo:
              path += Fmt(v.x1) + " " + Fmt(page.height_pt - v.y1) + " m\n";
              break;
            case PathVerbKind::LineTo:
              path += Fmt(v.x1) + " " + Fmt(page.height_pt - v.y1) + " l\n";
              break;
            case PathVerbKind::CubicTo:
              path += Fmt(v.x1) + " " + Fmt(page.height_pt - v.y1) + " " + Fmt(v.x2) + " " +
                      Fmt(page.height_pt - v.y2) + " " + Fmt(v.x3) + " " + Fmt(page.height_pt - v.y3) + " c\n";
              break;
            case PathVerbKind::Close:
              path += "h\n";
              break;
          }
        }
        if (p->has_fill) {
          content += Fmt(p->fill.r / 255.0) + " " + Fmt(p->fill.g / 255.0) + " " +
                     Fmt(p->fill.b / 255.0) + " rg\n" + path + "f\n";
        }
        if (p->has_stroke) {
          content += Fmt(p->stroke.r / 255.0) + " " + Fmt(p->stroke.g / 255.0) + " " +
                     Fmt(p->stroke.b / 255.0) + " RG\n" + Fmt(p->stroke_pt) + " w\n" + path + "S\n";
        }
      } else if (const auto* di = std::get_if<DrawImage>(&op)) {
        if (di->image.width <= 0 || di->image.height <= 0) continue;
        std::vector<uint8_t> rgb(static_cast<size_t>(di->image.width) * di->image.height * 3);
        std::vector<uint8_t> alpha(static_cast<size_t>(di->image.width) * di->image.height);
        bool has_alpha = false;
        for (size_t px = 0; px < static_cast<size_t>(di->image.width) * di->image.height; ++px) {
          rgb[px * 3 + 0] = di->image.rgba[px * 4 + 0];
          rgb[px * 3 + 1] = di->image.rgba[px * 4 + 1];
          rgb[px * 3 + 2] = di->image.rgba[px * 4 + 2];
          alpha[px] = di->image.rgba[px * 4 + 3];
          if (alpha[px] != 255) has_alpha = true;
        }
        int smask_obj = 0;
        if (has_alpha) {
          smask_obj = b.AddStream("/Type /XObject /Subtype /Image /Width " +
                                      std::to_string(di->image.width) + " /Height " +
                                      std::to_string(di->image.height) +
                                      " /ColorSpace /DeviceGray /BitsPerComponent 8",
                                  alpha, options.compress);
        }
        std::string extra = "/Type /XObject /Subtype /Image /Width " + std::to_string(di->image.width) +
                            " /Height " + std::to_string(di->image.height) +
                            " /ColorSpace /DeviceRGB /BitsPerComponent 8";
        if (smask_obj != 0) extra += " /SMask " + std::to_string(smask_obj) + " 0 R";
        const int img_obj = b.AddStream(extra, rgb, options.compress);
        const std::string name = "Im" + std::to_string(images.size() + 1);
        content += "q\n" + Fmt(di->w_pt) + " 0 0 " + Fmt(di->h_pt) + " " + Fmt(di->x) + " " +
                   Fmt(page.height_pt - di->y - di->h_pt) + " cm\n/" + name + " Do\nQ\n";
        images.emplace_back(img_obj, smask_obj);
      }
    }

    const int content_obj = b.AddStream("", std::vector<uint8_t>(content.begin(), content.end()),
                                        options.compress);
    std::string resources = "<< /ProcSet [/PDF /Text /ImageC]";
    if (!uses.empty()) {
      resources += " /Font << ";
      for (size_t i = 0; i < uses.size(); ++i) {
        resources += "/F" + std::to_string(i + 1) + " " + std::to_string(uses[i].type0_obj) + " 0 R ";
      }
      resources += ">>";
    }
    if (!images.empty()) {
      resources += " /XObject << ";
      for (size_t i = 0; i < images.size(); ++i) {
        resources += "/Im" + std::to_string(i + 1) + " " + std::to_string(images[i].first) + " 0 R ";
      }
      resources += ">>";
    }
    resources += " >>";
    std::string page_dict = "<< /Type /Page /Parent " + std::to_string(pages_obj) + " 0 R /MediaBox [0 0 " +
                            Fmt(page.width_pt) + " " + Fmt(page.height_pt) + "] /Resources " + resources +
                            " /Contents " + std::to_string(content_obj) + " 0 R";
    // 链接注释（PDF 32000-1 §12.5.6.5）。IR 的 y 以页顶为原点，这里转成 PDF 的
    // 页底原点坐标系。
    if (!page.links.empty()) {
      page_dict += " /Annots [";
      for (const Link& l : page.links) {
        const double x0 = l.x;
        const double y0 = page.height_pt - l.y - l.h_pt;
        const double x1 = l.x + l.w_pt;
        const double y1 = page.height_pt - l.y;
        page_dict += "<< /Type /Annot /Subtype /Link /Rect [" + Fmt(x0) + " " + Fmt(y0) + " " +
                     Fmt(x1) + " " + Fmt(y1) + "] /Border [0 0 0]";
        if (!l.uri.empty()) {
          page_dict += " /A << /S /URI /URI " + PdfTextString(l.uri) + " >>";
        } else if (!l.dest.empty()) {
          // 名字对象引用 catalog /Dests（PDF 1.1 形式），字符串形式会走名字树。
          page_dict += " /A << /S /GoTo /D /" + PdfName(l.dest) + " >>";
        }
        page_dict += " >>";
      }
      page_dict += "]";
    }
    page_dict += " >>";
    page_objs.push_back(b.Add(std::vector<uint8_t>(page_dict.begin(), page_dict.end())));
  }

  std::string kids = "[";
  for (int obj : page_objs) kids += std::to_string(obj) + " 0 R ";
  kids += "]";
  b.Set(pages_obj, "<< /Type /Pages /Kids " + kids + " /Count " + std::to_string(page_objs.size()) + " >>");

  // 4. 命名目标（w:bookmarkStart 之类）。放进 catalog 的 /Dests 名字树，供
  // /GoTo 动作按名字解析。重名时先到先得。
  struct DestEntry {
    std::string name;
    int page = 0;
    double x = 0, y = 0;  // PDF 坐标（页底原点）
  };
  std::vector<DestEntry> dests;
  for (size_t pi = 0; pi < doc.pages.size(); ++pi) {
    for (const NamedDest& d : doc.pages[pi].dests) {
      if (d.name.empty()) continue;
      bool dup = false;
      for (const DestEntry& e : dests) {
        if (e.name == d.name) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      DestEntry e;
      e.name = d.name;
      e.page = static_cast<int>(pi);
      e.x = d.x;
      e.y = doc.pages[pi].height_pt - d.y;
      dests.push_back(std::move(e));
    }
  }

  // 5. 大纲（书签）。doc.bookmarks 是按出现顺序的扁平表，用 level 建树。
  int outline_root = 0;
  if (!doc.bookmarks.empty()) {
    outline_root = b.Reserve();
    const int n = static_cast<int>(doc.bookmarks.size());
    std::vector<int> bm_objs(n, 0);
    for (int i = 0; i < n; ++i) bm_objs[i] = b.Reserve();

    std::vector<int> parent(n, -1), prev(n, -1), next(n, -1), first(n, -1), last(n, -1);
    // first_of/last_of 按「父节点下标 + 1」索引，槽位 0 表示根（顶层）。
    std::vector<int> first_of(static_cast<size_t>(n) + 1, -1);
    std::vector<int> last_of(static_cast<size_t>(n) + 1, -1);
    std::vector<int> stk, stk_level;
    for (int i = 0; i < n; ++i) {
      const int lvl = doc.bookmarks[i].level > 0 ? doc.bookmarks[i].level : 1;
      while (!stk.empty() && stk_level.back() >= lvl) {
        stk.pop_back();
        stk_level.pop_back();
      }
      const int p = stk.empty() ? -1 : stk.back();
      parent[i] = p;
      const size_t slot = static_cast<size_t>(p + 1);
      prev[i] = last_of[slot];
      if (last_of[slot] >= 0) next[last_of[slot]] = i;
      if (first_of[slot] < 0) first_of[slot] = i;
      last_of[slot] = i;
      stk.push_back(i);
      stk_level.push_back(lvl);
    }
    for (int i = 0; i < n; ++i) {
      first[i] = first_of[static_cast<size_t>(i) + 1];
      last[i] = last_of[static_cast<size_t>(i) + 1];
    }
    // 子节点索引必大于父节点索引，倒序即可自底向上累加后代数。
    std::vector<int> cnt(n, 0);
    for (int i = n - 1; i >= 0; --i) {
      for (int j = first[i]; j != -1; j = next[j]) cnt[i] += 1 + cnt[j];
    }

    for (int i = 0; i < n; ++i) {
      const Bookmark& bm = doc.bookmarks[i];
      std::string d = "<< /Title " + PdfTextString(bm.title);
      const int page = bm.page;
      if (page >= 0 && page < static_cast<int>(page_objs.size())) {
        const double y = doc.pages[static_cast<size_t>(page)].height_pt - bm.y;
        d += " /Dest [" + std::to_string(page_objs[static_cast<size_t>(page)]) + " 0 R /XYZ 0 " +
             Fmt(y) + " null]";
      }
      d += " /Parent " + std::to_string(parent[i] >= 0 ? bm_objs[parent[i]] : outline_root) + " 0 R";
      if (prev[i] >= 0) d += " /Prev " + std::to_string(bm_objs[prev[i]]) + " 0 R";
      if (next[i] >= 0) d += " /Next " + std::to_string(bm_objs[next[i]]) + " 0 R";
      if (first[i] >= 0) {
        d += " /First " + std::to_string(bm_objs[first[i]]) + " 0 R /Last " +
             std::to_string(bm_objs[last[i]]) + " 0 R /Count " + std::to_string(cnt[i]);
      }
      d += " >>";
      b.Set(bm_objs[i], std::vector<uint8_t>(d.begin(), d.end()));
    }

    std::string root = "<< /Type /Outlines";
    if (first_of[0] >= 0) {
      root += " /First " + std::to_string(bm_objs[first_of[0]]) + " 0 R /Last " +
              std::to_string(bm_objs[last_of[0]]) + " 0 R /Count " + std::to_string(n);
    }
    root += " >>";
    b.Set(outline_root, std::vector<uint8_t>(root.begin(), root.end()));
  }

  std::string catalog = "<< /Type /Catalog /Pages " + std::to_string(pages_obj) + " 0 R";
  if (!dests.empty()) {
    // 两种形式都写，兼容面最大：
    //   PDF 1.1：catalog /Dests 直接是 name -> dest 字典（供 /D /name 引用）。
    //   PDF 1.2+：catalog /Names /Dests 名字树（供 /D (string) 引用）。
    // 部分实现（如 PyMuPDF）只认前者的形状，只写名字树会被误当成旧式字典。
    std::string old_style = " /Dests << ";
    std::string tree = " /Names << /Dests << /Names [";
    for (const DestEntry& e : dests) {
      const std::string arr = "[" + std::to_string(page_objs[static_cast<size_t>(e.page)]) +
                              " 0 R /XYZ " + Fmt(e.x) + " " + Fmt(e.y) + " null] ";
      old_style += "/" + PdfName(e.name) + " " + arr;
      tree += PdfTextString(e.name) + " " + arr;
    }
    old_style += ">>";
    tree += "] >> >>";
    catalog += old_style + tree;
  }
  if (outline_root != 0) catalog += " /Outlines " + std::to_string(outline_root) + " 0 R";
  catalog += " >>";
  b.Set(catalog_obj, catalog);

  std::string info;
  if (!options.title.empty()) {
    const std::string info_dict =
        "<< /Title (" + EscapeText(options.title) + ") /Producer (DocCore) >>";
    const int info_obj = b.Add(std::vector<uint8_t>(info_dict.begin(), info_dict.end()));
    info = std::to_string(info_obj) + " 0 R";
  }
  return b.Finish(catalog_obj, info);
}

}  // namespace doccore
