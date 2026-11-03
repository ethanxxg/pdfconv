// DocCore - DOCX -> DOM reader. Design refs: docs/02 §3.3.3
//
// The file mirrors the OOXML part tree:
//   word/document.xml  -> sections/paragraphs/tables + final sectPr
//   word/styles.xml    -> docDefaults + styles
//   word/numbering.xml -> abstractNum + num
//   word/footnotes.xml, word/endnotes.xml, word/comments.xml
//   word/header*.xml, word/footer*.xml
//
// Every parse function is entered with the Start event of the element it owns
// and returns after that element's End event, so callers can use the generic
// WalkChildren() helper below. Unknown parts are collected verbatim for the
// save side (docs/02 §3.1.3 item 6).
#include "doccore/docx.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>

#include "doccore/opc.h"
#include "doccore/xml.h"

namespace doccore {
namespace text {

namespace {

using doccore::ErrorCode;
using doccore::WarningCode;
using doccore::XmlEvent;
using doccore::XmlEventType;
using doccore::XmlReader;

// ---- event-stream helpers ---------------------------------------------

// Wraps XmlReader with an explicit open-element count, because XmlEvent::depth
// is off by one for self-closing elements (Start reports the element's own
// depth while its emitted End reports the parent's).
class XmlWalker {
 public:
  explicit XmlWalker(std::string_view xml) : reader_(xml) {}

  bool Next(XmlEvent& ev) {
    const bool ok = reader_.Next(ev);
    if (ok) {
      if (ev.type == XmlEventType::StartElement) {
        ++open_;
      } else if (ev.type == XmlEventType::EndElement) {
        --open_;
      }
    }
    return ok;
  }

  int open() const noexcept { return open_; }

 private:
  XmlReader reader_;
  int open_ = 0;
};

// `start` must be a Start event already consumed from `w`. Consumes the rest of
// that element, dispatching every immediate child Start to `on_child`. A child
// handler normally consumes that child's subtree; if it consumes nothing the
// subtree is skipped here. Returns after the element's End tag.
template <class F>
void WalkChildren(XmlWalker& w, const XmlEvent& start, F on_child) {
  const int mine = w.open();  // nesting count including `start`
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) return;
    if (ev.type == XmlEventType::StartElement && w.open() == mine + 1) on_child(ev);
  }
  throw Error(ErrorCode::Corrupt, "docx: unterminated <" + std::string(start.name) + ">");
}

// Concatenated character data of a leaf element; consumes the whole element.
std::string ElemText(XmlWalker& w, const XmlEvent& start) {
  (void)start;  // 只按 open 深度对齐，元素名由调用方判定
  const int mine = w.open();
  std::string out;
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) break;
    if (ev.type == XmlEventType::Text) out.append(ev.text);
  }
  return out;
}

// Element and attribute names keep their prefix; OOXML consumers match local
// names so that producers who remap prefixes still read.
std::string_view LocalName(std::string_view name) {
  const size_t colon = name.rfind(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string_view Attr(const XmlEvent& ev, std::string_view name) {
  for (const auto& a : ev.attributes) {
    if (LocalName(a.name) == name) return a.value;
  }
  return {};
}

std::string_view AttrAny(const XmlEvent& ev, std::initializer_list<std::string_view> names) {
  for (const auto& a : ev.attributes) {
    const std::string_view local = LocalName(a.name);
    if (std::find(names.begin(), names.end(), local) != names.end()) return a.value;
  }
  return {};
}

int ToInt(std::string_view s, int fallback) {
  if (s.empty()) return fallback;
  try {
    size_t used = 0;
    const long long v = std::stoll(std::string(s), &used);
    if (used != s.size()) return fallback;
    return static_cast<int>(v);
  } catch (...) {
    return fallback;
  }
}

// "0" / "false" / "off" mean off for val-able toggles and switches.
bool BoolAttr(std::string_view s, bool fallback) {
  if (s.empty()) return fallback;
  return !(s == "0" || s == "false" || s == "off");
}

// OOXML colors: "RRGGBB" (or "auto", which means inherit).
int ColorAttr(std::string_view s) {
  if (s.empty() || s == "auto") return -1;
  std::string hex;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      hex.push_back(c);
    } else if (c >= 'a' && c <= 'f') {
      hex.push_back(static_cast<char>(c - 'a' + 'A'));
    } else if (c >= 'A' && c <= 'F') {
      hex.push_back(c);
    } else {
      return -1;
    }
  }
  if (hex.size() != 6) return -1;
  return static_cast<int>(std::stoul(hex, nullptr, 16));
}

Tri TriFrom(bool on) { return on ? Tri::On : Tri::Off; }

std::string_view AsView(const std::vector<uint8_t>& v) {
  return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

const Relationships& EmptyRels() {
  static const Relationships kEmpty;
  return kEmpty;
}

int JcFromName(std::string_view v) {
  if (v == "center") return 1;
  if (v == "right" || v == "end") return 2;
  if (v == "both" || v == "justify" || v == "distribute") return 3;
  if (v == "left" || v == "start") return 0;
  return -1;
}

int VertAlignFromName(std::string_view v) {
  if (v == "superscript") return 1;
  if (v == "subscript") return 2;
  if (v == "baseline") return 0;
  return -1;
}

// ---- VML / settings 小工具 --------------------------------------------

// `v:shape/@style` 是一串 CSS。取某个属性的值（已去空白、去引号）。
std::string CssValue(std::string_view style, std::string_view key) {
  size_t pos = 0;
  while (pos <= style.size()) {
    size_t semi = style.find(';', pos);
    const std::string_view item = style.substr(pos, semi == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : semi - pos);
    const size_t colon = item.find(':');
    if (colon != std::string_view::npos) {
      std::string_view k = item.substr(0, colon);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.remove_suffix(1);
      while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.remove_prefix(1);
      if (k == key) {
        std::string_view v = item.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v.remove_prefix(1);
        if (v.size() >= 1 && (v.back() == '"' || v.back() == '\'')) v.remove_suffix(1);
        return std::string(v);
      }
    }
    if (semi == std::string_view::npos) break;
    pos = semi + 1;
  }
  return {};
}

// "527.85pt" / "105.57" / "1in" -> pt。
double LengthPt(std::string_view v) {
  if (v.empty()) return 0.0;
  try {
    size_t used = 0;
    const double n = std::stod(std::string(v), &used);
    const std::string_view unit = v.substr(used);
    if (unit == "in") return n * 72.0;
    if (unit == "cm") return n * 72.0 / 2.54;
    if (unit == "mm") return n * 72.0 / 25.4;
    if (unit == "px") return n * 0.75;
    if (unit == "em" || unit == "ex" || unit == "%") return 0.0;  // 相对单位不猜
    return n;                                                     // pt 或无单位
  } catch (...) {
    return 0.0;
  }
}

bool ContainsWatermarkMarker(std::string_view id) {
  // Word 的命名是 PowerPlusWaterMarkObject<数字>；别的生成器写法五花八门，
  // 所以只认 "watermark" 这个子串（不区分大小写）。
  std::string lower;
  lower.reserve(id.size());
  for (char c : id) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lower.find("watermark") != std::string::npos;
}

int EditFromName(std::string_view v) {
  if (v == "readOnly") return 1;
  if (v == "comments") return 2;
  if (v == "trackedChanges") return 3;
  if (v == "forms") return 4;
  return 0;  // none
}

// `w:cryptAlgorithmSid` 用的是 CryptoAPI 的 ALG_ID，不是 ST_Algo 的序号。
// Word 2007 写 4（CALG_SHA1），2010+ 还会额外写 algorithmName，优先用后者。
std::string AlgorithmFromSid(int sid) {
  switch (sid) {
    case 3: return "MD5";
    case 4: return "SHA-1";
    case 12: return "SHA-256";
    case 13: return "SHA-384";
    case 14: return "SHA-512";
    default: return "SHA-1";
  }
}

int DigestBytesFor(const std::string& algorithm) {
  if (algorithm == "MD5") return 16;
  if (algorithm == "SHA-256") return 32;
  if (algorithm == "SHA-384") return 48;
  if (algorithm == "SHA-512") return 64;
  return 20;
}

// Scans a drawing subtree for the image relationship (a:blip/@r:embed or
// v:imagedata/@r:id) and resolves it through `rels`.
std::string FindDrawingTarget(XmlWalker& w, const XmlEvent& start, const Relationships& rels) {
  (void)start;
  const int mine = w.open();
  std::string target;
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) break;
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view n = LocalName(ev.name);
    if (n != "blip" && n != "imagedata") continue;
    const std::string_view ref = AttrAny(ev, {"embed", "id", "link"});
    if (ref.empty()) continue;
    const Relationship* rel = rels.Find(ref);
    if (rel == nullptr) continue;
    target = rel->external() ? rel->target : rel->resolved;
  }
  return target;
}

