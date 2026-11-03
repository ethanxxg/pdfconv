// DocCore - QR / Code128 生成。Design refs: docs/02 §3.4 / F-601.
#include "doccore/barcode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "doccore/render.h"

namespace doccore {
namespace barcode {
namespace {

using doccore::Error;
using doccore::ErrorCode;

// ---- QR 规格表（ISO/IEC 18004；版本 1..40，EC 等级 L/M/Q/H）----------------

struct EcCfg {
  uint8_t ec_per_block;
  uint8_t g1_blocks, g1_data;
  uint8_t g2_blocks, g2_data;  // g2 为 0 表示无第二组
};

// [version-1][level]，level 0=L 1=M 2=Q 3=H。
const EcCfg kEc[40][4] = {
    {{7, 1, 19, 0, 0}, {10, 1, 16, 0, 0}, {13, 1, 13, 0, 0}, {17, 1, 9, 0, 0}},
    {{10, 1, 34, 0, 0}, {16, 1, 28, 0, 0}, {22, 1, 22, 0, 0}, {28, 1, 16, 0, 0}},
    {{15, 1, 55, 0, 0}, {26, 1, 44, 0, 0}, {18, 2, 17, 0, 0}, {22, 2, 13, 0, 0}},
    {{20, 1, 80, 0, 0}, {18, 2, 32, 0, 0}, {26, 2, 24, 0, 0}, {16, 4, 9, 0, 0}},
    {{26, 1, 108, 0, 0}, {24, 2, 43, 0, 0}, {18, 2, 15, 2, 16}, {22, 2, 11, 2, 12}},
    {{18, 2, 68, 0, 0}, {16, 4, 27, 0, 0}, {24, 4, 19, 0, 0}, {28, 4, 15, 0, 0}},
    {{20, 2, 78, 0, 0}, {18, 4, 31, 0, 0}, {18, 2, 14, 4, 15}, {26, 4, 13, 1, 14}},
    {{24, 2, 97, 0, 0}, {22, 2, 38, 2, 39}, {22, 4, 18, 2, 19}, {26, 4, 14, 2, 15}},
    {{30, 2, 116, 0, 0}, {22, 3, 36, 2, 37}, {20, 4, 16, 4, 17}, {24, 4, 12, 4, 13}},
    {{18, 2, 68, 2, 69}, {26, 4, 43, 1, 44}, {24, 6, 19, 2, 20}, {28, 6, 15, 2, 16}},
    {{20, 4, 81, 0, 0}, {30, 1, 50, 4, 51}, {28, 4, 22, 4, 23}, {24, 3, 12, 8, 13}},
    {{24, 2, 92, 2, 93}, {22, 6, 36, 2, 37}, {26, 4, 20, 6, 21}, {28, 7, 14, 4, 15}},
    {{26, 4, 107, 0, 0}, {22, 8, 37, 1, 38}, {24, 8, 20, 4, 21}, {22, 12, 11, 4, 12}},
    {{30, 3, 115, 1, 116}, {24, 4, 40, 5, 41}, {20, 11, 16, 5, 17}, {24, 11, 12, 5, 13}},
    {{22, 5, 87, 1, 88}, {24, 5, 41, 5, 42}, {30, 5, 24, 7, 25}, {24, 11, 12, 7, 13}},
    {{24, 5, 98, 1, 99}, {28, 7, 45, 3, 46}, {24, 15, 19, 2, 20}, {30, 3, 15, 13, 16}},
    {{28, 1, 107, 5, 108}, {28, 10, 46, 1, 47}, {28, 1, 22, 15, 23}, {28, 2, 14, 17, 15}},
    {{30, 5, 120, 1, 121}, {26, 9, 43, 4, 44}, {28, 17, 22, 1, 23}, {28, 2, 14, 19, 15}},
    {{28, 3, 113, 4, 114}, {26, 3, 44, 11, 45}, {26, 17, 21, 4, 22}, {26, 9, 13, 16, 14}},
    {{28, 3, 107, 5, 108}, {26, 3, 41, 13, 42}, {30, 15, 24, 5, 25}, {28, 15, 15, 10, 16}},
    {{28, 4, 116, 4, 117}, {26, 17, 42, 0, 0}, {28, 17, 22, 6, 23}, {30, 19, 16, 6, 17}},
    {{28, 2, 111, 7, 112}, {28, 17, 46, 0, 0}, {30, 7, 24, 16, 25}, {24, 34, 13, 0, 0}},
    {{30, 4, 121, 5, 122}, {28, 4, 47, 14, 48}, {30, 11, 24, 14, 25}, {30, 16, 15, 14, 16}},
    {{30, 6, 117, 4, 118}, {28, 6, 45, 14, 46}, {30, 11, 24, 16, 25}, {30, 30, 16, 2, 17}},
    {{26, 8, 106, 4, 107}, {28, 8, 47, 13, 48}, {30, 7, 24, 22, 25}, {30, 22, 15, 13, 16}},
    {{28, 10, 114, 2, 115}, {28, 19, 46, 4, 47}, {28, 28, 22, 6, 23}, {30, 33, 16, 4, 17}},
    {{30, 8, 122, 4, 123}, {28, 22, 45, 3, 46}, {30, 8, 23, 26, 24}, {30, 12, 15, 28, 16}},
    {{30, 3, 117, 10, 118}, {28, 3, 45, 23, 46}, {30, 4, 24, 31, 25}, {30, 11, 15, 31, 16}},
    {{30, 7, 116, 7, 117}, {28, 21, 45, 7, 46}, {30, 1, 23, 37, 24}, {30, 19, 15, 26, 16}},
    {{30, 5, 115, 10, 116}, {28, 19, 47, 10, 48}, {30, 15, 24, 25, 25}, {30, 23, 15, 25, 16}},
    {{30, 13, 115, 3, 116}, {28, 2, 46, 29, 47}, {30, 42, 24, 1, 25}, {30, 23, 15, 28, 16}},
    {{30, 17, 115, 0, 0}, {28, 10, 46, 23, 47}, {30, 10, 24, 35, 25}, {30, 19, 15, 35, 16}},
    {{30, 17, 115, 1, 116}, {28, 14, 46, 21, 47}, {30, 29, 24, 19, 25}, {30, 11, 15, 46, 16}},
    {{30, 13, 115, 6, 116}, {28, 14, 46, 23, 47}, {30, 44, 24, 7, 25}, {30, 59, 16, 1, 17}},
    {{30, 12, 121, 7, 122}, {28, 12, 47, 26, 48}, {30, 39, 24, 14, 25}, {30, 22, 15, 41, 16}},
    {{30, 6, 121, 14, 122}, {28, 6, 47, 34, 48}, {30, 46, 24, 10, 25}, {30, 2, 15, 64, 16}},
    {{30, 17, 122, 4, 123}, {28, 29, 46, 14, 47}, {30, 49, 24, 10, 25}, {30, 24, 15, 46, 16}},
    {{30, 4, 122, 18, 123}, {28, 13, 46, 32, 47}, {30, 48, 24, 14, 25}, {30, 42, 15, 32, 16}},
    {{30, 20, 117, 4, 118}, {28, 40, 47, 7, 48}, {30, 43, 24, 22, 25}, {30, 10, 15, 67, 16}},
    {{30, 19, 118, 6, 119}, {28, 18, 47, 31, 48}, {30, 34, 24, 34, 25}, {30, 20, 15, 61, 16}},
};

const uint16_t kTotalCw[40] = {26, 44, 70, 100, 134, 172, 196, 242, 292, 346, 404, 466,
                               532, 581, 655, 733, 815, 901, 991, 1085, 1156, 1258, 1364,
                               1474, 1588, 1706, 1828, 1921, 2051, 2185, 2323, 2465, 2611,
                               2761, 2876, 3034, 3196, 3362, 3532, 3706};

// 对齐图案中心坐标（行列同）；未用位置补 0。版本 1 无对齐图案。
const int kAlign[40][7] = {
    {0},          {6, 18},       {6, 22},       {6, 26},       {6, 30},
    {6, 34},      {6, 22, 38},   {6, 24, 42},   {6, 26, 46},   {6, 28, 50},
    {6, 30, 54},  {6, 32, 58},   {6, 34, 62},   {6, 26, 46, 66},
    {6, 26, 48, 70}, {6, 26, 50, 74}, {6, 30, 54, 78}, {6, 30, 56, 82},
    {6, 30, 58, 86}, {6, 34, 62, 90}, {6, 28, 50, 72, 94}, {6, 26, 50, 74, 98},
    {6, 30, 54, 78, 102}, {6, 28, 54, 80, 106}, {6, 32, 58, 84, 110},
    {6, 30, 58, 86, 114}, {6, 34, 62, 90, 118}, {6, 26, 50, 74, 98, 122},
    {6, 30, 54, 78, 102, 126}, {6, 26, 52, 78, 104, 130}, {6, 30, 56, 82, 108, 134},
    {6, 34, 60, 86, 112, 138}, {6, 30, 58, 86, 114, 142}, {6, 34, 62, 90, 118, 146},
    {6, 30, 54, 78, 102, 126, 150}, {6, 24, 50, 76, 102, 128, 154},
    {6, 28, 54, 80, 106, 132, 158}, {6, 32, 58, 84, 110, 136, 162},
    {6, 26, 54, 82, 110, 138, 166}, {6, 30, 58, 86, 114, 142, 170},
};

// ---- GF(256)，本原多项式 0x11D ------------------------------------------

uint8_t GfMul(uint8_t a, uint8_t b) {
  uint16_t p = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) p ^= a;
    const bool hi = (a & 0x80) != 0;
    a = static_cast<uint8_t>(a << 1);
    if (hi) a ^= 0x1D;
    b >>= 1;
  }
  return static_cast<uint8_t>(p);
}

const std::array<uint8_t, 512>& GfExp() {
  static const std::array<uint8_t, 512> t = [] {
    std::array<uint8_t, 512> e{};
    uint8_t x = 1;
    for (int i = 0; i < 255; ++i) { e[i] = x; x = GfMul(x, 2); }
    for (int i = 255; i < 512; ++i) e[i] = e[i - 255];
    return e;
  }();
  return t;
}
uint8_t GfExpVal(int n) { return GfExp()[n % 255 < 0 ? (n % 255) + 255 : n % 255]; }

// 生成 RS 生成多项式（次数 = ec_len）。
std::vector<uint8_t> RsGenerator(int ec_len) {
  std::vector<uint8_t> g{1};
  for (int i = 0; i < ec_len; ++i) {
    std::vector<uint8_t> ng(g.size() + 1, 0);
    for (size_t j = 0; j < g.size(); ++j) {
      ng[j] ^= g[j];                        // 乘 x
      ng[j + 1] ^= GfMul(g[j], GfExpVal(i));  // 乘 α^i
    }
    g = ng;
  }
  return g;
}

// 对一段数据码字计算 ec_len 个 EC 码字。
std::vector<uint8_t> RsEncode(const std::vector<uint8_t>& data, int ec_len) {
  const std::vector<uint8_t> gen = RsGenerator(ec_len);
  std::vector<uint8_t> rem(ec_len, 0);
  for (uint8_t b : data) {
    const uint8_t factor = static_cast<uint8_t>(b ^ rem[0]);
    std::rotate(rem.begin(), rem.begin() + 1, rem.end());
    rem.back() = 0;
    for (int i = 0; i < ec_len; ++i) rem[i] ^= GfMul(gen[i + 1], factor);
  }
  return rem;
}

// ---------- QR 编码（ISO/IEC 18004，字节模式）--------------------------------

// 各版本数据码字之后的补余位（remainder bits）。
const int kRemainder[41] = {0, 0, 7, 7, 7, 7, 7, 0, 0, 0, 0, 0, 0, 0, 3, 3, 3,
                            3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3,
                            3, 0, 0, 0, 0, 0, 0};

// 每版本对齐图案中心个数。
const int kAlignCount[40] = {0, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4,
                             4, 4, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 7,
                             7, 7, 7, 7};

// 字节模式的字符计数指示位宽：1..9 → 8，10..40 → 16。
int CcIndicatorBits(int version) { return version <= 9 ? 8 : 16; }

// 该版本/等级可容纳的数据码字数。
int DataCapacityBytes(int version, int level) {
  const EcCfg& c = kEc[version - 1][level];
  return c.g1_blocks * c.g1_data + c.g2_blocks * c.g2_data;
}

// EC 等级 → 格式信息 2 位值（L=01 M=00 Q=11 H=10）。
int FormatBitsOfLevel(int level) {
  static const int kFmt[4] = {1, 0, 3, 2};
  return kFmt[level];
}

// 位缓冲（MSB first）。
struct BitBuf {
  std::vector<uint8_t> bytes;
  int nbits = 0;

