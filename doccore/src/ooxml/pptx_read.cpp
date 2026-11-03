// DocCore - PPTX -> deck DOM reader. Design refs: docs/02 §3.7
//
// 部件树：
//   ppt/presentation.xml      幻灯片顺序、尺寸、母版列表
//   ppt/slides/slideN.xml     每页的 spTree（sp / pic / graphicFrame）
//   ppt/theme/themeN.xml      主题字体（默认字体来源）
//   ppt/notesSlides/*.xml     备注
//
// 坐标一律 EMU。母版与版式只用来取默认字体与背景，不做任意母版编辑。
#include "doccore/pptx.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "doccore/opc.h"
#include "xml_walk.h"
#include "pptx_timing.h"

namespace doccore {
namespace deck {

namespace {

using doccore::ooxml::AsUtf8;
using doccore::ooxml::Attr;
using doccore::ooxml::AttrBool;
using doccore::ooxml::AttrDouble;
using doccore::ooxml::AttrInt;
using doccore::ooxml::AttrInt64;
using doccore::ooxml::AttrOr;
using doccore::ooxml::ElemText;
using doccore::ooxml::LocalName;
using doccore::ooxml::NameIs;
using doccore::ooxml::ParseRgb;
using doccore::ooxml::WalkChildren;
using doccore::ooxml::XmlWalker;

// ---- 文本 ---------------------------------------------------------------

// a:rPr / a:defRPr：字号单位是 1/100 点。
RunFormat ReadRunFormat(XmlWalker& w, const XmlEvent& start) {
  RunFormat format;
  const int size = AttrInt(start, "sz", 0);
  if (size > 0) format.size_pt = size / 100.0;
  format.bold = AttrBool(start, "b", false);
  format.italic = AttrBool(start, "i", false);
  const std::string underline = AttrOr(start, "u", "none");
  format.underline = !underline.empty() && underline != "none";
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "solidFill") {
      WalkChildren(w, child, [&](const XmlEvent& color) {
        if (!NameIs(color, "srgbClr")) return;
        const int rgb = ParseRgb(Attr(color, "val"));
        if (rgb >= 0) format.color = rgb;
      });
    } else if (name == "latin") {
      format.family = AttrOr(child, "typeface", std::string());
    }
  });
  return format;
}

struct ParagraphProps {
  int align = -1;
  int level = 0;
  double space_before_pt = 0.0;
  double space_after_pt = 0.0;
  double line_spacing = 0.0;
  bool bullet = false;
};

void ReadParagraphProps(XmlWalker& w, const XmlEvent& start, ParagraphProps* props) {
  const std::string align = AttrOr(start, "algn", "");
  if (align == "l") props->align = 0;
  else if (align == "ctr") props->align = 1;
  else if (align == "r") props->align = 2;
  else if (align == "just" || align == "dist") props->align = 3;
  props->level = AttrInt(start, "lvl", 0);

  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "buChar" || name == "buAutoNum") {
      props->bullet = true;
    } else if (name == "buNone") {
      props->bullet = false;
    } else if (name == "spcBef" || name == "spcAft") {
      WalkChildren(w, child, [&](const XmlEvent& amount) {
        if (!NameIs(amount, "spcPts")) return;
        const double pt = AttrInt(amount, "val", 0) / 100.0;
        if (name == "spcBef") props->space_before_pt = pt;
        else props->space_after_pt = pt;
      });
    } else if (name == "lnSpc") {
      WalkChildren(w, child, [&](const XmlEvent& amount) {
        if (NameIs(amount, "spcPct")) props->line_spacing = AttrInt(amount, "val", 100000) / 100000.0;
      });
    }
  });
}

Paragraph ReadParagraph(XmlWalker& w, const XmlEvent& start) {
  Paragraph paragraph;
  ParagraphProps props;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "pPr") {
      ReadParagraphProps(w, child, &props);
      return;
    }
    if (name == "r" || name == "fld") {
      Run run;
      WalkChildren(w, child, [&](const XmlEvent& part) {
        const std::string_view part_name = LocalName(part.name);
        if (part_name == "rPr") {
          run.format = ReadRunFormat(w, part);
        } else if (part_name == "t") {
          run.text = ElemText(w, part);
        }
      });
      if (!run.text.empty()) paragraph.runs.push_back(std::move(run));
      return;
    }
    if (name == "br") {
      Run run;
      run.text = "\n";
      paragraph.runs.push_back(std::move(run));
    }
  });
  paragraph.align = props.align;
  paragraph.level = props.level;
  paragraph.space_before_pt = props.space_before_pt;
  paragraph.space_after_pt = props.space_after_pt;
  paragraph.line_spacing = props.line_spacing;
  paragraph.bullet = props.bullet;
  return paragraph;
}

