#include "doccore/crypto.h"

#include <cstring>

#include "doccore/core.h"

namespace doccore {
namespace crypto {
namespace {

inline uint32_t Rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline uint32_t Rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
inline uint64_t Rotr64(uint64_t v, int n) { return (v >> n) | (v << (64 - n)); }

}  // namespace

// ---- MD5（RFC 1321）------------------------------------------------------

namespace {

struct Md5State {
  uint32_t h[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
  uint64_t total = 0;
  uint8_t buf[64];
  size_t used = 0;
};

void Md5Block(Md5State* s, const uint8_t* p) {
  static const uint32_t kK[64] = {
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
      0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
      0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
      0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
      0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
      0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
      0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
      0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
      0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
      0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
      0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
  static const int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f = 0;
    int g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    const uint32_t tmp = d;
    d = c;
    c = b;
    b = b + Rotl32(a + f + kK[i] + m[g], kS[i]);
    a = tmp;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
}

void Md5Update(Md5State* s, const uint8_t* data, size_t size) {
  s->total += size;
  while (size > 0) {
    const size_t take = (size < 64 - s->used) ? size : 64 - s->used;
    std::memcpy(s->buf + s->used, data, take);
    s->used += take;
    data += take;
    size -= take;
    if (s->used == 64) {
      Md5Block(s, s->buf);
      s->used = 0;
    }
  }
}

Bytes Md5Final(Md5State* s) {
  const uint64_t bits = s->total * 8;
  uint8_t pad[72] = {0x80};
  size_t pad_len = (s->used < 56) ? (56 - s->used) : (120 - s->used);
  Md5Update(s, pad, pad_len);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * i));
  Md5Update(s, len, 8);
  Bytes out(16);
  for (int i = 0; i < 4; ++i) {
    out[i * 4 + 0] = static_cast<uint8_t>(s->h[i]);
    out[i * 4 + 1] = static_cast<uint8_t>(s->h[i] >> 8);
    out[i * 4 + 2] = static_cast<uint8_t>(s->h[i] >> 16);
    out[i * 4 + 3] = static_cast<uint8_t>(s->h[i] >> 24);
  }
  return out;
}

}  // namespace

Bytes Md5(const uint8_t* data, size_t size) {
  Md5State s;
  Md5Update(&s, data, size);
  return Md5Final(&s);
}

// ---- SHA-256 / SHA-384 / SHA-512（FIPS 180-4）----------------------------

namespace {

const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

void Sha256Block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
    const uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void Sha512Block(uint64_t h[8], const uint8_t* p) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v = (v << 8) | p[i * 8 + b];
    w[i] = v;
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 80; ++i) {
    const uint64_t s1 = Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41);
    const uint64_t ch = (e & f) ^ (~e & g);
    const uint64_t t1 = hh + s1 + ch + kSha512K[i] + w[i];
    const uint64_t s0 = Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39);
    const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

// 通用 Merkle–Damgård 收尾：追加 0x80、补零到 (block-8/16)、写大端位长。
// 只更新状态，序列化由调用方在收尾之后做（状态在最后一轮才最终确定）。
template <typename BlockFn>
void HashFinish(const uint8_t* tail, size_t used, uint64_t bit_len, size_t block,
                BlockFn block_fn) {
  uint8_t buf[256] = {0};
  size_t total = used;
  std::memcpy(buf, tail, used);
  buf[total++] = 0x80;
  const size_t len_bytes = (block == 64) ? 8 : 16;
  if (total > block - len_bytes) {
    std::memset(buf + total, 0, block - total);
    block_fn(buf);
    total = 0;
    std::memset(buf, 0, block);
  }
  std::memset(buf + total, 0, block - total);
  // SHA-512 的长度字段是 128 位；bit_len 是 64 位，高 8 字节固定为 0。
  // 直接写 `bit_len >> (8 * i)` 在 i >= 8 时移位超宽是未定义行为。
  for (size_t i = 0; i < len_bytes; ++i) {
    buf[block - 1 - i] = (i < 8) ? static_cast<uint8_t>(bit_len >> (8 * i)) : uint8_t{0};
  }
  block_fn(buf);
}

// SHA-1（FIPS 180-4 §6.1）。分组 64 字节、大端，与 SHA-256 共用 HashFinish。
void Sha1Block(uint32_t h[5], const uint8_t* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
           (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = (v << 1) | (v >> 31);
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f = 0, k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
    e = d;
    d = c;
    c = (b << 30) | (b >> 2);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

Bytes Sha1(const uint8_t* data, size_t size) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  size_t off = 0;
  while (size - off >= 64) {
    Sha1Block(h, data + off);
    off += 64;
  }
  uint8_t tail[64] = {0};
  std::memcpy(tail, data + off, size - off);
  auto block_fn = [&h](const uint8_t* p) { Sha1Block(h, p); };
  HashFinish(tail, size - off, static_cast<uint64_t>(size) * 8, 64, block_fn);
  Bytes out(20);
  for (int i = 0; i < 5; ++i) {
    out[i * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
  return out;
}

Bytes Sha256(const uint8_t* data, size_t size) {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  size_t off = 0;
  while (size - off >= 64) {
    Sha256Block(h, data + off);
    off += 64;
  }
  uint8_t tail[64] = {0};
  std::memcpy(tail, data + off, size - off);
  auto block_fn = [&h](const uint8_t* p) { Sha256Block(h, p); };
  HashFinish(tail, size - off, static_cast<uint64_t>(size) * 8, 64, block_fn);
  Bytes out(32);
  for (int i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
  return out;
}

Bytes Sha512(const uint8_t* data, size_t size) {
  uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                   0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                   0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
  size_t off = 0;
  while (size - off >= 128) {
    Sha512Block(h, data + off);
    off += 128;
  }
  uint8_t tail[128] = {0};
  std::memcpy(tail, data + off, size - off);
  auto block_fn = [&h](const uint8_t* p) { Sha512Block(h, p); };
  HashFinish(tail, size - off, static_cast<uint64_t>(size) * 8, 128, block_fn);
  Bytes out(64);
  for (int i = 0; i < 8; ++i) {
    for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<uint8_t>(h[i] >> (56 - 8 * b));
  }
  return out;
}

Bytes Sha384(const uint8_t* data, size_t size) {
  uint64_t h[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
                   0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                   0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
  size_t off = 0;
  while (size - off >= 128) {
    Sha512Block(h, data + off);
    off += 128;
  }
  uint8_t tail[128] = {0};
  std::memcpy(tail, data + off, size - off);
  auto block_fn = [&h](const uint8_t* p) { Sha512Block(h, p); };
  HashFinish(tail, size - off, static_cast<uint64_t>(size) * 8, 128, block_fn);
  Bytes out(48);  // SHA-384 是 SHA-512 截断到 384 位
  for (int i = 0; i < 6; ++i) {
    for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<uint8_t>(h[i] >> (56 - 8 * b));
  }
  return out;
}

// ---- RC4 -----------------------------------------------------------------

void Rc4(const uint8_t* key, size_t key_len, uint8_t* data, size_t size) {
  if (key_len == 0 || size == 0) return;
  uint8_t s[256];
  for (int i = 0; i < 256; ++i) s[i] = static_cast<uint8_t>(i);
  uint8_t j = 0;
  for (int i = 0; i < 256; ++i) {
    j = static_cast<uint8_t>(j + s[i] + key[i % key_len]);
    const uint8_t t = s[i];
    s[i] = s[j];
    s[j] = t;
  }
  uint8_t i = 0;
  j = 0;
  for (size_t n = 0; n < size; ++n) {
    i = static_cast<uint8_t>(i + 1);
    j = static_cast<uint8_t>(j + s[i]);
    const uint8_t t = s[i];
    s[i] = s[j];
    s[j] = t;
    data[n] ^= s[static_cast<uint8_t>(s[i] + s[j])];
  }
}

// ---- AES -----------------------------------------------------------------

namespace {

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

uint8_t kInvSbox[256];

struct InvSboxInit {
  InvSboxInit() {
    for (int i = 0; i < 256; ++i) kInvSbox[kSbox[i]] = static_cast<uint8_t>(i);
  }
} g_inv_sbox_init;

inline uint8_t Xtime(uint8_t v) {
  return static_cast<uint8_t>((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
}

inline uint8_t Gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b != 0) {
    if ((b & 1) != 0) r ^= a;
    a = Xtime(a);
    b = static_cast<uint8_t>(b >> 1);
  }
  return r;
}

}  // namespace

Aes::Aes(const uint8_t* key, size_t key_len) {
  if (key_len != 16 && key_len != 24 && key_len != 32) {
    throw Error(ErrorCode::InvalidArgument, "aes: key length must be 16, 24 or 32 bytes");
  }
  rounds_ = static_cast<int>(key_len / 4) + 6;
  ExpandKey(key, key_len);
}

void Aes::ExpandKey(const uint8_t* key, size_t key_len) {
  const int nk = static_cast<int>(key_len / 4);
  const int total = 4 * (rounds_ + 1);
  std::memcpy(round_keys_, key, key_len);
  uint8_t rcon = 1;
  for (int i = nk; i < total; ++i) {
    uint8_t t[4];
    std::memcpy(t, round_keys_ + (i - 1) * 4, 4);
    if (i % nk == 0) {
      const uint8_t first = t[0];
      t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
      t[1] = kSbox[t[2]];
      t[2] = kSbox[t[3]];
      t[3] = kSbox[first];
      rcon = Xtime(rcon);
    } else if (nk > 6 && i % nk == 4) {
      for (int b = 0; b < 4; ++b) t[b] = kSbox[t[b]];
    }
    for (int b = 0; b < 4; ++b) {
      round_keys_[i * 4 + b] = static_cast<uint8_t>(round_keys_[(i - nk) * 4 + b] ^ t[b]);
    }
  }
}

void Aes::EncryptBlock(uint8_t s[16]) const {
  auto add_round_key = [&](int round) {
    for (int i = 0; i < 16; ++i) s[i] ^= round_keys_[round * 16 + i];
  };
  add_round_key(0);
  for (int round = 1; round <= rounds_; ++round) {
    // SubBytes + ShiftRows（状态按列主序：s[c*4+r]）
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[c * 4 + r] = kSbox[s[((c + r) % 4) * 4 + r]];
    }
    std::memcpy(s, t, 16);
    if (round != rounds_) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t* a = s + c * 4;
        const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
        s[c * 4 + 0] = static_cast<uint8_t>(a0 ^ all ^ Xtime(static_cast<uint8_t>(a0 ^ a1)));
        s[c * 4 + 1] = static_cast<uint8_t>(a1 ^ all ^ Xtime(static_cast<uint8_t>(a1 ^ a2)));
        s[c * 4 + 2] = static_cast<uint8_t>(a2 ^ all ^ Xtime(static_cast<uint8_t>(a2 ^ a3)));
        s[c * 4 + 3] = static_cast<uint8_t>(a3 ^ all ^ Xtime(static_cast<uint8_t>(a3 ^ a0)));
      }
    }
    add_round_key(round);
  }
}

void Aes::DecryptBlock(uint8_t s[16]) const {
  auto add_round_key = [&](int round) {
    for (int i = 0; i < 16; ++i) s[i] ^= round_keys_[round * 16 + i];
  };
  add_round_key(rounds_);
  for (int round = rounds_ - 1; round >= 0; --round) {
    // InvShiftRows + InvSubBytes
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) t[((c + r) % 4) * 4 + r] = kInvSbox[s[c * 4 + r]];
    }
    std::memcpy(s, t, 16);
    add_round_key(round);
    if (round != 0) {
      for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = s[c * 4 + 0], a1 = s[c * 4 + 1];
        const uint8_t a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
        s[c * 4 + 0] = static_cast<uint8_t>(Gmul(a0, 14) ^ Gmul(a1, 11) ^ Gmul(a2, 13) ^ Gmul(a3, 9));
        s[c * 4 + 1] = static_cast<uint8_t>(Gmul(a0, 9) ^ Gmul(a1, 14) ^ Gmul(a2, 11) ^ Gmul(a3, 13));
        s[c * 4 + 2] = static_cast<uint8_t>(Gmul(a0, 13) ^ Gmul(a1, 9) ^ Gmul(a2, 14) ^ Gmul(a3, 11));
        s[c * 4 + 3] = static_cast<uint8_t>(Gmul(a0, 11) ^ Gmul(a1, 13) ^ Gmul(a2, 9) ^ Gmul(a3, 14));
      }
    }
  }
}