// ---- reader ------------------------------------------------------------

class DocxReader {
 public:
  DocxReader(Package& pkg, WarningSink* warnings, CancelToken* cancel)
      : pkg_(pkg), warnings_(warnings), cancel_(cancel) {}

  void Read(Document& doc);

 private:
  // part-level parsing
  void ParseStyles(std::string_view xml, Document& doc);
  void ParseNumbering(std::string_view xml, Document& doc);
  void ParseHeaderFooterPart(const std::string& part_uri, std::string_view xml, HeaderFooter& out);
  void ParseNotes(const std::string& part_uri, std::string_view xml, std::vector<Note>& out);
  void ParseComments(const std::string& part_uri, std::string_view xml, Document& doc);
  void ParseBody(std::string_view xml, Document& doc);
  // w:settings：只取保护相关的两个元素，原文留在 doc.settings_xml 里。
  void ParseSettings(std::string_view xml, Document& doc);
  // 从页眉里认出 VML 水印。返回 true 表示找到（此时 out 已填好）。
  bool ParseHeaderWatermark(std::string_view xml, const Relationships* rels, Watermark& out);

  // element-level parsing
  void ParseBlockContainer(XmlWalker& w, const XmlEvent& container, const Relationships& rels,
                           std::vector<Block>& out, Section* closing_section, bool* sect_found);
  void ParseParagraph(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                      Paragraph& out, Section* closing_section, bool* sect_found);
  void ParseParagraphProps(XmlWalker& w, const XmlEvent& start, ParagraphFormat& out,
                           const Relationships& rels, Section* closing_section, bool* sect_found);
  void ParseRunProps(XmlWalker& w, const XmlEvent& start, RunFormat& out);
  void ParseTable(XmlWalker& w, const XmlEvent& start, const Relationships& rels, Table& out);
  void ParseSectPr(XmlWalker& w, const XmlEvent& start, Section& out, const Relationships& rels);
  void ParseNumberingLevel(XmlWalker& w, const XmlEvent& start, NumberingLevel& out);

  // run-level parsing shared by w:r, hyperlinks, ins/del, sdt and smartTag
  void ParseInlineChildren(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                           std::vector<InlineItem>& out, int revision, std::string_view author,
                           std::string_view date, const std::string& anchor,
                           const std::string& target);
  void ParseRunLike(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                    std::vector<InlineItem>& out, int revision, std::string_view author,
                    std::string_view date, const std::string& anchor, const std::string& target);
  void DispatchInlineChild(XmlWalker& w, const XmlEvent& c, const Relationships& rels,
                           std::vector<InlineItem>& out, int revision, std::string_view author,
                           std::string_view date, const std::string& anchor,
                           const std::string& target);
  void ParseInlineLeaf(XmlWalker& w, const XmlEvent& c, const Relationships& rels,
                       std::vector<InlineItem>& out, const RunFormat& fmt, int revision,
                       std::string_view author, std::string_view date, const std::string& anchor,
                       const std::string& target);

  void Warn(int code, const std::string& message);

  Package& pkg_;
  WarningSink* warnings_;
  CancelToken* cancel_;
  const Relationships* doc_rels_ = nullptr;  // rels of word/document.xml
  std::set<std::string> consumed_;           // Part-URIs claimed by this reader
  std::vector<int> open_bookmarks_;          // bookmark ids started but not yet closed
  Watermark* watermark_out_ = nullptr;       // 水印落到哪里（Read 时指向 doc）
};

void DocxReader::Warn(int code, const std::string& message) {
  if (warnings_ == nullptr) return;
  Warning w;
  w.code = code;
  w.module = "text";
  w.message = message;
  warnings_->Add(w);
}

void DocxReader::Read(Document& doc) {
  if (pkg_.kind() != PackageKind::Word) {
    throw Error(ErrorCode::Unsupported, "docx: package is not a Word family document");
  }
  Part* document = pkg_.main_part();
  if (document == nullptr) {
    throw Error(ErrorCode::Corrupt, "docx: officeDocument part missing");
  }
  consumed_.insert(document->uri());
  doc_rels_ = &document->relationships();

  // Styles, numbering, notes and comments live in fixed conventional places.
  auto find_part = [&](const char* suffix) -> Part* {
    for (auto& p : pkg_.parts()) {
      if (EndsWith(p->uri(), suffix)) return p.get();
    }
    return nullptr;
  };
  if (Part* styles = find_part("/styles.xml")) {
    consumed_.insert(styles->uri());
    ParseStyles(AsView(styles->content()), doc);
  }
  if (Part* numbering = find_part("/numbering.xml")) {
    consumed_.insert(numbering->uri());
    ParseNumbering(AsView(numbering->content()), doc);
  }
  if (Part* footnotes = find_part("/footnotes.xml")) {
    consumed_.insert(footnotes->uri());
    ParseNotes(footnotes->uri(), AsView(footnotes->content()), doc.footnotes);
  }
  if (Part* endnotes = find_part("/endnotes.xml")) {
    consumed_.insert(endnotes->uri());
    ParseNotes(endnotes->uri(), AsView(endnotes->content()), doc.endnotes);
  }
  if (Part* comments = find_part("/comments.xml")) {
    consumed_.insert(comments->uri());
    ParseComments(comments->uri(), AsView(comments->content()), doc);
  }
  // settings.xml 要留下来做"就地改写"：只解析保护相关的两个元素，原文整段
  // 存进 DOM，写回时再补丁式地替换，其余设置（兼容性开关、语言、修订参数）
  // 一个字节都不动。
  if (Part* settings = find_part("/settings.xml")) {
    consumed_.insert(settings->uri());
    doc.settings_uri = settings->uri();
    doc.settings_xml.assign(reinterpret_cast<const char*>(settings->content().data()),
                            settings->content().size());
    ParseSettings(AsView(settings->content()), doc);
  }
  if (cancel_ != nullptr) cancel_->ThrowIfCancelled();
  watermark_out_ = &doc.watermark;
  ParseBody(AsView(document->content()), doc);
  watermark_out_ = nullptr;

  // Everything not claimed above stays in the package and is written back
  // byte-identical on save (docs/02 §3.1.3 item 6).
  for (auto& p : pkg_.parts()) {
    if (consumed_.count(p->uri()) > 0) continue;
    UnknownPart up;
    up.uri = p->uri();
    up.mime = p->mime();
    up.bytes = p->content();
    doc.unknown_parts.push_back(std::move(up));
    Warn(static_cast<int>(WarningCode::UnknownPartKept), "docx: unknown part kept as-is: " + p->uri());
  }
}

