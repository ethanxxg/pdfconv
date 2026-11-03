// DocCore - 条码生成（QR / Code128）。Design refs: docs/02 §3.4 / F-601.
//
// 只做"生成可嵌入图像"：QR 输出模块矩阵并可渲染成 PNG；Code128 输出条空图案。
// 识别不在本模块（F-602 属 P2）。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace barcode {

enum class EcLevel : uint8_t { L, M, Q, H };

// QR 码：size×size 模块矩阵，modules[r*size+c]：1=黑 0=白。
struct QrCode {
  int size = 0;
  std::vector<uint8_t> modules;
  bool at(int r, int c) const {
    return modules[static_cast<size_t>(r) * size + c] != 0;
  }
};

// 生成 QR（字节模式，载荷按 UTF-8 编码）。数据超出所选 EC 等级与最高版本
// (40) 容量时抛 Error(LimitExceeded)，不输出残缺符号。ec 默认 M。
QrCode QrEncode(std::string_view text, EcLevel ec = EcLevel::M);

// 把 QR 渲染成 8-bit RGBA PNG（scale=每模块像素，quiet=静区模块数，默认 4）。
std::vector<uint8_t> QrToPng(const QrCode& qr, int scale = 8, int quiet = 4);

// ---- Code128 ------------------------------------------------------------

// 128 码（Code set B，ASCII 32..126）。返回每条"元素"的宽度（模块数），
// 黑白交替、以条（黑）起始；末位含停止码。bars[i] 为第 i 个元素的宽度。
std::vector<int> Code128Encode(std::string_view text);

// 把 Code128 图案渲染成 8-bit RGBA PNG（模块宽=模块像素，含静区）。
std::vector<uint8_t> Code128ToPng(const std::vector<int>& bars, int module_px = 2,
                                  int quiet_modules = 10, int height_px = 80);

}  // namespace barcode
}  // namespace doccore