TextBody ReadTextBody(XmlWalker& w, const XmlEvent& start) {
  TextBody body;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "bodyPr") {
      if (const std::string_view* anchor = Attr(child, "anchor")) {
        body.anchor = *anchor == "ctr" ? 1 : (*anchor == "b" ? 2 : 0);
      }
      body.wrap = AttrOr(child, "wrap", "square") != "none";
      body.inset_left = AttrInt64(child, "lIns", body.inset_left);
      body.inset_top = AttrInt64(child, "tIns", body.inset_top);
      body.inset_right = AttrInt64(child, "rIns", body.inset_right);
      body.inset_bottom = AttrInt64(child, "bIns", body.inset_bottom);
      ElemText(w, child);
      return;
    }
    if (name == "p") {
      body.paragraphs.push_back(ReadParagraph(w, child));
      return;
    }
    if (name == "lstStyle") ElemText(w, child);
  });
  return body;
}

// ---- 形状 ---------------------------------------------------------------

ShapeGeometry ReadTransform(XmlWalker& w, const XmlEvent& start) {
  ShapeGeometry geom;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "off") {
      geom.x = AttrInt64(child, "x", 0);
      geom.y = AttrInt64(child, "y", 0);
    } else if (name == "ext") {
      geom.cx = AttrInt64(child, "cx", 0);
      geom.cy = AttrInt64(child, "cy", 0);
    }
  });
  return geom;
}

// p:spPr / p:grpSpPr / a:xfrm 下的几何与填充。
void ReadShapeProperties(XmlWalker& w, const XmlEvent& start, Shape* shape) {
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "xfrm") {
      const ShapeGeometry geom = ReadTransform(w, child);
      if (geom.cx != 0 || geom.cy != 0) shape->geom = geom;
      const int rotation = AttrInt(child, "rot", 0);
      if (rotation != 0) shape->rotation = rotation;
    } else if (name == "solidFill") {
      WalkChildren(w, child, [&](const XmlEvent& color) {
        if (!NameIs(color, "srgbClr")) return;
        const int rgb = ParseRgb(Attr(color, "val"));
        if (rgb >= 0) shape->fill = rgb;
      });
    } else if (name == "ln") {
      const int width = AttrInt(child, "w", 0);
      if (width > 0) shape->line_width_pt = static_cast<double>(width) / kEmuPerPoint;
      WalkChildren(w, child, [&](const XmlEvent& part) {
        if (LocalName(part.name) != "solidFill") return;
        WalkChildren(w, part, [&](const XmlEvent& color) {
          if (!NameIs(color, "srgbClr")) return;
          const int rgb = ParseRgb(Attr(color, "val"));
          if (rgb >= 0) shape->line_color = rgb;
        });
      });
    } else if (name == "noFill") {
      shape->fill = -1;
    }
  });
}

Shape ReadShape(XmlWalker& w, const XmlEvent& start) {
  Shape shape;
  shape.kind = ShapeKind::Text;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "nvSpPr") {
      WalkChildren(w, child, [&](const XmlEvent& part) {
        const std::string_view part_name = LocalName(part.name);
        if (part_name == "cNvPr") {
          shape.name = AttrOr(part, "name", shape.name);
          shape.id = AttrInt(part, "id", shape.id);
        } else if (part_name == "nvPr") {
          WalkChildren(w, part, [&](const XmlEvent& ph) {
            if (!NameIs(ph, "ph")) return;
            shape.placeholder = AttrOr(ph, "type", "body");
            shape.placeholder_idx = AttrInt(ph, "idx", -1);
          });
        }
      });
    } else if (name == "spPr") {
      ReadShapeProperties(w, child, &shape);
    } else if (name == "txBody") {
      shape.text = ReadTextBody(w, child);
    }
  });
  return shape;
}

