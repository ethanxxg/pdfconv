// DocCore - DOCX 序列化（DOM → OOXML 包）。Design refs: docs/02 §3.3.3
//
// The writer is the mirror of docx_read.cpp: every part the reader understands
// gets a serialized form here, and doc.unknown_parts are copied back verbatim
// so a read-modify-write round trip never loses data (docs/02 §3.1.3 item 6).
#include "doccore/docx.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doccore/core.h"
#include "doccore/stream.h"
#include "doccore/zip.h"
#include "doccore/zip_write.h"

namespace doccore {
namespace text {
namespace {

const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kCtNs = "http://schemas.openxmlformats.org/package/2006/content-types";
const char* kRelNs = "http://schemas.openxmlformats.org/package/2006/relationships";

const char* kCtRels = "application/vnd.openxmlformats-package.relationships+xml";
const char* kCtXml = "application/xml";
const char* kCtMain =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
const char* kCtStyles = "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml";
const char* kCtNumbering =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml";
const char* kCtFootnotes =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml";
const char* kCtEndnotes =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml";
const char* kCtComments =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml";
const char* kCtHeader = "application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml";
const char* kCtFooter = "application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml";
const char* kCtSettings =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml";

const char* kRelOfficeDocument =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
const char* kRelStyles =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";
const char* kRelNumbering =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering";
const char* kRelFootnotes =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes";
const char* kRelEndnotes =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/endnotes";
const char* kRelComments =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments";
const char* kRelHeader = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/header";
const char* kRelFooter = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer";
const char* kRelImage = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
const char* kRelSettings =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings";
const char* kRelHyperlink =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";

// ---- escaping -------------------------------------------------------------

void EscText(std::string& out, const std::string& v) {
  for (char c : v) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(c);
    }
  }
}

void EscAttr(std::string& out, const std::string& v) {
  for (char c : v) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      case '\r': out += "&#13;"; break;
      case '\t': out += "&#9;"; break;
      default: out.push_back(c);
    }
  }
}

// XML forbids most C0 controls outright; a text run carrying one would make the
// whole part unreadable, so they are dropped instead of emitted.
bool IsLegalXmlChar(unsigned char c) {
  if (c < 0x20) return c == '\t' || c == '\n' || c == '\r';
  return true;
}

std::string Sanitize(const std::string& v) {
  bool clean = true;
  for (unsigned char c : v) {
    if (!IsLegalXmlChar(c)) {
      clean = false;
      break;
    }
  }
  if (clean) return v;
  std::string out;
  out.reserve(v.size());
  for (unsigned char c : v) {
    if (IsLegalXmlChar(c)) out.push_back(static_cast<char>(c));
  }
  return out;
}

// ---- XML builder ----------------------------------------------------------
//
// Prefix-aware: OPC parts ([Content_Types].xml, *.rels) have no prefix, the
// Word parts use "w", and a handful of attributes need their own prefix
// ("r:id" on hyperlinks, "xml:space" on text runs), hence AttrQ().
class Xml {
 public:
  void Prefix(std::string p) { prefix_ = std::move(p); }

  void Decl() {
    out_ += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
  }

  void Start(const std::string& name) {
    FlushTag();
    out_ += '<';
    Name(out_, name);
    stack_.push_back(name);
    tag_open_ = true;
  }

  void End() {
    if (stack_.empty()) throw Error(ErrorCode::Unknown, "docx write: unbalanced XML");
    if (tag_open_) {
      // Nothing was written inside: collapse to a self-closing element.
      out_ += "/>";
      tag_open_ = false;
      stack_.pop_back();
      return;
    }
    out_ += "</";
    Name(out_, stack_.back());
    out_ += '>';
    stack_.pop_back();
  }

  // Attribute on the element whose start tag is still open.
  void Attr(const std::string& name, const std::string& value) {
    if (!tag_open_) throw Error(ErrorCode::Unknown, "docx write: attribute after start tag");
    out_ += ' ';
    Name(out_, name);
    out_ += "=\"";
    EscAttr(out_, value);
    out_ += '"';
  }

  // Attribute whose qualified name the caller supplies ("r:id", "xml:space").
  void AttrQ(const std::string& qname, const std::string& value) {
    if (!tag_open_) throw Error(ErrorCode::Unknown, "docx write: attribute after start tag");
    out_ += ' ';
    out_ += qname;
    out_ += "=\"";
    EscAttr(out_, value);
    out_ += '"';
  }

  // Child element <w:name w:a="v" .../>.
  void Vals(const std::string& name,
            std::initializer_list<std::pair<std::string, std::string>> attrs) {
    FlushTag();
    out_ += '<';
    Name(out_, name);
    for (const auto& kv : attrs) {
      out_ += ' ';
      Name(out_, kv.first);
      out_ += "=\"";
      EscAttr(out_, kv.second);
      out_ += '"';
    }
    out_ += "/>";
  }

  void Empty(const std::string& name) {
    FlushTag();
    out_ += '<';
    Name(out_, name);
    out_ += "/>";
  }

  // 原样追加一段标记。VML 的 shapetype 样板是一整块固定文本（formulas 里
  // 十几个 v:f），逐元素生成只会让代码更难核对，所以按原文拼进去。
  void RawMarkup(const std::string& markup) {
    FlushTag();
    out_ += markup;
  }

  // Character data of the currently open element.
  void Chars(const std::string& value) {
    FlushTag();
    EscText(out_, Sanitize(value));
  }

  // Unescaped XML fragment (drawings need prefixed elements from four other
  // namespaces, which this builder does not model).
  void Raw(const std::string& xml) {
    FlushTag();
    out_ += xml;
  }

  const std::string& str() const { return out_; }

  std::string Take() {
    FlushTag();
    if (!stack_.empty()) throw Error(ErrorCode::Unknown, "docx write: unclosed XML");
    return std::move(out_);
  }

 private:
  void FlushTag() {
    if (tag_open_) {
      out_ += '>';
      tag_open_ = false;
    }
  }

  void Name(std::string& out, const std::string& name) const {
    if (prefix_.empty()) {
      out += name;
      return;
    }
    out += prefix_;
    out += ':';
    out += name;
  }

  std::string out_;
  std::string prefix_ = "w";
  std::vector<std::string> stack_;
  bool tag_open_ = false;
};

std::string Hex6(int v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%06X", static_cast<unsigned>(v) & 0xFFFFFFu);
  return buf;
}

// w:jc. The DOM keeps the raw index the reader assigned.
const char* JcName(int alignment) {
  switch (alignment) {
    case 0: return "left";
    case 1: return "center";
    case 2: return "right";
    case 3: return "both";
    case 4: return "distribute";
    default: return "left";
  }
}

const char* VertAlignName(int v) {
  switch (v) {
    case 1: return "superscript";
    case 2: return "subscript";
    default: return "baseline";
  }
}

bool NeedsSpacePreserve(const std::string& s) {
  if (s.empty()) return false;
  if (s.front() == ' ' || s.back() == ' ') return true;
  return s.find("  ") != std::string::npos || s.find('\t') != std::string::npos;
}

// ---- run properties -------------------------------------------------------

bool HasRunProps(const RunFormat& f) {
  return f.bold != Tri::Unset || f.italic != Tri::Unset || f.strike != Tri::Unset ||
         f.double_strike != Tri::Unset || f.all_caps != Tri::Unset ||
         f.small_caps != Tri::Unset || f.emboss != Tri::Unset || f.imprint != Tri::Unset ||
         f.hidden != Tri::Unset || f.no_proof != Tri::Unset || f.size_half >= 0 ||
         f.color >= 0 || f.shading >= 0 || f.vert_align >= 0 || !f.underline.empty() ||
         !f.font_ascii.empty() || !f.font_hansi.empty() || !f.font_eastasia.empty() ||
         !f.font_cs.empty() || !f.character_style.empty() || !f.lang.empty() ||
         f.spacing_twips != 0 || f.kerning_half != 0 || f.position_half != 0 ||
         f.outline_level >= 0;
}

// A toggle element: <w:b/> when on, <w:b w:val="0"/> when explicitly off.
// The reader turns both back into Tri::On / Tri::Off (BoolAttr).
void TriFlag(Xml& x, const char* name, Tri t) {
  if (t == Tri::Unset) return;
  if (t == Tri::On) {
    x.Empty(name);
  } else {
    x.Vals(name, {{"val", "0"}});
  }
}

