// DocCore - PDF 注释写入：在已有 PDF 上做增量更新（PDF 32000-1 §7.5.6）。
//
// 为什么用增量更新而不是整体重写：整体重写要把每个对象重新序列化，字体子集、
// 图像流、加密字典、对象流都可能被无意改变，风险远大于收益。增量更新只做三
// 件事——把原文件字节原样保留、追加新对象、给受影响的页面字典写一个新版本，
// 最后追加一个带 /Prev 的 xref 段。原有内容流一个字节都不动。
//
// 每条注释都生成自己的外观流（/AP /N），不依赖阅读器自行合成，否则 Highlight、
// Square 这类在多数阅读器里会显示成空白。
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doccore/pdf_read.h"

namespace doccore {
namespace pdf {
namespace {

std::string Fmt(double v) {
  if (std::fabs(v) < 0.00005) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

// 名字对象：常规字符原样，其余 #XX 转义。
std::string EscapeName(const std::string& in) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    const bool regular = c > 0x20 && c < 0x7F && std::strchr("()<>[]{}/%#", c) == nullptr;
    if (regular) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('#');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

void WriteHex(const std::string& bytes, std::string* out) {
  static const char* kHex = "0123456789ABCDEF";
  out->push_back('<');
  for (unsigned char c : bytes) {
    out->push_back(kHex[c >> 4]);
    out->push_back(kHex[c & 0xF]);
  }
  out->push_back('>');
}

// 字符串：可打印 ASCII 且无括号反斜杠时用字面量，否则用十六进制。
void WriteString(const std::string& s, std::string* out) {
  bool literal = true;
  for (unsigned char c : s) {
    if (c < 0x20 || c >= 0x7F || c == '(' || c == ')' || c == '\\') {
      literal = false;
      break;
    }
  }
  if (literal) {
    out->push_back('(');
    *out += s;
    out->push_back(')');
    return;
  }
  WriteHex(s, out);
}

// 文本字符串（作者、正文等）。非 ASCII 必须写成 UTF-16BE + BOM，否则读回来
// 会按 PDFDocEncoding 解释成乱码——PDF 32000-1 §7.9.2.2。
void WriteTextString(const std::string& utf8, std::string* out) {
  bool ascii = true;
  for (unsigned char c : utf8) {
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    WriteString(utf8, out);
    return;
  }
  std::string be;
  be.push_back(static_cast<char>(0xFE));
  be.push_back(static_cast<char>(0xFF));
  for (char32_t cp : Utf8ToU32(utf8)) {
    uint32_t v = static_cast<uint32_t>(cp);
    if (v > 0xFFFF) {
      v -= 0x10000;
      const uint16_t hi = static_cast<uint16_t>(0xD800 + (v >> 10));
      const uint16_t lo = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
      be.push_back(static_cast<char>(hi >> 8));
      be.push_back(static_cast<char>(hi & 0xFF));
      be.push_back(static_cast<char>(lo >> 8));
      be.push_back(static_cast<char>(lo & 0xFF));
    } else {
      be.push_back(static_cast<char>((v >> 8) & 0xFF));
      be.push_back(static_cast<char>(v & 0xFF));
    }
  }
  WriteHex(be, out);
}

void WriteObject(const Object& o, std::string* out) {
  switch (o.kind) {
    case ObjKind::Null:
      *out += "null";
      break;
    case ObjKind::Bool:
      *out += o.bool_value ? "true" : "false";
      break;
    case ObjKind::Int:
      *out += std::to_string(o.int_value);
      break;
    case ObjKind::Real:
      *out += Fmt(o.real_value);
      break;
    case ObjKind::String:
      WriteString(o.text, out);
      break;
    case ObjKind::Name:
      out->push_back('/');
      *out += EscapeName(o.text);
      break;
    case ObjKind::Array: {
      out->push_back('[');
      for (size_t i = 0; i < o.array.size(); ++i) {
        if (i != 0) out->push_back(' ');
        if (o.array[i] != nullptr) WriteObject(*o.array[i], out);
        else *out += "null";
      }
      out->push_back(']');
      break;
    }
    case ObjKind::Dict:
    case ObjKind::Stream: {
      *out += "<<";
      if (o.dict != nullptr) {
        for (const auto& kv : *o.dict) {
          out->push_back(' ');
          out->push_back('/');
          *out += EscapeName(kv.first);
          out->push_back(' ');
          if (kv.second != nullptr) WriteObject(*kv.second, out);
          else *out += "null";
        }
      }
      *out += " >>";
      if (o.kind == ObjKind::Stream) {
        *out += "\nstream\n";
        out->append(o.raw.begin(), o.raw.end());
        *out += "\nendstream";
      }
      break;
    }
    case ObjKind::Ref:
      *out += std::to_string(o.ref_num);
      out->push_back(' ');
      *out += std::to_string(o.ref_gen);
      *out += " R";
      break;
  }
}

// 构造一条注释的外观流内容（Form XObject 的 BBox 是 [0 0 w h]）。
std::string AppearanceContent(const PdfAnnotation& a, double w, double h) {
  const double r = ((a.color >> 16) & 0xFF) / 255.0;
  const double g = ((a.color >> 8) & 0xFF) / 255.0;
  const double b = (a.color & 0xFF) / 255.0;
  const std::string rgb = Fmt(r) + " " + Fmt(g) + " " + Fmt(b);
  std::string c;
  switch (a.subtype) {
    case AnnotSubtype::Square:
      c = "q " + rgb + " RG 1.5 w 0.75 0.75 " + Fmt(w - 1.5) + " " + Fmt(h - 1.5) + " re S Q\n";
      break;
    case AnnotSubtype::Circle: {
      // 四段三次贝塞尔逼近圆，k = 4/3·(√2−1)。
      const double kx = w * 0.5 * 0.5522847498;
      const double ky = h * 0.5 * 0.5522847498;
      const double cx = w * 0.5;
      const double cy = h * 0.5;
      const double rx = w * 0.5 - 0.75;
      const double ry = h * 0.5 - 0.75;
      c = "q " + rgb + " RG 1.5 w " + Fmt(cx + rx) + " " + Fmt(cy) + " m ";
      c += Fmt(cx + rx) + " " + Fmt(cy + ky) + " " + Fmt(cx + kx) + " " + Fmt(cy + ry) + " " +
           Fmt(cx) + " " + Fmt(cy + ry) + " c ";
      c += Fmt(cx - kx) + " " + Fmt(cy + ry) + " " + Fmt(cx - rx) + " " + Fmt(cy + ky) + " " +
           Fmt(cx - rx) + " " + Fmt(cy) + " c ";
      c += Fmt(cx - rx) + " " + Fmt(cy - ky) + " " + Fmt(cx - kx) + " " + Fmt(cy - ry) + " " +
           Fmt(cx) + " " + Fmt(cy - ry) + " c ";
      c += Fmt(cx + kx) + " " + Fmt(cy - ry) + " " + Fmt(cx + rx) + " " + Fmt(cy - ky) + " " +
           Fmt(cx + rx) + " " + Fmt(cy) + " c S Q\n";
      break;
    }
    case AnnotSubtype::Highlight:
      // 正片叠底 + 半透明：这才是荧光笔的观感。
      c = "q /GS0 gs " + rgb + " rg 0 0 " + Fmt(w) + " " + Fmt(h) + " re f Q\n";
      break;
    case AnnotSubtype::Underline:
      c = "q " + rgb + " RG 1 w 0 1 m " + Fmt(w) + " 1 l S Q\n";
      break;
    case AnnotSubtype::StrikeOut:
      c = "q " + rgb + " RG 1 w 0 " + Fmt(h * 0.5) + " m " + Fmt(w) + " " + Fmt(h * 0.5) +
          " l S Q\n";
      break;
    case AnnotSubtype::Squiggly: {
      c = "q " + rgb + " RG 1 w 0 1 m ";
      const double step = 3.0;
      bool up = true;
      for (double x = step; x <= w; x += step) {
        c += Fmt(x) + " " + Fmt(up ? 3.0 : 0.0) + " l ";
        up = !up;
      }
      c += "S Q\n";
      break;
    }
    case AnnotSubtype::Line:
      c = "q " + rgb + " RG 1.5 w 0 0 m " + Fmt(w) + " " + Fmt(h) + " l S Q\n";
      break;
    case AnnotSubtype::Ink: {
      // 没有笔迹点集时退化成一个矩形框，至少位置可见。
      c = "q " + rgb + " RG 1.5 w 1 1 " + Fmt(w - 2) + " " + Fmt(h - 2) + " re S Q\n";
      break;
    }
    case AnnotSubtype::FreeText: {
      const double size = 11.0;
      c = "q " + rgb + " rg BT /F1 " + Fmt(size) + " Tf " + Fmt(2.0) + " " +
          Fmt(std::max(2.0, h - size - 2.0)) + " Td (";
      std::string text;
      for (char ch : a.contents) {
        if (ch == '(' || ch == ')' || ch == '\\') text.push_back('\\');
        if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7F) {
          text.push_back(ch);
        } else {
          text.push_back('?');  // 非 ASCII 走不了 Helvetica 单字节编码
        }
      }
      c += text;
      c += ") Tj ET Q\n";
      break;
    }
    case AnnotSubtype::Stamp: {
      // 双线边框 + 居中的大写标签。
      const std::string label = a.icon.empty() ? "STAMP" : a.icon;
      std::string upper;
      for (char ch : label) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      const double size = std::min(18.0, std::max(8.0, h * 0.45));
      const double tw = size * 0.55 * static_cast<double>(upper.size());
      const double tx = std::max(2.0, (w - tw) * 0.5);
      const double ty = std::max(2.0, (h - size) * 0.5 + size * 0.2);
      c = "q " + rgb + " RG 2 w 1 1 " + Fmt(w - 2) + " " + Fmt(h - 2) + " re S ";
      c += "0.6 w 3 3 " + Fmt(w - 6) + " " + Fmt(h - 6) + " re S ";
      c += rgb + " rg BT /F1 " + Fmt(size) + " Tf " + Fmt(tx) + " " + Fmt(ty) + " Td (" + upper +
           ") Tj ET Q\n";
      break;
    }
    default:
      break;
  }
  return c;
}

struct NewObject {
  int num = 0;
  size_t offset = 0;
};

class IncrementalWriter {
 public:
  IncrementalWriter(const PdfFile& file, WarningSink* warnings)
      : file_(file), warnings_(warnings) {}