Shape ReadPicture(XmlWalker& w, const XmlEvent& start, const Relationships& rels,
                  Package* pkg) {
  Shape shape;
  shape.kind = ShapeKind::Picture;
  std::string embed;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "nvPicPr") {
      WalkChildren(w, child, [&](const XmlEvent& part) {
        if (LocalName(part.name) == "cNvPr") {
          shape.name = AttrOr(part, "name", shape.name);
          shape.id = AttrInt(part, "id", shape.id);
        }
      });
    } else if (name == "blipFill") {
      WalkChildren(w, child, [&](const XmlEvent& part) {
        if (!NameIs(part, "blip")) return;
        const std::string_view* id = Attr(part, "r:embed");
        if (id == nullptr) id = Attr(part, "embed");
        if (id != nullptr) embed = std::string(*id);
      });
    } else if (name == "spPr") {
      ReadShapeProperties(w, child, &shape);
    }
  });
  if (!embed.empty()) {
    const Relationship* rel = rels.Find(embed);
    if (rel != nullptr && pkg != nullptr) {
      shape.picture.part_uri = rel->resolved;
      if (Part* part = pkg->Find(rel->resolved)) {
        shape.picture.mime = part->mime();
        shape.picture.bytes = part->content();
        int width = 0;
        int height = 0;
        if (ImagePixelSize(shape.picture.bytes, &width, &height)) {
          shape.picture.pixel_width = width;
          shape.picture.pixel_height = height;
        }
      }
    }
  }
  return shape;
}

TableCell ReadTableCell(XmlWalker& w, const XmlEvent& start) {
  TableCell cell;
  const int row_span = AttrInt(start, "rowSpan", 1);
  const int col_span = AttrInt(start, "gridSpan", 1);
  cell.row_span = std::max(1, row_span);
  cell.col_span = std::max(1, col_span);
  cell.merged = AttrBool(start, "hMerge", false) || AttrBool(start, "vMerge", false);
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "txBody") {
      cell.body = ReadTextBody(w, child);
    } else if (name == "tcPr") {
      WalkChildren(w, child, [&](const XmlEvent& part) {
        if (LocalName(part.name) != "solidFill") return;
        WalkChildren(w, part, [&](const XmlEvent& color) {
          if (!NameIs(color, "srgbClr")) return;
          const int rgb = ParseRgb(Attr(color, "val"));
          if (rgb >= 0) cell.fill = rgb;
        });
      });
    }
  });
  return cell;
}

Shape ReadTableFrame(XmlWalker& w, const XmlEvent& start) {
  Shape shape;
  shape.kind = ShapeKind::Table;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "nvGraphicFramePr") {
      WalkChildren(w, child, [&](const XmlEvent& part) {
        if (LocalName(part.name) == "cNvPr") {
          shape.name = AttrOr(part, "name", shape.name);
          shape.id = AttrInt(part, "id", shape.id);
        }
      });
      return;
    }
    if (name == "xfrm") {
      shape.geom = ReadTransform(w, child);
      return;
    }
    if (name != "graphic") return;
    WalkChildren(w, child, [&](const XmlEvent& data) {
      if (LocalName(data.name) != "graphicData") return;
      WalkChildren(w, data, [&](const XmlEvent& tbl) {
        if (!NameIs(tbl, "tbl")) return;
        WalkChildren(w, tbl, [&](const XmlEvent& part) {
          const std::string_view part_name = LocalName(part.name);
          if (part_name == "tblGrid") {
            WalkChildren(w, part, [&](const XmlEvent& col) {
              if (NameIs(col, "gridCol")) {
                shape.table.col_widths.push_back(AttrInt64(col, "w", 0));
              }
            });
          } else if (part_name == "tr") {
            shape.table.row_heights.push_back(AttrInt64(part, "h", 0));
            std::vector<TableCell> row;
            WalkChildren(w, part, [&](const XmlEvent& cell) {
              if (NameIs(cell, "tc")) row.push_back(ReadTableCell(w, cell));
            });
            shape.table.rows.push_back(std::move(row));
          } else if (part_name == "tblPr") {
            WalkChildren(w, part, [&](const XmlEvent& flag) {
              if (NameIs(flag, "tableStyleId")) ElemText(w, flag);
            });
            shape.table.first_row_header = AttrBool(part, "firstRow", true);
            shape.table.banded_rows = AttrBool(part, "bandRow", true);
          }
        });
      });
    });
  });
  return shape;
}

void ReadShapeTree(XmlWalker& w, const XmlEvent& start, Slide* slide, const Relationships& rels,
                   Package* pkg) {
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "sp") {
      slide->shapes().push_back(ReadShape(w, child));
    } else if (name == "pic") {
      slide->shapes().push_back(ReadPicture(w, child, rels, pkg));
    } else if (name == "graphicFrame") {
      slide->shapes().push_back(ReadTableFrame(w, child));
    } else if (name == "grpSp") {
      // 组形状：P0 按平铺处理（组内坐标不做变换），保证内容不丢。
      ReadShapeTree(w, child, slide, rels, pkg);
    }
  });
}