void WriteRunPropsBody(Xml& x, const RunFormat& f) {
  if (!f.character_style.empty()) x.Vals("rStyle", {{"val", f.character_style}});
  if (!f.font_ascii.empty() || !f.font_hansi.empty() || !f.font_eastasia.empty() ||
      !f.font_cs.empty()) {
    x.Start("rFonts");
    if (!f.font_ascii.empty()) x.Attr("ascii", f.font_ascii);
    if (!f.font_hansi.empty()) x.Attr("hAnsi", f.font_hansi);
    if (!f.font_eastasia.empty()) x.Attr("eastAsia", f.font_eastasia);
    if (!f.font_cs.empty()) x.Attr("cs", f.font_cs);
    x.End();
  }
  // CT_RPr child order matters to Word: toggles first, then the measured
  // properties (see ECMA-376 §17.3.2).
  TriFlag(x, "b", f.bold);
  TriFlag(x, "i", f.italic);
  TriFlag(x, "caps", f.all_caps);
  TriFlag(x, "smallCaps", f.small_caps);
  TriFlag(x, "strike", f.strike);
  TriFlag(x, "dstrike", f.double_strike);
  TriFlag(x, "emboss", f.emboss);
  TriFlag(x, "imprint", f.imprint);
  TriFlag(x, "noProof", f.no_proof);
  TriFlag(x, "vanish", f.hidden);
  if (f.color >= 0) x.Vals("color", {{"val", Hex6(f.color)}});
  if (f.spacing_twips != 0) x.Vals("spacing", {{"val", std::to_string(f.spacing_twips)}});
  if (f.kerning_half != 0) x.Vals("kern", {{"val", std::to_string(f.kerning_half)}});
  if (f.position_half != 0) x.Vals("position", {{"val", std::to_string(f.position_half)}});
  if (f.size_half >= 0) x.Vals("sz", {{"val", std::to_string(f.size_half)}});
  if (!f.underline.empty()) x.Vals("u", {{"val", f.underline}});
  if (f.shading >= 0) {
    x.Vals("shd", {{"val", "clear"}, {"color", "auto"}, {"fill", Hex6(f.shading)}});
  }
  if (f.vert_align >= 0) x.Vals("vertAlign", {{"val", VertAlignName(f.vert_align)}});
  if (!f.lang.empty()) x.Vals("lang", {{"val", f.lang}});
  // outline_level is not a CT_RPr child; the reader tolerates it on input, but
  // writing it would produce a part Word rejects, so it is not emitted.
}

void WriteRunProps(Xml& x, const RunFormat& f) {
  if (!HasRunProps(f)) return;
  x.Start("rPr");
  WriteRunPropsBody(x, f);
  x.End();
}

// ---- paragraph properties -------------------------------------------------

bool HasParagraphProps(const ParagraphFormat& f) {
  return !f.style.empty() || f.alignment >= 0 || f.line_rule >= 0 || f.line_twips >= 0 ||
         f.before_twips >= 0 || f.after_twips >= 0 || f.before_autospacing >= 0 ||
         f.after_autospacing >= 0 || f.context_before >= 0 || f.ind_left >= 0 ||
         f.ind_right >= 0 || f.ind_first_line >= 0 || f.ind_first_line_chars >= 0 ||
         f.keep_next != Tri::Unset || f.keep_lines != Tri::Unset ||
         f.widow_control != Tri::Unset || f.suppress_line_numbers != Tri::Unset ||
         f.page_break_before != Tri::Unset || f.outline_level >= 0 || !f.num_id.empty() ||
         f.shading >= 0 || f.border_top >= 0 || f.border_bottom >= 0 || f.border_left >= 0 ||
         f.border_right >= 0 || f.rtl || f.snap_to_grid >= 0 || f.mirror_indents >= 0;
}

void WriteParagraphPropsBody(Xml& x, const ParagraphFormat& f) {
  // CT_PPrBase child order (ECMA-376 §17.3.1.26): pStyle, keep*, numPr,
  // suppressLineNumbers, pBdr, shd, bidi, snapToGrid, spacing, ind,
  // contextualSpacing, mirrorIndents, jc, outlineLvl.
  if (!f.style.empty()) x.Vals("pStyle", {{"val", f.style}});
  TriFlag(x, "keepNext", f.keep_next);
  TriFlag(x, "keepLines", f.keep_lines);
  TriFlag(x, "pageBreakBefore", f.page_break_before);
  TriFlag(x, "widowControl", f.widow_control);
  if (!f.num_id.empty()) {
    x.Start("numPr");
    if (f.num_level >= 0) x.Vals("ilvl", {{"val", std::to_string(f.num_level)}});
    x.Vals("numId", {{"val", f.num_id}});
    x.End();
  }
  TriFlag(x, "suppressLineNumbers", f.suppress_line_numbers);
  if (f.border_top >= 0 || f.border_bottom >= 0 || f.border_left >= 0 || f.border_right >= 0) {
    x.Start("pBdr");
    auto edge = [&](const char* name, int color) {
      if (color < 0) return;
      x.Vals(name, {{"val", "single"}, {"sz", "8"}, {"space", "0"}, {"color", Hex6(color)}});
    };
    edge("top", f.border_top);
    edge("left", f.border_left);
    edge("bottom", f.border_bottom);
    edge("right", f.border_right);
    x.End();
  }
  if (f.shading >= 0) {
    x.Vals("shd", {{"val", "clear"}, {"color", "auto"}, {"fill", Hex6(f.shading)}});
  }
  if (f.rtl) x.Empty("bidi");
  if (f.snap_to_grid >= 0) x.Vals("snapToGrid", {{"val", f.snap_to_grid ? "1" : "0"}});
  if (f.before_twips >= 0 || f.after_twips >= 0 || f.line_twips >= 0 ||
      f.before_autospacing >= 0 || f.after_autospacing >= 0) {
    x.Start("spacing");
    if (f.before_twips >= 0) x.Attr("before", std::to_string(f.before_twips));
    if (f.before_autospacing >= 0) {
      x.Attr("beforeAutospacing", f.before_autospacing ? "1" : "0");
    }
    if (f.after_twips >= 0) x.Attr("after", std::to_string(f.after_twips));
    if (f.after_autospacing >= 0) x.Attr("afterAutospacing", f.after_autospacing ? "1" : "0");
    if (f.line_twips >= 0) x.Attr("line", std::to_string(f.line_twips));
    if (f.line_rule == 0) {
      x.Attr("lineRule", "auto");
    } else if (f.line_rule == 1) {
      x.Attr("lineRule", "atLeast");
    } else if (f.line_rule == 2) {
      x.Attr("lineRule", "exact");
    }
    x.End();
  }
  if (f.ind_left >= 0 || f.ind_right >= 0 || f.ind_first_line >= 0 ||
      f.ind_first_line_chars >= 0) {
    x.Start("ind");
    if (f.ind_left >= 0) x.Attr("left", std::to_string(f.ind_left));
    if (f.ind_right >= 0) x.Attr("right", std::to_string(f.ind_right));
    if (f.ind_first_line > 0) {
      x.Attr("firstLine", std::to_string(f.ind_first_line));
    } else if (f.ind_first_line < 0) {
      x.Attr("hanging", std::to_string(-f.ind_first_line));
    }
    if (f.ind_first_line_chars >= 0) {
      x.Attr("firstLineChars", std::to_string(f.ind_first_line_chars));
    }
    x.End();
  }
  if (f.context_before >= 0) x.Vals("contextualSpacing", {{"val", f.context_before ? "1" : "0"}});
  if (f.mirror_indents >= 0) x.Vals("mirrorIndents", {{"val", f.mirror_indents ? "1" : "0"}});
  if (f.alignment >= 0) x.Vals("jc", {{"val", JcName(f.alignment)}});
  if (f.outline_level >= 0) x.Vals("outlineLvl", {{"val", std::to_string(f.outline_level)}});
}

// ---- parts and relationships ----------------------------------------------

struct PartOut {
  std::string name;           // zip entry name, e.g. "word/header1.xml"
  std::vector<uint8_t> data;
  std::string content_type;   // empty = covered by a Default in [Content_Types].xml
};

struct RelEntry {
  std::string id;
  std::string type;
  std::string target;
  bool external = false;
};

