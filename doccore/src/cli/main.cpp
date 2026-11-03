// doccore CLI. Design refs: docs/02 §3.9, exit codes per docs/01 §6.2.
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "doccore/convert.h"
#include "doccore/core.h"
#include "doccore/deflate.h"
#include "doccore/deck_layout.h"
#include "doccore/docx.h"
#include "doccore/pptx.h"
#include "doccore/sheet_layout.h"
#include "doccore/xlsx.h"
#include "doccore/layout.h"
#include "doccore/pdf.h"
#include "doccore/pdf_read.h"
#include "doccore/pdf_rebuild.h"
#include "doccore/render.h"
#include "doccore/stream.h"
#include "doccore/text_layout.h"
#include "doccore/zip.h"
#include "doccore/zip_write.h"

using namespace doccore;

namespace {

const char* kVersion = "0.1.0";

void PrintUsage() {
  std::fprintf(stderr,
               "doccore %s\n"
               "usage:\n"
               "  doccore version\n"
               "  doccore detect <file> [--json]\n"
               "  doccore zip-list <file> [--json]\n"
               "  doccore render --font <ttf> --out <file.pdf|file.png> [options]\n"
               "  doccore docx <file.docx> --mode info|text|save|pdf|png|txt|html\n"
               "  doccore pptx <file.pptx> --mode info|text|html|pdf|png|save\n"
               "  doccore convert <in> --to <fmt> --out <path> [--from <fmt>] [--font <ttf>] [--dpi <n>] [--page <n>] [--recalc]\n"
               "  doccore pdf <file.pdf> --mode info|runs|model|text|rebuild [--to docx|xlsx|pptx] [--out <path>]\n"
               "      --out <path>          output path\n"
               "      --font <ttf>          font for pdf/png (repeatable is not supported)\n"
               "      --dpi <n>             raster DPI for png (default 96)\n"
               "      --page <n>            page to rasterize for png (default 1)\n"
               "      --text <utf8>         inline text\n"
               "      --text-file <path>    UTF-8 text file\n"
               "      --size <pt>           font size (default 12)\n"
               "      --dpi <n>             raster DPI for png (default 96)\n"
               "      --title <utf8>        PDF document title\n"
               "  doccore xlsx <file.xlsx> --mode info|csv|html|pdf|save\n"
               "      --out <path>          output path\n"
               "      --font <ttf>          font for pdf\n"
               "      --recalc              recalculate formulas before exporting\n",
               kVersion);
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

Format DetectFormat(const std::string& path) {
  FileStream in(path, "rb");
  uint8_t head[8] = {0};
  const int64_t n = in.Read(head, 8);
  if (n >= 5 && std::memcmp(head, "%PDF-", 5) == 0) return Format::Pdf;
  if (n >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) {
    in.Seek(0);
    try {
      auto zip = ZipArchive::Open(in);
      if (zip.Has("word/document.xml")) return Format::Docx;
      if (zip.Has("xl/workbook.xml")) return Format::Xlsx;
      if (zip.Has("ppt/presentation.xml")) return Format::Pptx;
    } catch (const Error&) {
      return Format::Unknown;
    }
    return Format::Unknown;
  }
  if (n >= 4 && head[0] == 0xD0 && head[1] == 0xCF && head[2] == 0x11 && head[3] == 0xE0) {
    return Format::Msg;
  }
  return Format::Unknown;
}

int CmdDetect(const std::string& path, bool json) {
  const Format f = DetectFormat(path);
  FileStream in(path, "rb");
  const int64_t size = in.Size();
  if (json) {
    std::printf("{\"format\":\"%s\",\"size\":%lld}\n", FormatName(f), static_cast<long long>(size));
  } else {
    std::printf("%s %lld\n", FormatName(f), static_cast<long long>(size));
  }
  return 0;
}

int CmdZipList(const std::string& path, bool json) {
  FileStream in(path, "rb");
  auto zip = ZipArchive::Open(in);
  const auto& entries = zip.entries();
  if (json) {
    std::printf("{\"zip64\":%s,\"entries\":[", zip.used_zip64() ? "true" : "false");
    for (size_t i = 0; i < entries.size(); ++i) {
      if (i != 0) std::printf(",");
      std::printf("{\"name\":\"%s\",\"method\":%u,\"compressed\":%llu,\"uncompressed\":%llu}",
                  JsonEscape(entries[i].name).c_str(), unsigned(entries[i].method),
                  static_cast<unsigned long long>(entries[i].compressed),
                  static_cast<unsigned long long>(entries[i].uncompressed));
    }
    std::printf("]}\n");
  } else {
    for (const auto& e : entries) {
      std::printf("%10llu  %s\n", static_cast<unsigned long long>(e.uncompressed), e.name.c_str());
    }
  }
  return 0;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

int CmdRender(const std::vector<std::string>& args) {
  std::string font_path, out_path, text, text_file, title;
  double size = 12.0;
  int dpi = 96;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= args.size()) throw Error(ErrorCode::InvalidArgument, std::string("missing value for ") + name);
      return args[++i];
    };
    if (a == "--font") {
      font_path = next("--font");
    } else if (a == "--out") {
      out_path = next("--out");
    } else if (a == "--text") {
      text = next("--text");
    } else if (a == "--text-file") {
      text_file = next("--text-file");
    } else if (a == "--size") {
      size = std::atof(next("--size").c_str());
    } else if (a == "--dpi") {
      dpi = std::atoi(next("--dpi").c_str());
    } else if (a == "--title") {
      title = next("--title");
    } else {
      throw Error(ErrorCode::InvalidArgument, "unknown option: " + a);
    }
  }
  if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "--out is required");
  if (font_path.empty()) throw Error(ErrorCode::InvalidArgument, "--font is required");
  if (!text_file.empty()) {
    FileStream in(text_file, "rb");
    const auto bytes = ReadAll(in, 64ull << 20);
    text.assign(bytes.begin(), bytes.end());
  }

  FontRegistry fonts;
  fonts.AddFontFile(font_path);

  LayoutPage page;
  page.ops.push_back(MakeRect(40, 40, page.width_pt - 80, page.height_pt - 80, Rgba{200, 200, 200, 255}, true,
                              Rgba{60, 60, 60, 255}, 0.8));
  double y = 90.0;
  const double line_height = size * 1.6;
  for (const auto& line : SplitLines(text)) {
    if (line.empty()) {
      y += line_height;
      continue;
    }
    DrawText t;
    t.font.family = fonts.Match(FontKey{}) != nullptr ? fonts.Match(FontKey{})->family() : "";
    t.size_pt = size;
    t.color = Rgba{0, 0, 0, 255};
    t.x = 70;
    t.y = y;
    t.text_utf8 = line;
    page.ops.push_back(t);
    y += line_height;
    if (y > page.height_pt - 60) break;
  }

  LayoutDocument doc;
  doc.pages.push_back(page);

  const bool is_pdf = out_path.size() > 4 && out_path.compare(out_path.size() - 4, 4, ".pdf") == 0;
  if (is_pdf) {
    PdfSaveOptions opt;
    opt.title = title;
    auto bytes = PdfWriter::ToBytes(doc, fonts, opt);
    WriteFileAtomically(out_path, bytes);
  } else {
    RasterOptions opt;
    opt.dpi = dpi;
    WarningList warnings;
    auto img = RasterizePage(page, opt, &warnings);
    auto png = EncodePng(img);
    WriteFileAtomically(out_path, png);
    for (const auto& w : warnings.items()) {
      std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
    }
  }
  return 0;
}