  std::vector<uint8_t> Run(const std::vector<PdfAnnotation>& annots) {
    const std::string& original = file_.raw_bytes();
    if (file_.startxref_offset() == 0) {
      throw Error(ErrorCode::Unsupported,
                  "pdf: cannot append annotations to a file without a usable xref table");
    }
    out_.assign(original.begin(), original.end());
    if (!out_.empty() && out_.back() != '\n') out_.push_back('\n');

    next_num_ = NextObjectNumber();
    max_num_ = next_num_ - 1;

    // 按页分组：同一页的注释要合并进同一个 /Annots。
    std::map<int, std::vector<int>> by_page;  // page index -> 注释对象号
    for (const PdfAnnotation& a : annots) {
      if (a.page < 0 || a.page >= file_.page_count()) {
        Warn("annotation on missing page skipped: page " + std::to_string(a.page));
        continue;
      }
      const int num = EmitAnnotation(a);
      if (num != 0) by_page[a.page].push_back(num);
    }

    // 改写受影响的页面字典（同名对象的新版本）。
    for (const auto& kv : by_page) {
      const PdfPage& page = file_.page(kv.first);
      RewritePageAnnots(page, kv.second);
    }

    WriteXrefAndTrailer();
    return std::move(out_bytes_);
  }

 private:
  int NextObjectNumber() const {
    int max_num = 0;
    if (ObjectPtr trailer = file_.trailer()) {
      if (const ObjectPtr* size = trailer->Find("Size")) {
        const ObjectPtr n = file_.Resolve(*size);
        if (n != nullptr && n->is_number()) max_num = static_cast<int>(n->number());
      }
    }
    // /Size 比最大对象号大 1；再加上页面对象号，取两者较大值。
    for (int i = 0; i < file_.page_count(); ++i) {
      max_num = std::max(max_num, file_.page(i).obj_num);
    }
    return max_num + 1;
  }