std::vector<uint8_t> ToBytes(std::string s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// PNG IHDR -> pixel size. The DOM has no drawing-extent model, so an inline
// picture is sized from its own header instead of being dropped.
bool PngPixelSize(const std::vector<uint8_t>& b, int* px, int* py) {
  static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (b.size() < 24) return false;
  if (std::memcmp(b.data(), kSig, 8) != 0) return false;
  if (std::memcmp(b.data() + 12, "IHDR", 4) != 0) return false;
  auto be32 = [&](size_t off) {
    return (static_cast<uint32_t>(b[off]) << 24) | (static_cast<uint32_t>(b[off + 1]) << 16) |
           (static_cast<uint32_t>(b[off + 2]) << 8) | static_cast<uint32_t>(b[off + 3]);
  };
  const uint32_t w = be32(16);
  const uint32_t h = be32(20);
  if (w == 0 || h == 0 || w > 100000 || h > 100000) return false;
  *px = static_cast<int>(w);
  *py = static_cast<int>(h);
  return true;
}

bool IsExternalTarget(const std::string& t) {
  return t.find("://") != std::string::npos || t.compare(0, 7, "mailto:") == 0;
}

// Part-URI -> target relative to /word/, the base of word/document.xml.
std::string RelativeFromWord(const std::string& uri) {
  if (uri.size() > 6 && uri.compare(0, 6, "/word/") == 0) return uri.substr(6);
  if (!uri.empty() && uri[0] == '/') return "../" + uri.substr(1);
  return uri;
}

std::string BaseName(const std::string& uri) {
  const size_t slash = uri.rfind('/');
  return slash == std::string::npos ? uri : uri.substr(slash + 1);
}

// ---- writer ---------------------------------------------------------------

class DocxWriter {
 public:
  explicit DocxWriter(const Document& doc) : doc_(doc) {}

  std::vector<uint8_t> Build(WarningSink* warnings);

 private:
  std::string AddRel(const char* type, const std::string& target, bool external);
  std::string RelsXml(const std::vector<RelEntry>& rels) const;

  void WriteSectPrInto(Xml& x, const Section& s);
  void WriteParagraphProps(Xml& x, const ParagraphFormat& f, const Section* closing);
  void WriteLeaf(Xml& x, const InlineItem& it);
  void WriteInlineItem(Xml& x, const InlineItem& it);
  void WriteHyperlink(Xml& x, const std::vector<InlineItem>& runs, size_t begin, size_t end,
                      const std::string& target, const std::string& anchor);
  void WriteParagraph(Xml& x, const Paragraph& p, const Section* closing);
  void WriteTable(Xml& x, const Table& t);
  void WriteBlocks(Xml& x, const std::vector<Block>& blocks);
  std::string DrawingXml(const InlineItem& it);

  std::string StylesXml();
  std::string NumberingXml();
  std::string NotesXml(const std::vector<Note>& notes, bool footnotes);
  std::string CommentsXml();
  std::string HeaderFooterXml(const std::vector<Block>& blocks, bool header);
  std::string DocumentXml();
  std::string ContentTypesXml();
  std::string SettingsXml();

  // 水印：页眉里的 VML 形状（Word 就是这么存的）
  void WriteWatermarkParagraph(Xml& x);
  std::string WatermarkShapeStyle(const Watermark& wm, double w_pt, double h_pt) const;
  double WatermarkBoxWidth(const Watermark& wm) const;
  double WatermarkBoxHeight(const Watermark& wm) const;

  const UnknownPart* FindUnknown(const std::string& uri) const;

  const Document& doc_;
  WarningSink* warnings_ = nullptr;
  std::vector<PartOut> parts_;      // fixed parts, in emission order
  std::vector<PartOut> hf_parts_;   // headers and footers
  std::vector<RelEntry> rels_;      // relationships of word/document.xml
  std::map<const HeaderFooter*, std::string> hf_rid_;
  std::map<std::string, std::string> media_rid_;
  std::map<std::string, std::string> link_rid_;
  std::map<std::string, std::string> media_defaults_;  // 扩展名 -> 内容类型
  std::string watermark_media_;  // 图片水印的媒体部件名（"word/media/…"），空 = 无
  std::map<const Section*, std::string> wm_header_rid_;  // 合成的"只含水印"页眉
  std::vector<std::unique_ptr<HeaderFooter>> synthetic_headers_;  // 保持指针稳定
  int next_rel_ = 1;
  int next_doc_pr_ = 1;
  int next_rev_id_ = 1;
};

std::string DocxWriter::AddRel(const char* type, const std::string& target, bool external) {
  RelEntry r;
  r.id = "rId" + std::to_string(next_rel_++);
  r.type = type;
  r.target = target;
  r.external = external;
  rels_.push_back(std::move(r));
  return rels_.back().id;
}

std::string DocxWriter::RelsXml(const std::vector<RelEntry>& rels) const {
  Xml x;
  x.Prefix("");
  x.Decl();
  x.Start("Relationships");
  x.AttrQ("xmlns", kRelNs);
  for (const auto& r : rels) {
    x.Start("Relationship");
    x.Attr("Id", r.id);
    x.Attr("Type", r.type);
    x.Attr("Target", r.target);
    if (r.external) x.Attr("TargetMode", "External");
    x.End();
  }
  x.End();
  return x.Take();
}

const UnknownPart* DocxWriter::FindUnknown(const std::string& uri) const {
  for (const auto& up : doc_.unknown_parts) {
    if (up.uri == uri) return &up;
  }
  return nullptr;
}

// ---- inline content -------------------------------------------------------

const char* HfKindName(HeaderFooter::Kind kind) {
  switch (kind) {
    case HeaderFooter::Kind::First: return "first";
    case HeaderFooter::Kind::Even: return "even";
    default: return "default";
  }
}

std::string DocxWriter::DrawingXml(const InlineItem& it) {
  std::string rid;
  const std::string& target = it.drawing_target;
  if (!target.empty()) {
    auto found = media_rid_.find(target);
    if (found != media_rid_.end()) {
      rid = found->second;
    } else if (FindUnknown(target) != nullptr) {
      rid = AddRel(kRelImage, RelativeFromWord(target), false);
      media_rid_[target] = rid;
    } else if (warnings_ != nullptr) {
      Warning w;
      w.code = static_cast<int>(WarningCode::ImageSkipped);
      w.module = "docx";
      w.message = "docx write: drawing target not in package: " + target;
      warnings_->Add(w);
    }
  }
  if (rid.empty()) return std::string();

  // 96 dpi is the Word default; 1 px = 9525 EMU.
  int cx = 9525 * 96;
  int cy = 9525 * 96;
  const UnknownPart* media = FindUnknown(target);
  if (media != nullptr) {
    int px = 0;
    int py = 0;
    if (PngPixelSize(media->bytes, &px, &py)) {
      cx = px * 9525;
      cy = py * 9525;
    }
  }
  const std::string ext = std::to_string(cx);
  const std::string ecy = std::to_string(cy);
  const std::string name = BaseName(target).empty() ? "picture" : BaseName(target);
  std::string id;
  {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", next_doc_pr_++);
    id = buf;
  }

  std::string o;
  o += "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">";
  o += "<wp:extent cx=\"" + ext + "\" cy=\"" + ecy + "\"/>";
  o += "<wp:docPr id=\"" + id + "\" name=\"";
  EscAttr(o, name);
  o += "\"/>";
  o += "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">";
  o += "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + id + "\" name=\"";
  EscAttr(o, name);
  o += "\"/><pic:cNvPicPr/></pic:nvPicPr>";
  o += "<pic:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>";
  o += "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + ext + "\" cy=\"" + ecy +
       "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>";
  o += "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing>";
  return o;
}

void DocxWriter::WriteLeaf(Xml& x, const InlineItem& it) {
  switch (it.type) {
    case InlineItem::Type::Text: {
      const bool deleted = it.deleted_text || it.revision == 2;
      x.Start("r");
      WriteRunProps(x, it.format);
      x.Start(deleted ? "delText" : "t");
      if (NeedsSpacePreserve(it.text)) x.AttrQ("xml:space", "preserve");
      x.Chars(it.text);
      x.End();
      x.End();
      return;
    }
    case InlineItem::Type::Tab:
      x.Start("r");
      x.Empty("tab");
      x.End();
      return;
    case InlineItem::Type::Break:
      x.Start("r");
      if (it.break_kind == 1) {
        x.Vals("br", {{"type", "page"}});
      } else if (it.break_kind == 2) {
        x.Vals("br", {{"type", "column"}});
      } else {
        x.Empty("br");
      }
      x.End();
      return;
    case InlineItem::Type::Field: {
      const char* kind =
          it.field_kind == 1 ? "separate" : (it.field_kind == 2 ? "end" : "begin");
      x.Start("r");
      x.Vals("fldChar", {{"fldCharType", kind}});
      x.End();
      return;
    }
    case InlineItem::Type::InstrText:
      x.Start("r");
      WriteRunProps(x, it.format);
      x.Start("instrText");
      x.AttrQ("xml:space", "preserve");
      x.Chars(it.text);
      x.End();
      x.End();
      return;
    case InlineItem::Type::Drawing: {
      const std::string drawing = DrawingXml(it);
      if (drawing.empty()) return;
      x.Start("r");
      WriteRunProps(x, it.format);
      x.Raw(drawing);
      x.End();
      return;
    }
    case InlineItem::Type::Symbol: {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04X", static_cast<unsigned>(it.symbol) & 0xFFFFu);
      x.Start("r");
      WriteRunProps(x, it.format);
      x.Vals("sym", {{"font", it.symbol_font}, {"char", buf}});
      x.End();
      return;
    }
    case InlineItem::Type::BookmarkStart: {
      // The DOM models a bookmark as one item (the reader pairs the markers
      // and keeps the name), so the end marker is re-emitted right here. A
      // lone w:bookmarkStart would make Word report a damaged document.
      const std::string id = std::to_string(it.note_id);
      x.Vals("bookmarkStart", {{"id", id}, {"name", it.text}});
      x.Vals("bookmarkEnd", {{"id", id}});
      return;
    }
    case InlineItem::Type::BookmarkEnd:
      x.Vals("bookmarkEnd", {{"id", std::to_string(it.note_id)}});
      return;
    case InlineItem::Type::FootnoteRef:
    case InlineItem::Type::EndnoteRef:
    case InlineItem::Type::CommentRef: {
      const char* name = it.type == InlineItem::Type::FootnoteRef
                             ? "footnoteReference"
                             : (it.type == InlineItem::Type::EndnoteRef ? "endnoteReference"
                                                                        : "commentReference");
      x.Start("r");
      WriteRunProps(x, it.format);
      x.Vals(name, {{"id", std::to_string(it.note_id)}});
      x.End();
      return;
    }
    case InlineItem::Type::Separator:
      x.Start("r");
      x.Empty("separator");
      x.End();
      return;
  }
}

void DocxWriter::WriteInlineItem(Xml& x, const InlineItem& it) {
  if (it.revision == 1 || it.revision == 2) {
    // CT_TrackChange: w:id and w:author are required, w:date is optional.
    x.Start(it.revision == 1 ? "ins" : "del");
    x.Attr("id", std::to_string(next_rev_id_++));
    x.Attr("author", it.revision_author.empty() ? "DocCore" : it.revision_author);
    if (!it.revision_date.empty()) x.Attr("date", it.revision_date);
    WriteLeaf(x, it);
    x.End();
    return;
  }
  WriteLeaf(x, it);
}

void DocxWriter::WriteHyperlink(Xml& x, const std::vector<InlineItem>& runs, size_t begin,
                                size_t end, const std::string& target,
                                const std::string& anchor) {
  std::string rid;
  if (!target.empty()) {
    const bool external = IsExternalTarget(target);
    if (!external && FindUnknown(target) == nullptr) {
      // Internal target that the package does not carry: keep the anchor only,
      // otherwise the saved package would contain a dangling relationship.
      if (warnings_ != nullptr) {
        Warning w;
        w.code = static_cast<int>(WarningCode::UnknownPartKept);
        w.module = "docx";
        w.message = "docx write: hyperlink target not in package: " + target;
        warnings_->Add(w);
      }
    } else {
      const std::string key = std::string(external ? "e|" : "i|") + target;
      auto found = link_rid_.find(key);
      if (found != link_rid_.end()) {
        rid = found->second;
      } else {
        rid = AddRel(kRelHyperlink, external ? target : RelativeFromWord(target), external);
        link_rid_[key] = rid;
      }
    }
  }
  x.Start("hyperlink");
  if (!rid.empty()) x.AttrQ("r:id", rid);
  if (!anchor.empty()) x.Attr("anchor", anchor);
  for (size_t i = begin; i < end; ++i) WriteInlineItem(x, runs[i]);
  x.End();
}

// ---- blocks ---------------------------------------------------------------

void DocxWriter::WriteSectPrInto(Xml& x, const Section& s) {
  x.Start("sectPr");
  for (const auto& h : s.headers) {
    auto it = hf_rid_.find(&h);
    if (it == hf_rid_.end()) continue;
    x.Start("headerReference");
    x.Attr("type", HfKindName(h.kind));
    x.AttrQ("r:id", it->second);
    x.End();
  }
  // 水印需要一个页眉载体。本节原本没有页眉时，Build 已经合成一个只含水印的
  // 默认页眉，这里补上引用。
  {
    auto it = wm_header_rid_.find(&s);
    if (it != wm_header_rid_.end()) {
      x.Start("headerReference");
      x.Attr("type", "default");
      x.AttrQ("r:id", it->second);
      x.End();
    }
  }
  for (const auto& f : s.footers) {
    auto it = hf_rid_.find(&f);
    if (it == hf_rid_.end()) continue;
    x.Start("footerReference");
    x.Attr("type", HfKindName(f.kind));
    x.AttrQ("r:id", it->second);
    x.End();
  }
  // CT_SectPr order: header/footer references, pgSz, pgMar, pgNumType, cols,
  // titlePg (ECMA-376 §17.6.17).
  const PageSetup& p = s.page;
  if (p.orientation == 1) {
    x.Vals("pgSz", {{"w", std::to_string(p.width_twips)},
                    {"h", std::to_string(p.height_twips)},
                    {"orient", "landscape"}});
  } else {
    x.Vals("pgSz", {{"w", std::to_string(p.width_twips)}, {"h", std::to_string(p.height_twips)}});
  }
  x.Vals("pgMar", {{"top", std::to_string(p.margin_top)},
                   {"right", std::to_string(p.margin_right)},
                   {"bottom", std::to_string(p.margin_bottom)},
                   {"left", std::to_string(p.margin_left)},
                   {"header", std::to_string(p.header_dist)},
                   {"footer", std::to_string(p.footer_dist)},
                   {"gutter", std::to_string(p.gutter)}});
  if (s.page_number_start >= 0) {
    x.Vals("pgNumType", {{"start", std::to_string(s.page_number_start)}});
  }
  if (!s.columns.empty()) {
    x.Start("cols");
    x.Attr("num", std::to_string(s.column_count));
    x.Attr("space", std::to_string(s.column_space));
    for (const auto& c : s.columns) {
      x.Vals("col", {{"w", std::to_string(c.width_twips)},
                     {"space", std::to_string(c.space_twips)}});
    }
    x.End();
  } else if (s.column_count > 1 || s.column_space != 720) {
    x.Vals("cols", {{"num", std::to_string(s.column_count)},
                    {"space", std::to_string(s.column_space)}});
  }
  if (s.title_pg) x.Empty("titlePg");
  x.End();
}

void DocxWriter::WriteParagraphProps(Xml& x, const ParagraphFormat& f, const Section* closing) {
  if (!HasParagraphProps(f) && closing == nullptr) return;
  x.Start("pPr");
  WriteParagraphPropsBody(x, f);
  if (closing != nullptr) WriteSectPrInto(x, *closing);
  x.End();
}

void DocxWriter::WriteParagraph(Xml& x, const Paragraph& p, const Section* closing) {
  x.Start("p");
  WriteParagraphProps(x, p.format, closing);
  size_t i = 0;
  while (i < p.runs.size()) {
    const InlineItem& it = p.runs[i];
    // The reader flattens w:hyperlink into its runs and records the resolved
    // target on each item; regroup maximal runs of the same target on save.
    if (!it.hyperlink_target.empty() || !it.hyperlink_anchor.empty()) {
      size_t j = i + 1;
      while (j < p.runs.size() && p.runs[j].hyperlink_target == it.hyperlink_target &&
             p.runs[j].hyperlink_anchor == it.hyperlink_anchor) {
        ++j;
      }
      WriteHyperlink(x, p.runs, i, j, it.hyperlink_target, it.hyperlink_anchor);
      i = j;
      continue;
    }
    WriteInlineItem(x, it);
    ++i;
  }
  x.End();
}

void DocxWriter::WriteTable(Xml& x, const Table& t) {
  x.Start("tbl");
  x.Start("tblPr");
  if (!t.style.empty()) x.Vals("tblStyle", {{"val", t.style}});
  if (t.width_twips >= 0) {
    x.Vals("tblW", {{"w", std::to_string(t.width_twips)},
                    {"type", t.width_twips == 0 ? "auto" : "dxa"}});
  }
  if (t.alignment >= 0) {
    const char* jc = t.alignment == 1 ? "center" : (t.alignment == 2 ? "right" : "left");
    x.Vals("jc", {{"val", jc}});
  }
  if (t.indent_twips != 0) {
    x.Vals("tblInd", {{"w", std::to_string(t.indent_twips)}, {"type", "dxa"}});
  }
  if (t.borders >= 0) x.Empty("tblBorders");
  if (t.layout_fixed) x.Vals("tblLayout", {{"type", "fixed"}});
  x.End();
  if (!t.grid_twips.empty()) {
    x.Start("tblGrid");
    for (int w : t.grid_twips) x.Vals("gridCol", {{"w", std::to_string(w)}});
    x.End();
  }
  for (const Row& row : t.rows) {
    x.Start("tr");
    if (row.height_twips >= 0 || row.header_row || row.cant_split) {
      x.Start("trPr");
      if (row.cant_split) x.Empty("cantSplit");
      if (row.height_twips >= 0) {
        const char* rule =
            row.height_rule == 1 ? "atLeast" : (row.height_rule == 2 ? "exact" : "auto");
        x.Vals("trHeight",
               {{"val", std::to_string(row.height_twips)}, {"hRule", rule}});
      }
      if (row.header_row) x.Empty("tblHeader");
      x.End();
    }
    for (const Cell& cell : row.cells) {
      x.Start("tc");
      x.Start("tcPr");
      if (cell.format.width_twips >= 0) {
        x.Vals("tcW", {{"w", std::to_string(cell.format.width_twips)}, {"type", "dxa"}});
      }
      if (cell.format.grid_span > 1) {
        x.Vals("gridSpan", {{"val", std::to_string(cell.format.grid_span)}});
      }
      if (cell.format.vmerge == 1) x.Empty("vMerge");
      if (cell.format.vmerge == 2) x.Vals("vMerge", {{"val", "restart"}});
      if (cell.format.shading >= 0) {
        x.Vals("shd",
               {{"val", "clear"}, {"color", "auto"}, {"fill", Hex6(cell.format.shading)}});
      }
      if (cell.format.no_wrap) x.Empty("noWrap");
      if (cell.format.valign >= 0) {
        const char* v = cell.format.valign == 1 ? "center"
                                                : (cell.format.valign == 2 ? "bottom" : "top");
        x.Vals("vAlign", {{"val", v}});
      }
      x.End();
      if (cell.blocks.empty()) {
        x.Start("p");
        x.End();
      } else {
        WriteBlocks(x, cell.blocks);
      }
      x.End();
    }
    x.End();
  }
  x.End();
}

void DocxWriter::WriteBlocks(Xml& x, const std::vector<Block>& blocks) {
  for (const Block& b : blocks) {
    if (b.type == Block::Type::Table && b.table != nullptr) {
      WriteTable(x, *b.table);
    } else {
      WriteParagraph(x, b.paragraph, nullptr);
    }
  }
}

// ---- styles ---------------------------------------------------------------

const char* StyleKindName(Style::Kind kind) {
  switch (kind) {
    case Style::Kind::Character: return "character";
    case Style::Kind::Table: return "table";
    case Style::Kind::Numbering: return "numbering";
    default: return "paragraph";
  }
}

std::string DocxWriter::StylesXml() {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start("styles");
  x.AttrQ("xmlns:w", kW);
  if (doc_.has_doc_defaults) {
    x.Start("docDefaults");
    if (HasRunProps(doc_.default_run)) {
      x.Start("rPrDefault");
      WriteRunProps(x, doc_.default_run);
      x.End();
    }
    if (HasParagraphProps(doc_.default_paragraph)) {
      x.Start("pPrDefault");
      WriteParagraphProps(x, doc_.default_paragraph, nullptr);
      x.End();
    }
    x.End();
  }
  for (const Style& s : doc_.styles) {
    if (s.id.empty()) continue;
    x.Start("style");
    x.Attr("type", StyleKindName(s.kind));
    x.Attr("styleId", s.id);
    if (!s.name.empty()) x.Vals("name", {{"val", s.name}});
    if (!s.based_on.empty()) x.Vals("basedOn", {{"val", s.based_on}});
    if (!s.next.empty()) x.Vals("next", {{"val", s.next}});
    if (!s.link.empty()) x.Vals("link", {{"val", s.link}});
    if (s.hidden) x.Empty("hidden");
    if (s.priority >= 0) x.Vals("uiPriority", {{"val", std::to_string(s.priority)}});
    if (s.semi_hidden) x.Empty("semiHidden");
    if (s.unhide_when_used) x.Empty("unhideWhenUsed");
    if (s.quick_format) x.Empty("qFormat");
    // 表格样式的 CT_Style 只允许 tblPr/trPr/tcPr/tblStylePr；塞 pPr 进去是非法
    // 结构，所以这里单独走 tblBorders（重建器生成的 TableGrid 靠它出框线）。
    if (s.kind == Style::Kind::Table) {
      const bool any_border = s.paragraph.border_top >= 0 || s.paragraph.border_bottom >= 0 ||
                              s.paragraph.border_left >= 0 || s.paragraph.border_right >= 0;
      if (any_border) {
        x.Start("tblPr");
        x.Start("tblBorders");
        auto side = [&](const char* name, int color) {
          if (color < 0) return;
          x.Vals(name, {{"val", "single"}, {"sz", "4"}, {"space", "0"}, {"color", Hex6(color)}});
        };
        side("top", s.paragraph.border_top);
        side("left", s.paragraph.border_left);
        side("bottom", s.paragraph.border_bottom);
        side("right", s.paragraph.border_right);
        x.End();
        x.End();
      }
      x.End();
      continue;
    }
    if (HasParagraphProps(s.paragraph)) {
      WriteParagraphProps(x, s.paragraph, nullptr);
    }
    if (HasRunProps(s.run)) WriteRunProps(x, s.run);
    x.End();
  }
  x.End();
  return x.Take();
}

std::string DocxWriter::NumberingXml() {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start("numbering");
  x.AttrQ("xmlns:w", kW);
  for (const AbstractNumbering& a : doc_.abstract_numberings) {
    if (a.id.empty()) continue;
    x.Start("abstractNum");
    x.Attr("abstractNumId", a.id);
    const size_t levels = std::min<size_t>(a.levels.size(), 9);
    for (size_t i = 0; i < levels; ++i) {
      const NumberingLevel& l = a.levels[i];
      x.Start("lvl");
      x.Attr("ilvl", std::to_string(i));
      x.Vals("start", {{"val", std::to_string(l.start)}});
      if (!l.num_fmt.empty()) x.Vals("numFmt", {{"val", l.num_fmt}});
      // CT_Lvl order: start, numFmt, lvlRestart, pStyle, isLgl, suff, lvlText,
      // lvlJc, pPr, rPr.
      if (!l.suffix.empty()) x.Vals("suff", {{"val", l.suffix}});
      if (!l.lvl_text.empty()) x.Vals("lvlText", {{"val", l.lvl_text}});
      if (l.alignment >= 0) x.Vals("lvlJc", {{"val", JcName(l.alignment)}});
      if (HasParagraphProps(l.paragraph)) {
        WriteParagraphProps(x, l.paragraph, nullptr);
      }
      if (!l.font_ascii.empty() || !l.font_eastasia.empty() || l.color >= 0 ||
          l.run_size_half >= 0) {
        x.Start("rPr");
        if (!l.font_ascii.empty() || !l.font_eastasia.empty()) {
          x.Start("rFonts");
          if (!l.font_ascii.empty()) x.Attr("ascii", l.font_ascii);
          if (!l.font_eastasia.empty()) x.Attr("eastAsia", l.font_eastasia);
          x.End();
        }
        if (l.color >= 0) x.Vals("color", {{"val", Hex6(l.color)}});
        if (l.run_size_half >= 0) x.Vals("sz", {{"val", std::to_string(l.run_size_half)}});
        x.End();
      }
      x.End();
    }
    x.End();
  }
  for (const NumberingInstance& n : doc_.numbering) {
    if (n.num_id.empty()) continue;
    x.Start("num");
    x.Attr("numId", n.num_id);
    x.Vals("abstractNumId", {{"val", n.abstract_id}});
    for (size_t i = 0; i < n.level_start_overrides.size() && i < 9; ++i) {
      if (n.level_start_overrides[i] < 0) continue;
      x.Start("lvlOverride");
      x.Attr("ilvl", std::to_string(i));
      x.Vals("startOverride", {{"val", std::to_string(n.level_start_overrides[i])}});
      x.End();
    }
    x.End();
  }
  x.End();
  return x.Take();
}

// ---- notes, comments, headers and footers ---------------------------------

std::string DocxWriter::NotesXml(const std::vector<Note>& notes, bool footnotes) {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start(footnotes ? "footnotes" : "endnotes");
  x.AttrQ("xmlns:w", kW);
  // A Word notes part always carries the two rule notes. The reader drops them
  // (they are marked by @type), so they never reach the DOM and are rebuilt
  // here instead of being round-tripped.
  auto rule = [&](const char* type, const char* id, const char* marker) {
    x.Start(footnotes ? "footnote" : "endnote");
    x.Attr("type", type);
    x.Attr("id", id);
    x.Start("p");
    x.Start("r");
    x.Empty(marker);
    x.End();
    x.End();
    x.End();
  };
  rule("separator", "-1", "separator");
  rule("continuationSeparator", "0", "continuationSeparator");
  for (const Note& n : notes) {
    x.Start(footnotes ? "footnote" : "endnote");
    x.Attr("id", std::to_string(n.id));
    if (n.blocks.empty()) {
      x.Start("p");
      x.End();
    } else {
      WriteBlocks(x, n.blocks);
    }
    x.End();
  }
  x.End();
  return x.Take();
}

std::string DocxWriter::CommentsXml() {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start("comments");
  x.AttrQ("xmlns:w", kW);
  for (const Comment& c : doc_.comments) {
    x.Start("comment");
    x.Attr("id", std::to_string(c.id));
    if (!c.author.empty()) x.Attr("author", c.author);
    if (!c.initials.empty()) x.Attr("initials", c.initials);
    if (!c.date.empty()) x.Attr("date", c.date);
    if (c.blocks.empty()) {
      x.Start("p");
      x.End();
    } else {
      WriteBlocks(x, c.blocks);
    }
    x.End();
  }
  x.End();
  return x.Take();
}

std::string DocxWriter::HeaderFooterXml(const std::vector<Block>& blocks, bool header) {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start(header ? "hdr" : "ftr");
  x.AttrQ("xmlns:w", kW);
  const bool with_watermark = header && doc_.watermark.present();
  if (with_watermark) {
    // VML 需要 v/o 两个命名空间；图片水印还要 r 来引用媒体部件。
    x.AttrQ("xmlns:v", "urn:schemas-microsoft-com:vml");
    x.AttrQ("xmlns:o", "urn:schemas-microsoft-com:office:office");
    x.AttrQ("xmlns:r", kR);
  }
  WriteBlocks(x, blocks);
  if (with_watermark) WriteWatermarkParagraph(x);
  x.End();
  return x.Take();
}

// ---- 水印 -----------------------------------------------------------------

namespace {

// Word 的文字水印是把文字沿 VML 的"艺术字"路径拉伸，所以字号写 1pt、真实
// 尺寸由形状盒决定。这两块 shapetype 样板照抄 Word 的输出，不自创形状——
// 自创的形状 Word 能显示，但打印/导出 PDF 时会走不同分支。
const char* kVmlTextShapeType =
    "<v:shapetype id=\"_x0000_t136\" coordsize=\"21600,21600\" o:spt=\"136\" adj=\"10800\" "
    "path=\"m@7,l@8,m@5,21600l@6,21600e\">"
    "<v:formulas>"
    "<v:f eqn=\"sum #0 0 10800\"/><v:f eqn=\"prod #0 2 1\"/><v:f eqn=\"sum 21600 0 @1\"/>"
    "<v:f eqn=\"sum 0 0 @2\"/><v:f eqn=\"sum 21600 0 @3\"/><v:f eqn=\"if @0 @3 0\"/>"
    "<v:f eqn=\"if @0 21600 @1\"/><v:f eqn=\"if @0 0 @2\"/><v:f eqn=\"if @0 @4 21600\"/>"
    "<v:f eqn=\"mid @5 @6\"/><v:f eqn=\"mid @8 @5\"/><v:f eqn=\"mid @7 @8\"/>"
    "<v:f eqn=\"mid @6 @7\"/><v:f eqn=\"sum @6 0 @5\"/>"
    "</v:formulas>"
    "<v:path textpathok=\"t\" o:connecttype=\"custom\" "
    "o:connectlocs=\"@9,0;@10,10800;@11,21600;@12,10800\" "
    "o:connectangles=\"270,180,90,0\"/>"
    "<v:textpath on=\"t\" fitshape=\"t\"/>"
    "<v:handles><v:h position=\"#0,bottomRight\" xrange=\"6629,14971\"/></v:handles>"
    "<o:lock v:ext=\"edit\" text=\"t\" shapetype=\"t\"/>"
    "</v:shapetype>";

const char* kVmlPictureShapeType =
    "<v:shapetype id=\"_x0000_t75\" coordsize=\"21600,21600\" o:spt=\"75\" "
    "o:preferrelative=\"t\" path=\"m@4@5l@4@11@9@11@9@5xe\" filled=\"f\" stroked=\"f\">"
    "<v:stroke joinstyle=\"miter\"/>"
    "<v:formulas>"
    "<v:f eqn=\"if lineDrawn pixelLineWidth 0\"/><v:f eqn=\"sum @0 1 0\"/>"
    "<v:f eqn=\"sum 0 0 @1\"/><v:f eqn=\"prod @2 1 2\"/><v:f eqn=\"prod @3 21600 pixelWidth\"/>"
    "<v:f eqn=\"prod @3 21600 pixelHeight\"/><v:f eqn=\"sum @0 0 1\"/>"
    "<v:f eqn=\"prod @6 1 2\"/><v:f eqn=\"prod @7 21600 pixelWidth\"/>"
    "<v:f eqn=\"sum @8 21600 0\"/><v:f eqn=\"prod @7 21600 pixelHeight\"/>"
    "<v:f eqn=\"sum @10 21600 0\"/>"
    "</v:formulas>"
    "<v:path o:extrusionok=\"f\" gradientshapeok=\"t\" o:connecttype=\"rect\"/>"
    "<o:lock v:ext=\"edit\" aspectratio=\"t\"/>"
    "</v:shapetype>";

std::string HexColor(int rgb) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(rgb) & 0xFFFFFFu);
  return buf;
}

