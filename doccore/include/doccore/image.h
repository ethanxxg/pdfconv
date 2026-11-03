// DocCore - 图片解码（PNG / BMP / GIF 首帧），输出统一的 RGBA 位图。
// Design refs: docs/02 §3.7（图片形状）、§3.5.1（光栅化）
//
// 布局层只需要 "能画" 的 RGBA，不关心源格式。这里覆盖 P7 需要的无损格式；
// JPEG 需要 DCT 解码器，留到 P9，调用方拿到 Unsupported 后画占位框。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/layout.h"

namespace doccore {

// 从魔数推断 MIME（"image/png" / "image/jpeg" / "image/bmp" / "image/gif"）。
// 认不出时返回空串。
std::string ImageMimeFromBytes(const std::vector<uint8_t>& bytes);

// 只读尺寸，不做解码。失败返回 false。
bool ImageSize(const std::vector<uint8_t>& bytes, int* width, int* height);

// 解码为 RGBA。抛 Error(Corrupt) 表示格式识别但数据坏了，
// Error(Unsupported) 表示格式暂不支持（如 JPEG）。
ImageData DecodeImage(const std::vector<uint8_t>& bytes);

// 同上，但失败时返回 false 而不是抛异常（布局层用这个）。
bool TryDecodeImage(const std::vector<uint8_t>& bytes, ImageData* out,
                    WarningSink* warnings = nullptr);

}  // namespace doccore
