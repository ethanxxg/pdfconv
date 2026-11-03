// DocCore - deck DOM -> PPTX writer. Design refs: docs/02 §3.7
//
// 固定生成一份母版 + 一个"标题和内容"版式 + 一份主题，幻灯片按 DOM 逐页写出。
// 这样保证产物在 PowerPoint / python-pptx 里都能正常打开，不需要用户先有模板。
#include "doccore/pptx.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "doccore/zip_write.h"
#include "pptx_timing.h"

namespace doccore {
namespace deck {
namespace {

// ---- XML 输出 -----------------------------------------------------------

std::string EscText(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string EscAttr(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

using Attrs = std::vector<std::pair<std::string, std::string>>;

class Buf {
 public:
  void Decl() { out_ += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"; }

  void Open(const std::string& name, Attrs attrs = {}) {
    out_ += '<';
    out_ += name;
    for (const auto& kv : attrs) {
      out_ += ' ';
      out_ += kv.first;
      out_ += "=\"";
      out_ += EscAttr(kv.second);
      out_ += '"';
    }
    out_ += '>';
    stack_.push_back({name, out_.size()});
  }

  void Close() {
    if (stack_.empty()) return;
    const Frame frame = stack_.back();
    stack_.pop_back();
    if (frame.mark == out_.size()) {
      out_.resize(out_.size() - 1);
      out_ += "/>";
      return;
    }
    out_ += "</";
    out_ += frame.name;
    out_ += '>';
  }

  void Leaf(const std::string& name, Attrs attrs = {}) {
    Open(name, attrs);
    Close();
  }

  void TextLeaf(const std::string& name, const std::string& value, Attrs attrs = {}) {
    const bool preserve = !value.empty() && (value.front() == ' ' || value.back() == ' ' ||
                                             value.find('\n') != std::string::npos);
    if (preserve) attrs.emplace_back("xml:space", "preserve");
    Open(name, attrs);
    out_ += EscText(value);
    Close();
  }

  // 原样拼入一段已经成形的 XML（切换/动画子树由 pptx_timing.cpp 生成，
  // 里面混用 p:/a:/r: 三个前缀，交给自动加前缀的构造器必然写错）。
  void RawMarkup(const std::string& markup) { out_ += markup; }

  const std::string& str() const noexcept { return out_; }

 private:
  struct Frame {
    std::string name;
    size_t mark = 0;
  };
  std::string out_;
  std::vector<Frame> stack_;
};

std::vector<uint8_t> StrBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

std::string Rgb(int rgb) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
  return buf;
}

std::string Emu(int64_t value) { return std::to_string(value); }

// ---- 命名空间 -----------------------------------------------------------

const char* kNsA = "http://schemas.openxmlformats.org/drawingml/2006/main";
const char* kNsR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kNsP = "http://schemas.openxmlformats.org/presentationml/2006/main";

Attrs PresentationRoot() {
  return {{"xmlns:a", kNsA}, {"xmlns:r", kNsR}, {"xmlns:p", kNsP}};
}

// ---- 文本 ---------------------------------------------------------------

int SizeHundredths(const Presentation& pres, const RunFormat& format, const Shape& shape) {
  double size = format.size_pt;
  if (size <= 0.0) {
    const bool is_title = shape.placeholder == "title" || shape.placeholder == "ctrTitle";
    size = is_title ? 40.0 : pres.default_size_pt;
  }
  const int value = static_cast<int>(size * 100.0 + 0.5);
  return value > 0 ? value : 1800;
}

void WriteRunProps(Buf& x, const Presentation& pres, const Run& run, const Shape& shape) {
  Attrs attrs{{"lang", "en-US"}, {"sz", std::to_string(SizeHundredths(pres, run.format, shape))}};
  if (run.format.bold) attrs.emplace_back("b", "1");
  if (run.format.italic) attrs.emplace_back("i", "1");
  if (run.format.underline) attrs.emplace_back("u", "sng");
  x.Open("a:rPr", attrs);
  if (run.format.color >= 0) {
    x.Open("a:solidFill");
    x.Leaf("a:srgbClr", {{"val", Rgb(run.format.color)}});
    x.Close();
  }
  if (!run.format.family.empty()) {
    x.Leaf("a:latin", {{"typeface", run.format.family}});
  }
  x.Close();
}

std::string AlignName(int align) {
  switch (align) {
    case 0: return "l";
    case 1: return "ctr";
    case 2: return "r";
    case 3: return "just";
    default: return std::string();
  }
}

// 表格单元格里的文本容器是 a:txBody，形状里的是 p:txBody——写错 python-pptx 会读到空单元格。
void WriteTextBody(Buf& x, const Presentation& pres, const TextBody& body, const Shape& shape,
                   bool table_cell = false) {
  Attrs body_attrs{{"wrap", body.wrap ? "square" : "none"}};
  if (body.anchor == 1) body_attrs.emplace_back("anchor", "ctr");
  else if (body.anchor == 2) body_attrs.emplace_back("anchor", "b");
  body_attrs.emplace_back("lIns", Emu(body.inset_left));
  body_attrs.emplace_back("tIns", Emu(body.inset_top));
  body_attrs.emplace_back("rIns", Emu(body.inset_right));
  body_attrs.emplace_back("bIns", Emu(body.inset_bottom));
  x.Open(table_cell ? "a:txBody" : "p:txBody");
  x.Leaf("a:bodyPr", body_attrs);
  x.Leaf("a:lstStyle");
  if (body.paragraphs.empty()) {
    x.Open("a:p");
    x.Leaf("a:endParaRPr", {{"lang", "en-US"}});
    x.Close();
    x.Close();
    return;
  }
  for (const Paragraph& paragraph : body.paragraphs) {
    x.Open("a:p");
    Attrs props;
    const std::string align = AlignName(paragraph.align);
    if (!align.empty()) props.emplace_back("algn", align);
    if (paragraph.level > 0) props.emplace_back("lvl", std::to_string(paragraph.level));
    if (!props.empty()) x.Leaf("a:pPr", props);
    for (const Run& run : paragraph.runs) {
      // 段内换行：拆成 a:r / a:br / a:r，保证换行不丢。
      size_t start = 0;
      while (start <= run.text.size()) {
        const size_t newline = run.text.find('\n', start);
        const std::string piece = run.text.substr(
            start, newline == std::string::npos ? std::string::npos : newline - start);
        if (!piece.empty()) {
          x.Open("a:r");
          WriteRunProps(x, pres, run, shape);
          x.TextLeaf("a:t", piece);
          x.Close();
        }
        if (newline == std::string::npos) break;
        x.Leaf("a:br");
        start = newline + 1;
      }
    }
    x.Close();
  }
  x.Close();
}

// ---- 形状 ---------------------------------------------------------------

void WriteTransform(Buf& x, const ShapeGeometry& geom, int64_t rotation) {
  Attrs attrs;
  if (rotation != 0) attrs.emplace_back("rot", std::to_string(rotation));
  x.Open("a:xfrm", attrs);
  x.Leaf("a:off", {{"x", Emu(geom.x)}, {"y", Emu(geom.y)}});
  x.Leaf("a:ext", {{"cx", Emu(geom.cx)}, {"cy", Emu(geom.cy)}});
  x.Close();
}

void WriteShapeProps(Buf& x, const Shape& shape) {
  x.Open("p:spPr");
  WriteTransform(x, shape.geom, shape.rotation);
  x.Open("a:prstGeom", {{"prst", "rect"}});
  x.Leaf("a:avLst");
  x.Close();
  if (shape.fill >= 0) {
    x.Open("a:solidFill");
    x.Leaf("a:srgbClr", {{"val", Rgb(shape.fill)}});
    x.Close();
  } else {
    x.Leaf("a:noFill");
  }
  if (shape.line_color >= 0) {
    const int width = static_cast<int>(shape.line_width_pt > 0 ? shape.line_width_pt * kEmuPerPoint
                                                              : 9525);
    x.Open("a:ln", {{"w", std::to_string(width)}});
    x.Open("a:solidFill");
    x.Leaf("a:srgbClr", {{"val", Rgb(shape.line_color)}});
    x.Close();
    x.Close();
  }
  x.Close();
}

void WriteTextShape(Buf& x, const Presentation& pres, const Shape& shape, int id) {
  x.Open("p:sp");
  x.Open("p:nvSpPr");
  x.Leaf("p:cNvPr", {{"id", std::to_string(id)},
                     {"name", shape.name.empty() ? "TextBox " + std::to_string(id) : shape.name}});
  x.Open("p:cNvSpPr");
  x.Leaf("a:spLocks", {{"noGrp", "1"}});
  x.Close();
  if (shape.is_placeholder()) {
    x.Open("p:nvPr");
    x.Leaf("p:ph", {{"type", shape.placeholder}});
    x.Close();
  } else {
    x.Leaf("p:nvPr");
  }
  x.Close();
  WriteShapeProps(x, shape);
  WriteTextBody(x, pres, shape.text, shape);
  x.Close();
}

void WritePictureShape(Buf& x, const Shape& shape, int id, const std::string& rid) {
  x.Open("p:pic");
  x.Open("p:nvPicPr");
  x.Leaf("p:cNvPr", {{"id", std::to_string(id)},
                     {"name", shape.name.empty() ? "Picture " + std::to_string(id) : shape.name}});
  x.Open("p:cNvPicPr");
  x.Leaf("a:picLocks", {{"noChangeAspect", "1"}});
  x.Close();
  x.Leaf("p:nvPr");
  x.Close();
  x.Open("p:blipFill");
  x.Leaf("a:blip", {{"r:embed", rid}});
  x.Open("a:stretch");
  x.Leaf("a:fillRect");
  x.Close();
  x.Close();
  x.Open("p:spPr");
  WriteTransform(x, shape.geom, shape.rotation);
  x.Open("a:prstGeom", {{"prst", "rect"}});
  x.Leaf("a:avLst");
  x.Close();
  x.Close();
  x.Close();
}

void WriteTableShape(Buf& x, const Presentation& pres, const Shape& shape, int id) {
  x.Open("p:graphicFrame");
  x.Open("p:nvGraphicFramePr");
  x.Leaf("p:cNvPr", {{"id", std::to_string(id)},
                     {"name", shape.name.empty() ? "Table " + std::to_string(id) : shape.name}});
  x.Open("p:cNvGraphicFramePr");
  x.Leaf("a:graphicFrameLocks", {{"noGrp", "1"}});
  x.Close();
  x.Leaf("p:nvPr");
  x.Close();
  x.Open("p:xfrm");
  x.Leaf("a:off", {{"x", Emu(shape.geom.x)}, {"y", Emu(shape.geom.y)}});
  x.Leaf("a:ext", {{"cx", Emu(shape.geom.cx)}, {"cy", Emu(shape.geom.cy)}});
  x.Close();
  x.Open("a:graphic");
  x.Open("a:graphicData", {{"uri", "http://schemas.openxmlformats.org/drawingml/2006/table"}});
  x.Open("a:tbl");
  Attrs table_props;
  table_props.emplace_back("firstRow", shape.table.first_row_header ? "1" : "0");
  table_props.emplace_back("bandRow", shape.table.banded_rows ? "1" : "0");
  x.Leaf("a:tblPr", table_props);
  x.Open("a:tblGrid");
  for (int64_t width : shape.table.col_widths) {
    x.Leaf("a:gridCol", {{"w", Emu(width)}});
  }
  x.Close();
  for (size_t row_index = 0; row_index < shape.table.rows.size(); ++row_index) {
    const std::vector<TableCell>& row = shape.table.rows[row_index];
    const int64_t height =
        row_index < shape.table.row_heights.size() ? shape.table.row_heights[row_index] : 370840;
    x.Open("a:tr", {{"h", Emu(height)}});
    for (const TableCell& cell : row) {
      if (cell.merged) continue;
      Attrs cell_attrs;
      if (cell.col_span > 1) cell_attrs.emplace_back("gridSpan", std::to_string(cell.col_span));
      if (cell.row_span > 1) cell_attrs.emplace_back("rowSpan", std::to_string(cell.row_span));
      x.Open("a:tc", cell_attrs);
      WriteTextBody(x, pres, cell.body, shape, true);
      x.Open("a:tcPr");
      if (cell.fill >= 0) {
        x.Open("a:solidFill");
        x.Leaf("a:srgbClr", {{"val", Rgb(cell.fill)}});
        x.Close();
      }
      x.Close();
      x.Close();
    }
    x.Close();
  }
  x.Close();
  x.Close();
  x.Close();
  x.Close();
}

void WriteShapeTreeStart(Buf& x) {
  x.Open("p:spTree");
  x.Open("p:nvGrpSpPr");
  x.Leaf("p:cNvPr", {{"id", "1"}, {"name", ""}});
  x.Leaf("p:cNvGrpSpPr");
  x.Leaf("p:nvPr");
  x.Close();
  x.Open("p:grpSpPr");
  x.Open("a:xfrm");
  x.Leaf("a:off", {{"x", "0"}, {"y", "0"}});
  x.Leaf("a:ext", {{"cx", "0"}, {"cy", "0"}});
  x.Leaf("a:chOff", {{"x", "0"}, {"y", "0"}});
  x.Leaf("a:chExt", {{"cx", "0"}, {"cy", "0"}});
  x.Close();
  x.Close();
}


// ---- 固定部件 -----------------------------------------------------------

std::string ThemeXml(const Presentation& pres) {
  Buf x;
  x.Decl();
  x.Open("a:theme", {{"xmlns:a", kNsA}, {"name", "DocCore"}});
  x.Open("a:themeElements");
  x.Open("a:clrScheme", {{"name", "DocCore"}});
  x.Open("a:dk1");
  x.Leaf("a:sysClr", {{"val", "windowText"}, {"lastClr", "000000"}});
  x.Close();
  x.Open("a:lt1");
  x.Leaf("a:sysClr", {{"val", "window"}, {"lastClr", "FFFFFF"}});
  x.Close();
  x.Open("a:dk2");
  x.Leaf("a:srgbClr", {{"val", "44546A"}});
  x.Close();
  x.Open("a:lt2");
  x.Leaf("a:srgbClr", {{"val", "E7E6E6"}});
  x.Close();
  const char* accents[6] = {"4472C4", "ED7D31", "A5A5A5", "FFC000", "5B9BD5", "70AD47"};
  for (int i = 0; i < 6; ++i) {
    x.Open("a:accent" + std::to_string(i + 1));
    x.Leaf("a:srgbClr", {{"val", accents[i]}});
    x.Close();
  }
  x.Open("a:hlink");
  x.Leaf("a:srgbClr", {{"val", "0563C1"}});
  x.Close();
  x.Open("a:folHlink");
  x.Leaf("a:srgbClr", {{"val", "954F72"}});
  x.Close();
  x.Close();  // clrScheme

  x.Open("a:fontScheme", {{"name", "DocCore"}});
  x.Open("a:majorFont");
  x.Leaf("a:latin", {{"typeface", pres.default_font}});
  x.Leaf("a:ea", {{"typeface", ""}});
  x.Leaf("a:cs", {{"typeface", ""}});
  x.Close();
  x.Open("a:minorFont");
  x.Leaf("a:latin", {{"typeface", pres.default_font}});
  x.Leaf("a:ea", {{"typeface", ""}});
  x.Leaf("a:cs", {{"typeface", ""}});
  x.Close();
  x.Close();  // fontScheme

  // CT_FillStyleList / CT_LineStyleList / CT_EffectStyleList / CT_BackgroundFillStyleList
  // 各要求 3 个条目，少一个 PowerPoint 就会报修复。
  x.Open("a:fmtScheme", {{"name", "DocCore"}});
  x.Open("a:fillStyleLst");
  for (int i = 0; i < 3; ++i) {
    x.Open("a:solidFill");
    x.Leaf("a:schemeClr", {{"val", "phClr"}});
    x.Close();
  }
  x.Close();
  x.Open("a:lnStyleLst");
  for (int i = 0; i < 3; ++i) {
    x.Open("a:ln", {{"w", std::to_string(6350 + i * 3175)}, {"cap", "flat"}, {"cmpd", "sng"},
                    {"algn", "ctr"}});
    x.Open("a:solidFill");
    x.Leaf("a:schemeClr", {{"val", "phClr"}});
    x.Close();
    x.Leaf("a:prstDash", {{"val", "solid"}});
    x.Close();
  }
  x.Close();
  x.Open("a:effectStyleLst");
  for (int i = 0; i < 3; ++i) {
    x.Open("a:effectStyle");
    x.Leaf("a:effectLst");
    x.Close();
  }
  x.Close();
  x.Open("a:bgFillStyleLst");
  for (int i = 0; i < 3; ++i) {
    x.Open("a:solidFill");
    x.Leaf("a:schemeClr", {{"val", "phClr"}});
    x.Close();
  }
  x.Close();
  x.Close();  // fmtScheme

  x.Close();  // themeElements
  x.Leaf("a:objectDefaults");
  x.Leaf("a:extraClrSchemeLst");
  x.Close();
  return x.str();
}

std::string EmptySpTree() {
  Buf x;
  WriteShapeTreeStart(x);
  x.Close();
  return x.str();
}

std::string SlideMasterXml(const Presentation& pres) {
  Buf x;
  x.Decl();
  x.Open("p:sldMaster", PresentationRoot());
  x.Open("p:cSld");
  x.Open("p:bg");
  x.Open("p:bgPr");
  x.Open("a:solidFill");
  x.Leaf("a:srgbClr", {{"val", Rgb(pres.master_background)}});
  x.Close();
  x.Leaf("a:effectLst");
  x.Close();
  x.Close();
  WriteShapeTreeStart(x);
  x.Close();  // spTree
  x.Close();  // cSld
  x.Leaf("p:clrMap", {{"bg1", "lt1"}, {"tx1", "dk1"}, {"bg2", "lt2"}, {"tx2", "dk2"},
                      {"accent1", "accent1"}, {"accent2", "accent2"}, {"accent3", "accent3"},
                      {"accent4", "accent4"}, {"accent5", "accent5"}, {"accent6", "accent6"},
                      {"hlink", "hlink"}, {"folHlink", "folHlink"}});
  x.Open("p:sldLayoutIdLst");
  x.Leaf("p:sldLayoutId", {{"id", "2147483649"}, {"r:id", "rId1"}});
  x.Close();
  x.Open("p:txStyles");
  x.Open("p:titleStyle");
  x.Open("a:lvl1pPr");
  x.Leaf("a:defRPr", {{"sz", "4000"}, {"b", "1"}});
  x.Close();
  x.Close();
  x.Open("p:bodyStyle");
  x.Open("a:lvl1pPr");
  x.Leaf("a:defRPr", {{"sz", "1800"}});
  x.Close();
  x.Close();
  x.Open("p:otherStyle");
  x.Open("a:lvl1pPr");
  x.Leaf("a:defRPr", {{"sz", "1800"}});
  x.Close();
  x.Close();
  x.Close();  // txStyles
  x.Close();
  return x.str();
}

std::string SlideLayoutXml() {
  Buf x;
  x.Decl();
  x.Open("p:sldLayout", {{"xmlns:a", kNsA}, {"xmlns:r", kNsR}, {"xmlns:p", kNsP},
                         {"type", "obj"}, {"preserve", "1"}});
  x.Open("p:cSld", {{"name", "Title and Content"}});
  WriteShapeTreeStart(x);
  // 标题占位符
  x.Open("p:sp");
  x.Open("p:nvSpPr");
  x.Leaf("p:cNvPr", {{"id", "2"}, {"name", "Title 1"}});
  x.Open("p:cNvSpPr");
  x.Leaf("a:spLocks", {{"noGrp", "1"}});
  x.Close();
  x.Open("p:nvPr");
  x.Leaf("p:ph", {{"type", "title"}});
  x.Close();
  x.Close();
  x.Open("p:spPr");
  x.Open("a:xfrm");
  x.Leaf("a:off", {{"x", "838200"}, {"y", "365125"}});
  x.Leaf("a:ext", {{"cx", "10515600"}, {"cy", "1325563"}});
  x.Close();
  x.Close();
  x.Open("p:txBody");
  x.Leaf("a:bodyPr");
  x.Leaf("a:lstStyle");
  x.Open("a:p");
  x.Leaf("a:endParaRPr", {{"lang", "en-US"}});
  x.Close();
  x.Close();
  x.Close();
  // 正文占位符
  x.Open("p:sp");
  x.Open("p:nvSpPr");
  x.Leaf("p:cNvPr", {{"id", "3"}, {"name", "Content 2"}});
  x.Open("p:cNvSpPr");
  x.Leaf("a:spLocks", {{"noGrp", "1"}});
  x.Close();
  x.Open("p:nvPr");
  x.Leaf("p:ph", {{"type", "body"}, {"idx", "1"}});
  x.Close();
  x.Close();
  x.Open("p:spPr");
  x.Open("a:xfrm");
  x.Leaf("a:off", {{"x", "838200"}, {"y", "1825625"}});
  x.Leaf("a:ext", {{"cx", "10515600"}, {"cy", "4351338"}});
  x.Close();
  x.Close();
  x.Open("p:txBody");
  x.Leaf("a:bodyPr");
  x.Leaf("a:lstStyle");
  x.Open("a:p");
  x.Leaf("a:endParaRPr", {{"lang", "en-US"}});
  x.Close();
  x.Close();
  x.Close();
  x.Close();  // spTree
  x.Close();  // cSld
  x.Open("p:clrMapOvr");
  x.Leaf("a:masterClrMapping");
  x.Close();
  x.Close();
  return x.str();
}

std::string NotesMasterXml(const Presentation& pres) {
  Buf x;
  x.Decl();
  x.Open("p:notesMaster", PresentationRoot());
  x.Open("p:cSld");
  x.Open("p:bg");
  x.Open("p:bgRef", {{"idx", "1001"}});
  x.Leaf("a:schemeClr", {{"val", "bg1"}});
  x.Close();
  x.Close();
  WriteShapeTreeStart(x);
  x.Close();
  x.Close();
  x.Leaf("p:clrMap", {{"bg1", "lt1"}, {"tx1", "dk1"}, {"bg2", "lt2"}, {"tx2", "dk2"},
                      {"accent1", "accent1"}, {"accent2", "accent2"}, {"accent3", "accent3"},
                      {"accent4", "accent4"}, {"accent5", "accent5"}, {"accent6", "accent6"},
                      {"hlink", "hlink"}, {"folHlink", "folHlink"}});
  x.Open("p:notesStyle");
  x.Open("a:lvl1pPr");
  x.Leaf("a:defRPr", {{"sz", "1200"}});
  x.Close();
  x.Close();
  x.Close();
  (void)pres;
  return x.str();
}

std::string PresPropsXml() {
  Buf x;
  x.Decl();
  x.Open("p:presentationPr", PresentationRoot());
  x.Close();
  return x.str();
}

std::string ViewPropsXml() {
  Buf x;
  x.Decl();
  x.Open("p:viewPr", PresentationRoot());
  x.Open("p:normalViewPr");
  x.Open("p:restoredLeft", {{"sz", "15620"}});
  x.Close();
  x.Open("p:restoredTop", {{"sz", "94660"}});
  x.Close();
  x.Close();
  x.Open("p:slideViewPr");
  x.Open("p:cSldViewPr");
  x.Open("p:cViewPr", {{"varScale", "1"}});
  x.Open("p:scale");
  x.Leaf("a:sx", {{"n", "100"}, {"d", "100"}});
  x.Leaf("a:sy", {{"n", "100"}, {"d", "100"}});
  x.Close();
  x.Leaf("p:origin", {{"x", "0"}, {"y", "0"}});
  x.Close();
  x.Close();
  x.Close();
  x.Open("p:notesTextViewPr");
  x.Open("p:cViewPr");
  x.Open("p:scale");
  x.Leaf("a:sx", {{"n", "100"}, {"d", "100"}});
  x.Leaf("a:sy", {{"n", "100"}, {"d", "100"}});
  x.Close();
  x.Leaf("p:origin", {{"x", "0"}, {"y", "0"}});
  x.Close();
  x.Close();
  x.Leaf("p:gridSpacing", {{"cx", "72008"}, {"cy", "72008"}});
  x.Close();
  return x.str();
}

std::string TableStylesXml() {
  Buf x;
  x.Decl();
  x.Open("a:tblStyleLst", {{"xmlns:a", kNsA}, {"def", "{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}"}});
  x.Close();
  return x.str();
}

// ---- 媒体 ---------------------------------------------------------------

std::string MediaExtension(const std::string& mime) {
  if (mime == "image/jpeg" || mime == "image/jpg") return "jpeg";
  if (mime == "image/gif") return "gif";
  if (mime == "image/bmp") return "bmp";
  return "png";
}

std::string MediaMimeFor(const std::vector<uint8_t>& bytes, const std::string& fallback) {
  if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) return "image/jpeg";
  if (bytes.size() > 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
  if (bytes.size() > 3 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') return "image/gif";
  return fallback.empty() ? "image/png" : fallback;
}

struct MediaEntry {
  std::string uri;
  std::string mime;
  std::vector<uint8_t> bytes;
};

// ---- 幻灯片 -------------------------------------------------------------

std::string SlideXml(const Presentation& pres, const Slide& slide,
                     const std::vector<std::string>& picture_rids) {
  Buf x;
  x.Decl();
  x.Open("p:sld", PresentationRoot());
  x.Open("p:cSld");
  if (slide.background() >= 0) {
    x.Open("p:bg");
    x.Open("p:bgPr");
    x.Open("a:solidFill");
    x.Leaf("a:srgbClr", {{"val", Rgb(slide.background())}});
    x.Close();
    x.Leaf("a:effectLst");
    x.Close();
    x.Close();
  }
  WriteShapeTreeStart(x);
  // 形状 id 要稳定：动画的 p:spTgt/@spid 指着它。显式设过 id 的先用，剩下的
  // 从 2 开始补空位（1 留给 spTree 自己）。
  std::set<int> used_ids;
  for (const Shape& shape : slide.shapes()) {
    if (shape.id > 0) used_ids.insert(shape.id);
  }
  int next_id = 2;
  auto alloc_id = [&](const Shape& shape) {
    if (shape.id > 0) return shape.id;
    while (used_ids.count(next_id) != 0) ++next_id;
    used_ids.insert(next_id);
    return next_id++;
  };
  size_t picture_index = 0;
  for (const Shape& shape : slide.shapes()) {
    const int id = alloc_id(shape);
    switch (shape.kind) {
      case ShapeKind::Text:
        WriteTextShape(x, pres, shape, id);
        break;
      case ShapeKind::Picture: {
        const std::string rid =
            picture_index < picture_rids.size() ? picture_rids[picture_index] : std::string();
        ++picture_index;
        if (!rid.empty()) WritePictureShape(x, shape, id, rid);
        break;
      }
      case ShapeKind::Table:
        WriteTableShape(x, pres, shape, id);
        break;
    }
  }
  x.Close();  // spTree
  x.Close();  // cSld
  x.Open("p:clrMapOvr");
  x.Leaf("a:masterClrMapping");
  x.Close();
  // CT_Slide 的顺序：cSld -> clrMapOvr -> transition -> timing -> extLst。
  // 顺序写错 PowerPoint 会直接报"内容有问题"。
  x.RawMarkup(ooxml::TransitionXml(slide));
  x.RawMarkup(ooxml::TimingXml(slide));
  x.Close();
  return x.str();
}

std::string NotesSlideXml(const Presentation& pres, const Slide& slide) {
  Buf x;
  x.Decl();
  x.Open("p:notes", PresentationRoot());
  x.Open("p:cSld");
  WriteShapeTreeStart(x);
  // 备注正文占位符
  Shape shape;
  shape.placeholder = "body";
  shape.geom = ShapeGeometry{685800, 4400550, 5486400, 3600450};
  TextBody body;
  Paragraph paragraph;
  Run run;
  run.text = slide.notes();
  run.format.size_pt = 12.0;
  paragraph.runs.push_back(std::move(run));
  body.paragraphs.push_back(std::move(paragraph));
  x.Open("p:sp");
  x.Open("p:nvSpPr");
  x.Leaf("p:cNvPr", {{"id", "2"}, {"name", "Notes Placeholder"}});
  x.Leaf("p:cNvSpPr");
  x.Open("p:nvPr");
  x.Leaf("p:ph", {{"type", "body"}, {"idx", "1"}});
  x.Close();
  x.Close();
  x.Open("p:spPr");
  x.Open("a:xfrm");
  x.Leaf("a:off", {{"x", Emu(shape.geom.x)}, {"y", Emu(shape.geom.y)}});
  x.Leaf("a:ext", {{"cx", Emu(shape.geom.cx)}, {"cy", Emu(shape.geom.cy)}});
  x.Close();
  x.Close();
  WriteTextBody(x, pres, body, shape);
  x.Close();
  x.Close();  // spTree
  x.Close();  // cSld
  x.Open("p:clrMapOvr");
  x.Leaf("a:masterClrMapping");
  x.Close();
  x.Close();
  return x.str();
}

std::string PresentationXml(const Presentation& pres, bool has_notes_master) {
  Buf x;
  x.Decl();
  x.Open("p:presentation", PresentationRoot());
  x.Open("p:sldMasterIdLst");
  x.Leaf("p:sldMasterId", {{"id", "2147483648"}, {"r:id", "rId1"}});
  x.Close();
  if (has_notes_master) {
    x.Open("p:notesMasterIdLst");
    x.Leaf("p:notesMasterId", {{"r:id", "rIdNotesMaster"}});
    x.Close();
  }
  x.Open("p:sldIdLst");
  for (int i = 0; i < pres.slide_count(); ++i) {
    x.Leaf("p:sldId", {{"id", std::to_string(256 + i)}, {"r:id", "rId" + std::to_string(i + 2)}});
  }
  x.Close();
  x.Leaf("p:sldSz", {{"cx", Emu(pres.slide_width)}, {"cy", Emu(pres.slide_height)}});
  x.Leaf("p:notesSz", {{"cx", "6858000"}, {"cy", "9144000"}});
  x.Close();
  return x.str();
}

std::string PresentationRelsXml(const Presentation& pres, bool has_notes_master) {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  x.Leaf("Relationship", {{"Id", "rId1"},
                          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster"},
                          {"Target", "slideMasters/slideMaster1.xml"}});
  for (int i = 0; i < pres.slide_count(); ++i) {
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(i + 2)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"},
            {"Target", "slides/slide" + std::to_string(i + 1) + ".xml"}});
  }
  int next = pres.slide_count() + 2;
  if (has_notes_master) {
    x.Leaf("Relationship",
           {{"Id", "rIdNotesMaster"},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster"},
            {"Target", "notesMasters/notesMaster1.xml"}});
  }
  x.Leaf("Relationship",
         {{"Id", "rId" + std::to_string(next++)},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/presProps"},
          {"Target", "presProps.xml"}});
  x.Leaf("Relationship",
         {{"Id", "rId" + std::to_string(next++)},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/viewProps"},
          {"Target", "viewProps.xml"}});
  x.Leaf("Relationship",
         {{"Id", "rId" + std::to_string(next++)},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/tableStyles"},
          {"Target", "tableStyles.xml"}});
  x.Leaf("Relationship",
         {{"Id", "rId" + std::to_string(next++)},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme"},
          {"Target", "theme/theme1.xml"}});
  x.Close();
  return x.str();
}

std::string SlideRelsXml(const std::vector<std::string>& picture_targets,
                         const std::string& notes_target) {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  x.Leaf("Relationship",
         {{"Id", "rId1"},
          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout"},
          {"Target", "../slideLayouts/slideLayout1.xml"}});
  int next = 2;
  for (const std::string& target : picture_targets) {
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(next++)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"},
            {"Target", target}});
  }
  if (!notes_target.empty()) {
    // 备注页关系由类型隐式关联，幻灯片 XML 不需要 r:id 引用它。
    x.Leaf("Relationship",
           {{"Id", "rId" + std::to_string(next++)},
            {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide"},
            {"Target", notes_target}});
  }
  x.Close();
  return x.str();
}

std::string RootRelsXml() {
  Buf x;
  x.Decl();
  x.Open("Relationships", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/relationships"}});
  x.Leaf("Relationship", {{"Id", "rId1"},
                          {"Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"},
                          {"Target", "ppt/presentation.xml"}});
  x.Close();
  return x.str();
}

struct PartOut {
  std::string uri;
  std::string mime;
  std::string data;
};

std::string ExtensionOf(const std::string& uri) {
  const size_t dot = uri.rfind('.');
  if (dot == std::string::npos) return std::string();
  std::string ext = uri.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

std::string ContentTypesXml(const std::vector<PartOut>& parts,
                            const std::vector<MediaEntry>& media,
                            const std::vector<UnknownPart>& unknown) {
  Buf x;
  x.Decl();
  x.Open("Types", {{"xmlns", "http://schemas.openxmlformats.org/package/2006/content-types"}});
  x.Leaf("Default", {{"Extension", "rels"},
                     {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
  x.Leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
  std::map<std::string, std::string> image_defaults;
  for (const MediaEntry& entry : media) {
    image_defaults[MediaExtension(entry.mime)] = entry.mime;
  }
  // 原样保留的部件（缩略图、docProps 等）也要有默认类型，否则包不合法。
  for (const UnknownPart& part : unknown) {
    if (part.mime.empty() || part.mime == "application/xml") continue;
    const std::string ext = ExtensionOf(part.uri);
    if (ext.empty() || ext == "xml" || ext == "rels") continue;
    image_defaults.emplace(ext, part.mime);
  }
  for (const auto& kv : image_defaults) {
    x.Leaf("Default", {{"Extension", kv.first}, {"ContentType", kv.second}});
  }
  for (const PartOut& part : parts) {
    if (part.mime.empty() || part.mime == "application/xml") continue;
    x.Leaf("Override", {{"PartName", part.uri}, {"ContentType", part.mime}});
  }
  x.Close();
  return x.str();
}

std::string PartNameFor(const std::string& uri) {
  return uri.empty() || uri[0] != '/' ? uri : uri.substr(1);
}

const char* kMimePresentation =
    "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
const char* kMimeSlide =
    "application/vnd.openxmlformats-officedocument.presentationml.slide+xml";
const char* kMimeSlideMaster =
    "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml";
const char* kMimeSlideLayout =
    "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml";
const char* kMimeNotesMaster =
    "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml";
const char* kMimeNotesSlide =
    "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml";
const char* kMimeTheme = "application/vnd.openxmlformats-officedocument.theme+xml";
const char* kMimePresProps =
    "application/vnd.openxmlformats-officedocument.presentationml.presProps+xml";
const char* kMimeViewProps =
    "application/vnd.openxmlformats-officedocument.presentationml.viewProps+xml";
const char* kMimeTableStyles =
    "application/vnd.openxmlformats-officedocument.presentationml.tableStyles+xml";

}  // namespace

void Save(const Presentation& pres, Stream& out, const SaveOptions& opts) {
  if (opts.cancel != nullptr) opts.cancel->ThrowIfCancelled();

  // 1. 媒体去重（同一份图片只写一次）。
  std::vector<MediaEntry> media;
  std::map<std::string, size_t> media_index;
  std::vector<std::vector<std::string>> slide_picture_rids(static_cast<size_t>(pres.slide_count()));
  std::vector<std::vector<std::string>> slide_picture_targets(
      static_cast<size_t>(pres.slide_count()));

  for (int i = 0; i < pres.slide_count(); ++i) {
    const Slide* slide = pres.SlideAt(i);
    if (slide == nullptr) continue;
    int next_rid = 2;
    for (const Shape& shape : slide->shapes()) {
      if (shape.kind != ShapeKind::Picture) continue;
      if (shape.picture.bytes.empty()) {
        slide_picture_rids[static_cast<size_t>(i)].emplace_back();
        continue;
      }
      std::string key = std::to_string(shape.picture.bytes.size()) + ":";
      // 用前 64 字节 + 长度做键，足够区分真实素材。
      key.append(reinterpret_cast<const char*>(shape.picture.bytes.data()),
                 std::min<size_t>(64, shape.picture.bytes.size()));
      auto it = media_index.find(key);
      size_t index = 0;
      if (it == media_index.end()) {
        MediaEntry entry;
        entry.mime = MediaMimeFor(shape.picture.bytes, shape.picture.mime);
        entry.uri = "/ppt/media/image" + std::to_string(media.size() + 1) + "." +
                    MediaExtension(entry.mime);
        entry.bytes = shape.picture.bytes;
        index = media.size();
        media.push_back(std::move(entry));
        media_index.emplace(key, index);
      } else {
        index = it->second;
      }
      slide_picture_rids[static_cast<size_t>(i)].push_back("rId" + std::to_string(next_rid++));
      // 目标按文件名算：/ppt/media/image1.png -> ../media/image1.png
      const std::string& media_uri = media[index].uri;
      slide_picture_targets[static_cast<size_t>(i)].push_back(
          "../media/" + media_uri.substr(media_uri.rfind('/') + 1));
    }
  }

  const bool has_notes = [&pres] {
    for (int i = 0; i < pres.slide_count(); ++i) {
      const Slide* slide = pres.SlideAt(i);
      if (slide != nullptr && !slide->notes().empty()) return true;
    }
    return false;
  }();

  // 2. 部件
  std::vector<PartOut> parts;
  parts.push_back({"/ppt/presentation.xml", kMimePresentation,
                   PresentationXml(pres, has_notes)});
  parts.push_back({"/ppt/slideMasters/slideMaster1.xml", kMimeSlideMaster,
                   SlideMasterXml(pres)});
  parts.push_back({"/ppt/slideLayouts/slideLayout1.xml", kMimeSlideLayout, SlideLayoutXml()});
  parts.push_back({"/ppt/theme/theme1.xml", kMimeTheme, ThemeXml(pres)});
  parts.push_back({"/ppt/presProps.xml", kMimePresProps, PresPropsXml()});
  parts.push_back({"/ppt/viewProps.xml", kMimeViewProps, ViewPropsXml()});
  parts.push_back({"/ppt/tableStyles.xml", kMimeTableStyles, TableStylesXml()});
  if (has_notes) {
    parts.push_back({"/ppt/notesMasters/notesMaster1.xml", kMimeNotesMaster,
                     NotesMasterXml(pres)});
  }
  for (int i = 0; i < pres.slide_count(); ++i) {
    const Slide* slide = pres.SlideAt(i);
    if (slide == nullptr) continue;
    parts.push_back({"/ppt/slides/slide" + std::to_string(i + 1) + ".xml", kMimeSlide,
                     SlideXml(pres, *slide, slide_picture_rids[static_cast<size_t>(i)])});
    if (!slide->notes().empty()) {
      parts.push_back({"/ppt/notesSlides/notesSlide" + std::to_string(i + 1) + ".xml",
                       kMimeNotesSlide, NotesSlideXml(pres, *slide)});
    }
  }

  // 3. zip 条目
  std::vector<ZipWriteEntry> entries;
  entries.push_back({"[Content_Types].xml", StrBytes(ContentTypesXml(parts, media, pres.unknown_parts))});
  entries.push_back({"_rels/.rels", StrBytes(RootRelsXml())});
  entries.push_back({"ppt/_rels/presentation.xml.rels",
                     StrBytes(PresentationRelsXml(pres, has_notes))});
  entries.push_back({"ppt/slideMasters/_rels/slideMaster1.xml.rels",
                     StrBytes(std::string(
                         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
                         "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/>"
                         "</Relationships>"))});
  entries.push_back({"ppt/slideLayouts/_rels/slideLayout1.xml.rels",
                     StrBytes(std::string(
                         "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                         "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
                         "</Relationships>"))});
  if (has_notes) {
    entries.push_back({"ppt/notesMasters/_rels/notesMaster1.xml.rels",
                       StrBytes(std::string(
                           "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                           "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/>"
                           "</Relationships>"))});
  }
  for (int i = 0; i < pres.slide_count(); ++i) {
    const Slide* slide = pres.SlideAt(i);
    if (slide == nullptr) continue;
    entries.push_back({"ppt/slides/_rels/slide" + std::to_string(i + 1) + ".xml.rels",
                       StrBytes(SlideRelsXml(
                           slide_picture_targets[static_cast<size_t>(i)],
                           slide->notes().empty()
                               ? std::string()
                               : "../notesSlides/notesSlide" + std::to_string(i + 1) + ".xml"))});
    if (!slide->notes().empty()) {
      entries.push_back({"ppt/notesSlides/_rels/notesSlide" + std::to_string(i + 1) + ".xml.rels",
                         StrBytes(std::string(
                                      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"../slides/slide") +
                                  std::to_string(i + 1) +
                                  ".xml\"/>"
                                  "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster\" Target=\"../notesMasters/notesMaster1.xml\"/>"
                                  "</Relationships>")});
    }
  }
  std::set<std::string> written;
  for (const PartOut& part : parts) {
    written.insert(PartNameFor(part.uri));
    entries.push_back({PartNameFor(part.uri), StrBytes(part.data)});
  }
  for (const MediaEntry& entry : media) {
    written.insert(PartNameFor(entry.uri));
    entries.push_back({PartNameFor(entry.uri), entry.bytes});
  }
  // 原样保留的部件不能覆盖我们自己生成的（否则母版/版式会被旧文件顶掉，
  // 旧部件引用的 rId 在我们的 .rels 里不存在，包就坏了）。
  for (const UnknownPart& unknown : pres.unknown_parts) {
    const std::string name = PartNameFor(unknown.uri);
    if (written.count(name) != 0) continue;
    written.insert(name);
    entries.push_back({name, unknown.bytes});
  }

  ZipWriteToStream(out, entries);
}

void SaveToFile(const Presentation& pres, const std::string& path, const SaveOptions& opts) {
  MemoryStream mem;
  Save(pres, mem, opts);
  WriteFileAtomically(path, mem.data());
}

}  // namespace deck
}  // namespace doccore
