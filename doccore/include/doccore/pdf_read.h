// DocCore - PDF 读取：对象图、页面树、内容流、文本抽取、几何对象抽取。
// Design refs: docs/02 §3.5.2、§3.5.4
//
//   pdf::PdfFile file = pdf::PdfFile::OpenPath("in.pdf");
//   for (int i = 0; i < file.page_count(); ++i) {
//     const pdf::PdfPage& page = file.page(i);
//     std::string text = page.ExtractText();           // 阅读顺序
//     const auto& runs = page.text_runs();             // 带坐标，给重建器用
//   }
//
// 只读，不改对象图。加密文件在本波只做识别（NeedPassword / Unsupported），
// 解密留到 P9；扫描件（无文本层）由调用方按 NoTextLayer 处理。
//
// 坐标系：所有几何对象都是 PDF 用户空间（左下原点），已减去 MediaBox 原点；
// 旋转页（/Rotate 90/270）的 width/height 已交换，但几何对象不做旋转。
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace pdf {

// ---- 基础对象 -----------------------------------------------------------

enum class ObjKind : uint8_t {
  Null,
  Bool,
  Int,
  Real,
  String,  // 原始字节，可能是二进制
  Name,    // 不带前导 '/'
  Array,
  Dict,
  Stream,  // 带 /Length 与 /Filter
  Ref,
};

struct Object;
using ObjectPtr = std::shared_ptr<Object>;
using Dict = std::map<std::string, ObjectPtr>;

struct Object {
  ObjKind kind = ObjKind::Null;
  bool bool_value = false;
  int64_t int_value = 0;
  double real_value = 0.0;
  std::string text;              // String 的字节 / Name 的名字
  std::vector<ObjectPtr> array;  // Array
  std::shared_ptr<Dict> dict;    // Dict / Stream
  int32_t ref_num = 0;           // Ref
  int32_t ref_gen = 0;
  std::vector<uint8_t> raw;      // Stream 的原始（未解码）字节

  bool is_null() const noexcept { return kind == ObjKind::Null; }
  bool is_number() const noexcept { return kind == ObjKind::Int || kind == ObjKind::Real; }
  double number() const noexcept { return kind == ObjKind::Real ? real_value : double(int_value); }
  bool is_dict() const noexcept { return kind == ObjKind::Dict || kind == ObjKind::Stream; }
  bool is_name(std::string_view want) const noexcept {
    return kind == ObjKind::Name && text == want;
  }
  // Dict 查找；找不到返回 nullptr。
  const ObjectPtr* Find(std::string_view key) const;
};

// PDF 字符串转 UTF-8：带 BOM 走 UTF-16BE，否则按 PDFDocEncoding（Latin-1 近似）。
std::string PdfStringToUtf8(std::string_view bytes);

// ---- 页面几何对象 -------------------------------------------------------

// 文本片段：一次 Tj/TJ 里同一字体的连续字形。
struct PdfTextRun {
  double x = 0.0;         // 起点 x（用户空间）
  double y = 0.0;         // 基线 y
  double width = 0.0;     // 水平推进
  double size_pt = 12.0;  // 实际字号（已乘 CTM 缩放）
  std::string font_name;  // 资源名（/F1）
  std::string family;     // /BaseFont 去掉子集前缀
  bool bold = false;
  bool italic = false;
  int color = 0x000000;  // 填充色
  std::string text_utf8;

  double right() const noexcept { return x + width; }
  double top() const noexcept { return y + size_pt * 0.8; }
  double bottom() const noexcept { return y - size_pt * 0.2; }
};

// 水平或垂直线段（路径化简而来）。
struct PdfLine {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  double width_pt = 1.0;
  int color = 0x000000;

  bool horizontal() const noexcept { return std::abs(y1 - y2) < 0.5; }
  bool vertical() const noexcept { return std::abs(x1 - x2) < 0.5; }
  double min_x() const noexcept { return x1 < x2 ? x1 : x2; }
  double max_x() const noexcept { return x1 < x2 ? x2 : x1; }
  double min_y() const noexcept { return y1 < y2 ? y1 : y2; }
  double max_y() const noexcept { return y1 < y2 ? y2 : y1; }
};

// 矩形（re 算子 / 填充块）。
struct PdfRect {
  double x = 0, y = 0, w = 0, h = 0;
  int fill = -1;  // 纯色填充，-1 表示无
  int stroke = -1;
  double stroke_pt = 0.0;
};

// 图像 XObject 的落位矩形。
struct PdfImagePlacement {
  double x = 0, y = 0, w = 0, h = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  std::string xobject_name;
  // Do 时解析出的对象本体：图像也可能只挂在某个 Form XObject 的 /Resources 里，
  // 这时按名字在页级资源中是找不到的（见 ResourceScope）。
  ObjectPtr xobject;
};

// ---- 注释（PDF 32000-1 §12.5.6）----------------------------------------

enum class AnnotSubtype : uint8_t {
  Text,          // 便签
  Link,
  FreeText,      // 直接显示在页面上的文字框
  Line,
  Square,
  Circle,
  Polygon,
  PolyLine,
  Highlight,
  Underline,
  Squiggly,
  StrikeOut,
  Stamp,
  Caret,
  Ink,
  Popup,
  FileAttachment,
  Sound,
  Movie,
  Widget,
  Screen,
  PrinterMark,
  TrapNet,
  Watermark,
  ThreeD,
  Redact,
  Unknown,
};

const char* AnnotSubtypeName(AnnotSubtype s) noexcept;
AnnotSubtype AnnotSubtypeFromName(std::string_view name) noexcept;

// /QuadPoints 的一个四边形：8 个数，顺序是四个顶点（通常按行给出）。
struct PdfQuad {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  double x3 = 0, y3 = 0, x4 = 0, y4 = 0;
};

// 一条注释。坐标是 PDF 用户空间（原点在页左下），与 PdfTextRun 一致。
struct PdfAnnotation {
  AnnotSubtype subtype = AnnotSubtype::Unknown;
  std::string subtype_name;  // 原文子类型名；Unknown 时靠它保留信息
  int page = 0;
  double x = 0, y = 0, w = 0, h = 0;  // /Rect
  std::string contents;              // /Contents
  std::string author;                // /T
  std::string subject;               // /Subj
  std::string modified;              // /M
  std::string unique_name;           // /NM
  int color = -1;                    // /C 第一项（0xRRGGBB），-1 表示未设置
  double opacity = 1.0;              // /CA
  int flags = 0;                     // /F
  std::string icon;                  // /Name（Text 的图标、Stamp 的样式名）
  bool open = false;                 // /Open（Popup 展开）
  std::vector<PdfQuad> quads;        // /QuadPoints
  bool has_appearance = false;       // 是否存在 /AP

  double right() const noexcept { return x + w; }
  double top() const noexcept { return y + h; }
};

// ---- 字体 ---------------------------------------------------------------

struct FontDecoder {
  bool composite = false;
  int bytes_per_code = 1;
  // 预定义 Unicode CMap（UniGB-UCS2-H / UniGB-UTF16-H / UniCNS-* / UniJIS-* /
  // UniKS-*）没有 ToUnicode 时，码值本身就是 Unicode：直接当码点输出。
  bool identity_unicode = false;
  std::map<uint32_t, std::string> to_unicode;  // 码 -> UTF-8（来自 /ToUnicode）
  // 码 -> UTF-8（来自 /Encoding：基编码表 + /Differences）。仅在无 /ToUnicode 时兜底使用，
  // 用于无 ToUnicode 的非嵌入字体（如 LaTeX 的 Times / CMSY / MathTime 等）。
  std::map<uint32_t, std::string> enc_unicode;
  std::map<uint32_t, double> widths;           // 码 -> 宽度（千分之一字号）
  double default_width = 500.0;
  std::string family;
  bool bold = false;
  bool italic = false;

  std::vector<std::pair<uint32_t, std::string>> Decode(std::string_view bytes) const;
  double WidthOf(uint32_t code) const;
};

// ---- 页面 ---------------------------------------------------------------

struct PageGeometry {
  double width = 0.0;
  double height = 0.0;
};

// 内容流资源域：/Resources 可以挂在页上，也可以挂在 Form XObject 上，
// Form 内的资源覆盖同名页级资源。解释 Form 时按这个名字链逐级回退。
struct ResourceScope {
  std::map<std::string, FontDecoder> fonts;
  std::map<std::string, ObjectPtr> xobjects;
  const ResourceScope* parent = nullptr;

  const FontDecoder* FindFont(std::string_view name) const;
  const ObjectPtr* FindXObject(std::string_view name) const;
};

// 内部接口：内容流解释器经由它访问文件层（流字节解码 / 字体描述器 / 引用解析）。
// Form XObject 的 /Resources 是间接引用，解析必须回到 PDF 对象图。
struct InterpHost {
  virtual ~InterpHost() = default;
  virtual std::vector<uint8_t> StreamBytes(const ObjectPtr& stream) const = 0;
  virtual FontDecoder BuildFontDecoder(const ObjectPtr& font) const = 0;
  virtual ObjectPtr Resolve(const ObjectPtr& obj) const = 0;
  // 按对象号取对象。只有文件层实现，Form XObject 的局部宿主返回空。
  virtual ObjectPtr ObjectAt(int32_t num) const {
    (void)num;
    return nullptr;
  }
};

// 页面是纯数据 + 惰性解析：几何对象在第一次访问时从内容流算出来。
class PdfPage {
 public:
  int index = 0;
  int obj_num = 0;  // 页面字典的对象号（增量更新改写页面时要用）
  PageGeometry size;
  int rotation = 0;

  // 惰性解析后的几何对象（按内容流顺序）。
  const std::vector<PdfTextRun>& text_runs() const;
  const std::vector<PdfLine>& lines() const;
  const std::vector<PdfRect>& rects() const;
  const std::vector<PdfImagePlacement>& images() const;
  // 页面 /Annots 里的注释（按出现顺序）。
  const std::vector<PdfAnnotation>& annotations() const;

  // 阅读顺序纯文本：同基线合并成行，行按上到下。
  std::string ExtractText() const;

  // 最大图像是否覆盖页面 80% 以上（扫描页判定）。
  bool looks_scanned() const;

  // 内容流原文（已解码过滤器），诊断用。
  const std::string& content() const noexcept { return content_; }

  // 内部：由 PdfFile 填充。
  void SetContent(std::string content) { content_ = std::move(content); }
  void AddFont(std::string name, FontDecoder decoder) {
    fonts_[std::move(name)] = std::move(decoder);
  }
  void AddXObject(std::string name, ObjectPtr object) {
    xobjects_[std::move(name)] = std::move(object);
  }
  void SetOrigin(double x, double y) {
    origin_x_ = x;
    origin_y_ = y;
  }
  double origin_x() const noexcept { return origin_x_; }
  double origin_y() const noexcept { return origin_y_; }

  // 内部：解释 Form XObject 时需要文件层（流字节 / 字体 / 引用解析）。
  void SetHost(const InterpHost* host) noexcept { host_ = host; }

  // 内部：按资源名取 XObject（重建器取图像像素用），沿 Form 资源链回退。
  const ObjectPtr* FindXObject(std::string_view name) const;

 private:
  void EnsureParsed() const;
  void EnsureScope() const;

  std::string content_;
  double origin_x_ = 0.0;
  double origin_y_ = 0.0;
  const InterpHost* host_ = nullptr;
  std::map<std::string, FontDecoder> fonts_;
  std::map<std::string, ObjectPtr> xobjects_;
  mutable std::unique_ptr<ResourceScope> scope_root_;
  mutable bool parsed_ = false;
  mutable std::vector<PdfTextRun> runs_;
  mutable std::vector<PdfLine> lines_;
  mutable std::vector<PdfRect> rects_;
  mutable std::vector<PdfImagePlacement> images_;
  mutable std::vector<PdfAnnotation> annots_;
  mutable bool annots_loaded_ = false;
};

// ---- 文件 ---------------------------------------------------------------

struct PdfOpenOptions {
  std::string password;
  WarningSink* warnings = nullptr;
  CancelToken* cancel = nullptr;
};

// 加密文档的权限位（PDF 32000-1 Table 22）。这些位只对已加密文档有意义；
// 未加密的文档按"全都可以"处理，符合阅读器惯例。
struct PdfPermissions {
  int raw = 0;  // /P 原值
  bool print = true;
  bool modify = true;                 // 改内容（位 4）
  bool copy = true;                   // 复制/摘取文本与图形（位 5）
  bool annotate = true;               // 加注释、填表单（位 6）
  bool fill_forms = true;             // 只填已有表单域（位 9，R3+）
  bool extract_accessibility = true;  // 为无障碍提取（位 10，R3+）
  bool assemble = true;               // 插/删/旋转页面（位 11，R3+）
  bool print_high_quality = true;     // 高质量打印（位 12，R3+）
};

class PdfFile {
 public:
  PdfFile();
  ~PdfFile();
  PdfFile(PdfFile&&) noexcept;
  PdfFile& operator=(PdfFile&&) noexcept;

  static PdfFile OpenPath(const std::string& path, const PdfOpenOptions& opts = {});
  static PdfFile OpenMemory(std::vector<uint8_t> bytes, const PdfOpenOptions& opts = {});

  int page_count() const noexcept;
  const PdfPage& page(int index) const;

  const std::string& version() const noexcept;
  bool encrypted() const noexcept;
  // 未加密的文档返回全 true 的默认值。
  const PdfPermissions& permissions() const noexcept;
  const std::string& title() const noexcept;
  size_t object_count() const noexcept;

  ObjectPtr Resolve(const ObjectPtr& obj) const;

  // 全文（每页之间用 \f 分隔）。
  std::string ExtractText() const;

  // 内部：给重建器读页面资源（字体/XObject）。
  const PdfPage* page_at(int index) const;

  // 增量更新用：原文件字节、trailer 字典、startxref 偏移。
  const std::string& raw_bytes() const noexcept;
  ObjectPtr trailer() const noexcept;
  size_t startxref_offset() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 增量更新：把注释追加到已有 PDF 上，返回完整的新文件字节。
// 原文件内容逐字节保留，只追加新对象、改写受影响的页面字典，并在末尾写一个
// 带 /Prev 的新 xref 段（PDF 32000-1 §7.5.6）。这样原有内容流、字体、加密
// 状态都不受影响。同一页的既有 /Annots 会被保留并追加。
std::vector<uint8_t> AddAnnotations(const PdfFile& file, const std::vector<PdfAnnotation>& annots,
                                    WarningSink* warnings = nullptr);

}  // namespace pdf
}  // namespace doccore
