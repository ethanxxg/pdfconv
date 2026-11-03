// DocCore - 转换总线。Design refs: docs/02 §3.6，docs/01 F-901
//
//   Converter::Support s = Converter::SupportOf(Format::Pdf, Format::Docx);  // Lossy
//   Converter::Run({in, out, Format::Pdf, Format::Docx, opts});
//
// 调度是"源解码器 -> DOM 或 Layout IR -> 目标编码器"：办公文档导出走排版，
// PDF 反向走 pdf::Reconstructor。PDF -> DOCX/XLSX/PPTX 一律是 Lossy——调用方
// 拿到文件，同时知道这是重建而不是原样复制。
//
// 落地时对齐设计书的两处调整（docs/03 §P8 完成情况）：
//   1. 设计书里 ConvertRequest 用公共的 OpenOptions / SaveOptions；三个族的
//      选项类型同名不同命名空间，所以这里合并成一个 ConvertOptions。
//   2. PDF -> PNG 需要 PDF 光栅化器（本波没有），SupportOf 返回 No，而不是
//      悄悄降级成空白页。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doccore/core.h"
#include "doccore/pdf.h"
#include "doccore/pdf_rebuild.h"
#include "doccore/stream.h"

namespace doccore {

// Yes = 结构上可无损；Lossy = 能产出文件但内容经过重建；No = 不支持。
enum class Support { No, Yes, Lossy };

const char* SupportName(Support s) noexcept;

struct ConvertOptions {
  // PDF / PNG 目标必须给字体注册表（PDF 要嵌入子集字体）。
  const FontRegistry* fonts = nullptr;
  std::string font_family = "Calibri";
  double font_size_pt = 11.0;
  int dpi = 96;    // PNG 光栅化 DPI
  int page = 1;    // PNG 目标页（1 基）
  bool recalc = false;  // XLSX -> PDF/CSV 前先重算公式
  pdf::RebuildOptions rebuild;  // PDF -> 办公文档
};

struct ConvertRequest {
  Stream& input;
  Stream& output;
  Format input_format = Format::Unknown;   // Unknown 则按内容探测
  Format output_format = Format::Unknown;
  ConvertOptions options;
  CancelToken* cancel = nullptr;
  WarningSink* warnings = nullptr;
};

class Converter {
 public:
  // 纯函数：不碰任何 I/O。
  static Support SupportOf(Format in, Format out);

  // 内容探测：ZIP 容器按 [Content_Types].xml 判族，%PDF- 判 PDF；都不匹配时
  // 才看 path_hint 的扩展名。探测不出返回 Format::Unknown。
  static Format Detect(const uint8_t* data, size_t size, const std::string& path_hint = {});

  // 执行一次转换。返回本次转换的 Support（Yes 或 Lossy）。
  // 抛 Error：Unsupported（SupportOf 为 No）/ InvalidArgument（显式格式与内容
  // 冲突、目标流不可回滚、缺字体）/ 各族的解码编码错误。
  // 目标流必须先可 Seek + Truncate：否则在写出任何字节之前就失败；中途失败
  // 则截断回调用前的位置（docs/02 §3.6）。
  static Support Run(const ConvertRequest& request);

  // 便捷封装：读文件、按扩展名定目标、原子写文件。
  static Support ConvertFile(const std::string& in_path, const std::string& out_path,
                             Format output_format, const ConvertOptions& options = {},
                             WarningSink* warnings = nullptr);
};

}  // namespace doccore
