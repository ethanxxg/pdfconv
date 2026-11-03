// DocCore - Word 文档保护的口令哈希（ISO 29500-1 §17.15.1.29）。
//
// OOXML 不在文件里存口令，只存一个迭代哈希：
//   H0 = H(salt ‖ UTF-16LE(password))
//   Hi = H(H(i-1) ‖ uint32_le(i-1))        i = 1..spinCount
//   存储值 = base64( H(spinCount)[0 .. hashSize) )
//
// 迭代次数默认 100000，就是为了让暴力枚举足够贵。这也意味着校验一次要跑
// 十万次哈希——所以校验结果不做缓存，调用方自己决定要不要缓存。
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "doccore/core.h"
#include "doccore/crypto.h"
#include "doccore/text.h"

namespace doccore {
namespace text {
namespace {

using crypto::Bytes;

Bytes HashOnce(const std::string& algorithm, const uint8_t* data, size_t size) {
  // 名字比较不区分大小写与连字符：Word 写 "SHA-512"，有的实现写 "SHA512"。
  std::string a;
  for (char c : algorithm) {
    if (c == '-' || c == '_') continue;
    a.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (a == "MD5") return crypto::Md5(data, size);
  if (a == "SHA256") return crypto::Sha256(data, size);
  if (a == "SHA384") return crypto::Sha384(data, size);
  if (a == "SHA512") return crypto::Sha512(data, size);
  return crypto::Sha1(data, size);  // 缺省 SHA-1（Word 2007 的默认）
}

// 算法名对应的摘要长度，用于在文件没写 hashSize 时兜底。
int DigestSize(const std::string& algorithm) {
  std::string a;
  for (char c : algorithm) {
    if (c == '-' || c == '_') continue;
    a.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (a == "MD5") return 16;
  if (a == "SHA256") return 32;
  if (a == "SHA384") return 48;
  if (a == "SHA512") return 64;
  return 20;
}

Bytes Utf16Le(const std::string& utf8) {
  const std::u16string u16 = U32ToU16(Utf8ToU32(utf8));
  Bytes out;
  out.reserve(u16.size() * 2);
  for (char16_t c : u16) {
    out.push_back(static_cast<uint8_t>(c & 0xFF));
    out.push_back(static_cast<uint8_t>(c >> 8));
  }
  return out;
}

// 迭代哈希的核心，供校验与生成共用。
Bytes IterateHash(const std::string& password, const Bytes& salt, const std::string& algorithm,
                  int spin_count, int hash_size) {
  const Bytes pw = Utf16Le(password);
  Bytes input;
  input.reserve(salt.size() + pw.size());
  input.insert(input.end(), salt.begin(), salt.end());
  input.insert(input.end(), pw.begin(), pw.end());
  Bytes h = HashOnce(algorithm, input.data(), input.size());

  const int spins = std::max(0, spin_count);
  for (int i = 0; i < spins; ++i) {
    Bytes block = h;
    block.push_back(static_cast<uint8_t>(i & 0xFF));
    block.push_back(static_cast<uint8_t>((i >> 8) & 0xFF));
    block.push_back(static_cast<uint8_t>((i >> 16) & 0xFF));
    block.push_back(static_cast<uint8_t>((i >> 24) & 0xFF));
    h = HashOnce(algorithm, block.data(), block.size());
  }
  const int want = hash_size > 0 ? hash_size : DigestSize(algorithm);
  if (static_cast<int>(h.size()) > want) h.resize(static_cast<size_t>(want));
  return h;
}

}  // namespace

std::string ComputeProtectionHash(const std::string& password, const std::string& salt_base64,
                                  const std::string& algorithm, int spin_count, int hash_size) {
  const Bytes salt = Base64Decode(salt_base64);
  const Bytes h = IterateHash(password, salt, algorithm, spin_count, hash_size);
  return Base64Encode(h.data(), h.size());
}

bool VerifyProtectionPassword(const ProtectionHash& stored, const std::string& password) {
  if (!stored.present()) return false;
  const Bytes salt = Base64Decode(stored.salt);
  const int hash_size = stored.hash_size > 0 ? stored.hash_size : DigestSize(stored.algorithm);
  const Bytes h = IterateHash(password, salt, stored.algorithm, stored.spin_count, hash_size);
  const Bytes want = Base64Decode(stored.hash);
  // 比长度再比内容；长度不同直接判否（截断的哈希不能算通过）。
  if (want.size() != h.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < h.size(); ++i) diff |= static_cast<uint8_t>(h[i] ^ want[i]);
  return diff == 0;
}

}  // namespace text
}  // namespace doccore