// ---- docx ---------------------------------------------------------------

// Renders inline items as text; non-text items (drawings, bookmarks, field
// markers) contribute nothing, which is what the "text" mode is for.
void DumpInline(const text::Paragraph& p, std::string& out) {
  for (const auto& it : p.runs) {
    switch (it.type) {
      case text::InlineItem::Type::Text:
        out += it.text;
        break;
      case text::InlineItem::Type::Tab:
        out += '\t';
        break;
      case text::InlineItem::Type::Break:
        out += (it.break_kind == 1) ? "\n[page-break]\n" : "\n";
        break;
      case text::InlineItem::Type::Symbol:
        out += U32ToUtf8(std::u32string(1, it.symbol));
        break;
      default:
        break;
    }
  }
}

void DumpBlocks(const std::vector<text::Block>& blocks, std::string& out) {
  for (const auto& b : blocks) {
    if (b.type == text::Block::Type::Paragraph) {
      DumpInline(b.paragraph, out);
      out += '\n';
    } else if (b.table != nullptr) {
      for (const auto& row : b.table->rows) {
        for (const auto& cell : row.cells) {
          std::string cell_text;
          DumpBlocks(cell.blocks, cell_text);
          std::string flat;
          for (char c : cell_text) flat.push_back(c == '\n' ? ' ' : c);
          out += "[";
          out += flat;
          out += "]";
        }
        out += '\n';
      }
    }
  }
}

