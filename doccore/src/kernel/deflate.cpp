#include "doccore/deflate.h"

#include "doccore/core.h"

namespace doccore {

namespace {

const uint32_t* CrcTable() {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  return table;
}

struct BitReader {
  const uint8_t* data;
  size_t size;
  size_t pos = 0;
  uint32_t buf = 0;
  int bits = 0;

  bool Fill(int need) {
    while (bits < need) {
      if (pos >= size) return false;
      buf |= uint32_t(data[pos++]) << bits;
      bits += 8;
    }
    return true;
  }

  uint32_t Get(int need) {
    if (!Fill(need)) throw Error(ErrorCode::Corrupt, "deflate: unexpected end of data");
    const uint32_t mask = need == 32 ? 0xFFFFFFFFu : ((1u << need) - 1u);
    const uint32_t v = buf & mask;
    buf >>= need;
    bits -= need;
    return v;
  }

  void Align() {
    buf = 0;
    bits = 0;
  }
};

struct Huffman {
  uint16_t count[16];
  uint16_t symbol[288];

  // Returns false when the code is over-subscribed or incomplete.
  bool Build(const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; ++i) count[i] = 0;
    for (int i = 0; i < n; ++i) count[lengths[i]]++;
    if (count[0] == n) return true;  // no codes at all, valid for distances
    int left = 1;
    for (int len = 1; len < 16; ++len) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    for (int i = 0; i < n; ++i) {
      if (lengths[i] != 0) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
  }