std::string Esc(const std::string& v) {
  std::string out;
  EscAttr(out, v);
  return out;
}

// CryptoAPI 的 ALG_ID 尾号（Word 的 cryptAlgorithmSid 用的是这个，不是
// ST_Algo 的序号）。
int AlgorithmSid(const std::string& algorithm) {
  if (algorithm == "MD5") return 3;
  if (algorithm == "SHA-256") return 12;
  if (algorithm == "SHA-384") return 13;
  if (algorithm == "SHA-512") return 14;
  return 4;  // SHA-1
}

std::string FmtNum(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  std::string s(buf);
  // 去掉多余的尾零：527.85 保留，527.00 写成 527。
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

}  // namespace

// 水印盒宽：Word 在 A4 上写的是 527.85pt，正好是页宽的 0.8868 倍；没有盒
// 信息时按这个比例推，任何纸型都能得到同样的观感。高宽比固定 5:1。
double DocxWriter::WatermarkBoxWidth(const Watermark& wm) const {
  if (wm.box_width_pt > 0) return wm.box_width_pt;
  double page_w = 595.276;
  if (!doc_.sections.empty()) {
    page_w = static_cast<double>(doc_.sections[0].page.width_twips) / 20.0;  // 缇 -> pt
  }
  return page_w * 0.8868;
}

