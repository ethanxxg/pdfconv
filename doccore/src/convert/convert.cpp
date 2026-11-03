// DocCore - 转换总线实现。Design refs: docs/02 §3.6
#include "doccore/convert.h"

#include <algorithm>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

#include "doccore/deck_layout.h"
#include "doccore/docx.h"
#include "doccore/opc.h"
#include "doccore/pdf_read.h"
#include "doccore/pdf_rebuild.h"
#include "doccore/pptx.h"
#include "doccore/render.h"
#include "doccore/sheet_layout.h"
#include "doccore/text_layout.h"
#include "doccore/xlsx.h"

namespace doccore {
namespace {

constexpr int64_t kMaxInputBytes = 512LL * 1024 * 1024;

void Warn(WarningSink* sink, WarningCode code, const std::string& message) {
  if (sink == nullptr) return;
  Warning w;
  w.code = static_cast<int>(code);
  w.module = "convert";
  w.message = message;
  sink->Add(w);
}

bool IsWordFamily(Format f) {
  return f == Format::Docx || f == Format::Docm || f == Format::Dotx;
}
bool IsSheetFamily(Format f) { return f == Format::Xlsx || f == Format::Xlsm || f == Format::Csv; }
bool IsDeckFamily(Format f) { return f == Format::Pptx; }

// ZIP 容器里按 [Content_Types].xml 判族。条目是 deflate 压缩的，直接在字节里
// 找 "wordprocessingml" 找不到，必须真正解包，所以复用 OPC 的探测。
Format DetectZip(const uint8_t* data, size_t size) {
  try {
    MemoryStream stream(std::vector<uint8_t>(data, data + size));
    const std::unique_ptr<Package> pkg = Package::Open(stream);
    switch (pkg->kind()) {
      case PackageKind::Word: return Format::Docx;
      case PackageKind::Excel: return Format::Xlsx;
      case PackageKind::PowerPoint: return Format::Pptx;
      default: return Format::Unknown;
    }
  } catch (const Error&) {
    return Format::Unknown;
  }
}

Format FromExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return Format::Unknown;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (ext == "docx") return Format::Docx;
  if (ext == "docm") return Format::Docm;
  if (ext == "dotx") return Format::Dotx;
  if (ext == "pdf") return Format::Pdf;
  if (ext == "html" || ext == "htm") return Format::Html;
  if (ext == "txt") return Format::Text;
  if (ext == "png") return Format::Png;
  if (ext == "xlsx") return Format::Xlsx;
  if (ext == "xlsm") return Format::Xlsm;
  if (ext == "csv") return Format::Csv;
  if (ext == "pptx") return Format::Pptx;
  return Format::Unknown;
}

void WriteBytes(Stream& out, const std::string& text) {
  out.Write(reinterpret_cast<const uint8_t*>(text.data()), static_cast<int64_t>(text.size()));
}

std::vector<uint8_t> PngForPage(const LayoutDocument& laid, int page, int dpi,
                                WarningSink* warnings) {
  if (page < 1 || static_cast<size_t>(page) > laid.pages.size()) {
    throw Error(ErrorCode::InvalidArgument, "convert: page out of range");
  }
  RasterOptions raster;
  raster.dpi = dpi > 0 ? dpi : 96;
  const RasterImage img = RasterizePage(laid.pages[static_cast<size_t>(page) - 1], raster, warnings);
  return EncodePng(img);
}

const FontRegistry& RequireFonts(const ConvertOptions& opt) {
  if (opt.fonts == nullptr) {
    throw Error(ErrorCode::InvalidArgument,
                "convert: PDF output needs a FontRegistry (embedded subset font)");
  }
  return *opt.fonts;
}

// ---- 各族的"导出到目标格式" --------------------------------------------

void DocxTo(const std::vector<uint8_t>& bytes, const ConvertRequest& req) {
  text::Document doc = text::OpenMemory(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                         bytes.size()),
                                        req.warnings, req.cancel);
  const Format to = req.output_format;
  if (IsWordFamily(to)) {
    text::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    text::Save(doc, req.output, save);
    return;
  }
  if (to == Format::Text) {
    WriteBytes(req.output, text::ExtractText(doc));
    return;
  }
  if (to == Format::Html) {
    WriteBytes(req.output, text::ToHtml(doc));
    return;
  }
  text::LayoutOptions layout;
  layout.fonts = req.options.fonts;
  layout.default_family = req.options.font_family;
  layout.default_size_pt = req.options.font_size_pt;
  layout.warnings = req.warnings;
  const LayoutDocument laid = text::LayoutText(doc, layout, req.cancel);
  if (to == Format::Pdf) {
    PdfSaveOptions pdf;
    PdfWriter::Write(laid, req.output, RequireFonts(req.options), pdf, req.warnings);
    return;
  }
  const std::vector<uint8_t> png = PngForPage(laid, req.options.page, req.options.dpi, req.warnings);
  req.output.Write(png.data(), static_cast<int64_t>(png.size()));
}

