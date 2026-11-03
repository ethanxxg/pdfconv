#include "doccore/render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "doccore/deflate.h"

namespace doccore {

namespace {

struct Pt {
  double x, y;
};

void Blend(uint8_t* dst, const Rgba& c, double cover) {
  const double a = (c.a / 255.0) * cover;
  if (a <= 0.0) return;
  dst[0] = static_cast<uint8_t>(c.r * a + dst[0] * (1.0 - a));
  dst[1] = static_cast<uint8_t>(c.g * a + dst[1] * (1.0 - a));
  dst[2] = static_cast<uint8_t>(c.b * a + dst[2] * (1.0 - a));
  dst[3] = static_cast<uint8_t>(255 * a + dst[3] * (1.0 - a));
}

void Flatten(const DrawPath& path, std::vector<std::vector<Pt>>& polys, double scale) {
  std::vector<Pt> cur;
  Pt last{0, 0};
  Pt start{0, 0};
  for (const auto& v : path.verbs) {
    switch (v.kind) {
      case PathVerbKind::MoveTo:
        if (cur.size() > 2) polys.push_back(cur);
        cur.clear();
        last = {v.x1 * scale, v.y1 * scale};
        start = last;
        cur.push_back(last);
        break;
      case PathVerbKind::LineTo:
        last = {v.x1 * scale, v.y1 * scale};
        cur.push_back(last);
        break;
      case PathVerbKind::CubicTo: {
        const Pt p0 = last;
        const Pt p1{v.x1 * scale, v.y1 * scale};
        const Pt p2{v.x2 * scale, v.y2 * scale};
        const Pt p3{v.x3 * scale, v.y3 * scale};
        const int steps = 16;
        for (int i = 1; i <= steps; ++i) {
          const double t = double(i) / steps;
          const double mt = 1.0 - t;
          Pt p;
          p.x = mt * mt * mt * p0.x + 3 * mt * mt * t * p1.x + 3 * mt * t * t * p2.x + t * t * t * p3.x;
          p.y = mt * mt * mt * p0.y + 3 * mt * mt * t * p1.y + 3 * mt * t * t * p2.y + t * t * t * p3.y;
          cur.push_back(p);
        }
        last = p3;
        break;
      }
      case PathVerbKind::Close:
        if (!cur.empty()) {
          cur.push_back(start);
          polys.push_back(cur);
          cur.clear();
        }
        last = start;
        break;
    }
  }
  if (cur.size() > 2) polys.push_back(cur);
}

void FillPolys(RasterImage& img, const std::vector<std::vector<Pt>>& polys, const Rgba& color) {
  if (polys.empty()) return;
  double miny = 1e9, maxy = -1e9;
  for (const auto& poly : polys) {
    for (const auto& p : poly) {
      miny = std::min(miny, p.y);
      maxy = std::max(maxy, p.y);
    }
  }
  const int y0 = std::max(0, static_cast<int>(std::floor(miny)));
  const int y1 = std::min(img.height - 1, static_cast<int>(std::ceil(maxy)));
  std::vector<double> xs;
  for (int y = y0; y <= y1; ++y) {
    const double sy = y + 0.5;
    xs.clear();
    for (const auto& poly : polys) {
      for (size_t i = 0; i + 1 < poly.size(); ++i) {
        const Pt& a = poly[i];
        const Pt& b = poly[i + 1];
        if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
          const double t = (sy - a.y) / (b.y - a.y);
          xs.push_back(a.x + t * (b.x - a.x));
        }
      }
    }
    if (xs.size() < 2) continue;
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
      const int xa = std::max(0, static_cast<int>(std::lround(xs[i])));
      const int xb = std::min(img.width - 1, static_cast<int>(std::lround(xs[i + 1])));
      for (int x = xa; x <= xb; ++x) {
        Blend(img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4, color, 1.0);
      }
    }
  }
}