double DocxWriter::WatermarkBoxHeight(const Watermark& wm) const {
  if (wm.box_height_pt > 0) return wm.box_height_pt;
  return WatermarkBoxWidth(wm) / 5.0;
}

std::string DocxWriter::WatermarkShapeStyle(const Watermark& wm, double w_pt, double h_pt) const {
  // Word 用 z-index 负值把水印压到正文下面；位置相对页边距居中。
  std::string s = "position:absolute;margin-left:0;margin-top:0;width:" + FmtNum(w_pt) +
                  "pt;height:" + FmtNum(h_pt) + "pt;rotation:" + FmtNum(wm.rotation) +
                  ";z-index:-251656192;mso-position-horizontal:center;"
                  "mso-position-horizontal-relative:margin;mso-position-vertical:center;"
                  "mso-position-vertical-relative:margin";
  return s;
}

void DocxWriter::WriteWatermarkParagraph(Xml& x) {
  const Watermark& wm = doc_.watermark;
  const double w_pt = WatermarkBoxWidth(wm);
  const double h_pt = WatermarkBoxHeight(wm);
  const std::string style = Esc(WatermarkShapeStyle(wm, w_pt, h_pt));

  // 整段按原文拼：VML 的元素和属性前缀混用（v:shape 的 style 不带前缀、
  // o:spid 带前缀），交给通用的 XML 构造器反而更容易出错。
  std::string s = "<w:p><w:r><w:rPr><w:noProof/></w:rPr><w:pict>";
  if (wm.kind == Watermark::Kind::Text) {
    s += kVmlTextShapeType;
    s += "<v:shape id=\"PowerPlusWaterMarkObject1\" o:spid=\"_x0000_s2049\" "
         "type=\"#_x0000_t136\" style=\"";
    s += style;
    s += "\" o:allowincell=\"f\" fillcolor=\"";
    s += HexColor(wm.color);
    s += "\" stroked=\"f\">";
    if (wm.semitransparent) s += "<v:fill opacity=\".5\"/>";
    std::string tp = "font-family:\"" + wm.font + "\";font-size:1pt";
    if (wm.bold) tp += ";font-weight:bold";
    if (wm.italic) tp += ";font-style:italic";
    s += "<v:textpath style=\"" + Esc(tp) + "\" string=\"" + Esc(wm.text) + "\"/>";
    s += "</v:shape>";
  } else {
    s += kVmlPictureShapeType;
    s += "<v:shape id=\"PowerPlusWaterMarkObject2\" o:spid=\"_x0000_s2050\" "
         "type=\"#_x0000_t75\" style=\"";
    s += style;
    s += "\" o:allowincell=\"f\" fillcolor=\"white\" stroked=\"f\">";
    s += "<v:imagedata r:id=\"rId1\" o:title=\"watermark\"";
    if (wm.washout) {
      // Word 的"褪色"就是这两个增益参数，数值取自它自己的输出。
      s += " gain=\"19661f\" blacklevel=\"22938f\"";
    }
    s += "/></v:shape>";
  }
  s += "</w:pict></w:r></w:p>";
  x.RawMarkup(s);
}