  void Put(uint32_t val, int len) {
    for (int i = len - 1; i >= 0; --i) {
      if (nbits % 8 == 0) bytes.push_back(0);
      bytes.back() = static_cast<uint8_t>(bytes.back() |
                                          (((val >> i) & 1u) << (7 - nbits % 8)));
      ++nbits;
    }
  }
};

// ---------- 掩膜 ----------

bool MaskBit(int mask, int r, int c) {
  switch (mask) {
    case 0: return (r + c) % 2 == 0;
    case 1: return r % 2 == 0;
    case 2: return c % 3 == 0;
    case 3: return (r + c) % 3 == 0;
    case 4: return (r / 2 + c / 3) % 2 == 0;
    case 5: return (r * c) % 2 + (r * c) % 3 == 0;
    case 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
    case 7: return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
    default: return false;
  }
}

// 掩膜惩罚分（ISO/IEC 18004 表 11 的四条规则）。
long MaskPenalty(const std::vector<uint8_t>& m, int n) {
  long pen = 0;

  // 规则 1：行/列中连续同色 ≥5 的段。
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < n; ++i) {
      int run = 1;
      for (int j = 1; j < n; ++j) {
        const int a = pass == 0 ? m[static_cast<size_t>(i) * n + j]
                                : m[static_cast<size_t>(j) * n + i];
        const int b = pass == 0 ? m[static_cast<size_t>(i) * n + (j - 1)]
                                : m[static_cast<size_t>(j - 1) * n + i];
        if (a == b) {
          ++run;
        } else {
          if (run >= 5) pen += 3 + (run - 5);
          run = 1;
        }
      }
      if (run >= 5) pen += 3 + (run - 5);
    }
  }

  // 规则 2：2×2 同色块。
  for (int r = 0; r + 1 < n; ++r) {
    for (int c = 0; c + 1 < n; ++c) {
      const uint8_t v = m[static_cast<size_t>(r) * n + c];
      if (m[static_cast<size_t>(r) * n + c + 1] == v &&
          m[static_cast<size_t>(r + 1) * n + c] == v &&
          m[static_cast<size_t>(r + 1) * n + c + 1] == v) {
        pen += 3;
      }
    }
  }

  // 规则 3：类定位图案 1:1:3:1:1（一侧带 4 个浅色模块）。
  static const int kPatA[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
  static const int kPatB[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j + 11 <= n; ++j) {
        bool a = true, b = true;
        for (int t = 0; t < 11; ++t) {
          const int v = pass == 0 ? m[static_cast<size_t>(i) * n + j + t]
                                  : m[static_cast<size_t>(j + t) * n + i];
          if (v != kPatA[t]) a = false;
          if (v != kPatB[t]) b = false;
        }
        if (a) pen += 40;
        if (b) pen += 40;
      }
    }
  }

  // 规则 4：深浅比例偏离 50%。
  int dark = 0;
  for (uint8_t v : m) dark += v;
  const int total = n * n;
  int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
  if (k < 0) k = 0;
  pen += static_cast<long>(k) * 10;
  return pen;
}