void ReadSlideXml(const std::string& xml, Slide* slide, const Relationships& rels, Package* pkg,
                  std::vector<std::string>* unmapped) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "bg") {
      WalkChildren(w, ev, [&](const XmlEvent& bg) {
        WalkChildren(w, bg, [&](const XmlEvent& fill) {
          if (!NameIs(fill, "solidFill")) return;
          WalkChildren(w, fill, [&](const XmlEvent& color) {
            if (!NameIs(color, "srgbClr")) return;
            const int rgb = ParseRgb(Attr(color, "val"));
            if (rgb >= 0) slide->set_background(rgb);
          });
        });
      });
    } else if (name == "spTree") {
      ReadShapeTree(w, ev, slide, rels, pkg);
    }
  }
  // 切换与动画单独走一遍：它们都在根下，且 <p:timing> 要抠原始字节，
  // 顺手在上面那趟里做会把游标搅乱。
  ooxml::ReadTransitionAndTiming(xml, slide, unmapped);
}

// ---- 备注 ---------------------------------------------------------------

std::string ReadNotesText(const std::string& xml) {
  XmlWalker w(xml);
  XmlEvent ev;
  std::string out;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (!NameIs(ev, "sp")) continue;
    // 只取 body 占位符（备注正文），跳过幻灯片缩略图占位符。
    bool is_body = false;
    WalkChildren(w, ev, [&](const XmlEvent& child) {
      const std::string_view name = LocalName(child.name);
      if (name == "nvSpPr") {
        WalkChildren(w, child, [&](const XmlEvent& part) {
          if (!NameIs(part, "nvPr")) return;
          WalkChildren(w, part, [&](const XmlEvent& ph) {
            if (!NameIs(ph, "ph")) return;
            if (AttrOr(ph, "type", "body") == "body") is_body = true;
          });
        });
      } else if (name == "txBody") {
        TextBody body = ReadTextBody(w, child);
        if (is_body) {
          if (!out.empty()) out += '\n';
          out += body.PlainText();
        }
      }
    });
  }
  return out;
}

// ---- 主题 ---------------------------------------------------------------

std::string ReadThemeFont(const std::string& xml) {
  XmlWalker w(xml);
  XmlEvent ev;
  std::string minor;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (!NameIs(ev, "minorFont")) continue;
    WalkChildren(w, ev, [&](const XmlEvent& child) {
      if (!NameIs(child, "latin") || !minor.empty()) return;
      minor = AttrOr(child, "typeface", std::string());
    });
  }
  return minor;
}

// ---- 演示文稿 -----------------------------------------------------------

struct SlideRef {
  std::string rid;
  std::string part;
};

struct ParsedPresentation {
  std::vector<SlideRef> slides;
  int64_t width = 12192000;
  int64_t height = 6858000;
  std::string theme_part;
};

ParsedPresentation ReadPresentationXml(const std::string& xml,
                                       const std::map<std::string, std::string>& rid_target) {
  ParsedPresentation out;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "sldSz") {
      out.width = AttrInt64(ev, "cx", out.width);
      out.height = AttrInt64(ev, "cy", out.height);
    } else if (name == "sldIdLst") {
      WalkChildren(w, ev, [&](const XmlEvent& id) {
        if (!NameIs(id, "sldId")) return;
        std::string rid;
        for (const XmlAttribute& a : id.attributes) {
          if (LocalName(a.name) == "id") rid = std::string(a.value);
        }
        auto it = rid_target.find(rid);
        if (it != rid_target.end()) out.slides.push_back({rid, it->second});
      });
    }
  }
  return out;
}

// ---- 占位符几何继承 -----------------------------------------------------

// 版式/母版里的占位符几何。幻灯片上的占位符经常不写 xfrm，靠 idx/type 继承，
// 不补这一层的话导出 PDF 会把标题和正文全挤到 (0,0)。
struct PlaceholderGeom {
  ShapeGeometry geom;
  int anchor = -1;  // a:bodyPr/@anchor，-1 表示没写

  bool valid() const noexcept { return geom.cx > 0 || geom.cy > 0; }
};

struct PlaceholderTable {
  std::map<int, PlaceholderGeom> by_idx;
  std::map<std::string, PlaceholderGeom> by_type;

