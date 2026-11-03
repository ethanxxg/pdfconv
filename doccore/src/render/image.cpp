// DocCore - 图片解码实现：PNG（8/16 位、调色板、tRNS、Adam7 交织）、BMP、GIF 首帧。
// Design refs: docs/02 §3.7、§3.5.1
#include "doccore/image.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "doccore/deflate.h"

namespace doccore {
namespace {

[[noreturn]] void ThrowCorrupt(const std::string& what) {
  throw Error(ErrorCode::Corrupt, "image: " + what);
}

void Warn(WarningSink* sink, const std::string& message) {
  if (sink == nullptr) return;
  Warning w;
  w.code = static_cast<int>(WarningCode::ImageSkipped);
  w.module = "image";
  w.message = message;
  sink->Add(w);
}

// ---- PNG -----------------------------------------------------------------

constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;  // 64 Mpx 上限，防止解压炸弹

struct PngInfo {
  int width = 0;
  int height = 0;
  int bit_depth = 8;
  int color_type = 0;
  int interlace = 0;
  std::vector<uint8_t> palette;   // RGB 三元组
  std::vector<uint8_t> palette_a; // 调色板 alpha（tRNS），可为空
  std::vector<uint8_t> trns;      // 灰度/真彩 tRNS 原始样本
  std::vector<uint8_t> idat;
};

int ChannelsForColorType(int color_type) {
  switch (color_type) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
  }
}

void ParseIhdr(const uint8_t* data, size_t size, PngInfo* info) {
  if (size < 13) ThrowCorrupt("IHDR truncated");
  info->width = static_cast<int>(ReadBe32(data));
  info->height = static_cast<int>(ReadBe32(data + 4));
  info->bit_depth = data[8];
  info->color_type = data[9];
  info->interlace = data[12];
  if (info->width <= 0 || info->height <= 0) ThrowCorrupt("bad dimensions");
  if (static_cast<uint64_t>(info->width) * static_cast<uint64_t>(info->height) > kMaxPixels) {
    throw Error(ErrorCode::LimitExceeded, "image: PNG too large");
  }
  if (ChannelsForColorType(info->color_type) == 0) ThrowCorrupt("bad color type");
  const bool depth_ok = info->bit_depth == 1 || info->bit_depth == 2 || info->bit_depth == 4 ||
                        info->bit_depth == 8 || info->bit_depth == 16;
  if (!depth_ok) ThrowCorrupt("bad bit depth");
  if (info->interlace != 0 && info->interlace != 1) ThrowCorrupt("bad interlace");
}

// 把一行原始字节按 bit_depth 展开成 8 位样本（16 位取高字节）。
void ExpandRow(const uint8_t* src, size_t src_bytes, int width, int channels, int bit_depth,
               std::vector<uint8_t>* out) {
  out->assign(static_cast<size_t>(width) * static_cast<size_t>(channels), 0);
  if (bit_depth == 8) {
    std::memcpy(out->data(), src, std::min<size_t>(src_bytes, out->size()));
    return;
  }
  if (bit_depth == 16) {
    const size_t samples = static_cast<size_t>(width) * static_cast<size_t>(channels);
    for (size_t i = 0; i < samples && i * 2 + 1 < src_bytes; ++i) (*out)[i] = src[i * 2];
    return;
  }
  // 1/2/4 位：高位在前
  const int per_byte = 8 / bit_depth;
  const int mask = (1 << bit_depth) - 1;
  const size_t samples = static_cast<size_t>(width) * static_cast<size_t>(channels);
  for (size_t i = 0; i < samples; ++i) {
    const size_t byte = i / static_cast<size_t>(per_byte);
    if (byte >= src_bytes) break;
    const int shift = 8 - bit_depth * (static_cast<int>(i % per_byte) + 1);
    (*out)[i] = static_cast<uint8_t>(((src[byte] >> shift) & mask) * 255 / mask);
  }
}

uint8_t Paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

void Unfilter(uint8_t* row, const uint8_t* prev, size_t bytes, int filter, int bpp) {
  switch (filter) {
    case 0:
      break;
    case 1:
      for (size_t i = static_cast<size_t>(bpp); i < bytes; ++i) row[i] = uint8_t(row[i] + row[i - bpp]);
      break;
    case 2:
      if (prev != nullptr) {
        for (size_t i = 0; i < bytes; ++i) row[i] = uint8_t(row[i] + prev[i]);
      }
      break;
    case 3:
      for (size_t i = 0; i < bytes; ++i) {
        const int left = i >= static_cast<size_t>(bpp) ? row[i - bpp] : 0;
        const int up = prev != nullptr ? prev[i] : 0;
        row[i] = uint8_t(row[i] + ((left + up) >> 1));
      }
      break;
    case 4:
      for (size_t i = 0; i < bytes; ++i) {
        const int left = i >= static_cast<size_t>(bpp) ? row[i - bpp] : 0;
        const int up = prev != nullptr ? prev[i] : 0;
        const int ul = (prev != nullptr && i >= static_cast<size_t>(bpp)) ? prev[i - bpp] : 0;
        row[i] = uint8_t(row[i] + Paeth(left, up, ul));
      }
      break;
    default:
      ThrowCorrupt("bad filter type");
  }
}

// 把一个样本转成 RGBA（tRNS 透明度也在这一步处理）。
void SampleToRgba(const uint8_t* s, const PngInfo& info, uint8_t* dst) {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  switch (info.color_type) {
    case 0:  // 灰度
      r = g = b = s[0];
      if (info.trns.size() >= 2 && s[0] == info.trns[1]) a = 0;
      break;
    case 2:  // 真彩
      r = s[0];
      g = s[1];
      b = s[2];
      if (info.trns.size() >= 6 && r == info.trns[1] && g == info.trns[3] &&
          b == info.trns[5]) {
        a = 0;
      }
      break;
    case 3: {  // 调色板
      const size_t index = s[0];
      const size_t base = index * 3;
      if (base + 2 < info.palette.size()) {
        r = info.palette[base];
        g = info.palette[base + 1];
        b = info.palette[base + 2];
      }
      if (index < info.palette_a.size()) a = info.palette_a[index];
      break;
    }
    case 4:  // 灰度 + alpha
      r = g = b = s[0];
      a = s[1];
      break;
    case 6:  // RGBA
      r = s[0];
      g = s[1];
      b = s[2];
      a = s[3];
      break;
    default:
      break;
  }
  dst[0] = r;
  dst[1] = g;
  dst[2] = b;
  dst[3] = a;
}

// 单个 pass（非交织时就是整图）。pass 尺寸按 PNG 规范里的 Adam7 表。
void DecodePass(const std::vector<uint8_t>& raw, size_t* offset, const PngInfo& info, int x0,
                int y0, int dx, int dy, std::vector<uint8_t>* rgba) {
  const int channels = ChannelsForColorType(info.color_type);
  const int pass_width = (info.width - x0 + dx - 1) / dx;
  const int pass_height = (info.height - y0 + dy - 1) / dy;
  if (pass_width <= 0 || pass_height <= 0) return;

  const int bits_per_pixel = channels * info.bit_depth;
  const size_t row_bytes = (static_cast<size_t>(pass_width) * bits_per_pixel + 7) / 8;
  const int bpp = std::max(1, bits_per_pixel / 8);

  std::vector<uint8_t> prev(row_bytes, 0);
  std::vector<uint8_t> cur(row_bytes, 0);
  std::vector<uint8_t> samples;
  for (int y = 0; y < pass_height; ++y) {
    if (*offset + 1 + row_bytes > raw.size()) ThrowCorrupt("PNG data truncated");
    const int filter = raw[*offset];
    ++*offset;
    std::memcpy(cur.data(), raw.data() + *offset, row_bytes);
    *offset += row_bytes;
    Unfilter(cur.data(), y == 0 ? nullptr : prev.data(), row_bytes, filter, bpp);
    ExpandRow(cur.data(), row_bytes, pass_width, channels, info.bit_depth, &samples);
    for (int x = 0; x < pass_width; ++x) {
      const int px = x0 + x * dx;
      const int py = y0 + y * dy;
      if (px >= info.width || py >= info.height) continue;
      const size_t dst = (static_cast<size_t>(py) * static_cast<size_t>(info.width) +
                          static_cast<size_t>(px)) * 4;
      SampleToRgba(samples.data() + static_cast<size_t>(x) * static_cast<size_t>(channels),
                   info, rgba->data() + dst);
    }
    prev.swap(cur);
  }
}

ImageData DecodePng(const std::vector<uint8_t>& bytes) {
  PngInfo info;
  size_t pos = 8;  // 跳过签名
  bool saw_ihdr = false;
  while (pos + 8 <= bytes.size()) {
    const uint32_t length = ReadBe32(bytes.data() + pos);
    const std::string type(reinterpret_cast<const char*>(bytes.data() + pos + 4), 4);
    const size_t data_pos = pos + 8;
    if (data_pos + length > bytes.size()) ThrowCorrupt("PNG chunk overruns file");
    const uint8_t* data = bytes.data() + data_pos;
    if (type == "IHDR") {
      ParseIhdr(data, length, &info);
      saw_ihdr = true;
    } else if (type == "PLTE") {
      info.palette.assign(data, data + length);
    } else if (type == "tRNS") {
      if (info.color_type == 3) {
        info.palette_a.assign(data, data + length);
      } else {
        info.trns.assign(data, data + length);
      }
    } else if (type == "IDAT") {
      info.idat.insert(info.idat.end(), data, data + length);
    } else if (type == "IEND") {
      break;
    }
    pos = data_pos + length + 4;  // 跳过 CRC
  }
  if (!saw_ihdr) ThrowCorrupt("PNG without IHDR");
  if (info.idat.empty()) ThrowCorrupt("PNG without IDAT");

  const uint64_t expected = static_cast<uint64_t>(info.width) * info.height * 4 +
                            static_cast<uint64_t>(info.height) * 8 + 1024;
  const std::vector<uint8_t> raw =
      ZlibDecompress(info.idat.data(), info.idat.size(), expected + (expected / 4));

  ImageData out;
  out.width = info.width;
  out.height = info.height;
  out.rgba.assign(static_cast<size_t>(info.width) * static_cast<size_t>(info.height) * 4, 0);
  size_t offset = 0;
  if (info.interlace == 0) {
    DecodePass(raw, &offset, info, 0, 0, 1, 1, &out.rgba);
  } else {
    static const int kX0[7] = {0, 4, 0, 2, 0, 1, 0};
    static const int kY0[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int kDx[7] = {8, 8, 4, 4, 2, 2, 1};
    static const int kDy[7] = {8, 8, 8, 4, 4, 2, 2};
    for (int pass = 0; pass < 7; ++pass) {
      DecodePass(raw, &offset, info, kX0[pass], kY0[pass], kDx[pass], kDy[pass], &out.rgba);
    }
  }
  return out;
}

// ---- BMP -----------------------------------------------------------------

ImageData DecodeBmp(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 54) ThrowCorrupt("BMP header truncated");
  const uint32_t data_offset = ReadLe32(bytes.data() + 10);
  const uint32_t header_size = ReadLe32(bytes.data() + 14);
  if (header_size < 40) throw Error(ErrorCode::Unsupported, "image: OS/2 BMP not supported");
  const int32_t width = static_cast<int32_t>(ReadLe32(bytes.data() + 18));
  const int32_t height_raw = static_cast<int32_t>(ReadLe32(bytes.data() + 22));
  const uint16_t planes = static_cast<uint16_t>(ReadLe16(bytes.data() + 26));
  const uint16_t bpp = static_cast<uint16_t>(ReadLe16(bytes.data() + 28));
  const uint32_t compression = ReadLe32(bytes.data() + 30);
  if (planes != 1) ThrowCorrupt("BMP bad plane count");
  if (width <= 0 || height_raw == 0) ThrowCorrupt("BMP bad dimensions");
  if (compression != 0 && compression != 3) {
    throw Error(ErrorCode::Unsupported, "image: compressed BMP not supported");
  }
  if (bpp != 24 && bpp != 32) {
    throw Error(ErrorCode::Unsupported, "image: only 24/32-bit BMP supported");
  }
  const bool top_down = height_raw < 0;
  const int height = top_down ? -height_raw : height_raw;
  if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxPixels) {
    throw Error(ErrorCode::LimitExceeded, "image: BMP too large");
  }
  const int bytes_per_pixel = bpp / 8;
  const size_t stride = ((static_cast<size_t>(width) * bytes_per_pixel) + 3) & ~size_t(3);
  if (data_offset + stride * static_cast<size_t>(height) > bytes.size()) {
    ThrowCorrupt("BMP pixel data truncated");
  }