// ---------- BCH ----------

uint32_t FormatInfoBits(int level, int mask) {
  const uint32_t data = (static_cast<uint32_t>(FormatBitsOfLevel(level)) << 3) |
                        static_cast<uint32_t>(mask);
  uint32_t rem = data;
  for (int i = 0; i < 10; ++i) {
    rem = (rem << 1) ^ (((rem >> 9) & 1u) * 0x537u);
  }
  return ((data << 10) | rem) ^ 0x5412u;
}

uint32_t VersionInfoBits(int version) {
  uint32_t rem = static_cast<uint32_t>(version);
  for (int i = 0; i < 12; ++i) {
    rem = (rem << 1) ^ (((rem >> 11) & 1u) * 0x1F25u);
  }
  return (static_cast<uint32_t>(version) << 12) | rem;
}

// ---------- 矩阵绘制 ----------

struct QrMatrix {
  int n = 0;
  std::vector<uint8_t> mod;   // 0/1 模块值
  std::vector<uint8_t> func;  // 1=功能模块（不参与掩膜与数据填充）

  bool IsFunc(int r, int c) const { return func[static_cast<size_t>(r) * n + c] != 0; }
  void SetF(int r, int c, int v) {
    mod[static_cast<size_t>(r) * n + c] = static_cast<uint8_t>(v);
    func[static_cast<size_t>(r) * n + c] = 1;
  }
  void Reserve(int r, int c) { func[static_cast<size_t>(r) * n + c] = 1; }
  void PutBit(int r, int c, uint32_t bits, int idx) {
    mod[static_cast<size_t>(r) * n + c] =
        static_cast<uint8_t>((bits >> idx) & 1u);
    func[static_cast<size_t>(r) * n + c] = 1;
  }
};