// ---- styles / numbering -------------------------------------------------

void DocxReader::ParseStyles(std::string_view xml, Document& doc) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "styles") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "docDefaults") {
        doc.has_doc_defaults = true;
        WalkChildren(w, c, [&](const XmlEvent& d) {
          const std::string_view dn = LocalName(d.name);
          if (dn != "rPrDefault" && dn != "pPrDefault") return;
          WalkChildren(w, d, [&](const XmlEvent& e) {
            const std::string_view en = LocalName(e.name);
            if (en == "rPr") {
              ParseRunProps(w, e, doc.default_run);
            } else if (en == "pPr") {
              ParseParagraphProps(w, e, doc.default_paragraph, EmptyRels(), nullptr, nullptr);
            }
          });
        });
        return;
      }
      if (n != "style") return;
      Style s;
      const std::string_view type = Attr(c, "type");
      if (type == "character") {
        s.kind = Style::Kind::Character;
      } else if (type == "table") {
        s.kind = Style::Kind::Table;
      } else if (type == "numbering") {
        s.kind = Style::Kind::Numbering;
      } else {
        s.kind = Style::Kind::Paragraph;
      }
      s.id = std::string(Attr(c, "styleId"));
      WalkChildren(w, c, [&](const XmlEvent& f) {
        const std::string_view fn = LocalName(f.name);
        if (fn == "name") {
          s.name = std::string(Attr(f, "val"));
        } else if (fn == "basedOn") {
          s.based_on = std::string(Attr(f, "val"));
        } else if (fn == "next") {
          s.next = std::string(Attr(f, "val"));
        } else if (fn == "link") {
          s.link = std::string(Attr(f, "val"));
        } else if (fn == "uiPriority") {
          s.priority = ToInt(Attr(f, "val"), -1);
        } else if (fn == "qFormat") {
          s.quick_format = true;
        } else if (fn == "semiHidden") {
          s.semi_hidden = true;
        } else if (fn == "unhideWhenUsed") {
          s.unhide_when_used = true;
        } else if (fn == "hidden") {
          s.hidden = true;
        } else if (fn == "pPr") {
          ParseParagraphProps(w, f, s.paragraph, EmptyRels(), nullptr, nullptr);
        } else if (fn == "rPr") {
          ParseRunProps(w, f, s.run);
        }
      });
      doc.styles.push_back(std::move(s));
    });
  }
}

void DocxReader::ParseNumberingLevel(XmlWalker& w, const XmlEvent& start, NumberingLevel& out) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    if (n == "start") {
      out.start = ToInt(Attr(c, "val"), 1);
    } else if (n == "numFmt") {
      out.num_fmt = std::string(Attr(c, "val"));
    } else if (n == "lvlText") {
      out.lvl_text = std::string(Attr(c, "val"));
    } else if (n == "lvlJc") {
      out.alignment = JcFromName(Attr(c, "val"));
    } else if (n == "suff") {
      out.suffix = std::string(Attr(c, "val"));
    } else if (n == "pPr") {
      ParseParagraphProps(w, c, out.paragraph, EmptyRels(), nullptr, nullptr);
    } else if (n == "rPr") {
      RunFormat rf;
      ParseRunProps(w, c, rf);
      out.font_ascii = rf.font_ascii;
      out.font_eastasia = rf.font_eastasia;
      out.color = rf.color;
      out.run_size_half = rf.size_half;
    }
  });
}

void DocxReader::ParseNumbering(std::string_view xml, Document& doc) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "numbering") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "abstractNum") {
        AbstractNumbering a;
        a.id = std::string(Attr(c, "abstractNumId"));
        a.levels.resize(9);
        WalkChildren(w, c, [&](const XmlEvent& l) {
          if (LocalName(l.name) != "lvl") return;
          const int ilvl = ToInt(Attr(l, "ilvl"), -1);
          if (ilvl < 0 || ilvl >= 9) return;  // out-of-range level: skip subtree
          ParseNumberingLevel(w, l, a.levels[ilvl]);
        });
        doc.abstract_numberings.push_back(std::move(a));
      } else if (n == "num") {
        NumberingInstance inst;
        inst.num_id = std::string(Attr(c, "numId"));
        WalkChildren(w, c, [&](const XmlEvent& g) {
          const std::string_view gn = LocalName(g.name);
          if (gn == "abstractNumId") {
            inst.abstract_id = std::string(Attr(g, "val"));
          } else if (gn == "lvlOverride") {
            const int ilvl = ToInt(Attr(g, "ilvl"), -1);
            WalkChildren(w, g, [&](const XmlEvent& h) {
              if (LocalName(h.name) != "startOverride") return;
              if (ilvl < 0) return;
              if (static_cast<int>(inst.level_start_overrides.size()) <= ilvl) {
                inst.level_start_overrides.resize(ilvl + 1, -1);
              }
              inst.level_start_overrides[ilvl] = ToInt(Attr(h, "val"), -1);
            });
          }
        });
        if (inst.level_start_overrides.size() < 9) {
          inst.level_start_overrides.resize(9, -1);
        }
        doc.numbering.push_back(std::move(inst));
      }
      // w:numPicBullet (picture bullets) has no DOM slot in this slice.
    });
  }
}

// ---- settings：文档保护 --------------------------------------------------

namespace {

// CT_DocProtect / CT_WriteProtection 的保护字段是同构的，抽一个读法。
ProtectionHash ReadProtectionHash(const XmlEvent& ev) {
  ProtectionHash h;
  h.hash = std::string(AttrAny(ev, {"hash", "hashValue"}));
  h.salt = std::string(AttrAny(ev, {"salt", "saltValue"}));
  h.algorithm = std::string(Attr(ev, "algorithmName"));
  if (h.algorithm.empty()) h.algorithm = AlgorithmFromSid(ToInt(Attr(ev, "cryptAlgorithmSid"), 4));
  h.spin_count = ToInt(Attr(ev, "cryptSpinCount"), 0);
  h.hash_size = ToInt(Attr(ev, "hashSize"), 0);
  if (h.hash_size <= 0) h.hash_size = DigestBytesFor(h.algorithm);
  return h;
}

}  // namespace

void DocxReader::ParseSettings(std::string_view xml, Document& doc) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "settings") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "documentProtection") {
        DocumentProtection& p = doc.protection;
        p.enabled = BoolAttr(Attr(c, "enforcement"), false);
        p.formatting = BoolAttr(Attr(c, "formatting"), false);
        p.edit = EditFromName(Attr(c, "edit"));
        p.document_hash = ReadProtectionHash(c);
      } else if (n == "writeProtection") {
        DocumentProtection& p = doc.protection;
        p.write_protected = true;
        p.recommended = BoolAttr(Attr(c, "recommended"), false);
        p.write_hash = ReadProtectionHash(c);
      }
    });
    break;
  }
}

// ---- 水印（页眉里的 VML 形状）-------------------------------------------