void StrokeSegments(RasterImage& img, const std::vector<std::vector<Pt>>& polys, const Rgba& color,
                    double width_px) {
  const double half = std::max(0.5, width_px * 0.5);
  for (const auto& poly : polys) {
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
      const Pt a = poly[i];
      const Pt b = poly[i + 1];
      const int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - half)));
      const int x1 = std::min(img.width - 1, static_cast<int>(std::ceil(std::max(a.x, b.x) + half)));
      const int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - half)));
      const int y1 = std::min(img.height - 1, static_cast<int>(std::ceil(std::max(a.y, b.y) + half)));
      const double dx = b.x - a.x;
      const double dy = b.y - a.y;
      const double len2 = dx * dx + dy * dy;
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const double px = x + 0.5 - a.x;
          const double py = y + 0.5 - a.y;
          double t = len2 > 0 ? (px * dx + py * dy) / len2 : 0.0;
          t = std::max(0.0, std::min(1.0, t));
          const double cx = px - t * dx;
          const double cy = py - t * dy;
          const double dist = std::sqrt(cx * cx + cy * cy);
          if (dist <= half) {
            Blend(img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4, color,
                  1.0 - (dist / half) * 0.5);
          }
        }
      }
    }
  }
}

void BlitImage(RasterImage& img, const DrawImage& di, double scale) {
  const int dst_x = static_cast<int>(std::lround(di.x * scale));
  const int dst_y = static_cast<int>(std::lround(di.y * scale));
  const int dst_w = std::max(1, static_cast<int>(std::lround(di.w_pt * scale)));
  const int dst_h = std::max(1, static_cast<int>(std::lround(di.h_pt * scale)));
  if (di.image.width <= 0 || di.image.height <= 0) return;
  for (int y = 0; y < dst_h; ++y) {
    const int ty = dst_y + y;
    if (ty < 0 || ty >= img.height) continue;
    const int sy = y * di.image.height / dst_h;
    for (int x = 0; x < dst_w; ++x) {
      const int tx = dst_x + x;
      if (tx < 0 || tx >= img.width) continue;
      const int sx = x * di.image.width / dst_w;
      const uint8_t* src = di.image.rgba.data() + (static_cast<size_t>(sy) * di.image.width + sx) * 4;
      Rgba c{src[0], src[1], src[2], src[3]};
      Blend(img.rgba.data() + (static_cast<size_t>(ty) * img.width + tx) * 4, c, 1.0);
    }
  }
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void Chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& data) {
  PutBe32(out, static_cast<uint32_t>(data.size()));
  const size_t crc_start = out.size();
  out.insert(out.end(), tag, tag + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uint32_t crc = Crc32(out.data() + crc_start, out.size() - crc_start);
  PutBe32(out, crc);
}

}  // namespace

RasterImage RasterizePage(const LayoutPage& page, const RasterOptions& opt, WarningSink* warnings) {
  const double scale = opt.dpi / 72.0;
  RasterImage img;
  img.width = std::max(1, static_cast<int>(std::lround(page.width_pt * scale)));
  img.height = std::max(1, static_cast<int>(std::lround(page.height_pt * scale)));
  img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0);
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i + 0] = opt.background.r;
    img.rgba[i + 1] = opt.background.g;
    img.rgba[i + 2] = opt.background.b;
    img.rgba[i + 3] = opt.background.a;
  }

  for (const auto& op : page.ops) {
    if (const auto* path = std::get_if<DrawPath>(&op)) {
      std::vector<std::vector<Pt>> polys;
      Flatten(*path, polys, scale);
      if (path->has_fill) FillPolys(img, polys, path->fill);
      if (path->has_stroke) StrokeSegments(img, polys, path->stroke, path->stroke_pt * scale);
    } else if (const auto* di = std::get_if<DrawImage>(&op)) {
      BlitImage(img, *di, scale);
    } else if (std::get_if<DrawText>(&op) != nullptr) {
      if (warnings != nullptr) {
        warnings->Add({static_cast<int>(WarningCode::DrawOpSkipped), "render", "",
                       "text rasterization not implemented in this wave"});
      }
    }
  }
  return img;
}

std::vector<uint8_t> EncodePng(const RasterImage& image) {
  std::vector<uint8_t> out;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  out.insert(out.end(), sig, sig + 8);

  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, static_cast<uint32_t>(image.width));
  PutBe32(ihdr, static_cast<uint32_t>(image.height));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // color type RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  Chunk(out, "IHDR", ihdr);

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(image.height) * (image.width * 4 + 1));
  for (int y = 0; y < image.height; ++y) {
    raw.push_back(0);  // filter type none
    const uint8_t* row = image.rgba.data() + static_cast<size_t>(y) * image.width * 4;
    raw.insert(raw.end(), row, row + static_cast<size_t>(image.width) * 4);
  }
  // 真实 deflate 编码（P1 补齐）：同样的图像从数 MB 降到几十 KB。
  Chunk(out, "IDAT", ZlibCompress(raw.data(), raw.size()));
  Chunk(out, "IEND", {});
  return out;
}

}  // namespace doccore