void DrawFunctionPatterns(QrMatrix& q, int version) {
  const int n = q.n;

  // 定位图案 + 分隔符（-1..7 相对中心 (3,3)，edge=4 即分隔符）。
  const int corners[3][2] = {{0, 0}, {0, n - 7}, {n - 7, 0}};
  for (const auto& co : corners) {
    const int r0 = co[0], c0 = co[1];
    for (int dr = -1; dr <= 7; ++dr) {
      for (int dc = -1; dc <= 7; ++dc) {
        const int r = r0 + dr, c = c0 + dc;
        if (r < 0 || r >= n || c < 0 || c >= n) continue;
        const int edge = std::max(std::abs(dr - 3), std::abs(dc - 3));
        q.SetF(r, c, (edge == 3 || edge <= 1) ? 1 : 0);
      }
    }
  }

  // 定时图案。
  for (int i = 8; i <= n - 9; ++i) {
    q.SetF(6, i, i % 2 == 0 ? 1 : 0);
    q.SetF(i, 6, i % 2 == 0 ? 1 : 0);
  }

  // 对齐图案（跳过与定位图案重叠的三个角）。
  const int* al = kAlign[version - 1];
  const int na = kAlignCount[version - 1];
  for (int ai = 0; ai < na; ++ai) {
    for (int aj = 0; aj < na; ++aj) {
      if ((ai == 0 && aj == 0) || (ai == 0 && aj == na - 1) ||
          (ai == na - 1 && aj == 0)) {
        continue;
      }
      const int cr = al[ai], cc = al[aj];
      for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
          const int edge = std::max(std::abs(dr), std::abs(dc));
          q.SetF(cr + dr, cc + dc, edge != 1 ? 1 : 0);
        }
      }
    }
  }

  // 预留格式信息位与版本信息位（稍后写入）。
  // 注意：QrMatrix::SetF/Reserve 的第一个参数是行、第二个是列。
  for (int i = 0; i <= 8; ++i) {
    q.Reserve(8, i);  // 第 8 行
    q.Reserve(i, 8);  // 第 8 列
  }
  for (int i = 0; i < 8; ++i) q.Reserve(8, n - 1 - i);       // 第 8 行右侧
  for (int i = 8; i < 15; ++i) q.Reserve(n - 15 + i, 8);    // 第 8 列下侧
  q.Reserve(n - 8, 8);  // 固定为深色的模块（第 n-8 行、第 8 列）
  if (version >= 7) {
    for (int i = 0; i < 18; ++i) {
      const int a = n - 11 + i % 3, b = i / 3;
      q.Reserve(a, b);
      q.Reserve(b, a);
    }
  }
}