  ImageData out;
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 255);
  for (int y = 0; y < height; ++y) {
    const int src_y = top_down ? y : (height - 1 - y);
    const uint8_t* row = bytes.data() + data_offset + stride * static_cast<size_t>(src_y);
    for (int x = 0; x < width; ++x) {
      const uint8_t* p = row + static_cast<size_t>(x) * bytes_per_pixel;
      const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)) * 4;
      out.rgba[dst + 0] = p[2];
      out.rgba[dst + 1] = p[1];
      out.rgba[dst + 2] = p[0];
      out.rgba[dst + 3] = bytes_per_pixel == 4 ? p[3] : 255;
    }
  }
  return out;
}

// ---- GIF（仅首帧，LZW 解码）---------------------------------------------

ImageData DecodeGif(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 13) ThrowCorrupt("GIF header truncated");
  const int width = static_cast<int>(ReadLe16(bytes.data() + 6));
  const int height = static_cast<int>(ReadLe16(bytes.data() + 8));
  const uint8_t flags = bytes[10];
  if (width <= 0 || height <= 0) ThrowCorrupt("GIF bad dimensions");
  if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxPixels) {
    throw Error(ErrorCode::LimitExceeded, "image: GIF too large");
  }
  size_t pos = 13;
  std::vector<uint8_t> global;
  if ((flags & 0x80) != 0) {
    const size_t count = size_t(2) << ((flags & 0x07) + 1);
    if (pos + count * 3 > bytes.size()) ThrowCorrupt("GIF global palette truncated");
    global.assign(bytes.begin() + static_cast<long>(pos),
                  bytes.begin() + static_cast<long>(pos + count * 3));
    pos += count * 3;
  }
  // 找第一个图像描述符，跳过扩展块。
  while (pos < bytes.size()) {
    const uint8_t block = bytes[pos++];
    if (block == 0x3B) break;  // trailer，没有图像
    if (block == 0x21) {       // 扩展块
      if (pos >= bytes.size()) ThrowCorrupt("GIF extension truncated");
      ++pos;  // label
      while (pos < bytes.size()) {
        const uint8_t size = bytes[pos++];
        if (size == 0) break;
        pos += size;
      }
      continue;
    }
    if (block != 0x2C) ThrowCorrupt("GIF unknown block");
    if (pos + 9 > bytes.size()) ThrowCorrupt("GIF descriptor truncated");
    const int fx = static_cast<int>(ReadLe16(bytes.data() + pos));
    const int fy = static_cast<int>(ReadLe16(bytes.data() + pos + 2));
    const int fw = static_cast<int>(ReadLe16(bytes.data() + pos + 4));
    const int fh = static_cast<int>(ReadLe16(bytes.data() + pos + 6));
    const uint8_t iflags = bytes[pos + 8];
    pos += 9;
    std::vector<uint8_t> palette = global;
    if ((iflags & 0x80) != 0) {
      const size_t count = size_t(2) << ((iflags & 0x07) + 1);
      if (pos + count * 3 > bytes.size()) ThrowCorrupt("GIF local palette truncated");
      palette.assign(bytes.begin() + static_cast<long>(pos),
                     bytes.begin() + static_cast<long>(pos + count * 3));
      pos += count * 3;
    }
    if (pos >= bytes.size()) ThrowCorrupt("GIF LZW code size missing");
    const int min_code_size = bytes[pos++];
    std::vector<uint8_t> lzw;
    while (pos < bytes.size()) {
      const uint8_t size = bytes[pos++];
      if (size == 0) break;
      if (pos + size > bytes.size()) ThrowCorrupt("GIF LZW data truncated");
      lzw.insert(lzw.end(), bytes.begin() + static_cast<long>(pos),
                 bytes.begin() + static_cast<long>(pos + size));
      pos += size;
    }

    // LZW 解码
    const int clear_code = 1 << min_code_size;
    const int end_code = clear_code + 1;
    std::vector<std::vector<uint8_t>> table;
    auto reset = [&] {
      table.clear();
      table.reserve(static_cast<size_t>(4096));
      for (int i = 0; i < clear_code; ++i) table.push_back({static_cast<uint8_t>(i)});
      table.push_back({});
      table.push_back({});
    };
    reset();
    int code_size = min_code_size + 1;
    int next_code = end_code + 1;
    std::vector<uint8_t> prev;
    std::vector<uint8_t> pixels;
    pixels.reserve(static_cast<size_t>(fw) * static_cast<size_t>(fh));
    uint32_t bitbuf = 0;
    int bitcount = 0;
    size_t cursor = 0;
    while (true) {
      while (bitcount < code_size) {
        if (cursor >= lzw.size()) { bitcount = 0; break; }
        bitbuf |= uint32_t(lzw[cursor++]) << bitcount;
        bitcount += 8;
      }
      if (bitcount < code_size) break;
      const int code = static_cast<int>(bitbuf & uint32_t((1 << code_size) - 1));
      bitbuf >>= code_size;
      bitcount -= code_size;
      if (code == clear_code) {
        reset();
        code_size = min_code_size + 1;
        next_code = end_code + 1;
        prev.clear();
        continue;
      }
      if (code == end_code) break;
      std::vector<uint8_t> entry;
      if (code < next_code && static_cast<size_t>(code) < table.size()) {
        entry = table[static_cast<size_t>(code)];
      } else if (!prev.empty()) {
        entry = prev;
        entry.push_back(prev.front());
      } else {
        ThrowCorrupt("GIF LZW code out of range");
      }
      pixels.insert(pixels.end(), entry.begin(), entry.end());
      if (!prev.empty() && next_code < 4096) {
        std::vector<uint8_t> added = prev;
        added.push_back(entry.front());
        if (static_cast<size_t>(next_code) >= table.size()) table.resize(static_cast<size_t>(next_code) + 1);
        table[static_cast<size_t>(next_code)] = std::move(added);
        ++next_code;
        if (next_code == (1 << code_size) && code_size < 12) ++code_size;
      }
      prev = std::move(entry);
    }

    ImageData out;
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    for (int y = 0; y < fh; ++y) {
      for (int x = 0; x < fw; ++x) {
        const int px = fx + x;
        const int py = fy + y;
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        const size_t index = static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x);
        if (index >= pixels.size()) continue;
        const size_t base = static_cast<size_t>(pixels[index]) * 3;
        const size_t dst = (static_cast<size_t>(py) * static_cast<size_t>(width) +
                            static_cast<size_t>(px)) * 4;
        if (base + 2 < palette.size()) {
          out.rgba[dst + 0] = palette[base];
          out.rgba[dst + 1] = palette[base + 1];
          out.rgba[dst + 2] = palette[base + 2];
        }
        out.rgba[dst + 3] = 255;
      }
    }
    return out;
  }
  ThrowCorrupt("GIF has no image block");
}

}  // namespace