bool DocxReader::ParseHeaderWatermark(std::string_view xml, const Relationships* rels,
                                      Watermark& out) {
  XmlWalker w(xml);
  XmlEvent ev;
  bool found = false;
  while (w.Next(ev) && !found) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "pict") continue;
    WalkChildren(w, ev, [&](const XmlEvent& shape) {
      if (found) return;
      const std::string_view shape_name = LocalName(shape.name);
      if (shape_name != "shape" && shape_name != "rect" && shape_name != "group") return;
      // 只认明确带水印标记的形状：页眉里还可能有普通图片或艺术字，不能误吞。
      if (!ContainsWatermarkMarker(Attr(shape, "id")) && Attr(shape, "Watermark").empty()) {
        return;
      }

      Watermark wm;
      const std::string style(Attr(shape, "style"));
      const std::string rot = CssValue(style, "rotation");
      if (!rot.empty()) {
        try {
          wm.rotation = std::stod(rot);
        } catch (...) {
        }
      }
      wm.box_width_pt = LengthPt(CssValue(style, "width"));
      wm.box_height_pt = LengthPt(CssValue(style, "height"));
      const int fill = ColorAttr(Attr(shape, "fillcolor"));
      if (fill >= 0) wm.color = fill;

      bool has_text = false;
      bool has_image = false;
      WalkChildren(w, shape, [&](const XmlEvent& part) {
        const std::string_view pn = LocalName(part.name);
        if (pn == "textpath") {
          wm.text = std::string(Attr(part, "string"));
          if (wm.text.empty()) wm.text = ElemText(w, part);
          has_text = true;
          const std::string ts(Attr(part, "style"));
          const std::string fam = CssValue(ts, "font-family");
          if (!fam.empty()) wm.font = fam;
          // Word 写的是 font-size:1pt + fitshape，真实字号由形状盒拉伸决定；
          // 只有当字号明显不是占位值时，才把它当作权威尺寸。
          const double fs = LengthPt(CssValue(ts, "font-size"));
          if (fs >= 4.0) wm.size_pt = static_cast<int>(fs + 0.5);
          const std::string weight = CssValue(ts, "font-weight");
          wm.bold = weight == "bold" || weight == "700";
          wm.italic = CssValue(ts, "font-style") == "italic";
        } else if (pn == "fill") {
          const std::string op = std::string(Attr(part, "opacity"));
          if (!op.empty()) {
            try {
              wm.semitransparent = std::stod(op) < 0.999;
            } catch (...) {
            }
          }
        } else if (pn == "imagedata") {
          has_image = true;
          const std::string_view rid = AttrAny(part, {"id", "embed"});
          if (rels != nullptr) {
            const Relationship* rel = rels->Find(rid);
            if (rel != nullptr && !rel->external()) wm.image_uri = rel->resolved;
          }
          // gain/blacklevel 是 Word 做"褪色"的两个参数。
          wm.washout = !Attr(part, "gain").empty() || !Attr(part, "blacklevel").empty();
        }
      });

      if (has_text) {
        wm.kind = Watermark::Kind::Text;
        if (wm.text.empty()) return;  // 空文本不是有效水印
        out = std::move(wm);
        found = true;
      } else if (has_image) {
        wm.kind = Watermark::Kind::Image;
        // 图片字节跟着 DOM 走，写回时不必再依赖 unknown_parts 的顺序。
        if (!wm.image_uri.empty()) {
          if (Part* img = pkg_.Find(wm.image_uri)) wm.image_bytes = img->content();
        }
        if (wm.image_bytes.empty()) return;
        out = std::move(wm);
        found = true;
      }
    });
  }
  return found;
}

// ---- document body ------------------------------------------------------

void DocxReader::ParseBody(std::string_view xml, Document& doc) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "document") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      if (LocalName(c.name) != "body") return;
      const Relationships& rels = *doc_rels_;
      Section current;
      WalkChildren(w, c, [&](const XmlEvent& b) {
        const std::string_view n = LocalName(b.name);
        if (n == "p" || n == "tbl") {
      Block block;
      bool closed = false;
      if (n == "p") {
        ParseParagraph(w, b, rels, block.paragraph, &current, &closed);
        block.type = Block::Type::Paragraph;
      } else {
        block.type = Block::Type::Table;
        block.table = std::make_shared<Table>();
        ParseTable(w, b, rels, *block.table);
      }
          current.blocks.push_back(std::move(block));
          if (closed) {
            doc.sections.push_back(std::move(current));
            current = Section();
          }
        } else if (n == "sectPr") {
          // Body-level sectPr describes the final section.
          ParseSectPr(w, b, current, rels);
          doc.sections.push_back(std::move(current));
          current = Section();
        } else if (n == "sdt") {
          // Block-level content control: its sdtContent holds real blocks.
          WalkChildren(w, b, [&](const XmlEvent& sdt_child) {
            if (LocalName(sdt_child.name) != "sdtContent") return;
            bool closed = false;
            ParseBlockContainer(w, sdt_child, rels, current.blocks, &current, &closed);
            if (closed) {
              doc.sections.push_back(std::move(current));
              current = Section();
            }
          });
        }
        // body-level bookmarks / proofErr have no DOM slot: skipped
      });
      if (!current.blocks.empty() || doc.sections.empty()) {
        doc.sections.push_back(std::move(current));
      }
    });
  }
}

// ---- blocks -------------------------------------------------------------

void DocxReader::ParseBlockContainer(XmlWalker& w, const XmlEvent& container,
                                     const Relationships& rels, std::vector<Block>& out,
                                     Section* closing_section, bool* sect_found) {
  WalkChildren(w, container, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    if (n == "p" || n == "tbl") {
      Block block;
      bool closed = false;
      if (n == "p") {
        ParseParagraph(w, c, rels, block.paragraph, closing_section, &closed);
        block.type = Block::Type::Paragraph;
      } else {
        block.type = Block::Type::Table;
        block.table = std::make_shared<Table>();
        ParseTable(w, c, rels, *block.table);
      }
      out.push_back(std::move(block));
      if (closed && sect_found != nullptr) *sect_found = true;
      return;
    }
    if (n == "sectPr" && closing_section != nullptr) {
      ParseSectPr(w, c, *closing_section, rels);
      if (sect_found != nullptr) *sect_found = true;
      return;
    }
    if (n == "sdt") {
      WalkChildren(w, c, [&](const XmlEvent& sdt_child) {
        if (LocalName(sdt_child.name) != "sdtContent") return;
        ParseBlockContainer(w, sdt_child, rels, out, closing_section, sect_found);
      });
    }
  });
}

void DocxReader::ParseParagraph(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                                Paragraph& out, Section* closing_section, bool* sect_found) {
  out.pstyle_name = out.format.style;
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    if (n == "pPr") {
      ParseParagraphProps(w, c, out.format, rels, closing_section, sect_found);
      out.pstyle_name = out.format.style;
      return;
    }
    if (n == "r") {
      // A plain run: parse its own content directly.
      ParseRunLike(w, c, rels, out.runs, 0, {}, {}, std::string(), std::string());
      return;
    }
    DispatchInlineChild(w, c, rels, out.runs, 0, {}, {}, std::string(), std::string());
  });
}