void WriteFormatInfo(QrMatrix& q, int level, int mask) {
  const int n = q.n;
  const uint32_t bits = FormatInfoBits(level, mask);
  // 第一份：绕左上角定位图案。
  for (int i = 0; i <= 5; ++i) q.PutBit(i, 8, bits, i);        // 第 8 列，行 0..5
  q.PutBit(7, 8, bits, 6);                                     // (7, 8)
  q.PutBit(8, 8, bits, 7);                                     // (8, 8)
  q.PutBit(8, 7, bits, 8);                                     // (8, 7)
  for (int i = 9; i < 15; ++i) q.PutBit(8, 14 - i, bits, i);   // 第 8 行，列 5..0
  // 第二份：绕右上角与左下角定位图案。
  for (int i = 0; i < 8; ++i) q.PutBit(8, n - 1 - i, bits, i);       // 第 8 行右侧
  for (int i = 8; i < 15; ++i) q.PutBit(n - 15 + i, 8, bits, i);    // 第 8 列下侧
  q.SetF(n - 8, 8, 1);  // 固定深色模块
}

void WriteVersionInfo(QrMatrix& q, int version) {
  if (version < 7) return;
  const int n = q.n;
  const uint32_t bits = VersionInfoBits(version);
  for (int i = 0; i < 18; ++i) {
    const int a = n - 11 + i % 3, b = i / 3;
    q.mod[static_cast<size_t>(a) * n + b] =
        static_cast<uint8_t>((bits >> i) & 1u);
    q.func[static_cast<size_t>(a) * n + b] = 1;
    q.mod[static_cast<size_t>(b) * n + a] =
        static_cast<uint8_t>((bits >> i) & 1u);
    q.func[static_cast<size_t>(b) * n + a] = 1;
  }
}