  // 先按 idx 再按 type 找；找不到返回 nullptr。
  const PlaceholderGeom* Find(const Shape& shape) const {
    if (shape.placeholder_idx >= 0) {
      auto it = by_idx.find(shape.placeholder_idx);
      if (it != by_idx.end()) return &it->second;
    }
    auto it = by_type.find(shape.placeholder);
    if (it != by_type.end()) return &it->second;
    // 母版只有 title/body 两种：版式里的 ctrTitle/subTitle/obj 按别名回退。
    const char* alias = nullptr;
    if (shape.placeholder == "ctrTitle") alias = "title";
    else if (shape.placeholder == "subTitle" || shape.placeholder == "obj") alias = "body";
    if (alias == nullptr) return nullptr;
    auto fallback = by_type.find(alias);
    return fallback == by_type.end() ? nullptr : &fallback->second;
  }
};

void ReadPlaceholderGeom(const std::string& xml, PlaceholderTable* out) {
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (LocalName(ev.name) != "sp") continue;
    std::string type;
    int idx = -1;
    PlaceholderGeom entry;
    WalkChildren(w, ev, [&](const XmlEvent& child) {
      const std::string_view name = LocalName(child.name);
      if (name == "nvSpPr") {
        WalkChildren(w, child, [&](const XmlEvent& part) {
          if (!NameIs(part, "nvPr")) return;
          WalkChildren(w, part, [&](const XmlEvent& ph) {
            if (!NameIs(ph, "ph")) return;
            type = AttrOr(ph, "type", "body");
            idx = AttrInt(ph, "idx", -1);
          });
        });
      } else if (name == "spPr") {
        WalkChildren(w, child, [&](const XmlEvent& part) {
          if (LocalName(part.name) != "xfrm") return;
          WalkChildren(w, part, [&](const XmlEvent& xf) {
            const std::string_view xf_name = LocalName(xf.name);
            if (xf_name == "off") {
              entry.geom.x = AttrInt64(xf, "x", 0);
              entry.geom.y = AttrInt64(xf, "y", 0);
            } else if (xf_name == "ext") {
              entry.geom.cx = AttrInt64(xf, "cx", 0);
              entry.geom.cy = AttrInt64(xf, "cy", 0);
            }
          });
        });
      } else if (name == "txBody") {
        WalkChildren(w, child, [&](const XmlEvent& part) {
          if (LocalName(part.name) != "bodyPr") return;
          if (const std::string_view* anchor = Attr(part, "anchor")) {
            entry.anchor = *anchor == "ctr" ? 1 : (*anchor == "b" ? 2 : 0);
          }
        });
      }
    });
    if (type.empty()) continue;
    if (idx >= 0) out->by_idx.emplace(idx, entry);
    out->by_type.emplace(type, entry);
  }
}

// 幻灯片上的占位符没有几何时，按 idx -> type 顺序从版式、再从严版式（母版）继承。
void InheritPlaceholderGeometry(Slide* slide, Package* pkg) {
  if (slide->layout_uri().empty() || pkg == nullptr) return;
  Part* layout = pkg->Find(slide->layout_uri());
  if (layout == nullptr) return;
  PlaceholderTable layout_geom;
  ReadPlaceholderGeom(AsUtf8(layout->content()), &layout_geom);

  PlaceholderTable master_geom;
  for (const Relationship& rel : layout->relationships().items()) {
    if (rel.type.find("/slideMaster") == std::string::npos) continue;
    Part* master = pkg->Find(rel.resolved);
    if (master != nullptr) ReadPlaceholderGeom(AsUtf8(master->content()), &master_geom);
    break;
  }

  for (Shape& shape : slide->shapes()) {
    if (!shape.is_placeholder()) continue;
    // 版式里的占位符经常只写 <a:bodyPr/>、几何继续从母版继承，
    // 所以要"几何取第一个有效的、锚点取第一个写了的"。
    bool have_geom = shape.geom.cx > 0 || shape.geom.cy > 0;
    int anchor = shape.text.anchor;
    for (const PlaceholderTable* table : {&layout_geom, &master_geom}) {
      const PlaceholderGeom* entry = table->Find(shape);
      if (entry == nullptr) continue;
      if (!have_geom && entry->valid()) {
        shape.geom = entry->geom;
        have_geom = true;
      }
      if (anchor < 0 && entry->anchor >= 0) anchor = entry->anchor;
    }
    shape.text.anchor = anchor;
  }
}