// Dispatches ONE inline element. Used both for the children of a paragraph
// (through ParseInlineChildren) and for elements that arrive individually
// (e.g. a stray <w:tab/> or <w:bookmarkStart/> directly under <w:p>).
void DocxReader::DispatchInlineChild(XmlWalker& w, const XmlEvent& c, const Relationships& rels,
                                     std::vector<InlineItem>& out, int revision,
                                     std::string_view author, std::string_view date,
                                     const std::string& anchor, const std::string& target) {
  {
    const std::string_view n = LocalName(c.name);
    // Range markers and stray parse noise carry no content.
    if (n == "proofErr" || n == "permStart" || n == "permEnd" || n == "commentRangeStart" ||
        n == "commentRangeEnd" || n == "lastRenderedPageBreak" || n == "moveFromRangeStart" ||
        n == "moveFromRangeEnd" || n == "moveToRangeStart" || n == "moveToRangeEnd" ||
        n == "customXmlDelRangeStart" || n == "customXmlDelRangeEnd" ||
        n == "customXmlInsRangeStart" || n == "customXmlInsRangeEnd" ||
        n == "customXmlMoveFromRangeStart" || n == "customXmlMoveFromRangeEnd" ||
        n == "customXmlMoveToRangeStart" || n == "customXmlMoveToRangeEnd") {
      return;
    }
    if (n == "r") {
      ParseRunLike(w, c, rels, out, revision, author, date, anchor, target);
      return;
    }
    if (n == "hyperlink") {
      const std::string h_anchor(Attr(c, "anchor"));
      std::string h_target;
      const std::string_view rid(Attr(c, "id"));
      if (!rid.empty()) {
        const Relationship* rel = rels.Find(rid);
        if (rel != nullptr) h_target = rel->external() ? rel->target : rel->resolved;
      }
      ParseInlineChildren(w, c, rels, out, revision, author, date, h_anchor, h_target);
      return;
    }
    if (n == "ins" || n == "del" || n == "moveFrom" || n == "moveTo") {
      const int rev = (n == "ins" || n == "moveTo") ? 1 : 2;
      ParseInlineChildren(w, c, rels, out, rev, Attr(c, "author"), Attr(c, "date"), anchor, target);
      return;
    }
    if (n == "sdt" || n == "sdtContent" || n == "smartTag" || n == "customXml") {
      ParseInlineChildren(w, c, rels, out, revision, author, date, anchor, target);
      return;
    }
    if (n == "fldSimple") {
      InlineItem it;
      it.type = InlineItem::Type::InstrText;
      it.text = std::string(Attr(c, "instr"));
      it.revision = revision;
      it.hyperlink_anchor = anchor;
      it.hyperlink_target = target;
      if (!it.text.empty()) out.push_back(std::move(it));
      ParseInlineChildren(w, c, rels, out, revision, author, date, anchor, target);
      return;
    }
    if (n == "bookmarkStart" || n == "bookmarkEnd") {
      // One item per bookmark: the start carries the name, the end just closes
      // it. An end without a matching open start is kept so it is not lost.
      const int bm_id = ToInt(Attr(c, "id"), -1);
      if (n == "bookmarkStart") {
        InlineItem it;
        it.type = InlineItem::Type::BookmarkStart;
        it.text = std::string(Attr(c, "name"));
        it.note_id = bm_id;
        open_bookmarks_.push_back(bm_id);
        out.push_back(std::move(it));
        return;
      }
      for (size_t i = 0; i < open_bookmarks_.size(); ++i) {
        if (open_bookmarks_[i] == bm_id) {
          open_bookmarks_.erase(open_bookmarks_.begin() + static_cast<long>(i));
          return;
        }
      }
      InlineItem it;
      it.type = InlineItem::Type::BookmarkEnd;
      it.note_id = bm_id;
      out.push_back(std::move(it));
      return;
    }
    // Run-level leaves that OOXML also allows as direct children of w:p,
    // w:hyperlink, w:ins / w:del ... (e.g. a stray <w:tab/> after a run).
    if (n == "t" || n == "delText" || n == "noBreakHyphen" || n == "softHyphen" || n == "tab" ||
        n == "ptab" || n == "br" || n == "cr" || n == "sym" || n == "separator" ||
        n == "continuationSeparator" || n == "footnoteReference" || n == "endnoteReference" ||
        n == "commentReference" || n == "fldChar" || n == "instrText" || n == "drawing" ||
        n == "pict" || n == "object") {
      ParseInlineLeaf(w, c, rels, out, RunFormat{}, revision, author, date, anchor, target);
      return;
    }
  }
}

// Walks the children of `start` (a paragraph, hyperlink, ins/del, sdt ...) and
// dispatches each of them.
void DocxReader::ParseInlineChildren(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                                     std::vector<InlineItem>& out, int revision,
                                     std::string_view author, std::string_view date,
                                     const std::string& anchor, const std::string& target) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    DispatchInlineChild(w, c, rels, out, revision, author, date, anchor, target);
  });
}

// One child of a run (or a run-level leaf that is a direct child of w:p /
// w:hyperlink / w:ins / ...). Shared by ParseRunLike and ParseInlineChildren so a
// stray <w:tab/> directly under <w:p> is not dropped.
void DocxReader::ParseInlineLeaf(XmlWalker& w, const XmlEvent& c, const Relationships& rels,
                               std::vector<InlineItem>& out, const RunFormat& fmt,
                               int revision, std::string_view author, std::string_view date,
                               const std::string& anchor, const std::string& target) {
const std::string_view n = LocalName(c.name);
auto push = [&](InlineItem it) {
    it.format = fmt;
    it.revision = revision;
    it.revision_author = std::string(author);
    it.revision_date = std::string(date);
    it.hyperlink_anchor = anchor;
    it.hyperlink_target = target;
    out.push_back(std::move(it));
  };
  if (n == "t" || n == "delText") {
    InlineItem it;
    it.type = InlineItem::Type::Text;
    it.text = ElemText(w, c);
    it.deleted_text = (n == "delText");
    if (!it.text.empty()) push(std::move(it));
    return;
  }
  if (n == "noBreakHyphen") {
    InlineItem it;
    it.type = InlineItem::Type::Text;
    it.text = "\xE2\x80\x91";  // U+2011
    push(std::move(it));
    return;
  }
  if (n == "softHyphen") {
    InlineItem it;
    it.type = InlineItem::Type::Text;
    it.text = "\xC2\xAD";  // U+00AD
    push(std::move(it));
    return;
  }
  if (n == "tab" || n == "ptab") {
    InlineItem it;
    it.type = InlineItem::Type::Tab;
    push(std::move(it));
    return;
  }
  if (n == "br" || n == "cr") {
    InlineItem it;
    it.type = InlineItem::Type::Break;
    const std::string_view kind = Attr(c, "type");
    if (kind == "page") {
      it.break_kind = 1;
    } else if (kind == "column") {
      it.break_kind = 2;
    } else if (kind == "textWrapping") {
      it.break_kind = 3;
    }
    push(std::move(it));
    return;
  }
  if (n == "sym") {
    InlineItem it;
    it.type = InlineItem::Type::Symbol;
    const std::string_view ch = Attr(c, "char");
    try {
      it.symbol = static_cast<char32_t>(std::stoul(std::string(ch), nullptr, 16));
    } catch (...) {
      it.symbol = 0xFFFD;
    }
    it.symbol_font = std::string(Attr(c, "font"));
    push(std::move(it));
    return;
  }
  if (n == "separator" || n == "continuationSeparator") {
    InlineItem it;
    it.type = InlineItem::Type::Separator;
    push(std::move(it));
    return;
  }
  if (n == "footnoteReference" || n == "endnoteReference" || n == "commentReference") {
    InlineItem it;
    it.type = n == "footnoteReference" ? InlineItem::Type::FootnoteRef
             : n == "endnoteReference" ? InlineItem::Type::EndnoteRef
                                       : InlineItem::Type::CommentRef;
    it.note_id = ToInt(Attr(c, "id"), 0);
    it.comment_id = it.note_id;
    push(std::move(it));
    return;
  }
  if (n == "bookmarkStart" || n == "bookmarkEnd") {
    InlineItem it;
    it.type = n == "bookmarkStart" ? InlineItem::Type::BookmarkStart : InlineItem::Type::BookmarkEnd;
    it.text = std::string(Attr(c, "name"));
    it.note_id = ToInt(Attr(c, "id"), 0);
    push(std::move(it));
    return;
  }
  if (n == "fldChar") {
    InlineItem it;
    it.type = InlineItem::Type::Field;
    const std::string_view kind = Attr(c, "fldCharType");
    it.field_kind = kind == "separate" ? 1 : (kind == "end" ? 2 : 0);
    push(std::move(it));
    return;
  }
  if (n == "instrText") {
    InlineItem it;
    it.type = InlineItem::Type::InstrText;
    it.text = ElemText(w, c);
    if (!it.text.empty()) push(std::move(it));
    return;
  }
  if (n == "drawing" || n == "pict" || n == "object") {
    InlineItem it;
    it.type = InlineItem::Type::Drawing;
    it.drawing_target = FindDrawingTarget(w, c, rels);
    push(std::move(it));
    return;
  }
}
void DocxReader::ParseRunLike(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                              std::vector<InlineItem>& out, int revision, std::string_view author,
                              std::string_view date, const std::string& anchor,
                              const std::string& target) {
  RunFormat fmt;
  WalkChildren(w, start, [&](const XmlEvent& c) {
    if (LocalName(c.name) == "rPr") {
      ParseRunProps(w, c, fmt);
      return;
    }
    ParseInlineLeaf(w, c, rels, out, fmt, revision, author, date, anchor, target);
  });
}