// 按蛇形顺序把位流写入非功能模块。
void PlaceData(QrMatrix& q, const std::vector<uint8_t>& stream) {
  const int n = q.n;
  const size_t total = stream.size() * 8;
  size_t idx = 0;
  for (int right = n - 1; right >= 1; right -= 2) {
    if (right == 6) right = 5;
    for (int vert = 0; vert < n; ++vert) {
      for (int j = 0; j < 2; ++j) {
        const int c = right - j;
        const bool upward = ((right + 1) & 2) == 0;
        const int r = upward ? n - 1 - vert : vert;
        if (q.IsFunc(r, c)) continue;
        uint8_t bit = 0;
        if (idx < total) bit = (stream[idx / 8] >> (7 - idx % 8)) & 1u;
        ++idx;
        q.mod[static_cast<size_t>(r) * n + c] = bit;
      }
    }
  }
}

void ApplyMask(QrMatrix& q, int mask) {
  const int n = q.n;
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      if (q.IsFunc(r, c)) continue;
      if (MaskBit(mask, r, c)) {
        q.mod[static_cast<size_t>(r) * n + c] ^= 1u;
      }
    }
  }
}

}  // namespace


using doccore::Error;
using doccore::ErrorCode;
using doccore::EncodePng;
using doccore::RasterImage;

// ---- QR 公开 API ----------------------------------------------------------

const char* EcLevelName(int level) {
  static const char* kName[4] = {"L", "M", "Q", "H"};
  return kName[level];
}