std::string ImageMimeFromBytes(const std::vector<uint8_t>& bytes) {
  if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
      bytes[3] == 'G') {
    return "image/png";
  }
  if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) return "image/jpeg";
  if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
  if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') {
    return "image/gif";
  }
  return std::string();
}

bool ImageSize(const std::vector<uint8_t>& bytes, int* width, int* height) {
  if (width == nullptr || height == nullptr) return false;
  const std::string mime = ImageMimeFromBytes(bytes);
  if (mime == "image/png") {
    if (bytes.size() < 24) return false;
    *width = static_cast<int>(ReadBe32(bytes.data() + 16));
    *height = static_cast<int>(ReadBe32(bytes.data() + 20));
    return *width > 0 && *height > 0;
  }
  if (mime == "image/bmp") {
    if (bytes.size() < 26) return false;
    *width = static_cast<int32_t>(ReadLe32(bytes.data() + 18));
    const int32_t h = static_cast<int32_t>(ReadLe32(bytes.data() + 22));
    *height = h < 0 ? -h : h;
    return *width > 0 && *height > 0;
  }
  if (mime == "image/gif") {
    if (bytes.size() < 10) return false;
    *width = static_cast<int>(ReadLe16(bytes.data() + 6));
    *height = static_cast<int>(ReadLe16(bytes.data() + 8));
    return *width > 0 && *height > 0;
  }
  if (mime == "image/jpeg") {
    // 扫 SOF 段拿尺寸；C4/C8/CC 不是 SOF。
    size_t pos = 2;
    while (pos + 9 < bytes.size()) {
      if (bytes[pos] != 0xFF) {
        ++pos;
        continue;
      }
      const uint8_t marker = bytes[pos + 1];
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        pos += 2;
        continue;
      }
      const size_t length = (size_t(bytes[pos + 2]) << 8) | bytes[pos + 3];
      if (length < 2) return false;
      const bool is_sof = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                          marker != 0xC8 && marker != 0xCC;
      if (is_sof) {
        if (pos + 9 > bytes.size()) return false;
        *height = static_cast<int>((size_t(bytes[pos + 5]) << 8) | bytes[pos + 6]);
        *width = static_cast<int>((size_t(bytes[pos + 7]) << 8) | bytes[pos + 8]);
        return *width > 0 && *height > 0;
      }
      pos += 2 + length;
    }
    return false;
  }
  return false;
}

ImageData DecodeImage(const std::vector<uint8_t>& bytes) {
  const std::string mime = ImageMimeFromBytes(bytes);
  if (mime == "image/png") return DecodePng(bytes);
  if (mime == "image/bmp") return DecodeBmp(bytes);
  if (mime == "image/gif") return DecodeGif(bytes);
  if (mime == "image/jpeg") {
    throw Error(ErrorCode::Unsupported, "image: JPEG decoding is not implemented yet");
  }
  throw Error(ErrorCode::Unsupported, "image: unrecognized image format");
}

bool TryDecodeImage(const std::vector<uint8_t>& bytes, ImageData* out, WarningSink* warnings) {
  if (out == nullptr) return false;
  try {
    *out = DecodeImage(bytes);
    return true;
  } catch (const Error& e) {
    Warn(warnings, e.what());
    return false;
  }
}

}  // namespace doccore