int CmdDocx(const std::string& path, const std::string& mode, const std::string& out_path,
            const std::string& font_path, int dpi, int page_number) {
  const bool export_mode = mode == "pdf" || mode == "png" || mode == "txt" || mode == "html";
  if (mode != "info" && mode != "text" && mode != "save" && !export_mode) {
    throw Error(ErrorCode::InvalidArgument, "docx: --mode must be info, text, save, pdf, png, txt or html");
  }
  WarningList warnings;
  text::OpenOptions opt;
  opt.path = path;
  opt.warnings = &warnings;
  text::Document doc = text::Open(opt);

  if (mode == "save") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "docx --mode save needs --out");
    text::SaveOptions save;
    save.warnings = &warnings;
    text::SaveToFile(doc, out_path, save);
    FileStream written(out_path, "rb");
    std::printf("{\"format\":\"docx\",\"wrote\":\"%s\",\"bytes\":%lld,\"sections\":%zu,"
                "\"styles\":%zu,\"unknown_parts\":%zu}\n",
                JsonEscape(out_path).c_str(), static_cast<long long>(written.Size()),
                doc.sections.size(), doc.styles.size(), doc.unknown_parts.size());
    for (const auto& w : warnings.items()) {
      std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
    }
    return 0;
  }

  if (export_mode) {
    if (out_path.empty()) {
      throw Error(ErrorCode::InvalidArgument, "docx --mode " + mode + " needs --out");
    }
    if (mode == "txt") {
      const std::string text = text::ExtractText(doc);
      std::vector<uint8_t> bytes(text.begin(), text.end());
      WriteFileAtomically(out_path, bytes);
      std::printf("{\"format\":\"docx\",\"mode\":\"txt\",\"bytes\":%zu}\n", bytes.size());
    } else if (mode == "html") {
      const std::string html = text::ToHtml(doc);
      std::vector<uint8_t> bytes(html.begin(), html.end());
      WriteFileAtomically(out_path, bytes);
      std::printf("{\"format\":\"docx\",\"mode\":\"html\",\"bytes\":%zu}\n", bytes.size());
    } else {
      FontRegistry fonts;
      if (!font_path.empty()) fonts.AddFontFile(font_path);
      text::LayoutOptions layout_opts;
      layout_opts.fonts = &fonts;
      layout_opts.warnings = &warnings;
      const LayoutDocument laid = text::LayoutText(doc, layout_opts);
      if (mode == "pdf") {
        PdfSaveOptions pdf;
        pdf.title = path;
        const auto bytes = PdfWriter::ToBytes(laid, fonts, pdf, &warnings);
        WriteFileAtomically(out_path, bytes);
        std::printf("{\"format\":\"docx\",\"mode\":\"pdf\",\"pages\":%zu,\"bytes\":%zu}\n",
                    laid.pages.size(), bytes.size());
      } else {
        if (page_number < 1 || static_cast<size_t>(page_number) > laid.pages.size()) {
          throw Error(ErrorCode::InvalidArgument,
                      "docx --mode png: --page out of range (1.." +
                          std::to_string(laid.pages.size()) + ")");
        }
        RasterOptions raster;
        raster.dpi = dpi;
        const auto img = RasterizePage(laid.pages[static_cast<size_t>(page_number) - 1], raster,
                                       &warnings);
        const auto png = EncodePng(img);
        WriteFileAtomically(out_path, png);
        std::printf("{\"format\":\"docx\",\"mode\":\"png\",\"page\":%d,\"pages\":%zu,"
                    "\"width\":%d,\"height\":%d,\"bytes\":%zu}\n",
                    page_number, laid.pages.size(), img.width, img.height, png.size());
      }
    }
    for (const auto& w : warnings.items()) {
      std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
    }
    return 0;
  }

  size_t paragraphs = 0;
  size_t tables = 0;
  size_t table_rows = 0;
  size_t table_cells = 0;
  size_t runs = 0;
  std::function<void(const std::vector<text::Block>&)> count_blocks =
      [&](const std::vector<text::Block>& blocks) {
    for (const auto& b : blocks) {
      if (b.type == text::Block::Type::Paragraph) {
        ++paragraphs;
        runs += b.paragraph.runs.size();
      } else if (b.table != nullptr) {
        ++tables;
        table_rows += b.table->rows.size();
        for (const auto& row : b.table->rows) {
          table_cells += row.cells.size();
          for (const auto& cell : row.cells) count_blocks(cell.blocks);
        }
      }
    }
  };
  for (const auto& sec : doc.sections) {
    count_blocks(sec.blocks);
    for (const auto& hf : sec.headers) count_blocks(hf.blocks);
    for (const auto& hf : sec.footers) count_blocks(hf.blocks);
  }

  if (mode == "info") {
    std::printf("{\"format\":\"docx\",\"styles\":%zu,\"numbering\":%zu,\"abstract_numbering\":%zu,"
                "\"sections\":%zu,\"paragraphs\":%zu,\"runs\":%zu,\"tables\":%zu,"
                "\"table_rows\":%zu,\"table_cells\":%zu,\"footnotes\":%zu,\"endnotes\":%zu,"
                "\"comments\":%zu,\"unknown_parts\":%zu}\n",
                doc.styles.size(), doc.numbering.size(), doc.abstract_numberings.size(),
                doc.sections.size(), paragraphs, runs, tables, table_rows, table_cells,
                doc.footnotes.size(), doc.endnotes.size(), doc.comments.size(),
                doc.unknown_parts.size());
  } else {
    std::string text;
    for (const auto& sec : doc.sections) {
      text += "=== section ===\n";
      DumpBlocks(sec.blocks, text);
      for (const auto& hf : sec.headers) {
        text += "--- header ";
        text += (hf.kind == text::HeaderFooter::Kind::First
                     ? "first"
                     : (hf.kind == text::HeaderFooter::Kind::Even ? "even" : "default"));
        text += " ---\n";
        DumpBlocks(hf.blocks, text);
      }
      for (const auto& hf : sec.footers) {
        text += "--- footer ";
        text += (hf.kind == text::HeaderFooter::Kind::First
                     ? "first"
                     : (hf.kind == text::HeaderFooter::Kind::Even ? "even" : "default"));
        text += " ---\n";
        DumpBlocks(hf.blocks, text);
      }
    }
    std::fputs(text.c_str(), stdout);
  }

  for (const auto& w : warnings.items()) {
    std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
  }
  return 0;
}

// ---- xlsx ---------------------------------------------------------------