  void Warn(const std::string& message) {
    if (warnings_ == nullptr) return;
    Warning w;
    w.code = static_cast<int>(WarningCode::AnnotSkipped);
    w.module = "pdf";
    w.message = message;
    warnings_->Add(w);
  }

  // 追加一个对象，返回它的对象号。
  int EmitObject(const std::string& body) {
    const int num = next_num_++;
    new_objects_.push_back(NewObject{num, out_.size()});
    out_ += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
    max_num_ = std::max(max_num_, num);
    return num;
  }

  int EmitStream(const std::string& dict_extra, const std::string& body) {
    const std::string dict = "<< " + dict_extra + " /Length " + std::to_string(body.size()) +
                             " >>\nstream\n" + body + "\nendstream";
    return EmitObject(dict);
  }

  int EmitAnnotation(const PdfAnnotation& a) {
    const std::string subtype = a.subtype_name.empty() ? AnnotSubtypeName(a.subtype)
                                                       : a.subtype_name;
    if (subtype.empty() || subtype == "Unknown") return 0;

    // 外观流（/AP /N）。便签（Text）与 Popup 由阅读器自己画图标，不生成。
    int ap_obj = 0;
    const bool wants_ap = a.subtype != AnnotSubtype::Text && a.subtype != AnnotSubtype::Popup &&
                          a.subtype != AnnotSubtype::Link;
    if (wants_ap && a.w > 0.0 && a.h > 0.0) {
      const std::string content = AppearanceContent(a, a.w, a.h);
      if (!content.empty()) {
        std::string resources = "<< /ProcSet [/PDF /Text]";
        if (a.subtype == AnnotSubtype::Highlight) {
          resources += " /ExtGState << /GS0 << /Type /ExtGState /BM /Multiply /ca 0.55 >> >>";
        }
        if (a.subtype == AnnotSubtype::FreeText || a.subtype == AnnotSubtype::Stamp) {
          resources += " /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >>";
        }
        resources += " >>";
        const std::string dict = "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 " +
                                 Fmt(a.w) + " " + Fmt(a.h) + "] /Resources " + resources;
        ap_obj = EmitStream(dict, content);
      }
    }

    std::string d = "<< /Type /Annot /Subtype /" + EscapeName(subtype);
    d += " /Rect [" + Fmt(a.x) + " " + Fmt(a.y) + " " + Fmt(a.x + a.w) + " " + Fmt(a.y + a.h) + "]";
    d += " /F " + std::to_string(a.flags != 0 ? a.flags : 4);  // 默认 Print
    if (a.color >= 0) {
      d += " /C [" + Fmt(((a.color >> 16) & 0xFF) / 255.0) + " " +
           Fmt(((a.color >> 8) & 0xFF) / 255.0) + " " + Fmt((a.color & 0xFF) / 255.0) + "]";
    }
    if (a.opacity < 1.0) d += " /CA " + Fmt(a.opacity);
    if (!a.author.empty()) {
      d += " /T ";
      WriteTextString(a.author, &d);
    }
    if (!a.contents.empty()) {
      d += " /Contents ";
      WriteTextString(a.contents, &d);
    }
    if (!a.subject.empty()) {
      d += " /Subj ";
      WriteTextString(a.subject, &d);
    }
    if (!a.modified.empty()) {
      d += " /M ";
      WriteTextString(a.modified, &d);
    }
    if (!a.unique_name.empty()) {
      d += " /NM ";
      WriteTextString(a.unique_name, &d);
    }
    if (!a.icon.empty()) d += " /Name /" + EscapeName(a.icon);
    if (a.open) d += " /Open true";
    if (!a.quads.empty()) {
      d += " /QuadPoints [";
      for (const PdfQuad& q : a.quads) {
        d += Fmt(q.x1) + " " + Fmt(q.y1) + " " + Fmt(q.x2) + " " + Fmt(q.y2) + " " + Fmt(q.x3) +
             " " + Fmt(q.y3) + " " + Fmt(q.x4) + " " + Fmt(q.y4) + " ";
      }
      d += "]";
    }
    if (ap_obj != 0) d += " /AP << /N " + std::to_string(ap_obj) + " 0 R >>";
    d += " >>";
    return EmitObject(d);
  }