  int Decode(BitReader& br) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      code |= static_cast<int>(br.Get(1));
      const int cnt = count[len];
      if (code - first < cnt) return symbol[index + (code - first)];
      index += cnt;
      first = (first + cnt) << 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Emit(std::vector<uint8_t>& out, uint8_t b, uint64_t max_out) {
  if (out.size() >= max_out) throw Error(ErrorCode::LimitExceeded, "inflate output limit");
  out.push_back(b);
}

void InflateCodes(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out,
                  uint64_t max_out) {
  for (;;) {
    const int sym = lit.Decode(br);
    if (sym < 0) throw Error(ErrorCode::Corrupt, "deflate: bad literal code");
    if (sym < 256) {
      Emit(out, static_cast<uint8_t>(sym), max_out);
    } else if (sym == 256) {
      return;
    } else {
      const int li = sym - 257;
      if (li >= 29) throw Error(ErrorCode::Corrupt, "deflate: bad length symbol");
      const int len = kLenBase[li] + static_cast<int>(br.Get(kLenExtra[li]));
      const int ds = dist.Decode(br);
      if (ds < 0 || ds >= 30) throw Error(ErrorCode::Corrupt, "deflate: bad distance code");
      const int d = kDistBase[ds] + static_cast<int>(br.Get(kDistExtra[ds]));
      if (d > static_cast<int>(out.size())) throw Error(ErrorCode::Corrupt, "deflate: distance too far");
      for (int i = 0; i < len; ++i) {
        Emit(out, out[out.size() - static_cast<size_t>(d)], max_out);
      }
    }
  }
}

void BuildFixed(Huffman& lit, Huffman& dist) {
  uint8_t lengths[288];
  for (int i = 0; i < 144; ++i) lengths[i] = 8;
  for (int i = 144; i < 256; ++i) lengths[i] = 9;
  for (int i = 256; i < 280; ++i) lengths[i] = 7;
  for (int i = 280; i < 288; ++i) lengths[i] = 8;
  lit.Build(lengths, 288);
  uint8_t dl[30];
  for (int i = 0; i < 30; ++i) dl[i] = 5;
  dist.Build(dl, 30);
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t seed) {
  const uint32_t* table = CrcTable();
  uint32_t c = seed ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const uint8_t* data, size_t size) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

std::vector<uint8_t> InflateRaw(const uint8_t* data, size_t size, uint64_t max_out) {
  BitReader br{data, size};
  std::vector<uint8_t> out;
  out.reserve(static_cast<size_t>(size * 3 < max_out ? size * 3 : max_out));

  for (;;) {
    const uint32_t last = br.Get(1);
    const uint32_t type = br.Get(2);
    if (type == 0) {
      br.Align();
      if (br.pos + 4 > br.size) throw Error(ErrorCode::Corrupt, "deflate: truncated stored block");
      const uint32_t len = uint32_t(data[br.pos]) | (uint32_t(data[br.pos + 1]) << 8);
      br.pos += 4;  // len + nlen
      if (br.pos + len > br.size) throw Error(ErrorCode::Corrupt, "deflate: stored block overflow");
      for (uint32_t i = 0; i < len; ++i) Emit(out, data[br.pos + i], max_out);
      br.pos += len;
    } else if (type == 1) {
      Huffman lit, dist;
      BuildFixed(lit, dist);
      InflateCodes(br, lit, dist, out, max_out);
    } else if (type == 2) {
      const int hlit = static_cast<int>(br.Get(5)) + 257;
      const int hdist = static_cast<int>(br.Get(5)) + 1;
      const int hclen = static_cast<int>(br.Get(4)) + 4;
      static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                        11, 4,  12, 3, 13, 2, 14, 1, 15};
      uint8_t cl[19] = {0};
      for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(br.Get(3));
      Huffman clh;
      if (!clh.Build(cl, 19)) throw Error(ErrorCode::Corrupt, "deflate: bad code-length code");

      std::vector<uint8_t> lengths(static_cast<size_t>(hlit + hdist), 0);
      int i = 0;
      while (i < hlit + hdist) {
        const int sym = clh.Decode(br);
        if (sym < 0) throw Error(ErrorCode::Corrupt, "deflate: bad code-length symbol");
        if (sym < 16) {
          lengths[static_cast<size_t>(i++)] = static_cast<uint8_t>(sym);
        } else if (sym == 16) {
          if (i == 0) throw Error(ErrorCode::Corrupt, "deflate: repeat with no previous length");
          const int rep = 3 + static_cast<int>(br.Get(2));
          const uint8_t prev = lengths[static_cast<size_t>(i - 1)];
          for (int k = 0; k < rep && i < hlit + hdist; ++k) lengths[static_cast<size_t>(i++)] = prev;
        } else if (sym == 17) {
          const int rep = 3 + static_cast<int>(br.Get(3));
          for (int k = 0; k < rep && i < hlit + hdist; ++k) lengths[static_cast<size_t>(i++)] = 0;
        } else {
          const int rep = 11 + static_cast<int>(br.Get(7));
          for (int k = 0; k < rep && i < hlit + hdist; ++k) lengths[static_cast<size_t>(i++)] = 0;
        }
      }
      Huffman lit, dist;
      if (!lit.Build(lengths.data(), hlit)) throw Error(ErrorCode::Corrupt, "deflate: bad literal tree");
      if (!dist.Build(lengths.data() + hlit, hdist)) {
        throw Error(ErrorCode::Corrupt, "deflate: bad distance tree");
      }
      InflateCodes(br, lit, dist, out, max_out);
    } else {
      throw Error(ErrorCode::Corrupt, "deflate: reserved block type");
    }
    if (last) break;
  }
  return out;
}

std::vector<uint8_t> ZlibDecompress(const uint8_t* data, size_t size, uint64_t max_out) {
  if (size < 6) throw Error(ErrorCode::Corrupt, "zlib: stream too short");
  const uint32_t cmf = data[0];
  const uint32_t flg = data[1];
  if ((cmf & 0x0F) != 8) throw Error(ErrorCode::Corrupt, "zlib: unsupported compression method");
  if (((cmf << 8) | flg) % 31 != 0) throw Error(ErrorCode::Corrupt, "zlib: bad header check");
  if ((flg & 0x20) != 0) throw Error(ErrorCode::Corrupt, "zlib: preset dictionary not supported");
  auto out = InflateRaw(data + 2, size - 2 - 4, max_out);
  const uint32_t expect = ReadBe32(data + size - 4);
  if (Adler32(out.data(), out.size()) != expect) throw Error(ErrorCode::Corrupt, "zlib: adler mismatch");
  return out;
}

std::vector<uint8_t> ZlibStoreCompress(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.push_back(0x78);
  out.push_back(0x01);
  size_t pos = 0;
  do {
    const size_t chunk = (size - pos) < 65535 ? (size - pos) : 65535;
    const bool final_block = (pos + chunk) >= size;
    out.push_back(final_block ? 1 : 0);
    out.push_back(static_cast<uint8_t>(chunk & 0xFF));
    out.push_back(static_cast<uint8_t>((chunk >> 8) & 0xFF));
    const uint16_t nlen = static_cast<uint16_t>(~chunk);
    out.push_back(static_cast<uint8_t>(nlen & 0xFF));
    out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
    out.insert(out.end(), data + pos, data + pos + chunk);
    pos += chunk;
  } while (pos < size);
  const uint32_t adler = Adler32(data, size);
  out.push_back(static_cast<uint8_t>(adler >> 24));
  out.push_back(static_cast<uint8_t>(adler >> 16));
  out.push_back(static_cast<uint8_t>(adler >> 8));
  out.push_back(static_cast<uint8_t>(adler));
  return out;
}


// ---- encoder: fixed-Huffman DEFLATE (RFC 1951) --------------------------
//
// LZ77 with a 3-byte hash chain over a 32 KiB window, emitted as fixed-Huffman
// blocks. This is the "real" deflate the engine needs to write ZIP entries and
// PDF /FlateDecode streams (docs/01 F-301 / docs/02 §3.1.1).

namespace {

struct BitWriter {
  std::vector<uint8_t>& out;
  uint32_t bitbuf = 0;
  int bitcnt = 0;

  explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}

  // LSB-first, as DEFLATE requires. Written in slices so that the shift below
  // can never overflow the 32-bit accumulator (extra-bit fields are up to 13
  // bits wide and the accumulator may already hold up to 31).
  void Put(uint32_t code, int bits) {
    while (bits > 0) {
      const int room = 32 - bitcnt;
      const int take = bits < room ? bits : room;
      bitbuf |= (code & ((1u << take) - 1u)) << bitcnt;
      bitcnt += take;
      bits -= take;
      code >>= take;
      while (bitcnt >= 8) {
        out.push_back(static_cast<uint8_t>(bitbuf & 0xFF));
        bitbuf >>= 8;
        bitcnt -= 8;
      }
    }
  }

  // Fixed-Huffman codes are written MSB-first even though extra bits are
  // LSB-first, hence the two different entry points.
  void PutCode(uint32_t code, int bits) {
    for (int i = bits - 1; i >= 0; --i) Put((code >> i) & 1u, 1);
  }

  void Flush() {
    if (bitcnt > 0) {
      out.push_back(static_cast<uint8_t>(bitbuf & 0xFF));
      bitbuf = 0;
      bitcnt = 0;
    }
  }
};

// RFC 1951 §3.2.5 / §3.2.6 static tables.
int FixedLitCode(uint32_t sym) {
  if (sym < 144) return static_cast<int>(0x30 + sym);
  if (sym < 256) return static_cast<int>(0x190 + (sym - 144));
  if (sym < 280) return static_cast<int>(sym - 256);
  return static_cast<int>(0xC0 + (sym - 280));
}

int FixedLitBits(uint32_t sym) {
  if (sym < 144) return 8;
  if (sym < 256) return 9;
  if (sym < 280) return 7;
  return 8;
}

constexpr uint16_t Enc_kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13, 15,  17,  19,  23,
                                   27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t Enc_kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t Enc_kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                   33,   49,   65,   97,   129,  193,   257,   385,   513,
                                   769,  1025, 1537, 2049, 3073, 4097,  6145,  8193,  12289,
                                   16385, 24577};
constexpr uint8_t Enc_kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

int LengthCode(uint32_t len) {
  for (int i = 28; i >= 0; --i) {
    if (len >= Enc_kLenBase[i]) return i;
  }
  return 0;
}

int DistCode(uint32_t dist) {
  for (int i = 29; i >= 0; --i) {
    if (dist >= Enc_kDistBase[i]) return i;
  }
  return 0;
}

constexpr int Enc_kWindow = 32768;
constexpr int Enc_kMinMatch = 3;
constexpr int Enc_kMaxMatch = 258;
constexpr int Enc_kHashBits = 15;
constexpr int Enc_kHashSize = 1 << Enc_kHashBits;
constexpr int Enc_kMaxChain = 128;

inline uint32_t Hash3(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

}  // namespace

// An empty input yields {0x03, 0x00}: BFINAL=1, BTYPE=01 (fixed Huffman) and
// the 7 zero bits of the end-of-block symbol, padded to a byte boundary.
std::vector<uint8_t> DeflateRaw(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size / 2 + 64);
  BitWriter bw(out);