int CmdXlsx(const std::string& path, const std::string& mode, const std::string& out_path,
            bool recalc, const std::string& font_path) {
  sheet::OpenOptions opts;
  opts.path = path;
  WarningList warnings;
  opts.warnings = &warnings;
  sheet::Workbook book = sheet::Open(opts);
  const int computed = recalc ? book.Recalculate() : 0;

  if (mode == "info") {
    size_t cells = 0;
    size_t formulas = 0;
    size_t merges = 0;
    for (int i = 0; i < book.sheet_count(); ++i) {
      const sheet::Worksheet* ws = book.SheetAt(i);
      merges += ws->merges().size();
      for (const auto& column : ws->columns()) {
        for (const auto& row : column.second) {
          ++cells;
          if (row.second.has_formula()) ++formulas;
        }
      }
    }
    std::printf("{\"format\":\"xlsx\",\"sheets\":%d,\"cells\":%zu,\"formulas\":%zu,"
                "\"merges\":%zu,\"shared_strings\":%zu,\"styles\":%zu,"
                "\"unknown_parts\":%zu,\"charts\":%zu,\"date1904\":%s,\"recalculated\":%d}\n",
                book.sheet_count(), cells, formulas, merges, book.shared_strings.size(),
                book.styles.size(), book.unknown_parts.size(), book.charts.size(),
                book.date1904 ? "true" : "false", computed);
    for (int i = 0; i < book.sheet_count(); ++i) {
      const sheet::Worksheet* ws = book.SheetAt(i);
      const sheet::Range used = ws->UsedRange();
      std::printf("  [%d] %s  %s:%s\n", i, ws->name().c_str(),
                  sheet::CellName(used.row1, used.col1).c_str(),
                  sheet::CellName(used.row2, used.col2).c_str());
    }
  } else if (mode == "charts") {
    // 图表 DOM 的诊断视图：种类、标题、锚定矩形、每个系列的名字与点数。
    std::printf("charts=%zu\n", book.charts.size());
    for (const sheet::Chart& chart : book.charts) {
      std::printf("--- %s  title=\"%s\"  sheet=%s  x=%.1f y=%.1f w=%.1f h=%.1f\n",
                  sheet::ChartKindName(chart.kind), chart.title.c_str(),
                  chart.sheet.c_str(), chart.x, chart.y, chart.w, chart.h);
      for (const sheet::ChartSeries& series : chart.series) {
        std::string values;
        for (size_t i = 0; i < series.values.size() && i < 12; ++i) {
          if (i > 0) values += ",";
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%g", series.values[i]);
          values += buf;
        }
        std::string cats;
        for (size_t i = 0; i < series.categories.size() && i < 12; ++i) {
          if (i > 0) cats += ",";
          cats += series.categories[i];
        }
        std::printf("    series \"%s\" n=%zu cats=[%s] vals=[%s]\n", series.name.c_str(),
                    series.values.size(), cats.c_str(), values.c_str());
      }
    }
  } else if (mode == "csv") {
    if (book.sheet_count() == 0) throw Error(ErrorCode::Corrupt, "xlsx: no sheets");
    const std::string csv = sheet::ToCsv(*book.SheetAt(0));
    if (out_path.empty()) {
      std::fwrite(csv.data(), 1, csv.size(), stdout);
    } else {
      WriteFileAtomically(out_path, std::vector<uint8_t>(csv.begin(), csv.end()));
      std::printf("{\"format\":\"csv\",\"wrote\":\"%s\",\"bytes\":%zu}\n", out_path.c_str(),
                  csv.size());
    }
  } else if (mode == "html") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "xlsx --mode html needs --out");
    const std::string html = sheet::ToHtml(book);
    WriteFileAtomically(out_path, std::vector<uint8_t>(html.begin(), html.end()));
    std::printf("{\"format\":\"html\",\"wrote\":\"%s\",\"bytes\":%zu}\n", out_path.c_str(),
                html.size());
  } else if (mode == "pdf") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "xlsx --mode pdf needs --out");
    FontRegistry fonts;
    if (!font_path.empty()) fonts.AddFontFile(font_path);
    sheet::SheetLayoutOptions layout_opts;
    layout_opts.fonts = &fonts;
    layout_opts.warnings = &warnings;
    const LayoutDocument laid = sheet::LayoutWorkbook(book, layout_opts);
    PdfSaveOptions pdf;
    pdf.title = path;
    const std::vector<uint8_t> bytes = PdfWriter::ToBytes(laid, fonts, pdf, &warnings);
    WriteFileAtomically(out_path, bytes);
    std::printf("{\"format\":\"xlsx\",\"mode\":\"pdf\",\"pages\":%zu,\"bytes\":%zu}\n",
                laid.pages.size(), bytes.size());
  } else if (mode == "save") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "xlsx --mode save needs --out");
    sheet::SaveOptions save_opts;
    save_opts.warnings = &warnings;
    save_opts.recalculate = false;  // 已经算过了
    sheet::SaveToFile(book, out_path, save_opts);
    std::printf("{\"format\":\"xlsx\",\"wrote\":\"%s\"}\n", out_path.c_str());
  } else {
    throw Error(ErrorCode::InvalidArgument,
                "xlsx: --mode must be info, charts, csv, html, pdf or save");
  }

  for (const auto& w : warnings.items()) {
    std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
  }
  return 0;
}

// ---- pptx ---------------------------------------------------------------