// ---- properties ---------------------------------------------------------

void DocxReader::ParseRunProps(XmlWalker& w, const XmlEvent& start, RunFormat& out) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    const std::string_view v = Attr(c, "val");
    if (n == "b" || n == "bCs") {
      out.bold = TriFrom(BoolAttr(v, true));
    } else if (n == "i" || n == "iCs") {
      out.italic = TriFrom(BoolAttr(v, true));
    } else if (n == "caps") {
      out.all_caps = TriFrom(BoolAttr(v, true));
    } else if (n == "smallCaps") {
      out.small_caps = TriFrom(BoolAttr(v, true));
    } else if (n == "strike") {
      out.strike = TriFrom(BoolAttr(v, true));
    } else if (n == "dstrike") {
      out.double_strike = TriFrom(BoolAttr(v, true));
    } else if (n == "emboss") {
      out.emboss = TriFrom(BoolAttr(v, true));
    } else if (n == "imprint") {
      out.imprint = TriFrom(BoolAttr(v, true));
    } else if (n == "vanish") {
      out.hidden = TriFrom(BoolAttr(v, true));
    } else if (n == "noProof") {
      out.no_proof = TriFrom(BoolAttr(v, true));
    } else if (n == "sz" || n == "szCs") {
      out.size_half = ToInt(v, -1);
    } else if (n == "color") {
      out.color = ColorAttr(v);
    } else if (n == "highlight") {
      out.highlight = ToInt(v, -1);
    } else if (n == "shd") {
      out.shading = ColorAttr(Attr(c, "fill"));
    } else if (n == "vertAlign") {
      out.vert_align = VertAlignFromName(v);
    } else if (n == "spacing") {
      out.spacing_twips = ToInt(v, 0);
    } else if (n == "kern") {
      out.kerning_half = ToInt(v, 0);
    } else if (n == "position") {
      out.position_half = ToInt(v, 0);
    } else if (n == "u") {
      out.underline = std::string(v);
    } else if (n == "rFonts") {
      out.font_ascii = std::string(Attr(c, "ascii"));
      out.font_hansi = std::string(Attr(c, "hAnsi"));
      out.font_eastasia = std::string(Attr(c, "eastAsia"));
      out.font_cs = std::string(Attr(c, "cs"));
    } else if (n == "rStyle") {
      out.character_style = std::string(v);
    } else if (n == "lang") {
      out.lang = std::string(v);
    } else if (n == "outlineLvl") {
      out.outline_level = ToInt(v, -1);
    }
  });
}

void DocxReader::ParseParagraphProps(XmlWalker& w, const XmlEvent& start, ParagraphFormat& out,
                                     const Relationships& rels, Section* closing_section,
                                     bool* sect_found) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    const std::string_view v = Attr(c, "val");
    if (n == "pStyle") {
      out.style = std::string(v);
    } else if (n == "keepNext") {
      out.keep_next = TriFrom(BoolAttr(v, true));
    } else if (n == "keepLines") {
      out.keep_lines = TriFrom(BoolAttr(v, true));
    } else if (n == "widowControl") {
      out.widow_control = TriFrom(BoolAttr(v, true));
    } else if (n == "suppressLineNumbers") {
      out.suppress_line_numbers = TriFrom(BoolAttr(v, true));
    } else if (n == "pageBreakBefore") {
      out.page_break_before = TriFrom(BoolAttr(v, true));
    } else if (n == "bidi") {
      out.rtl = BoolAttr(v, true);
    } else if (n == "numPr") {
      WalkChildren(w, c, [&](const XmlEvent& g) {
        const std::string_view gn = LocalName(g.name);
        if (gn == "ilvl") {
          out.num_level = ToInt(Attr(g, "val"), -1);
        } else if (gn == "numId") {
          out.num_id = std::string(Attr(g, "val"));
        }
      });
    } else if (n == "spacing") {
      out.before_twips = ToInt(Attr(c, "before"), -1);
      out.after_twips = ToInt(Attr(c, "after"), -1);
      out.line_twips = ToInt(Attr(c, "line"), -1);
      const std::string_view rule = Attr(c, "lineRule");
      if (rule == "atLeast") {
        out.line_rule = 1;
      } else if (rule == "exact") {
        out.line_rule = 2;
      } else if (rule == "auto") {
        out.line_rule = 0;
      }
      out.before_autospacing = ToInt(Attr(c, "beforeAutospacing"), -1);
      out.after_autospacing = ToInt(Attr(c, "afterAutospacing"), -1);
    } else if (n == "ind") {
      out.ind_left = ToInt(Attr(c, "left"), -1);
      out.ind_right = ToInt(Attr(c, "right"), -1);
      out.ind_first_line_chars = ToInt(Attr(c, "firstLineChars"), -1);
      const int first_line = ToInt(Attr(c, "firstLine"), -1);
      const int hanging = ToInt(Attr(c, "hanging"), -1);
      if (first_line >= 0) {
        out.ind_first_line = first_line;
      } else if (hanging >= 0) {
        out.ind_first_line = -hanging;
      }
    } else if (n == "contextualSpacing") {
      out.context_before = BoolAttr(v, true) ? 1 : 0;
    } else if (n == "jc") {
      out.alignment = JcFromName(v);
      if (out.alignment < 0) out.alignment = ToInt(v, -1);
    } else if (n == "outlineLvl") {
      out.outline_level = ToInt(v, -1);
    } else if (n == "shd") {
      out.shading = ColorAttr(Attr(c, "fill"));
    } else if (n == "pBdr") {
      WalkChildren(w, c, [&](const XmlEvent& g) {
        const std::string_view gn = LocalName(g.name);
        const int color = ColorAttr(Attr(g, "color"));
        if (gn == "top") {
          out.border_top = color;
        } else if (gn == "bottom") {
          out.border_bottom = color;
        } else if (gn == "left") {
          out.border_left = color;
        } else if (gn == "right") {
          out.border_right = color;
        }
      });
    } else if (n == "tabs") {
      out.tabs = 0;
      WalkChildren(w, c, [&](const XmlEvent& g) {
        if (LocalName(g.name) == "tab") ++out.tabs;
      });
    } else if (n == "framePr") {
      out.frame_props = 1;
    } else if (n == "snapToGrid") {
      out.snap_to_grid = ToInt(v, -1);
    } else if (n == "textDirection") {
      out.text_direction = ToInt(v, -1);
    } else if (n == "mirrorIndents") {
      out.mirror_indents = ToInt(v, -1);
    } else if (n == "wordWrap") {
      out.word_wrap = ToInt(v, -1);
    } else if (n == "overflowPunct") {
      out.overflow_punct = ToInt(v, -1);
    } else if (n == "kinsoku") {
      out.kinsoku = ToInt(v, -1);
    } else if (n == "sectPr" && closing_section != nullptr) {
      // A sectPr inside a paragraph closes the section the paragraph ends.
      ParseSectPr(w, c, *closing_section, rels);
      if (sect_found != nullptr) *sect_found = true;
    }
  });
}