// ---- CBC + 填充 ----------------------------------------------------------

void Pkcs7Pad(Bytes* data, size_t block) {
  const size_t pad = block - (data->size() % block);
  data->insert(data->end(), pad, static_cast<uint8_t>(pad));
}

bool Pkcs7Unpad(Bytes* data, size_t block) {
  if (data->empty() || data->size() % block != 0) return false;
  const size_t pad = data->back();
  if (pad == 0 || pad > block || pad > data->size()) return false;
  for (size_t i = 0; i < pad; ++i) {
    if ((*data)[data->size() - 1 - i] != pad) return false;
  }
  data->resize(data->size() - pad);
  return true;
}

void AesCbcEncrypt(const Aes& aes, const uint8_t iv[16], Bytes* data) {
  Pkcs7Pad(data, 16);
  uint8_t chain[16];
  std::memcpy(chain, iv, 16);
  for (size_t off = 0; off < data->size(); off += 16) {
    uint8_t* block = data->data() + off;
    for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
    aes.EncryptBlock(block);
    std::memcpy(chain, block, 16);
  }
}

bool AesCbcDecrypt(const Aes& aes, const uint8_t iv[16], Bytes* data) {
  if (data->size() % 16 != 0) return false;
  uint8_t chain[16];
  std::memcpy(chain, iv, 16);
  for (size_t off = 0; off < data->size(); off += 16) {
    uint8_t* block = data->data() + off;
    uint8_t cipher[16];
    std::memcpy(cipher, block, 16);
    aes.DecryptBlock(block);
    for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
    std::memcpy(chain, cipher, 16);
  }
  return Pkcs7Unpad(data, 16);
}

}  // namespace crypto
}  // namespace doccore