void XlsxTo(const std::vector<uint8_t>& bytes, const ConvertRequest& req) {
  sheet::Workbook book = sheet::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), req.warnings,
      req.cancel);
  if (req.options.recalc) book.Recalculate(req.cancel);
  const Format to = req.output_format;
  if (IsSheetFamily(to)) {
    if (to == Format::Csv) {
      const sheet::Worksheet* ws = book.SheetAt(0);
      if (ws == nullptr) throw Error(ErrorCode::Corrupt, "convert: workbook has no sheet");
      WriteBytes(req.output, sheet::ToCsv(*ws));
      return;
    }
    sheet::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    sheet::Save(book, req.output, save);
    return;
  }
  if (to == Format::Html) {
    WriteBytes(req.output, sheet::ToHtml(book));
    return;
  }
  sheet::SheetLayoutOptions layout;
  layout.fonts = req.options.fonts;
  layout.default_family = req.options.font_family;
  layout.default_size_pt = req.options.font_size_pt;
  layout.warnings = req.warnings;
  const LayoutDocument laid = sheet::LayoutWorkbook(book, layout);
  if (to == Format::Pdf) {
    PdfSaveOptions pdf;
    PdfWriter::Write(laid, req.output, RequireFonts(req.options), pdf, req.warnings);
    return;
  }
  const std::vector<uint8_t> png = PngForPage(laid, req.options.page, req.options.dpi, req.warnings);
  req.output.Write(png.data(), static_cast<int64_t>(png.size()));
}

void PptxTo(const std::vector<uint8_t>& bytes, const ConvertRequest& req) {
  deck::Presentation pres = deck::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), req.warnings,
      req.cancel);
  const Format to = req.output_format;
  if (IsDeckFamily(to)) {
    deck::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    deck::Save(pres, req.output, save);
    return;
  }
  if (to == Format::Text) {
    WriteBytes(req.output, deck::ToText(pres));
    return;
  }
  if (to == Format::Html) {
    WriteBytes(req.output, deck::ToHtml(pres));
    return;
  }
  deck::DeckLayoutOptions layout;
  layout.fonts = req.options.fonts;
  layout.default_family = req.options.font_family;
  layout.default_size_pt = req.options.font_size_pt;
  layout.warnings = req.warnings;
  const LayoutDocument laid = deck::LayoutPresentation(pres, layout);
  if (to == Format::Pdf) {
    PdfSaveOptions pdf;
    PdfWriter::Write(laid, req.output, RequireFonts(req.options), pdf, req.warnings);
    return;
  }
  const std::vector<uint8_t> png = PngForPage(laid, req.options.page, req.options.dpi, req.warnings);
  req.output.Write(png.data(), static_cast<int64_t>(png.size()));
}

void PdfTo(const std::vector<uint8_t>& bytes, const ConvertRequest& req) {
  pdf::PdfOpenOptions open;
  open.warnings = req.warnings;
  open.cancel = req.cancel;
  const Format to = req.output_format;
  if (to == Format::Pdf) {
    // 原样复制页：不改对象图（§3.5.3 的拆分/合并是另一条路径）。
    req.output.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
    return;
  }
  pdf::PdfFile file = pdf::PdfFile::OpenMemory(bytes, open);
  if (to == Format::Text) {
    WriteBytes(req.output, file.ExtractText());
    return;
  }
  if (to == Format::Docx) {
    text::Document doc = pdf::Reconstructor::ToDocument(file, req.options.rebuild, req.warnings);
    text::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    text::Save(doc, req.output, save);
    return;
  }
  if (to == Format::Xlsx) {
    sheet::Workbook book = pdf::Reconstructor::ToWorkbook(file, req.options.rebuild, req.warnings);
    sheet::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    sheet::Save(book, req.output, save);
    return;
  }
  if (to == Format::Pptx) {
    deck::Presentation pres =
        pdf::Reconstructor::ToPresentation(file, req.options.rebuild, req.warnings);
    deck::SaveOptions save;
    save.warnings = req.warnings;
    save.cancel = req.cancel;
    deck::Save(pres, req.output, save);
    return;
  }
  throw Error(ErrorCode::Unsupported, "convert: unsupported PDF target");
}

}  // namespace

const char* SupportName(Support s) noexcept {
  switch (s) {
    case Support::Yes: return "yes";
    case Support::Lossy: return "lossy";
    default: return "no";
  }
}