QrCode QrEncode(std::string_view text, EcLevel ec) {
  const std::string data(text);
  const int len = static_cast<int>(data.size());
  const int level = static_cast<int>(ec);

  // 选最小可用版本。
  int version = 0;
  for (int v = 1; v <= 40; ++v) {
    if (DataCapacityBytes(v, level) * 8 >= 4 + CcIndicatorBits(v) + len * 8) {
      version = v;
      break;
    }
  }
  if (version == 0) {
    throw Error(ErrorCode::LimitExceeded,
                "QR payload of " + std::to_string(len) +
                    " bytes exceeds version 40 capacity at EC level " +
                    EcLevelName(level));
  }

  const EcCfg& cfg = kEc[version - 1][level];
  const int capacity = cfg.g1_blocks * cfg.g1_data + cfg.g2_blocks * cfg.g2_data;

  // 1) 数据位流：模式(4) + 计数 + 载荷 + 终止符 + 位对齐 + 填充字节。
  BitBuf bb;
  bb.Put(0x4, 4);
  bb.Put(static_cast<uint32_t>(len), CcIndicatorBits(version));
  for (unsigned char ch : data) bb.Put(ch, 8);

  const int cap_bits = capacity * 8;
  if (const int term = std::min(4, cap_bits - bb.nbits); term > 0) bb.Put(0, term);
  while (bb.nbits % 8 != 0) bb.Put(0, 1);
  static const uint8_t kPad[2] = {0xEC, 0x11};
  int padi = 0;
  while (static_cast<int>(bb.bytes.size()) < capacity) {
    bb.bytes.push_back(kPad[padi % 2]);
    ++padi;
  }

  // 2) 分块 + Reed-Solomon。
  std::vector<std::vector<uint8_t>> blocks;
  size_t pos = 0;
  auto take = [&](int nbytes) {
    std::vector<uint8_t> b(bb.bytes.begin() + pos, bb.bytes.begin() + pos + nbytes);
    pos += static_cast<size_t>(nbytes);
    return b;
  };
  for (int i = 0; i < cfg.g1_blocks; ++i) blocks.push_back(take(cfg.g1_data));
  for (int i = 0; i < cfg.g2_blocks; ++i) blocks.push_back(take(cfg.g2_data));
  for (auto& b : blocks) {
    const std::vector<uint8_t> ecc = RsEncode(b, cfg.ec_per_block);
    b.insert(b.end(), ecc.begin(), ecc.end());
  }

  // 3) 交织（先数据后 EC）。
  std::vector<uint8_t> stream;
  const int max_data = std::max(cfg.g1_data, cfg.g2_data);
  for (int i = 0; i < max_data; ++i) {
    for (auto& b : blocks) {
      if (i < static_cast<int>(b.size()) - cfg.ec_per_block) stream.push_back(b[i]);
    }
  }
  for (int i = 0; i < cfg.ec_per_block; ++i) {
    for (auto& b : blocks) {
      stream.push_back(b[static_cast<int>(b.size()) - cfg.ec_per_block + i]);
    }
  }

  // 4) 功能图案 + 数据 + 版本信息。
  QrMatrix q;
  q.n = 17 + 4 * version;
  q.mod.assign(static_cast<size_t>(q.n) * q.n, 0);
  q.func.assign(static_cast<size_t>(q.n) * q.n, 0);
  DrawFunctionPatterns(q, version);
  WriteVersionInfo(q, version);
  PlaceData(q, stream);

  // 5) 8 个掩膜中选惩罚最小者。
  int best_mask = 0;
  long best_pen = -1;
  for (int m = 0; m < 8; ++m) {
    QrMatrix cand = q;
    ApplyMask(cand, m);
    WriteFormatInfo(cand, level, m);
    const long pen = MaskPenalty(cand.mod, cand.n);
    if (best_pen < 0 || pen < best_pen) {
      best_pen = pen;
      best_mask = m;
    }
  }
  ApplyMask(q, best_mask);
  WriteFormatInfo(q, level, best_mask);

  QrCode out;
  out.size = q.n;
  out.modules = std::move(q.mod);
  return out;
}

std::vector<uint8_t> QrToPng(const QrCode& qr, int scale, int quiet) {
  if (scale < 1) scale = 1;
  if (quiet < 0) quiet = 0;
  const int px = (qr.size + quiet * 2) * scale;
  RasterImage img;
  img.width = px;
  img.height = px;
  img.rgba.assign(static_cast<size_t>(px) * px * 4, 0xFF);
  for (int r = 0; r < qr.size; ++r) {
    for (int c = 0; c < qr.size; ++c) {
      if (!qr.at(r, c)) continue;
      const int x0 = (c + quiet) * scale;
      const int y0 = (r + quiet) * scale;
      for (int y = y0; y < y0 + scale; ++y) {
        for (int x = x0; x < x0 + scale; ++x) {
          const size_t o = (static_cast<size_t>(y) * px + x) * 4;
          img.rgba[o + 0] = 0;
          img.rgba[o + 1] = 0;
          img.rgba[o + 2] = 0;
          img.rgba[o + 3] = 0xFF;
        }
      }
    }
  }
  return EncodePng(img);
}

