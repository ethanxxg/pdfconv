// DocCore - 旧格式与开放格式读取（DOC / RTF / ODT）。Design refs: docs/02 §1 边界 / F-111.
//
// 三个读取器各自解析自己的容器与语法，产出同一个 `text::Document`，
// 之后转 PDF 复用字处理那条排版路径（P5 的 Layout IR → pdf 编码器）。
// 识别不在本模块（F-602 属 P2）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/text.h"

namespace doccore {
namespace legacy {

// ---- RTF ---------------------------------------------------------------

// 解析 RTF（Rich Text Format 1.9）为 text::Document。
// 未识别的控制字跳过并向 warnings 追加 `RtfControlSkipped`(1019)，
// 已识别的文本与格式保留。输入不是 RTF 时抛 Error(InvalidArgument)。
text::Document ParseRtf(std::string_view rtf, WarningSink* warnings);

// ---- ODT ---------------------------------------------------------------

// 解析 ODF 文本文档（.odt，ZIP 容器内的 content.xml）为 text::Document。
// 容器不是合法 ZIP 或缺少 content.xml 时抛 Error(Corrupt)。
text::Document ParseOdt(const std::vector<uint8_t>& zip_bytes, WarningSink* warnings);

// ---- DOC ---------------------------------------------------------------

// 解析 Word 97-2003 二进制（.doc，CFB 容器内的 WordDocument/Table 流）
// 为 text::Document。容器不是合法 CFB 或缺少 WordDocument 流时抛
// Error(Corrupt)。
text::Document ParseDoc(const std::vector<uint8_t>& doc_bytes, WarningSink* warnings);

}  // namespace legacy
}  // namespace doccore