int CmdPptx(const std::string& path, const std::string& mode, const std::string& out_path,
            const std::string& font_path, int dpi, int page_number) {
  deck::OpenOptions opts;
  opts.path = path;
  WarningList warnings;
  opts.warnings = &warnings;
  deck::Presentation pres = deck::Open(opts);

  if (mode == "info") {
    size_t shapes = 0;
    size_t pictures = 0;
    size_t tables = 0;
    size_t notes = 0;
    for (int i = 0; i < pres.slide_count(); ++i) {
      const deck::Slide* slide = pres.SlideAt(i);
      for (const auto& shape : slide->shapes()) {
        ++shapes;
        if (shape.kind == deck::ShapeKind::Picture) ++pictures;
        if (shape.kind == deck::ShapeKind::Table) ++tables;
      }
      if (!slide->notes().empty()) ++notes;
    }
    std::printf("{\"format\":\"pptx\",\"slides\":%d,\"shapes\":%zu,\"pictures\":%zu,"
                "\"tables\":%zu,\"notes\":%zu,\"unknown_parts\":%zu,"
                "\"width_emu\":%lld,\"height_emu\":%lld,\"width_pt\":%.2f,\"height_pt\":%.2f,"
                "\"default_font\":\"%s\"}\n",
                pres.slide_count(), shapes, pictures, tables, notes, pres.unknown_parts.size(),
                static_cast<long long>(pres.slide_width),
                static_cast<long long>(pres.slide_height),
                static_cast<double>(pres.slide_width) / deck::kEmuPerPoint,
                static_cast<double>(pres.slide_height) / deck::kEmuPerPoint,
                pres.default_font.c_str());
  } else if (mode == "text") {
    const std::string text = pres.ExtractText();
    if (out_path.empty()) {
      std::fwrite(text.data(), 1, text.size(), stdout);
    } else {
      WriteFileAtomically(out_path, std::vector<uint8_t>(text.begin(), text.end()));
      std::printf("{\"format\":\"pptx\",\"mode\":\"text\",\"wrote\":\"%s\",\"bytes\":%zu}\n",
                  out_path.c_str(), text.size());
    }
  } else if (mode == "html") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "pptx --mode html needs --out");
    const std::string html = deck::ToHtml(pres);
    WriteFileAtomically(out_path, std::vector<uint8_t>(html.begin(), html.end()));
    std::printf("{\"format\":\"pptx\",\"mode\":\"html\",\"wrote\":\"%s\",\"bytes\":%zu}\n",
                out_path.c_str(), html.size());
  } else if (mode == "pdf") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "pptx --mode pdf needs --out");
    FontRegistry fonts;
    if (!font_path.empty()) fonts.AddFontFile(font_path);
    deck::DeckLayoutOptions layout_opts;
    layout_opts.fonts = &fonts;
    layout_opts.warnings = &warnings;
    const LayoutDocument laid = deck::LayoutPresentation(pres, layout_opts);
    PdfSaveOptions pdf;
    pdf.title = path;
    const std::vector<uint8_t> bytes = PdfWriter::ToBytes(laid, fonts, pdf, &warnings);
    WriteFileAtomically(out_path, bytes);
    std::printf("{\"format\":\"pptx\",\"mode\":\"pdf\",\"pages\":%zu,\"bytes\":%zu,"
                "\"page_width_pt\":%.2f,\"page_height_pt\":%.2f}\n",
                laid.pages.size(), bytes.size(),
                laid.pages.empty() ? 0.0 : laid.pages.front().width_pt,
                laid.pages.empty() ? 0.0 : laid.pages.front().height_pt);
  } else if (mode == "png") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "pptx --mode png needs --out");
    FontRegistry fonts;
    if (!font_path.empty()) fonts.AddFontFile(font_path);
    deck::DeckLayoutOptions layout_opts;
    layout_opts.fonts = &fonts;
    layout_opts.warnings = &warnings;
    const LayoutDocument laid = deck::LayoutPresentation(pres, layout_opts);
    if (page_number < 1 || static_cast<size_t>(page_number) > laid.pages.size()) {
      throw Error(ErrorCode::InvalidArgument,
                  "pptx --mode png: --page out of range (1.." +
                      std::to_string(laid.pages.size()) + ")");
    }
    RasterOptions raster;
    raster.dpi = dpi;
    const auto img =
        RasterizePage(laid.pages[static_cast<size_t>(page_number) - 1], raster, &warnings);
    const auto png = EncodePng(img);
    WriteFileAtomically(out_path, png);
    std::printf("{\"format\":\"pptx\",\"mode\":\"png\",\"page\":%d,\"pages\":%zu,"
                "\"width\":%d,\"height\":%d,\"bytes\":%zu}\n",
                page_number, laid.pages.size(), img.width, img.height, png.size());
  } else if (mode == "save") {
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "pptx --mode save needs --out");
    deck::SaveOptions save_opts;
    save_opts.warnings = &warnings;
    deck::SaveToFile(pres, out_path, save_opts);
    std::printf("{\"format\":\"pptx\",\"mode\":\"save\",\"wrote\":\"%s\"}\n", out_path.c_str());
  } else {
    throw Error(ErrorCode::InvalidArgument,
                "pptx: --mode must be info, text, html, pdf, png or save");
  }

  for (const auto& w : warnings.items()) {
    std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
  }
  return 0;
}