  std::vector<int32_t> head(Enc_kHashSize, -1);
  std::vector<int32_t> prev(size, -1);

  bw.Put(1, 1);  // BFINAL: a single final fixed-Huffman block
  bw.Put(1, 2);  // BTYPE = 01 fixed

  size_t pos = 0;
  while (pos < size) {
    uint32_t best_len = 0;
    size_t best_dist = 0;
    if (pos + Enc_kMinMatch <= size) {
      const uint32_t h = (Hash3(data + pos) * 2654435761u) >> (32 - Enc_kHashBits) & (Enc_kHashSize - 1);
      int32_t cand = head[h];
      int chain = 0;
      const size_t limit = pos > static_cast<size_t>(Enc_kWindow) ? pos - Enc_kWindow : 0;
      while (cand >= 0 && chain < Enc_kMaxChain && static_cast<size_t>(cand) >= limit) {
        const size_t c = static_cast<size_t>(cand);
        uint32_t len = 0;
        const uint32_t max_len = static_cast<uint32_t>(
            (size - pos) < static_cast<size_t>(Enc_kMaxMatch) ? (size - pos) : Enc_kMaxMatch);
        while (len < max_len && data[c + len] == data[pos + len]) ++len;
        if (static_cast<int>(len) > static_cast<int>(best_len)) {
          best_len = len;
          best_dist = pos - c;
          if (len >= Enc_kMaxMatch) break;
        }
        cand = prev[c];
        ++chain;
      }
    }
    if (best_len >= Enc_kMinMatch) {
      const int lc = LengthCode(best_len);
      const uint32_t len_sym = static_cast<uint32_t>(257 + lc);  // RFC 1951 3.2.5
      bw.PutCode(static_cast<uint32_t>(FixedLitCode(len_sym)), FixedLitBits(len_sym));
      if (Enc_kLenExtra[lc] > 0) bw.Put(best_len - Enc_kLenBase[lc], Enc_kLenExtra[lc]);
      const int dc = DistCode(static_cast<uint32_t>(best_dist));
      bw.PutCode(static_cast<uint32_t>(dc), 5);
      if (Enc_kDistExtra[dc] > 0) {
        bw.Put(static_cast<uint32_t>(best_dist - Enc_kDistBase[dc]), Enc_kDistExtra[dc]);
      }
      // Index every position of the match so later matches can reference it.
      for (uint32_t k = 0; k < best_len; ++k) {
        const size_t at = pos + k;
        if (at + Enc_kMinMatch <= size) {
          const uint32_t h =
              (Hash3(data + at) * 2654435761u) >> (32 - Enc_kHashBits) & (Enc_kHashSize - 1);
          prev[at] = head[h];
          head[h] = static_cast<int32_t>(at);
        }
      }
      pos += best_len;
    } else {
      const uint8_t lit = data[pos];
      bw.PutCode(static_cast<uint32_t>(FixedLitCode(lit)), FixedLitBits(lit));
      if (pos + Enc_kMinMatch <= size) {
        const uint32_t h = (Hash3(data + pos) * 2654435761u) >> (32 - Enc_kHashBits) & (Enc_kHashSize - 1);
        prev[pos] = head[h];
        head[h] = static_cast<int32_t>(pos);
      }
      ++pos;
    }
  }
  bw.PutCode(static_cast<uint32_t>(FixedLitCode(256)), FixedLitBits(256));  // end of block
  bw.Flush();
  return out;
}

std::vector<uint8_t> ZlibCompress(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.push_back(0x78);
  out.push_back(0x01);
  auto raw = DeflateRaw(data, size);
  out.insert(out.end(), raw.begin(), raw.end());
  const uint32_t adler = Adler32(data, size);
  out.push_back(static_cast<uint8_t>(adler >> 24));
  out.push_back(static_cast<uint8_t>(adler >> 16));
  out.push_back(static_cast<uint8_t>(adler >> 8));
  out.push_back(static_cast<uint8_t>(adler));
  return out;
}

}  // namespace doccore