// ---- tables -------------------------------------------------------------

void DocxReader::ParseTable(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                            Table& out) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    if (n == "tblPr") {
      WalkChildren(w, c, [&](const XmlEvent& g) {
        const std::string_view gn = LocalName(g.name);
        const std::string_view v = Attr(g, "val");
        if (gn == "tblW") {
          out.width_twips = ToInt(AttrAny(g, {"val", "w"}), -1);
        } else if (gn == "jc") {
          out.alignment = JcFromName(v);
        } else if (gn == "tblBorders") {
          out.borders = 1;
        } else if (gn == "tblLayout") {
          out.layout_fixed = (v == "fixed");
        } else if (gn == "tblInd") {
          out.indent_twips = ToInt(v, 0);
        } else if (gn == "tblStyle") {
          out.style = std::string(v);
        } else if (gn == "tblOverlap") {
          out.overlap = (v == "never");
        }
      });
      return;
    }
    if (n == "tblGrid") {
      WalkChildren(w, c, [&](const XmlEvent& g) {
        if (LocalName(g.name) == "gridCol") {
          out.grid_twips.push_back(ToInt(Attr(g, "w"), 0));
        }
      });
      return;
    }
    if (n != "tr") return;
    Row row;
    WalkChildren(w, c, [&](const XmlEvent& r) {
      const std::string_view rn = LocalName(r.name);
      if (rn == "trPr") {
        WalkChildren(w, r, [&](const XmlEvent& p) {
          const std::string_view pn = LocalName(p.name);
          const std::string_view v = Attr(p, "val");
          if (pn == "trHeight") {
            row.height_twips = ToInt(Attr(p, "val"), -1);
            const std::string_view rule = Attr(p, "hRule");
            row.height_rule = rule == "atLeast" ? 1 : (rule == "exact" ? 2 : 0);
          } else if (pn == "tblHeader") {
            row.header_row = BoolAttr(v, true);
          } else if (pn == "cantSplit") {
            row.cant_split = BoolAttr(v, true);
          }
        });
        return;
      }
      if (rn != "tc") return;
      Cell cell;
      WalkChildren(w, r, [&](const XmlEvent& tc) {
        const std::string_view tcn = LocalName(tc.name);
        if (tcn == "tcPr") {
          WalkChildren(w, tc, [&](const XmlEvent& k) {
            const std::string_view kn = LocalName(k.name);
            const std::string_view v = Attr(k, "val");
            if (kn == "tcW") {
              const std::string_view type = Attr(k, "type");
              cell.format.width_twips = (type == "auto" || type == "pct")
                                            ? -1
                                            : ToInt(AttrAny(k, {"val", "w"}), -1);
            } else if (kn == "vMerge") {
              const std::string_view mv = Attr(k, "val");
              cell.format.vmerge = (mv == "continue") ? 1 : (mv == "restart" ? 2 : 1);
            } else if (kn == "gridSpan") {
              cell.format.grid_span = ToInt(v, 1);
            } else if (kn == "shd") {
              cell.format.shading = ColorAttr(Attr(k, "fill"));
            } else if (kn == "vAlign") {
              cell.format.valign = (v == "center") ? 1 : (v == "bottom" ? 2 : 0);
            } else if (kn == "tcBorders") {
              cell.format.borders = 1;
            } else if (kn == "noWrap") {
              cell.format.no_wrap = BoolAttr(v, true);
            } else if (kn == "hideMark") {
              cell.format.hide_mark = BoolAttr(v, true);
            } else if (kn == "tcStyle") {
              cell.format.style = std::string(v);
            }
          });
          return;
        }
        if (tcn == "p" || tcn == "tbl" || tcn == "sdt") {
          if (tcn == "p") {
            Block block;
            ParseParagraph(w, tc, rels, block.paragraph, nullptr, nullptr);
            block.type = Block::Type::Paragraph;
            cell.blocks.push_back(std::move(block));
          } else if (tcn == "tbl") {
            Block block;
            block.type = Block::Type::Table;
            block.table = std::make_shared<Table>();
            ParseTable(w, tc, rels, *block.table);
            cell.blocks.push_back(std::move(block));
          } else {
            // Block-level content control: its sdtContent holds the real blocks.
            WalkChildren(w, tc, [&](const XmlEvent& sdt_child) {
              if (LocalName(sdt_child.name) != "sdtContent") return;
              ParseBlockContainer(w, sdt_child, rels, cell.blocks, nullptr, nullptr);
            });
          }
        }
      });
      row.cells.push_back(std::move(cell));
    });
    out.rows.push_back(std::move(row));
  });

  // A missing tblGrid is rebuilt from cell widths (docs/02 §3.3.3).
  if (!out.grid_twips.empty()) return;
  if (out.rows.empty() || out.rows.front().cells.empty()) return;
  int cursor = 0;
  for (const auto& cell : out.rows.front().cells) {
    const int span = cell.format.grid_span > 0 ? cell.format.grid_span : 1;
    const int width = cell.format.width_twips > 0 ? cell.format.width_twips : -1;
    for (int i = 0; i < span; ++i) {
      if (width < 0) {
        Warn(static_cast<int>(WarningCode::ColumnWidthApproximated),
             "docx: table grid rebuilt without cell widths, columns will be equal");
        const int available = out.width_twips > 0 ? out.width_twips : 9026;
        const int cols = static_cast<int>(out.rows.front().cells.size());
        for (int c = 0; c < cols; ++c) out.grid_twips.push_back(available / cols);
        return;
      }
      if (i == 0) cursor = width;
      out.grid_twips.push_back(width / span);
    }
  }
}

// ---- section properties, headers and footers ----------------------------

void DocxReader::ParseHeaderFooterPart(const std::string& part_uri, std::string_view xml,
                                       HeaderFooter& out) {
  Part* part = pkg_.Find(part_uri);
  const Relationships* rels = part != nullptr ? &part->relationships() : nullptr;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view n = LocalName(ev.name);
    if (n != "hdr" && n != "ftr") continue;
    if (rels != nullptr) {
      ParseBlockContainer(w, ev, *rels, out.blocks, nullptr, nullptr);
    } else {
      WalkChildren(w, ev, [](const XmlEvent&) {});
    }
  }
}