// ---- pdf ----------------------------------------------------------------

int CmdPdf(const std::string& path, const std::string& mode, const std::string& out_path,
           const std::string& to_format) {
  WarningList warnings;
  pdf::PdfOpenOptions opts;
  opts.warnings = &warnings;
  pdf::PdfFile file = pdf::PdfFile::OpenPath(path, opts);

  if (mode == "info") {
    size_t runs = 0;
    size_t lines = 0;
    size_t rects = 0;
    size_t images = 0;
    size_t scanned = 0;
    for (int i = 0; i < file.page_count(); ++i) {
      const pdf::PdfPage& page = file.page(i);
      runs += page.text_runs().size();
      lines += page.lines().size();
      rects += page.rects().size();
      images += page.images().size();
      if (page.looks_scanned()) ++scanned;
    }
    std::printf("{\"format\":\"pdf\",\"version\":\"%s\",\"pages\":%d,\"objects\":%zu,"
                "\"encrypted\":%s,\"title\":\"%s\",\"text_runs\":%zu,\"lines\":%zu,"
                "\"rects\":%zu,\"images\":%zu,\"scanned_pages\":%zu}\n",
                file.version().c_str(), file.page_count(), file.object_count(),
                file.encrypted() ? "true" : "false", file.title().c_str(), runs, lines, rects,
                images, scanned);
    for (int i = 0; i < file.page_count(); ++i) {
      const pdf::PdfPage& page = file.page(i);
      std::printf("  [%d] %.1fx%.1f pt rot=%d runs=%zu lines=%zu images=%zu\n", i,
                  page.size.width, page.size.height, page.rotation, page.text_runs().size(),
                  page.lines().size(), page.images().size());
    }
  } else if (mode == "runs") {
    // 调试用：逐条打印文本片段（坐标、字号、字体、内容），重建器排错靠它。
    for (int i = 0; i < file.page_count(); ++i) {
      const pdf::PdfPage& page = file.page(i);
      std::printf("--- page %d (%.1fx%.1f) ---\n", i, page.size.width, page.size.height);
      for (const pdf::PdfTextRun& run : page.text_runs()) {
        std::printf("  x=%.2f y=%.2f w=%.2f sz=%.2f %s/%s %s | %s\n", run.x, run.y, run.width,
                    run.size_pt, run.font_name.c_str(), run.family.c_str(),
                    run.bold ? "bold" : "-", run.text_utf8.c_str());
      }
      for (const pdf::PdfImagePlacement& image : page.images()) {
        std::printf("  image %s x=%.2f y=%.2f w=%.2f h=%.2f px=%dx%d\n",
                    image.xobject_name.c_str(), image.x, image.y, image.w, image.h,
                    image.pixel_width, image.pixel_height);
      }
    }
  } else if (mode == "lines") {
    // 调试用：线段与矩形（表格检测的输入）。
    for (int i = 0; i < file.page_count(); ++i) {
      const pdf::PdfPage& page = file.page(i);
      std::printf("--- page %d lines=%zu rects=%zu ---\n", i, page.lines().size(),
                  page.rects().size());
      for (const pdf::PdfLine& line : page.lines()) {
        std::printf("  %s %s (%.2f,%.2f)-(%.2f,%.2f) w=%.2f\n",
                    line.horizontal() ? "H" : (line.vertical() ? "V" : "D"), "", line.x1, line.y1,
                    line.x2, line.y2, line.width_pt);
      }
    }
  } else if (mode == "images") {
    // 调试用：图像 XObject 的落位与可解出的像素（重建器插图排错靠它）。
    for (int i = 0; i < file.page_count(); ++i) {
      const pdf::PdfPage& page = file.page(i);
      for (const pdf::PdfImagePlacement& image : page.images()) {
        pdf::ExtractedImage extracted =
            pdf::ExtractImage(file, page, image.xobject_name, &warnings);
        std::printf("page %d %s x=%.1f y=%.1f w=%.1f h=%.1f px=%dx%d -> %s %zu bytes\n", i,
                    image.xobject_name.c_str(), image.x, image.y, image.w, image.h,
                    image.pixel_width, image.pixel_height,
                    extracted.empty() ? "(failed)" : extracted.mime.c_str(), extracted.bytes.size());
      }
    }
  } else if (mode == "model") {
    // 重建中间模型：视觉行 -> 块（含栏号）与检测出的表格，供 verify_pdf.py 断言。
    const std::vector<pdf::PageModel> pages = pdf::Reconstructor::BuildPages(file, {}, &warnings);
    for (const pdf::PageModel& page : pages) {
      std::printf("--- page %d %.1fx%.1f scanned=%d blocks=%zu tables=%zu images=%zu\n",
                  page.index, page.width, page.height, page.scanned ? 1 : 0, page.blocks.size(),
                  page.tables.size(), page.images.size());
      for (const pdf::TextBlock& block : page.blocks) {
        std::printf("  block col=%d x=%.1f y=%.1f w=%.1f h=%.1f sz=%.1f | %s\n", block.column,
                    block.left, block.top, block.right - block.left, block.bottom - block.top,
                    block.size_pt, block.Text().c_str());
      }
      for (const pdf::DetectedTable& table : page.tables) {
        std::printf("  table %s %dx%d x=%.1f y=%.1f w=%.1f h=%.1f\n",
                    table.bordered ? "bordered" : "aligned", table.rows(), table.cols(), table.left,
                    table.top, table.right - table.left, table.bottom - table.top);
        for (const auto& row : table.cells) {
          std::printf("   |");
          for (const auto& cell : row) std::printf(" %s |", cell.c_str());
          std::printf("\n");
        }
      }
    }
  } else if (mode == "rebuild") {
    if (out_path.empty()) {
      throw Error(ErrorCode::InvalidArgument, "pdf: --mode rebuild needs --out");
    }
    const std::string to = to_format.empty() ? "docx" : to_format;
    pdf::RebuildOptions rebuild;
    if (to == "docx") {
      text::Document doc = pdf::Reconstructor::ToDocument(file, rebuild, &warnings);
      text::SaveToFile(doc, out_path);
    } else if (to == "xlsx") {
      sheet::Workbook book = pdf::Reconstructor::ToWorkbook(file, rebuild, &warnings);
      sheet::SaveToFile(book, out_path);
    } else if (to == "pptx") {
      deck::Presentation pres = pdf::Reconstructor::ToPresentation(file, rebuild, &warnings);
      deck::SaveToFile(pres, out_path);
    } else {
      throw Error(ErrorCode::InvalidArgument, "pdf: --to must be docx, xlsx or pptx");
    }
    std::printf("{\"format\":\"pdf\",\"mode\":\"rebuild\",\"to\":\"%s\",\"wrote\":\"%s\"}\n",
                to.c_str(), out_path.c_str());
  } else if (mode == "text") {
    const std::string text = file.ExtractText();
    if (out_path.empty()) {
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fputc('\n', stdout);
    } else {
      WriteFileAtomically(out_path, std::vector<uint8_t>(text.begin(), text.end()));
      std::printf("{\"format\":\"pdf\",\"mode\":\"text\",\"wrote\":\"%s\",\"bytes\":%zu}\n",
                  out_path.c_str(), text.size());
    }
  } else {
    throw Error(ErrorCode::InvalidArgument, "pdf: --mode must be info, runs or text");
  }

  for (const auto& w : warnings.items()) {
    std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
  }
  return 0;
}