Support Converter::SupportOf(Format in, Format out) {
  if (in == Format::Unknown || out == Format::Unknown) return Support::No;
  if (IsWordFamily(in)) {
    if (IsWordFamily(out)) return Support::Yes;
    if (out == Format::Pdf || out == Format::Png || out == Format::Text || out == Format::Html) {
      return Support::Yes;
    }
    return Support::No;
  }
  if (IsSheetFamily(in)) {
    if (in == Format::Csv) return Support::No;  // CSV 只作为目标
    if (out == Format::Xlsx || out == Format::Xlsm || out == Format::Csv) return Support::Yes;
    if (out == Format::Pdf || out == Format::Png || out == Format::Html) return Support::Yes;
    return Support::No;
  }
  if (IsDeckFamily(in)) {
    if (out == Format::Pptx) return Support::Yes;
    if (out == Format::Pdf || out == Format::Png || out == Format::Text || out == Format::Html) {
      return Support::Yes;
    }
    return Support::No;
  }
  if (in == Format::Pdf) {
    if (out == Format::Pdf || out == Format::Text) return Support::Yes;
    // 重建方向一律 Lossy：产出的是可编辑文档，不是原文件的复制。
    if (out == Format::Docx || out == Format::Xlsx || out == Format::Pptx) return Support::Lossy;
    // PDF -> PNG 需要 PDF 光栅化器，本波没有。
    return Support::No;
  }
  return Support::No;
}

Format Converter::Detect(const uint8_t* data, size_t size, const std::string& path_hint) {
  if (data == nullptr || size < 5) return FromExtension(path_hint);
  if (std::memcmp(data, "%PDF-", 5) == 0) return Format::Pdf;
  if (size >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04) {
    const Format zip = DetectZip(data, size);
    if (zip != Format::Unknown) return zip;
  }
  return FromExtension(path_hint);
}

Support Converter::Run(const ConvertRequest& request) {
  if (request.cancel != nullptr) request.cancel->ThrowIfCancelled();
  const Format out = request.output_format;
  if (out == Format::Unknown) {
    throw Error(ErrorCode::InvalidArgument, "convert: output_format must be explicit");
  }

  const std::vector<uint8_t> bytes = ReadAll(request.input, kMaxInputBytes);
  Format in = request.input_format;
  const Format detected = Detect(bytes.data(), bytes.size());
  if (in == Format::Unknown) {
    in = detected;
  } else if (detected != Format::Unknown) {
    // 显式格式与内容冲突时直接报错，不静默改用探测结果（docs/02 §3.6）。
    const bool same = (in == detected) ||
                      (IsWordFamily(in) && IsWordFamily(detected)) ||
                      ((in == Format::Xlsx || in == Format::Xlsm) &&
                       (detected == Format::Xlsx || detected == Format::Xlsm));
    if (!same) {
      throw Error(ErrorCode::InvalidArgument,
                  std::string("convert: input_format ") + FormatName(in) +
                      " conflicts with detected " + FormatName(detected));
    }
  }
  if (in == Format::Unknown) {
    throw Error(ErrorCode::InvalidArgument, "convert: cannot detect input format");
  }

  const Support support = SupportOf(in, out);
  if (support == Support::No) {
    throw Error(ErrorCode::Unsupported,
                std::string("convert: ") + FormatName(in) + " -> " + FormatName(out) +
                    " is not supported");
  }

  // 目标流必须可回滚：先在当前位置截断一次（对 Memory/File 流是空操作），
  // 失败就说明它不支持 Seek/Truncate，此时还没有写出任何字节。
  const int64_t start = request.output.Tell();
  request.output.Truncate(start);

  try {
    if (IsWordFamily(in)) {
      DocxTo(bytes, request);
    } else if (in == Format::Xlsx || in == Format::Xlsm) {
      XlsxTo(bytes, request);
    } else if (IsDeckFamily(in)) {
      PptxTo(bytes, request);
    } else if (in == Format::Pdf) {
      PdfTo(bytes, request);
    } else {
      throw Error(ErrorCode::Unsupported, "convert: unsupported input family");
    }
  } catch (...) {
    request.output.Truncate(start);  // 中途失败不留半成品
    throw;
  }

  if (support == Support::Lossy) {
    Warn(request.warnings, WarningCode::RebuildHeuristic,
         std::string("convert: ") + FormatName(in) + " -> " + FormatName(out) +
             " rebuilds content and is lossy");
  }
  return support;
}

Support Converter::ConvertFile(const std::string& in_path, const std::string& out_path,
                               Format output_format, const ConvertOptions& options,
                               WarningSink* warnings) {
  FileStream in(in_path, "rb");
  MemoryStream out;
  ConvertRequest request{in, out, Format::Unknown, output_format, options, nullptr, warnings};
  const Support support = Run(request);
  WriteFileAtomically(out_path, out.data());
  return support;
}

}  // namespace doccore