// ---- settings：文档保护 ---------------------------------------------------

namespace {

// 删掉 XML 里所有名为 `local` 的元素（自闭合与带内容两种写法都处理），
// 前缀任意。只做这一件事，所以不引入完整 DOM——settings.xml 里还有大量
// 我们没建模的开关，整段重写会丢东西。
void EraseElement(std::string* xml, const std::string& local) {
  size_t pos = 0;
  for (;;) {
    bool removed = false;
    size_t scan = pos;
    while ((scan = xml->find('<', scan)) != std::string::npos) {
      size_t name_end = scan + 1;
      while (name_end < xml->size() && xml->compare(name_end, 1, "/") == 0) ++name_end;
      const size_t name_start = name_end;
      while (name_end < xml->size()) {
        const char c = (*xml)[name_end];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') break;
        ++name_end;
      }
      std::string qname = xml->substr(name_start, name_end - name_start);
      const size_t colon = qname.rfind(':');
      const std::string lname = colon == std::string::npos ? qname : qname.substr(colon + 1);
      if (lname != local) {
        scan = name_end;
        continue;
      }
      const size_t gt = xml->find('>', name_end);
      if (gt == std::string::npos) return;
      const bool self_closing = gt > 0 && (*xml)[gt - 1] == '/';
      size_t erase_end = gt + 1;
      if (!self_closing) {
        const std::string close = "</" + qname + ">";
        const size_t close_at = xml->find(close, gt);
        if (close_at == std::string::npos) return;  // 结构坏了，别硬删
        erase_end = close_at + close.size();
      }
      xml->erase(scan, erase_end - scan);
      pos = scan;
      removed = true;
      break;
    }
    if (!removed) return;
  }
}

// 在根元素的开始标签之后插入一段标记。
void InsertAfterRootStart(std::string* xml, const std::string& markup) {
  const size_t root = xml->find('<');
  if (root == std::string::npos) return;
  if (xml->compare(root, 2, "<?") == 0) {
    const size_t decl = xml->find("?>", root);
    if (decl == std::string::npos) return;
    const size_t next = xml->find('<', decl + 2);
    if (next == std::string::npos) return;
    const size_t gt = xml->find('>', next);
    if (gt == std::string::npos) return;
    xml->insert(gt + 1, markup);
    return;
  }
  const size_t gt = xml->find('>', root);
  if (gt == std::string::npos) return;
  xml->insert(gt + 1, markup);
}

std::string ProtectionElement(const char* element, const ProtectionHash& h, bool formatting,
                             int edit, bool recommended) {
  std::string s = "<w:";
  s += element;
  if (edit >= 0) {
    static const char* kEdits[] = {"none", "readOnly", "comments", "trackedChanges", "forms"};
    const int idx = (edit >= 0 && edit <= 4) ? edit : 0;
    s += " w:edit=\"" + std::string(kEdits[idx]) + "\"";
  }
  if (formatting) s += " w:formatting=\"1\"";
  if (recommended) s += " w:recommended=\"1\"";
  s += " w:enforcement=\"1\"";
  s += " w:cryptProviderType=\"rsaAES\" w:cryptAlgorithmClass=\"hash\" "
       "w:cryptAlgorithmType=\"typeAny\"";
  // 同时写两套属性名：hash/salt 是老名字（Word 2007+），hashValue/saltValue
  // 是新名字（Word 2013+），不同版本的读取方各认一套。
  s += " w:cryptAlgorithmSid=\"" + std::to_string(AlgorithmSid(h.algorithm)) + "\"";
  s += " w:cryptSpinCount=\"" + std::to_string(h.spin_count) + "\"";
  s += " w:algorithmName=\"" + Esc(h.algorithm) + "\"";
  s += " w:hashSize=\"" + std::to_string(h.hash_size) + "\"";
  s += " w:hash=\"" + Esc(h.hash) + "\" w:hashValue=\"" + Esc(h.hash) + "\"";
  s += " w:salt=\"" + Esc(h.salt) + "\" w:saltValue=\"" + Esc(h.salt) + "\"";
  s += "/>";
  return s;
}

}  // namespace

