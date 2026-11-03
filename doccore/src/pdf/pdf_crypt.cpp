// DocCore - PDF 标准安全处理器（PDF 32000-1 §7.6.3 + ISO 32000-2 的 AES-256 扩展）。
//
// 覆盖四种组合：
//   /V1 /R2  RC4 40 位
//   /V2 /R3  RC4 128 位（/Length 可调）
//   /V4 /R4  RC4 或 AES-128（按 /CF 的 /CFM 选）
//   /V5 /R5 /R6  AES-256（R6 用 Algorithm 2.B 的迭代哈希）
//
// 只做解密：DocCore 读 PDF 的目的是转成办公文档，写出加密 PDF 不是当前诉求，
// 而且写侧加密一旦做错会静默产出打不开的文件。
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pdf_crypt.h"

#include "doccore/core.h"
#include "doccore/crypto.h"

namespace doccore {
namespace pdf {
namespace {

// Algorithm 2 的 32 字节口令填充串（PDF 32000-1 Table 21）。
const uint8_t kPasswordPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41,
                                  0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
                                  0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80,
                                  0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

using crypto::Bytes;

Bytes Concat(std::initializer_list<const Bytes*> parts) {
  Bytes out;
  for (const Bytes* p : parts) {
    if (p != nullptr) out.insert(out.end(), p->begin(), p->end());
  }
  return out;
}

Bytes Slice(const Bytes& src, size_t from, size_t count) {
  if (from >= src.size()) return Bytes();
  const size_t end = std::min(src.size(), from + count);
  return Bytes(src.begin() + static_cast<long>(from), src.begin() + static_cast<long>(end));
}

Bytes StrBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// 4 字节小端有符号整数（/P 的存储形式）。
Bytes Int32Le(int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  return Bytes{static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8),
               static_cast<uint8_t>(u >> 16), static_cast<uint8_t>(u >> 24)};
}

Bytes ZeroIv(size_t n) { return Bytes(n, 0); }

Cfm ParseCfm(const std::string& name) {
  if (name == "V2") return Cfm::V2;
  if (name == "AESV2") return Cfm::AesV2;
  if (name == "AESV3") return Cfm::AesV3;
  return Cfm::None;
}

}  // namespace

bool SecurityHandler::Init(const ObjectPtr& encrypt_dict, const PdfFile& file,
                           const std::string& password, const Bytes& id0) {
    if (encrypt_dict == nullptr || !encrypt_dict->is_dict()) return false;
    auto int_of = [&](const char* key, int fallback) {
      const ObjectPtr* v = encrypt_dict->Find(key);
      if (v == nullptr) return fallback;
      const ObjectPtr r = file.Resolve(*v);
      return r != nullptr && r->is_number() ? static_cast<int>(r->number()) : fallback;
    };
    auto str_of = [&](const char* key) {
      const ObjectPtr* v = encrypt_dict->Find(key);
      if (v == nullptr) return Bytes();
      const ObjectPtr r = file.Resolve(*v);
      if (r == nullptr || r->kind != ObjKind::String) return Bytes();
      return StrBytes(r->text);
    };

    v_ = int_of("V", 0);
    r_ = int_of("R", 0);
    p_ = int_of("P", 0);
    length_bits_ = int_of("Length", 40);
    o_ = str_of("O");
    u_ = str_of("U");
    oe_ = str_of("OE");
    ue_ = str_of("UE");
    if (v_ == 0 || r_ == 0) return false;
    if (r_ <= 4 && (o_.size() < 32 || u_.size() < 32)) return false;
    if (r_ >= 5 && (o_.size() < 48 || u_.size() < 48)) return false;

    // /EncryptMetadata 默认 true。
    encrypt_metadata_ = true;
    if (const ObjectPtr* em = encrypt_dict->Find("EncryptMetadata")) {
      const ObjectPtr r = file.Resolve(*em);
      if (r != nullptr && r->kind == ObjKind::Bool) encrypt_metadata_ = r->bool_value;
    }

    // 密码过滤器：/StmF 与 /StrF 指向 /CF 里的名字。
    std::string stmf = "Identity";
    std::string strf = "Identity";
    if (const ObjectPtr* f = encrypt_dict->Find("StmF")) {
      const ObjectPtr r = file.Resolve(*f);
      if (r != nullptr && r->kind == ObjKind::Name) stmf = r->text;
    }
    if (const ObjectPtr* f = encrypt_dict->Find("StrF")) {
      const ObjectPtr r = file.Resolve(*f);
      if (r != nullptr && r->kind == ObjKind::Name) strf = r->text;
    }
    if (const ObjectPtr* cf = encrypt_dict->Find("CF")) {
      const ObjectPtr dict = file.Resolve(*cf);
      if (dict != nullptr && dict->is_dict()) {
        for (const auto& kv : *dict->dict) {
          const ObjectPtr one = file.Resolve(kv.second);
          if (one == nullptr || !one->is_dict()) continue;
          CryptFilter filter;
          if (const ObjectPtr* cfm = one->Find("CFM")) {
            const ObjectPtr n = file.Resolve(*cfm);
            if (n != nullptr && n->kind == ObjKind::Name) filter.cfm = ParseCfm(n->text);
          }
          if (const ObjectPtr* len = one->Find("Length")) {
            const ObjectPtr n = file.Resolve(*len);
            if (n != nullptr && n->is_number()) {
              // /Length 有的写字节数、有的写位数，小于 32 的按字节理解。
              const int raw = static_cast<int>(n->number());
              filter.length_bits = raw < 32 ? raw * 8 : raw;
            }
          }
          filters_[kv.first] = filter;
        }
      }
    }
    // V1/V2 没有 /CF，直接按版本推定。
    if (v_ == 1) {
      stream_filter_ = CryptFilter{Cfm::V2, 40};
      string_filter_ = stream_filter_;
    } else if (v_ == 2) {
      stream_filter_ = CryptFilter{Cfm::V2, length_bits_};
      string_filter_ = stream_filter_;
    } else {
      auto pick = [&](const std::string& name, CryptFilter fallback) {
        if (name == "Identity") return CryptFilter{};
        const auto it = filters_.find(name);
        return it == filters_.end() ? fallback : it->second;
      };
      stream_filter_ = pick(stmf, CryptFilter{Cfm::None, 0});
      string_filter_ = pick(strf, CryptFilter{Cfm::None, 0});
      // V4 若没写 /CF 但 R4，按 AES-128 处理（少见但存在）。
      if (stream_filter_.cfm == Cfm::None && v_ == 4 && filters_.empty()) {
        stream_filter_ = CryptFilter{Cfm::AesV2, 128};
        string_filter_ = stream_filter_;
      }
    }

    if (r_ >= 5) return InitAes256(password);
    return InitLegacy(password, id0);
  }

// 按对象号解密一条字符串 / 一个流的原始字节。
Bytes SecurityHandler::Decrypt(const Bytes& data, int num, int gen, bool is_stream) const {
    if (key_.empty() || data.empty()) return data;
    const CryptFilter& filter = is_stream ? stream_filter_ : string_filter_;
    switch (filter.cfm) {
      case Cfm::None:
        return data;
      case Cfm::V2: {
        Bytes out = data;
        const Bytes obj_key = ObjectKey(num, gen, filter.length_bits);
        crypto::Rc4(obj_key.data(), obj_key.size(), out.data(), out.size());
        return out;
      }
      case Cfm::AesV2:
      case Cfm::AesV3: {
        if (data.size() < 32) return data;  // IV + 至少一个块
        const Bytes obj_key = filter.cfm == Cfm::AesV3 ? key_ : ObjectKey(num, gen, 128);
        const Bytes iv = Slice(data, 0, 16);
        Bytes body = Slice(data, 16, data.size() - 16);
        if (body.size() % 16 != 0) body.resize(body.size() - body.size() % 16);
        if (body.empty()) return Bytes();
        crypto::Aes aes(obj_key.data(), std::min<size_t>(32, obj_key.size()));
        if (!crypto::AesCbcDecrypt(aes, iv.data(), &body)) {
          // 填充非法：多半是口令不对或长度被截断。返回已解密的内容，让上层
          // 自己去判断（抛错还是当明文）。
          return body;
        }
        return body;
      }
    }
  return data;
}

// Algorithm 2：从用户口令派生文件密钥（R2..R4）。
Bytes SecurityHandler::DeriveLegacyKey(const std::string& password, const Bytes& id0) const {
    Bytes padded(32, 0);
    const size_t n = std::min<size_t>(32, password.size());
    std::memcpy(padded.data(), password.data(), n);
    std::memcpy(padded.data() + n, kPasswordPad, 32 - n);

    Bytes input = Concat({&padded, &o_, nullptr});
    const Bytes p = Int32Le(p_);
    input.insert(input.end(), p.begin(), p.end());
    input.insert(input.end(), id0.begin(), id0.end());
    if (r_ >= 4 && !encrypt_metadata_) {
      input.insert(input.end(), {0xFF, 0xFF, 0xFF, 0xFF});
    }
    Bytes hash = crypto::Md5(input.data(), input.size());
    const size_t key_len = r_ == 2 ? 5 : static_cast<size_t>(std::max(40, length_bits_) / 8);
    if (r_ >= 3) {
      for (int i = 0; i < 50; ++i) {
        hash = crypto::Md5(hash.data(), key_len);
      }
    }
    hash.resize(key_len);
    return hash;
  }

// Algorithm 3.A：算 /U 用于校验用户口令（R2..R4）。
Bytes SecurityHandler::ComputeU(const Bytes& key, const Bytes& id0) const {
    if (r_ == 2) {
      Bytes out(kPasswordPad, kPasswordPad + 32);
      crypto::Rc4(key.data(), key.size(), out.data(), out.size());
      return out;
    }
    Bytes input(kPasswordPad, kPasswordPad + 32);
    input.insert(input.end(), id0.begin(), id0.end());
    Bytes hash = crypto::Md5(input.data(), input.size());
    Bytes out = hash;
    crypto::Rc4(key.data(), key.size(), out.data(), out.size());
    for (int i = 1; i <= 19; ++i) {
      Bytes step = key;
      for (uint8_t& b : step) b ^= static_cast<uint8_t>(i);
      crypto::Rc4(step.data(), step.size(), out.data(), out.size());
    }
    out.resize(32, 0);  // 后 16 字节是任意填充，比较时只看前 16
    return out;
  }

bool SecurityHandler::InitLegacy(const std::string& password, const Bytes& id0) {
    // 先当用户口令试。
    Bytes candidate = DeriveLegacyKey(password, id0);
    const Bytes want = ComputeU(candidate, id0);
    const size_t cmp = r_ == 2 ? 32 : 16;
    if (want.size() >= cmp && u_.size() >= cmp &&
        std::memcmp(want.data(), u_.data(), cmp) == 0) {
      key_ = candidate;
      return true;
    }
    // 再当所有者口令试：Algorithm 3 的变体——用 /O 解出用户口令再走一遍。
    Bytes user_pw(32, 0);
    {
      // Algorithm 3 步骤 (a)：所有者口令同样要先按 32 字节填充串补齐/截断，
      // 再进 MD5。直接哈希原始口令是最容易写错的一步。
      Bytes padded_owner(32, 0);
      const size_t pn = std::min<size_t>(32, password.size());
      std::memcpy(padded_owner.data(), password.data(), pn);
      std::memcpy(padded_owner.data() + pn, kPasswordPad, 32 - pn);
      Bytes okey = crypto::Md5(padded_owner.data(), padded_owner.size());
      if (r_ >= 3) {
        for (int i = 0; i < 50; ++i) okey = crypto::Md5(okey.data(), okey.size());
      }
      const size_t key_len = r_ == 2 ? 5 : static_cast<size_t>(std::max(40, length_bits_) / 8);
      okey.resize(key_len);
      user_pw = o_.size() >= 32 ? Slice(o_, 0, 32) : Bytes();
      if (user_pw.size() != 32) return false;
      if (r_ == 2) {
        crypto::Rc4(okey.data(), okey.size(), user_pw.data(), user_pw.size());
      } else {
        for (int i = 19; i >= 0; --i) {
          Bytes step = okey;
          for (uint8_t& b : step) b ^= static_cast<uint8_t>(i);
          crypto::Rc4(step.data(), step.size(), user_pw.data(), user_pw.size());
        }
      }
    }
    const std::string recovered(user_pw.begin(), user_pw.end());
    candidate = DeriveLegacyKey(recovered, id0);
    const Bytes want2 = ComputeU(candidate, id0);
    if (want2.size() >= cmp && u_.size() >= cmp &&
        std::memcmp(want2.data(), u_.data(), cmp) == 0) {
      key_ = candidate;
      return true;
    }
    return false;
  }

// Algorithm 2.B：R6 的迭代哈希；R5 就是一次 SHA-256。
Bytes SecurityHandler::HashR6(const std::string& password, const Bytes& salt,
                             const Bytes& udata) const {
    const Bytes pw = StrBytes(password);
    Bytes input = Concat({&pw, &salt, &udata});
    Bytes k = crypto::Sha256(input.data(), input.size());
    if (r_ == 5) return k;
    uint8_t last = 0;
    for (int round = 0;; ++round) {
      // K1 = 64 份 (password + K + udata) 串接。
      Bytes unit = Concat({&pw, &k, &udata});
      Bytes k1;
      k1.reserve(unit.size() * 64);
      for (int i = 0; i < 64; ++i) k1.insert(k1.end(), unit.begin(), unit.end());
      // E = AES-128-CBC(key = K[0..16), iv = K[16..32), K1)，不补填充。
      const Bytes aes_key = Slice(k, 0, 16);
      const Bytes iv = Slice(k, 16, 16);
      Bytes e = k1;
      if (aes_key.size() == 16 && iv.size() == 16 && e.size() % 16 == 0) {
        crypto::Aes aes(aes_key.data(), 16);
        uint8_t chain[16];
        std::memcpy(chain, iv.data(), 16);
        for (size_t off = 0; off < e.size(); off += 16) {
          uint8_t* block = e.data() + off;
          for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
          aes.EncryptBlock(block);
          std::memcpy(chain, block, 16);
        }
      }
      int sum = 0;
      for (size_t i = 0; i < std::min<size_t>(16, e.size()); ++i) sum += e[i];
      switch (sum % 3) {
        case 0:
          k = crypto::Sha256(e.data(), e.size());
          break;
        case 1:
          k = crypto::Sha384(e.data(), e.size());
          break;
        default:
          k = crypto::Sha512(e.data(), e.size());
          break;
      }
      last = e.empty() ? 0 : e.back();
      if (round >= 63 && last <= round - 32) break;
      if (round > 4096) break;  // 防御性上限，正常实现最多 64+255 轮
    }
    k.resize(32);
    return k;
  }

bool SecurityHandler::InitAes256(const std::string& password) {
    const Bytes empty;
    // 用户口令：U 的前 32 字节是校验值，32..40 是校验盐，40..48 是密钥盐。
    if (u_.size() >= 48) {
      const Bytes vsalt = Slice(u_, 32, 8);
      const Bytes ksalt = Slice(u_, 40, 8);
      const Bytes hash = HashR6(password, vsalt, empty);
      if (hash.size() >= 32 && std::memcmp(hash.data(), u_.data(), 32) == 0) {
        const Bytes k = HashR6(password, ksalt, empty);
        if (k.size() == 32 && ue_.size() >= 32) {
          Bytes file_key = Slice(ue_, 0, 32);
          crypto::Aes aes(k.data(), 32);
          const Bytes iv = ZeroIv(16);
          crypto::AesCbcDecrypt(aes, iv.data(), &file_key);
          if (file_key.size() >= 32) {
            file_key.resize(32);
            key_ = file_key;
            return true;
          }
        }
      }
    }
    // 所有者口令：哈希输入要多带 48 字节的 /U。
    if (o_.size() >= 48) {
      const Bytes vsalt = Slice(o_, 32, 8);
      const Bytes ksalt = Slice(o_, 40, 8);
      const Bytes u48 = Slice(u_, 0, 48);
      const Bytes hash = HashR6(password, vsalt, u48);
      if (hash.size() >= 32 && std::memcmp(hash.data(), o_.data(), 32) == 0) {
        const Bytes k = HashR6(password, ksalt, u48);
        if (k.size() == 32 && oe_.size() >= 32) {
          Bytes file_key = Slice(oe_, 0, 32);
          crypto::Aes aes(k.data(), 32);
          const Bytes iv = ZeroIv(16);
          crypto::AesCbcDecrypt(aes, iv.data(), &file_key);
          if (file_key.size() >= 32) {
            file_key.resize(32);
            key_ = file_key;
            return true;
          }
        }
      }
    }
    return false;
  }

// Algorithm 1：按对象号派生一次性密钥（V2 / AESV2）。
Bytes SecurityHandler::ObjectKey(int num, int gen, int length_bits) const {
    Bytes input = key_;
    input.push_back(static_cast<uint8_t>(num));
    input.push_back(static_cast<uint8_t>(num >> 8));
    input.push_back(static_cast<uint8_t>(num >> 16));
    input.push_back(static_cast<uint8_t>(gen));
    input.push_back(static_cast<uint8_t>(gen >> 8));
    if (stream_filter_.cfm == Cfm::AesV2 || string_filter_.cfm == Cfm::AesV2) {
      input.insert(input.end(), {'s', 'A', 'l', 'T'});
    }
    Bytes hash = crypto::Md5(input.data(), input.size());
    const size_t n = std::min<size_t>(16, static_cast<size_t>(std::max(40, length_bits) / 8) + 5);
  hash.resize(n);
  return hash;
}

}  // namespace pdf
}  // namespace doccore
