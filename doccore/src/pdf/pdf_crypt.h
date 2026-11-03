// DocCore - PDF 标准安全处理器（内部头文件，不对外暴露）。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "doccore/pdf_read.h"

namespace doccore {
namespace pdf {

enum class Cfm { None, V2, AesV2, AesV3 };

struct CryptFilter {
  Cfm cfm = Cfm::None;
  int length_bits = 0;
};

class SecurityHandler {
 public:
  // 返回 true 表示口令可用（用户口令或所有者口令之一）。
  bool Init(const ObjectPtr& encrypt_dict, const PdfFile& file, const std::string& password,
            const std::vector<uint8_t>& id0);

  bool encrypt_metadata() const noexcept { return encrypt_metadata_; }
  bool needs_decryption() const noexcept { return !key_.empty(); }
  // /P 的原始值（PDF 32000-1 Table 22，权限位是 1 基的位号 3..12）。
  int permissions() const noexcept { return p_; }

  // 按对象号解密一条字符串 / 一个流的原始字节。
  std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& data, int num, int gen,
                               bool is_stream) const;

 private:
  std::vector<uint8_t> DeriveLegacyKey(const std::string& password,
                                       const std::vector<uint8_t>& id0) const;
  std::vector<uint8_t> ComputeU(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& id0) const;
  bool InitLegacy(const std::string& password, const std::vector<uint8_t>& id0);
  std::vector<uint8_t> HashR6(const std::string& password, const std::vector<uint8_t>& salt,
                              const std::vector<uint8_t>& udata) const;
  bool InitAes256(const std::string& password);
  std::vector<uint8_t> ObjectKey(int num, int gen, int length_bits) const;

  int v_ = 0;
  int r_ = 0;
  int p_ = 0;
  int length_bits_ = 40;
  bool encrypt_metadata_ = true;
  std::vector<uint8_t> o_, u_, oe_, ue_;
  std::vector<uint8_t> key_;
  CryptFilter stream_filter_;
  CryptFilter string_filter_;
  std::map<std::string, CryptFilter> filters_;
};

}  // namespace pdf
}  // namespace doccore
