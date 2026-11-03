// DocCore - 自带的密码学原语。
//
// 两处用途：
//   1. PDF 标准安全处理器（PDF 32000-1 §7.6）——MD5（Algorithm 2/3.A 的密钥
//      派生）、RC4（/V1 /V2）、AES（/V4 /V5 的 CBC 模式）、SHA-256/384/512
//      （/R6 的迭代哈希 Algorithm 2.B）。
//   2. Word 文档保护的口令校验（ISO 29500 §17.15.1.29）——SHA-1/256/384/512
//      的迭代哈希。
// 全部按公开标准实现，不引入外部依赖。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace doccore {
namespace crypto {

using Bytes = std::vector<uint8_t>;

Bytes Md5(const uint8_t* data, size_t size);
// SHA-1 只服务老式文档保护（Word 2007 的默认算法），PDF 侧不用。
Bytes Sha1(const uint8_t* data, size_t size);
Bytes Sha256(const uint8_t* data, size_t size);
Bytes Sha384(const uint8_t* data, size_t size);
Bytes Sha512(const uint8_t* data, size_t size);

// RC4 就地加解密（对称）。key 长度 1..256。
void Rc4(const uint8_t* key, size_t key_len, uint8_t* data, size_t size);

// AES 分组密码，密钥 16/24/32 字节。
class Aes {
 public:
  // key_len 必须是 16/24/32；否则抛 Error(InvalidArgument)。
  Aes(const uint8_t* key, size_t key_len);
  void EncryptBlock(uint8_t block[16]) const;
  void DecryptBlock(uint8_t block[16]) const;

 private:
  void ExpandKey(const uint8_t* key, size_t key_len);
  uint8_t round_keys_[240] = {0};
  int rounds_ = 10;
};

// AES-CBC。加密会按 PKCS#7 追加填充；解密会校验并去掉填充，填充非法时返回
// false（调用方决定是当作损坏还是当作明文）。
void AesCbcEncrypt(const Aes& aes, const uint8_t iv[16], Bytes* data);
bool AesCbcDecrypt(const Aes& aes, const uint8_t iv[16], Bytes* data);

// PKCS#7 填充/去填充（块大小 16）。去填充失败返回 false。
void Pkcs7Pad(Bytes* data, size_t block);
bool Pkcs7Unpad(Bytes* data, size_t block);

}  // namespace crypto
}  // namespace doccore