void DocxReader::ParseSectPr(XmlWalker& w, const XmlEvent& start, Section& out,
                             const Relationships& rels) {
  WalkChildren(w, start, [&](const XmlEvent& c) {
    const std::string_view n = LocalName(c.name);
    if (n == "pgSz") {
      out.page.width_twips = ToInt(Attr(c, "w"), out.page.width_twips);
      out.page.height_twips = ToInt(Attr(c, "h"), out.page.height_twips);
      out.page.orientation = Attr(c, "orient") == "landscape" ? 1 : 0;
    } else if (n == "pgMar") {
      out.page.margin_top = ToInt(Attr(c, "top"), out.page.margin_top);
      out.page.margin_bottom = ToInt(Attr(c, "bottom"), out.page.margin_bottom);
      out.page.margin_left = ToInt(Attr(c, "left"), out.page.margin_left);
      out.page.margin_right = ToInt(Attr(c, "right"), out.page.margin_right);
      out.page.header_dist = ToInt(Attr(c, "header"), out.page.header_dist);
      out.page.footer_dist = ToInt(Attr(c, "footer"), out.page.footer_dist);
      out.page.gutter = ToInt(Attr(c, "gutter"), out.page.gutter);
    } else if (n == "cols") {
      out.column_count = ToInt(Attr(c, "num"), 1);
      out.column_space = ToInt(Attr(c, "space"), 720);
      WalkChildren(w, c, [&](const XmlEvent& g) {
        if (LocalName(g.name) != "col") return;
        ColumnDef col;
        col.width_twips = ToInt(Attr(g, "w"), 0);
        col.space_twips = ToInt(Attr(g, "space"), out.column_space);
        out.columns.push_back(col);
      });
    } else if (n == "pgNumType") {
      out.page_number_start = ToInt(Attr(c, "start"), -1);
      out.page_number_fmt = ToInt(Attr(c, "fmt"), 0);
    } else if (n == "titlePg") {
      out.title_pg = BoolAttr(Attr(c, "val"), true);
    } else if (n == "headerReference" || n == "footerReference") {
      const std::string_view rid(Attr(c, "id"));
      HeaderFooter::Kind kind = HeaderFooter::Kind::Default;
      const std::string_view type = Attr(c, "type");
      if (type == "first") {
        kind = HeaderFooter::Kind::First;
      } else if (type == "even") {
        kind = HeaderFooter::Kind::Even;
      }
      const Relationship* rel = rels.Find(rid);
      if (rel == nullptr || rel->external()) {
        Warn(static_cast<int>(WarningCode::HeaderFooterUnsplit),
             "docx: header/footer reference unresolvable: " + std::string(rid));
        return;
      }
      Part* part = pkg_.Find(rel->resolved);
      if (part == nullptr) {
        Warn(static_cast<int>(WarningCode::HeaderFooterUnsplit),
             "docx: header/footer part missing: " + rel->resolved);
        return;
      }
      consumed_.insert(part->uri());
      HeaderFooter hf;
      hf.kind = kind;
      hf.part_uri = part->uri();
      ParseHeaderFooterPart(part->uri(), AsView(part->content()), hf);
      // 水印藏在页眉里：认出来之后要把它从页眉排版流里摘掉，否则会被当成
      // 一张普通图片排进页眉（而且水印是整页装饰，不该参与页眉排版）。
      if (n == "headerReference" && watermark_out_ != nullptr && !watermark_out_->present()) {
        if (ParseHeaderWatermark(AsView(part->content()), &part->relationships(),
                                 *watermark_out_)) {
          std::vector<Block> kept;
          kept.reserve(hf.blocks.size());
          for (Block& b : hf.blocks) {
            if (b.type != Block::Type::Paragraph) {
              kept.push_back(std::move(b));
              continue;
            }
            bool has_text = false;
            bool has_drawing = false;
            for (const InlineItem& it : b.paragraph.runs) {
              if (it.type == InlineItem::Type::Text && !it.text.empty()) has_text = true;
              if (it.type == InlineItem::Type::Tab || it.type == InlineItem::Type::Break) {
                has_text = true;
              }
              if (it.type == InlineItem::Type::Drawing) has_drawing = true;
            }
            if (has_drawing && !has_text) continue;  // 纯图形段落 = 水印载体
            kept.push_back(std::move(b));
          }
          hf.blocks.swap(kept);
        }
      }
      if (n == "headerReference") {
        out.headers.push_back(std::move(hf));
      } else {
        out.footers.push_back(std::move(hf));
      }
    }
    // pgBorders / formProt / vAlign / docGrid / printerSettings: no DOM slot
  });

  // Column count without explicit columns: derive equal columns from the text
  // area (docs/02 §3.3.3 warns instead of failing).
  if (out.column_count > 1 && out.columns.empty()) {
    const int available =
        out.page.width_twips - out.page.margin_left - out.page.margin_right;
    const int usable = available - out.column_space * (out.column_count - 1);
    const int width = usable > 0 ? usable / out.column_count : 0;
    for (int i = 0; i < out.column_count; ++i) {
      ColumnDef col;
      col.width_twips = width;
      col.space_twips = out.column_space;
      out.columns.push_back(col);
    }
  }
}

// ---- notes and comments -------------------------------------------------

void DocxReader::ParseNotes(const std::string& part_uri, std::string_view xml,
                            std::vector<Note>& out) {
  Part* part = pkg_.Find(part_uri);
  const Relationships* rels = part != nullptr ? &part->relationships() : nullptr;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view root = LocalName(ev.name);
    if (root != "footnotes" && root != "endnotes") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n != "footnote" && n != "endnote") return;
      const std::string_view type = Attr(c, "type");
      if (type == "separator" || type == "continuationSeparator") return;  // rules only
      Note note;
      note.id = ToInt(Attr(c, "id"), -1);
      if (rels != nullptr) {
        ParseBlockContainer(w, c, *rels, note.blocks, nullptr, nullptr);
      } else {
        WalkChildren(w, c, [](const XmlEvent&) {});
      }
      out.push_back(std::move(note));
    });
  }
}

void DocxReader::ParseComments(const std::string& part_uri, std::string_view xml, Document& doc) {
  Part* part = pkg_.Find(part_uri);
  const Relationships* rels = part != nullptr ? &part->relationships() : nullptr;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "comments") continue;
    WalkChildren(w, ev, [&](const XmlEvent& c) {
      if (LocalName(c.name) != "comment") return;
      Comment comment;
      comment.id = ToInt(Attr(c, "id"), -1);
      comment.author = std::string(Attr(c, "author"));
      comment.initials = std::string(Attr(c, "initials"));
      comment.date = std::string(Attr(c, "date"));
      if (rels != nullptr) {
        ParseBlockContainer(w, c, *rels, comment.blocks, nullptr, nullptr);
      } else {
        WalkChildren(w, c, [](const XmlEvent&) {});
      }
      doc.comments.push_back(std::move(comment));
    });
  }
}

}  // namespace

// ---- public entry points ------------------------------------------------

Document Open(const OpenOptions& opts) {
  FileStream in(opts.path, "rb");
  const std::vector<uint8_t> bytes = ReadAll(in, 256 << 20);
  return OpenMemory(AsView(bytes), opts.warnings, opts.cancel);
}

Document OpenMemory(std::string_view bytes, WarningSink* warnings, CancelToken* cancel) {
  std::vector<uint8_t> data(bytes.begin(), bytes.end());
  MemoryStream mem(std::move(data));
  auto pkg = Package::Open(mem, warnings);
  Document doc;
  DocxReader reader(*pkg, warnings, cancel);
  reader.Read(doc);
  return doc;
}

}  // namespace text
}  // namespace doccore