// ---- Code128 公开 API -----------------------------------------------------

std::vector<int> Code128Encode(std::string_view text) {
  // ISO/IEC 15417 表 1：107 个符号的条空宽度（条起始，黑白交替）。
  static const char* const kPat[107] = {
      "212222", "222122", "222221", "121223", "121322", "131222", "122213",
      "122312", "132212", "221213", "221312", "231212", "112232", "122132",
      "122231", "113222", "123122", "123221", "223211", "221132", "221231",
      "213212", "223112", "312131", "311222", "321122", "321221", "312212",
      "322112", "322211", "212123", "212321", "232121", "111323", "131123",
      "131321", "112313", "132113", "132311", "211313", "231113", "231311",
      "112133", "112331", "132131", "113123", "113321", "133121", "313121",
      "211331", "231131", "213113", "213311", "213131", "311123", "311321",
      "331121", "312113", "312311", "332111", "314111", "221411", "431111",
      "111224", "111422", "121124", "121421", "141122", "141221", "112214",
      "112412", "122114", "122411", "142112", "142211", "241211", "221114",
      "413111", "241112", "134111", "111242", "121142", "121241", "114212",
      "124112", "124211", "411212", "421112", "421211", "212141", "214121",
      "412121", "111143", "111341", "131141", "114113", "114311", "411113",
      "411311", "113141", "114131", "311141", "411131", "211412", "211214",
      "211232", "233111"};

  std::vector<int> out_bars;
  auto push_pat = [&](int value) {
    for (const char* p = kPat[value]; *p; ++p) out_bars.push_back(*p - '0');
  };

  const int kStartB = 104;
  push_pat(kStartB);
  int checksum = kStartB;
  int weight = 1;
  for (unsigned char ch : text) {
    if (ch < 32 || ch > 126) {
      throw Error(ErrorCode::InvalidArgument,
                  std::string("Code128 supports ASCII 32..126 only, got byte ") +
                      std::to_string(static_cast<int>(ch)));
    }
    const int value = ch - 32;
    push_pat(value);
    checksum += value * weight;
    ++weight;
  }
  push_pat(checksum % 103);
  push_pat(106);
  out_bars.push_back(2);  // 停止码后接 2 模块终止条
  return out_bars;
}

std::vector<uint8_t> Code128ToPng(const std::vector<int>& bars, int module_px,
                                  int quiet_modules, int height_px) {
  if (module_px < 1) module_px = 1;
  if (quiet_modules < 0) quiet_modules = 0;
  if (height_px < 1) height_px = 1;
  int total_modules = 0;
  for (int w : bars) total_modules += w;
  const int width_modules = total_modules + 2 * quiet_modules;

  RasterImage img;
  img.width = width_modules * module_px;
  img.height = height_px;
  img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0xFF);

  int x = quiet_modules;
  bool dark = true;
  for (size_t i = 0; i < bars.size(); ++i) {
    if (dark) {
      for (int m = 0; m < bars[i]; ++m) {
        for (int y = 0; y < height_px; ++y) {
          for (int px = 0; px < module_px; ++px) {
            const int xx = (x + m) * module_px + px;
            const size_t o = (static_cast<size_t>(y) * img.width + xx) * 4;
            img.rgba[o + 0] = 0;
            img.rgba[o + 1] = 0;
            img.rgba[o + 2] = 0;
            img.rgba[o + 3] = 0xFF;
          }
        }
      }
    }
    x += bars[i];
    dark = !dark;
  }
  return EncodePng(img);
}

}  // namespace barcode
}  // namespace doccore