Presentation ReadPackage(Package& pkg, WarningSink* warnings, CancelToken* cancel) {
  if (pkg.kind() != PackageKind::PowerPoint) {
    throw Error(ErrorCode::Unsupported, "pptx: package is not a presentation");
  }
  Part* main = pkg.main_part();
  if (main == nullptr) throw Error(ErrorCode::Corrupt, "pptx: no presentation part");

  Presentation pres;
  std::map<std::string, std::string> rid_target;
  std::string theme_part;
  for (const Relationship& rel : main->relationships().items()) {
    if (rel.external()) continue;
    rid_target[rel.id] = rel.resolved;
    if (rel.type.find("/theme") != std::string::npos && theme_part.empty()) {
      theme_part = rel.resolved;
    }
  }

  const ParsedPresentation parsed =
      ReadPresentationXml(AsUtf8(main->content()), rid_target);
  pres.slide_width = parsed.width > 0 ? parsed.width : pres.slide_width;
  pres.slide_height = parsed.height > 0 ? parsed.height : pres.slide_height;

  // 主题字体：presentation.xml 不一定直接连主题，母版那边通常有。
  if (theme_part.empty()) {
    for (const std::unique_ptr<Part>& part : pkg.parts()) {
      if (part->uri().find("/theme/theme") != std::string::npos) {
        theme_part = part->uri();
        break;
      }
    }
  }
  if (!theme_part.empty()) {
    if (Part* theme = pkg.Find(theme_part)) {
      const std::string font = ReadThemeFont(AsUtf8(theme->content()));
      if (!font.empty()) pres.default_font = font;
    }
  }

  for (const SlideRef& ref : parsed.slides) {
    if (cancel != nullptr) cancel->ThrowIfCancelled();
    Part* part = pkg.Find(ref.part);
    if (part == nullptr) {
      if (warnings != nullptr) {
        Warning warning;
        warning.code = static_cast<int>(WarningCode::UnknownPartKept);
        warning.module = "pptx";
        warning.path = ref.part;
        warning.message = "slide part missing; slide skipped";
        warnings->Add(warning);
      }
      continue;
    }
    Slide& slide = pres.AddSlide();
    // 版式引用：slide 的关系里找 slideLayout。
    for (const Relationship& rel : part->relationships().items()) {
      if (rel.type.find("/slideLayout") != std::string::npos) {
        slide.set_layout_uri(rel.resolved);
        break;
      }
    }
    std::vector<std::string> unmapped;
    ReadSlideXml(AsUtf8(part->content()), &slide, part->relationships(), &pkg, &unmapped);
    if (warnings != nullptr) {
      for (const std::string& message : unmapped) {
        Warning warning;
        warning.code = static_cast<int>(WarningCode::UnknownPartKept);
        warning.module = "pptx";
        warning.path = part->uri();
        warning.message = message;
        warnings->Add(warning);
      }
    }
    InheritPlaceholderGeometry(&slide, &pkg);
    // 备注页
    for (const Relationship& rel : part->relationships().items()) {
      if (rel.type.find("/notesSlide") == std::string::npos) continue;
      Part* notes = pkg.Find(rel.resolved);
      if (notes == nullptr) continue;
      slide.notes() = ReadNotesText(AsUtf8(notes->content()));
      break;
    }
  }

  // 未识别部件原样保留。
  std::set<std::string> consumed;
  consumed.insert(main->uri());
  for (const SlideRef& ref : parsed.slides) consumed.insert(ref.part);
  for (const std::unique_ptr<Part>& part : pkg.parts()) {
    if (consumed.count(part->uri()) != 0) continue;
    UnknownPart unknown;
    unknown.uri = part->uri();
    unknown.mime = part->mime();
    unknown.bytes = part->content();
    pres.unknown_parts.push_back(std::move(unknown));
  }
  return pres;
}

}  // namespace

Presentation Open(const OpenOptions& opts) {
  if (opts.path.empty()) throw Error(ErrorCode::InvalidArgument, "pptx: empty path");
  FileStream in(opts.path, "rb");
  auto pkg = Package::Open(in, opts.warnings);
  return ReadPackage(*pkg, opts.warnings, opts.cancel);
}

Presentation OpenMemory(std::string_view bytes, WarningSink* warnings, CancelToken* cancel) {
  std::vector<uint8_t> data(bytes.begin(), bytes.end());
  MemoryStream mem(std::move(data));
  auto pkg = Package::Open(mem, warnings);
  return ReadPackage(*pkg, warnings, cancel);
}

}  // namespace deck
}  // namespace doccore