  void RewritePageAnnots(const PdfPage& page, const std::vector<int>& new_annots) {
    ObjectPtr page_dict = PageDict(page.obj_num);
    if (page_dict == nullptr || !page_dict->is_dict() || page.obj_num == 0) {
      Warn("cannot rewrite page " + std::to_string(page.index) + ": no page object number");
      return;
    }
    // 复制一份字典，把既有 /Annots 与新注释接起来。
    std::string d = "<<";
    for (const auto& kv : *page_dict->dict) {
      if (kv.first == "Annots") continue;
      d += " /" + EscapeName(kv.first) + " ";
      if (kv.second != nullptr) WriteObject(*kv.second, &d);
      else d += "null";
    }
    d += " /Annots [";
    if (const ObjectPtr* existing = page_dict->Find("Annots")) {
      const ObjectPtr list = file_.Resolve(*existing);
      if (list != nullptr && list->kind == ObjKind::Array) {
        for (const ObjectPtr& item : list->array) {
          if (item == nullptr) continue;
          WriteObject(*item, &d);
          d.push_back(' ');
        }
      }
    }
    for (int num : new_annots) d += std::to_string(num) + " 0 R ";
    d += "] >>";

    new_objects_.push_back(NewObject{page.obj_num, out_.size()});
    out_ += std::to_string(page.obj_num) + " 0 obj\n" + d + "\nendobj\n";
    max_num_ = std::max(max_num_, page.obj_num);
  }