std::string DocxWriter::SettingsXml() {
  const DocumentProtection& p = doc_.protection;
  std::string base = doc_.settings_xml;
  if (base.empty()) {
    base = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings xmlns:w=\"" +
           std::string(kW) + "\"></w:settings>";
  }
  // 先把旧的保护元素清掉：同一个元素出现两次时 Word 的行为不确定。
  EraseElement(&base, "documentProtection");
  EraseElement(&base, "writeProtection");

  std::string insert;
  if (p.enabled) {
    insert += ProtectionElement("documentProtection", p.document_hash, p.formatting, p.edit, false);
  }
  if (p.write_protected && p.write_hash.present()) {
    insert += ProtectionElement("writeProtection", p.write_hash, false, -1, p.recommended);
  }
  if (!insert.empty()) InsertAfterRootStart(&base, insert);
  return base;
}

// ---- document part --------------------------------------------------------

std::string DocxWriter::DocumentXml() {
  Xml x;
  x.Prefix("w");
  x.Decl();
  x.Start("document");
  x.AttrQ("xmlns:w", kW);
  x.AttrQ("xmlns:r", kR);
  x.AttrQ("xmlns:wp",
          "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
  x.AttrQ("xmlns:a", "http://schemas.openxmlformats.org/drawingml/2006/main");
  x.AttrQ("xmlns:pic", "http://schemas.openxmlformats.org/drawingml/2006/picture");
  x.Start("body");

  const size_t count = doc_.sections.size();
  for (size_t i = 0; i < count; ++i) {
    const Section& s = doc_.sections[i];
    const bool last = (i + 1 == count);
    const size_t blocks = s.blocks.size();
    for (size_t b = 0; b < blocks; ++b) {
      const Block& blk = s.blocks[b];
      // A section break lives in the pPr of the paragraph that closes the
      // section, so the last paragraph of every non-final section carries it.
      const bool carrier = !last && b + 1 == blocks && blk.type == Block::Type::Paragraph;
      if (blk.type == Block::Type::Table && blk.table != nullptr) {
        WriteTable(x, *blk.table);
      } else {
        WriteParagraph(x, blk.paragraph, carrier ? &s : nullptr);
      }
    }
    if (last) {
      if (blocks != 0 && s.blocks.back().type == Block::Type::Table) {
        // Word wants a paragraph after a table that ends the body.
        x.Start("p");
        x.End();
      }
      WriteSectPrInto(x, s);
    } else if (blocks == 0 || s.blocks.back().type != Block::Type::Paragraph) {
      Paragraph empty;
      WriteParagraph(x, empty, &s);
    }
  }
  if (count == 0) {
    Section defaults;
    WriteSectPrInto(x, defaults);
  }

  x.End();  // body
  x.End();  // document
  return x.Take();
}

std::string ExtensionOf(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return {};
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return ext;
}

std::string DocxWriter::ContentTypesXml() {
  Xml x;
  x.Prefix("");
  x.Decl();
  x.Start("Types");
  x.AttrQ("xmlns", kCtNs);
  x.Start("Default");
  x.Attr("Extension", "rels");
  x.Attr("ContentType", kCtRels);
  x.End();
  x.Start("Default");
  x.Attr("Extension", "xml");
  x.Attr("ContentType", kCtXml);
  x.End();
  // 媒体部件按扩展名给 Default（Word 也是这么写的，不逐件写 Override）。
  for (const auto& kv : media_defaults_) {
    x.Start("Default");
    x.Attr("Extension", kv.first);
    x.Attr("ContentType", kv.second);
    x.End();
  }
  auto override_part = [&](const std::string& name, const char* mime) {
    x.Start("Override");
    x.Attr("PartName", "/" + name);
    x.Attr("ContentType", mime);
    x.End();
  };
  override_part("word/document.xml", kCtMain);
  for (const PartOut& p : parts_) {
    if (p.content_type.empty()) continue;
    override_part(p.name, p.content_type.c_str());
  }
  x.End();
  return x.Take();
}

std::vector<uint8_t> DocxWriter::Build(WarningSink* warnings) {
  warnings_ = warnings;
  parts_.clear();
  hf_parts_.clear();
  rels_.clear();
  hf_rid_.clear();
  media_rid_.clear();
  link_rid_.clear();
  media_defaults_.clear();
  watermark_media_.clear();
  wm_header_rid_.clear();
  synthetic_headers_.clear();
  next_rel_ = 1;
  next_doc_pr_ = 1;
  next_rev_id_ = 1;

  // 1. Header and footer parts. The body needs their relationship ids, so this
  //    pass has to run before word/document.xml is serialized.
  //
  // 图片水印先落一个媒体部件：页眉的 VML 要按 rId1 引用它。
  if (doc_.watermark.present() && doc_.watermark.kind == Watermark::Kind::Image) {
    std::string ext = ExtensionOf(doc_.watermark.image_uri);
    if (ext.empty()) ext = "png";
    watermark_media_ = "word/media/watermark1." + ext;
    PartOut media;
    media.name = watermark_media_;
    media.data = doc_.watermark.image_bytes;
    // 媒体部件走 Default 扩展名映射，不写 Override。
    media_defaults_[ext] = "image/" + (ext == "jpg" ? std::string("jpeg") : ext);
    parts_.push_back(std::move(media));
  }

  int hf_index[2] = {0, 0};  // headers and footers are numbered separately
  auto add_one_hf = [&](const HeaderFooter& hf, bool header) -> std::string {
    auto hit = hf_rid_.find(&hf);
    if (hit != hf_rid_.end()) return hit->second;  // shared by two sections
    char file[32];
    std::snprintf(file, sizeof(file), "%s%d.xml", header ? "header" : "footer",
                  ++hf_index[header ? 0 : 1]);
    const std::string rid = AddRel(header ? kRelHeader : kRelFooter, file, false);
    hf_rid_[&hf] = rid;
    PartOut part;
    part.name = std::string("word/") + file;
    part.data = ToBytes(HeaderFooterXml(hf.blocks, header));
    part.content_type = header ? kCtHeader : kCtFooter;
    // 水印是图片时，页眉部件要有一条指向媒体部件的关系。每个页眉只有这一
    // 条关系，所以 id 固定 rId1（VML 里也是写死的 rId1）。
    if (header && !watermark_media_.empty()) {
      std::vector<RelEntry> one;
      RelEntry r;
      r.id = "rId1";
      r.type = kRelImage;
      r.target = watermark_media_.substr(std::string("word/").size());  // 相对 word/
      r.external = false;
      one.push_back(std::move(r));
      PartOut rels_part;
      rels_part.name = "word/_rels/" + std::string(file) + ".rels";
      rels_part.data = ToBytes(RelsXml(one));
      rels_part.content_type = kCtRels;
      hf_parts_.push_back(std::move(rels_part));
    }
    hf_parts_.push_back(std::move(part));
    return rid;
  };
  for (const Section& s : doc_.sections) {
    for (const HeaderFooter& hf : s.headers) add_one_hf(hf, true);
    for (const HeaderFooter& hf : s.footers) add_one_hf(hf, false);
    // 本节没有页眉但要写水印：合成一个只含水印的默认页眉。
    if (doc_.watermark.present() && s.headers.empty()) {
      auto own = std::make_unique<HeaderFooter>();
      own->kind = HeaderFooter::Kind::Default;
      wm_header_rid_[&s] = add_one_hf(*own, true);
      synthetic_headers_.push_back(std::move(own));
    }
  }

  // 2. Fixed parts: styles, numbering, notes, comments.
  auto add_fixed = [&](const char* rel_type, const char* file, std::string xml,
                       const char* content_type) {
    AddRel(rel_type, file, false);
    PartOut p;
    p.name = std::string("word/") + file;
    p.data = ToBytes(std::move(xml));
    p.content_type = content_type;
    parts_.push_back(std::move(p));
  };
  if (!doc_.styles.empty() || doc_.has_doc_defaults) {
    add_fixed(kRelStyles, "styles.xml", StylesXml(), kCtStyles);
  }
  if (!doc_.abstract_numberings.empty() || !doc_.numbering.empty()) {
    add_fixed(kRelNumbering, "numbering.xml", NumberingXml(), kCtNumbering);
  }
  if (!doc_.footnotes.empty()) {
    add_fixed(kRelFootnotes, "footnotes.xml", NotesXml(doc_.footnotes, true), kCtFootnotes);
  }
  if (!doc_.endnotes.empty()) {
    add_fixed(kRelEndnotes, "endnotes.xml", NotesXml(doc_.endnotes, false), kCtEndnotes);
  }
  if (!doc_.comments.empty()) {
    add_fixed(kRelComments, "comments.xml", CommentsXml(), kCtComments);
  }
  // settings.xml：原文里可能有一堆我们没建模的开关，所以只在"有东西要写"
  // 的时候输出，并且是在原文上打补丁（见 SettingsXml）。
  if (!doc_.settings_xml.empty() || doc_.protection.enabled || doc_.protection.write_protected) {
    add_fixed(kRelSettings, "settings.xml", SettingsXml(), kCtSettings);
  }

  // 3. The body. Image and hyperlink relationships are created while walking.
  const std::string document_xml = DocumentXml();

  // 4. Everything the reader did not model, byte for byte.
  for (const UnknownPart& up : doc_.unknown_parts) {
    std::string name = up.uri;
    if (!name.empty() && name[0] == '/') name.erase(0, 1);
    std::string reason;
    if (name.empty() || !IsSafePartName(name, &reason)) {
      if (warnings_ != nullptr) {
        Warning w;
        w.code = static_cast<int>(WarningCode::UnknownPartKept);
        w.module = "docx";
        w.message = "docx write: part name rejected (" + reason + "): " + up.uri;
        warnings_->Add(w);
      }
      continue;
    }
    PartOut p;
    p.name = name;
    p.data = up.bytes;
    if (!(up.mime == kCtXml && ExtensionOf(name) == "xml")) p.content_type = up.mime;
    parts_.push_back(std::move(p));
  }
  for (auto& p : hf_parts_) parts_.push_back(std::move(p));

  // 5. Package plumbing.
  const std::vector<RelEntry> root_rels = {
      {"rId1", kRelOfficeDocument, "word/document.xml", false}};
  std::vector<ZipWriteEntry> entries;
  auto push = [&](const std::string& name, std::vector<uint8_t> data) {
    ZipWriteEntry e;
    e.name = name;
    e.data = std::move(data);
    entries.push_back(std::move(e));
  };
  push("[Content_Types].xml", ToBytes(ContentTypesXml()));
  push("_rels/.rels", ToBytes(RelsXml(root_rels)));
  push("word/document.xml", ToBytes(document_xml));
  push("word/_rels/document.xml.rels", ToBytes(RelsXml(rels_)));
  for (const PartOut& p : parts_) push(p.name, p.data);
  return ZipWriteToBytes(entries);
}

}  // namespace

// ---- public entry points --------------------------------------------------

void Save(const Document& doc, Stream& out, const SaveOptions& opts) {
  if (opts.cancel != nullptr) opts.cancel->ThrowIfCancelled();
  DocxWriter writer(doc);
  const std::vector<uint8_t> bytes = writer.Build(opts.warnings);
  out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
}

void SaveToFile(const Document& doc, const std::string& path, const SaveOptions& opts) {
  if (opts.cancel != nullptr) opts.cancel->ThrowIfCancelled();
  DocxWriter writer(doc);
  const std::vector<uint8_t> bytes = writer.Build(opts.warnings);
  WriteFileAtomically(path, bytes);
}

}  // namespace text
}  // namespace doccore
