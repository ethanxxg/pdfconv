#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace doccore {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t seed = 0);
uint32_t Adler32(const uint8_t* data, size_t size);

// Raw DEFLATE (RFC 1951). Throws Error(Corrupt) on malformed input,
// Error(LimitExceeded) when output would exceed max_out.
std::vector<uint8_t> InflateRaw(const uint8_t* data, size_t size, uint64_t max_out);

// zlib wrapper (RFC 1950) around the same decoder.
std::vector<uint8_t> ZlibDecompress(const uint8_t* data, size_t size, uint64_t max_out);

// Produces a valid zlib stream using stored (uncompressed) blocks.
std::vector<uint8_t> ZlibStoreCompress(const uint8_t* data, size_t size);

// Real encoders: LZ77 + fixed-Huffman DEFLATE (RFC 1951) and its zlib wrapper.
// Round-trips through InflateRaw / ZlibDecompress.
std::vector<uint8_t> DeflateRaw(const uint8_t* data, size_t size);
std::vector<uint8_t> ZlibCompress(const uint8_t* data, size_t size);

}  // namespace doccore