int Run(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string cmd = argv[1];
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) rest.emplace_back(argv[i]);

  if (cmd == "version" || cmd == "--version") {
    std::printf("doccore %s\n", kVersion);
    return 0;
  }
  if (cmd == "detect") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "detect needs a file");
    bool json = false;
    for (const auto& a : rest) json = json || a == "--json";
    return CmdDetect(rest[0], json);
  }
  if (cmd == "zip-list") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "zip-list needs a file");
    bool json = false;
    for (const auto& a : rest) json = json || a == "--json";
    return CmdZipList(rest[0], json);
  }
  if (cmd == "render") return CmdRender(rest);
  if (cmd == "docx") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "docx needs a file");
    std::string mode = "text";
    std::string out_path;
    std::string font_path;
    int dpi = 96;
    int page_number = 1;
    const std::string path = rest[0];
    for (size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] == "--mode" && i + 1 < rest.size()) {
        mode = rest[++i];
      } else if (rest[i] == "--out" && i + 1 < rest.size()) {
        out_path = rest[++i];
      } else if (rest[i] == "--font" && i + 1 < rest.size()) {
        font_path = rest[++i];
      } else if (rest[i] == "--dpi" && i + 1 < rest.size()) {
        dpi = std::atoi(rest[++i].c_str());
      } else if (rest[i] == "--page" && i + 1 < rest.size()) {
        page_number = std::atoi(rest[++i].c_str());
      } else {
        throw Error(ErrorCode::InvalidArgument, "unknown option: " + rest[i]);
      }
    }
    return CmdDocx(path, mode, out_path, font_path, dpi, page_number);
  }
  if (cmd == "xlsx") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "xlsx needs a file");
    std::string mode = "info";
    std::string out_path;
    std::string font_path;
    bool recalc = false;
    const std::string path = rest[0];
    for (size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] == "--mode" && i + 1 < rest.size()) {
        mode = rest[++i];
      } else if (rest[i] == "--out" && i + 1 < rest.size()) {
        out_path = rest[++i];
      } else if (rest[i] == "--font" && i + 1 < rest.size()) {
        font_path = rest[++i];
      } else if (rest[i] == "--recalc") {
        recalc = true;
      } else {
        throw Error(ErrorCode::InvalidArgument, "unknown option: " + rest[i]);
      }
    }
    return CmdXlsx(path, mode, out_path, recalc, font_path);
  }
  if (cmd == "pptx") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "pptx needs a file");
    std::string mode = "info";
    std::string out_path;
    std::string font_path;
    int dpi = 96;
    int page_number = 1;
    const std::string path = rest[0];
    for (size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] == "--mode" && i + 1 < rest.size()) {
        mode = rest[++i];
      } else if (rest[i] == "--out" && i + 1 < rest.size()) {
        out_path = rest[++i];
      } else if (rest[i] == "--font" && i + 1 < rest.size()) {
        font_path = rest[++i];
      } else if (rest[i] == "--dpi" && i + 1 < rest.size()) {
        dpi = std::atoi(rest[++i].c_str());
      } else if (rest[i] == "--page" && i + 1 < rest.size()) {
        page_number = std::atoi(rest[++i].c_str());
      } else {
        throw Error(ErrorCode::InvalidArgument, "unknown option: " + rest[i]);
      }
    }
    return CmdPptx(path, mode, out_path, font_path, dpi, page_number);
  }
  if (cmd == "pdf") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "pdf needs a file");
    std::string mode = "info";
    std::string out_path;
    std::string to_format;
    const std::string path = rest[0];
    for (size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] == "--mode" && i + 1 < rest.size()) {
        mode = rest[++i];
      } else if (rest[i] == "--out" && i + 1 < rest.size()) {
        out_path = rest[++i];
      } else if (rest[i] == "--to" && i + 1 < rest.size()) {
        to_format = rest[++i];
      } else {
        throw Error(ErrorCode::InvalidArgument, "unknown option: " + rest[i]);
      }
    }
    return CmdPdf(path, mode, out_path, to_format);
  }
  if (cmd == "convert") {
    if (rest.empty()) throw Error(ErrorCode::InvalidArgument, "convert needs an input file");
    std::string out_path;
    std::string to_name;
    std::string font_path;
    std::string from_name;
    int dpi = 96;
    int page_number = 1;
    bool recalc = false;
    const std::string path = rest[0];
    for (size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] == "--out" && i + 1 < rest.size()) {
        out_path = rest[++i];
      } else if (rest[i] == "--to" && i + 1 < rest.size()) {
        to_name = rest[++i];
      } else if (rest[i] == "--from" && i + 1 < rest.size()) {
        from_name = rest[++i];
      } else if (rest[i] == "--font" && i + 1 < rest.size()) {
        font_path = rest[++i];
      } else if (rest[i] == "--dpi" && i + 1 < rest.size()) {
        dpi = std::atoi(rest[++i].c_str());
      } else if (rest[i] == "--page" && i + 1 < rest.size()) {
        page_number = std::atoi(rest[++i].c_str());
      } else if (rest[i] == "--recalc") {
        recalc = true;
      } else {
        throw Error(ErrorCode::InvalidArgument, "unknown option: " + rest[i]);
      }
    }
    if (out_path.empty()) throw Error(ErrorCode::InvalidArgument, "--out is required");
    if (to_name.empty()) throw Error(ErrorCode::InvalidArgument, "--to is required");
    const Format to = FormatFromName(to_name);
    if (to == Format::Unknown) {
      throw Error(ErrorCode::InvalidArgument, "unknown target format: " + to_name);
    }
    WarningList warnings;
    FontRegistry fonts;
    if (!font_path.empty()) fonts.AddFontFile(font_path);
    ConvertOptions options;
    options.fonts = fonts.size() > 0 ? &fonts : nullptr;
    options.dpi = dpi;
    options.page = page_number;
    options.recalc = recalc;
    FileStream in(path, "rb");
    const std::vector<uint8_t> input_bytes = ReadAll(in, 512ull << 20);
    const Format from = FormatFromName(from_name) != Format::Unknown
                            ? FormatFromName(from_name)
                            : Converter::Detect(input_bytes.data(), input_bytes.size(), path);
    MemoryStream input(input_bytes);
    MemoryStream out;
    ConvertRequest request{input, out, from, to, options, nullptr, &warnings};
    const Support support = Converter::Run(request);
    WriteFileAtomically(out_path, out.data());
    std::printf(
        "{\"format\":\"convert\",\"from\":\"%s\",\"to\":\"%s\",\"support\":\"%s\","
        "\"wrote\":\"%s\",\"bytes\":%zu}\n",
        FormatName(from), FormatName(to), SupportName(support), out_path.c_str(),
        out.data().size());
    for (const auto& w : warnings.items()) {
      std::fprintf(stderr, "warning %d %s\n", w.code, w.message.c_str());
    }
    return 0;
  }
  if (cmd == "zip-test") {
    // Development helper: emits a small deterministic ZIP so the writer can be
    // cross-checked with python -m zipfile.
    std::vector<ZipWriteEntry> entries;
    ZipWriteEntry a;
    a.name = "hello.txt";
    a.data.assign(64 << 10, 'y');
    entries.push_back(a);
    ZipWriteEntry b;
    b.name = "small.bin";
    b.data = {1, 2, 3, 4, 5};
    b.method = ZipMethod::Stored;
    entries.push_back(b);
    const std::string out_path = rest.empty() ? "out/test.zip" : rest[0];
    std::vector<uint8_t> bytes = ZipWriteToBytes(entries);
    FileStream out(out_path, "wb");
    out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
    std::printf("wrote %s (%zu bytes)\n", out_path.c_str(), bytes.size());
    return 0;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    PrintUsage();
    return 0;
  }
  PrintUsage();
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const Error& e) {
    std::fprintf(stderr, "error %d (%s): %s\n", static_cast<int>(e.code()), ErrorCodeName(e.code()),
                 e.what());
    return ErrorCodeToExitCode(e.code());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