  ObjectPtr PageDict(int obj_num) const {
    if (obj_num == 0) return nullptr;
    // 通过 trailer 之外的路子拿不到私有解析器，这里用 Resolve 的 Ref 语义。
    Object ref;
    ref.kind = ObjKind::Ref;
    ref.ref_num = obj_num;
    ref.ref_gen = 0;
    return file_.Resolve(std::make_shared<Object>(ref));
  }

  void WriteXrefAndTrailer() {
    // 传统 xref 表：对象 0 的 free 项 + 每个新写对象的 20 字节条目。
    const size_t table_offset = out_.size();
    std::sort(new_objects_.begin(), new_objects_.end(),
              [](const NewObject& a, const NewObject& b) { return a.num < b.num; });
    // 同一个对象号可能写多次（页面被改写），只保留最后一次。
    std::vector<NewObject> entries;
    for (const NewObject& o : new_objects_) {
      if (!entries.empty() && entries.back().num == o.num) {
        entries.back().offset = o.offset;
      } else {
        entries.push_back(o);
      }
    }

    out_ += "xref\n0 1\n0000000000 65535 f \n";
    size_t i = 0;
    while (i < entries.size()) {
      size_t j = i;
      while (j + 1 < entries.size() && entries[j + 1].num == entries[j].num + 1) ++j;
      out_ += std::to_string(entries[i].num) + " " + std::to_string(j - i + 1) + "\n";
      for (size_t k = i; k <= j; ++k) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010llu %05d n \n",
                      static_cast<unsigned long long>(entries[k].offset), 0);
        out_ += buf;
      }
      i = j + 1;
    }

    out_ += "trailer\n<< /Size " + std::to_string(max_num_ + 1);
    ObjectPtr trailer = file_.trailer();
    if (trailer != nullptr) {
      if (const ObjectPtr* root = trailer->Find("Root")) {
        out_ += " /Root ";
        if (*root != nullptr) WriteObject(**root, &out_);
        else out_ += "null";
      }
      if (const ObjectPtr* info = trailer->Find("Info")) {
        out_ += " /Info ";
        if (*info != nullptr) WriteObject(**info, &out_);
        else out_ += "null";
      }
      if (const ObjectPtr* id = trailer->Find("ID")) {
        out_ += " /ID ";
        if (*id != nullptr) WriteObject(**id, &out_);
        else out_ += "null";
      }
    }
    out_ += " /Prev " + std::to_string(file_.startxref_offset());
    out_ += " >>\nstartxref\n" + std::to_string(table_offset) + "\n%%EOF\n";
    out_bytes_.assign(out_.begin(), out_.end());
  }

  const PdfFile& file_;
  WarningSink* warnings_ = nullptr;
  std::string out_;
  std::vector<NewObject> new_objects_;
  int next_num_ = 1;
  int max_num_ = 0;
  std::vector<uint8_t> out_bytes_;
};

}  // namespace

std::vector<uint8_t> AddAnnotations(const PdfFile& file, const std::vector<PdfAnnotation>& annots,
                                    WarningSink* warnings) {
  IncrementalWriter writer(file, warnings);
  return writer.Run(annots);
}

}  // namespace pdf
}  // namespace doccore
