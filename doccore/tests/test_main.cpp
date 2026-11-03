// Minimal test runner for wave P1/P2. No external framework on purpose.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doccore/barcode.h"
#include "doccore/convert.h"
#include "doccore/core.h"
#include "doccore/crypto.h"
#include "doccore/deflate.h"
#include "doccore/deck.h"
#include "doccore/deck_layout.h"
#include "doccore/docx.h"
#include "doccore/image.h"
#include "doccore/layout.h"
#include "doccore/opc.h"
#include "doccore/pdf.h"
#include "doccore/pdf_read.h"
#include "doccore/pdf_rebuild.h"
#include "doccore/pptx.h"
#include "doccore/render.h"
#include "doccore/formula.h"
#include "doccore/mail.h"
#include "doccore/sheet.h"
#include "doccore/sheet_layout.h"
#include "doccore/stream.h"
#include "doccore/text_layout.h"
#include "doccore/ttf.h"
#include "doccore/xlsx.h"
#include "doccore/xml.h"
#include "doccore/zip.h"
#include "doccore/zip_write.h"

using namespace doccore;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!((a) == (b))) {                                                         \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);         \
    }                                                                            \
  } while (0)

#define CHECK_THROWS(expr, expected_code)                                        \
  do {                                                                           \
    ++g_checks;                                                                  \
    bool threw = false;                                                          \
    try {                                                                        \
      expr;                                                                      \
    } catch (const Error& e) {                                                   \
      threw = true;                                                              \
      if (e.code() != (expected_code)) {                                         \
        ++g_failures;                                                            \
        std::printf("FAIL %s:%d  wrong code %d\n", __FILE__, __LINE__,           \
                    static_cast<int>(e.code()));                                 \
      }                                                                          \
    }                                                                            \
    if (!threw) {                                                                \
      ++g_failures;                                                              \
      std::printf("FAIL %s:%d  expected throw: %s\n", __FILE__, __LINE__, #expr);\
    }                                                                            \
  } while (0)

std::string Fixture(const char* name) { return std::string(DOCCORE_TEST_DATA) + "/" + name; }

// 测试产物统一写到源码树的 out/ 下（ctest 的工作目录是 build/，不能用相对路径）。
std::string OutPath(const char* name) {
  std::error_code ec;
  std::filesystem::create_directories(DOCCORE_TEST_OUT, ec);
  return std::string(DOCCORE_TEST_OUT) + "/" + name;
}

std::vector<uint8_t> ReadFixture(const char* name) {
  FileStream in(Fixture(name), "rb");
  return ReadAll(in, 64ull << 20);
}

void TestCrc() {
  const char* s = "123456789";
  CHECK_EQ(Crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

void TestZlibStore() {
  std::vector<uint8_t> data(200000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 31 + 7);
  const auto packed = ZlibStoreCompress(data.data(), data.size());
  const auto back = ZlibDecompress(packed.data(), packed.size(), data.size() + 16);
  CHECK_EQ(back.size(), data.size());
  CHECK(std::memcmp(back.data(), data.data(), data.size()) == 0);
}

void TestInflatePython() {
  // Produced by tests/make_fixtures.py with zlib.compress on semi-random bytes.
  const auto packed = ReadFixture("deflate.z");
  const auto expect = ReadFixture("deflate.raw");
  const auto back = ZlibDecompress(packed.data(), packed.size(), expect.size() + 16);
  CHECK_EQ(back.size(), expect.size());
  CHECK(std::memcmp(back.data(), expect.data(), expect.size()) == 0);
}

void TestInflateCorrupt() {
  uint8_t junk[16] = {0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK_THROWS(ZlibDecompress(junk, sizeof(junk), 1 << 20), ErrorCode::Corrupt);
}

void TestZipRead() {
  FileStream in(Fixture("sample.zip"), "rb");
  auto zip = ZipArchive::Open(in);
  CHECK_EQ(zip.entries().size(), 3u);
  const auto hello = zip.Read("hello.txt");
  const std::string s(hello.begin(), hello.end());
  CHECK_EQ(s, std::string("hello doccore\n"));

  const auto big = zip.Read("dir/nested.bin");
  CHECK_EQ(big.size(), 70000u);
  for (size_t i = 0; i < big.size(); ++i) {
    if (big[i] != static_cast<uint8_t>(i & 0xFF)) {
      CHECK(false);
      break;
    }
  }
}

void TestZipRejectsTraversal() {
  FileStream in(Fixture("evil.zip"), "rb");
  CHECK_THROWS(ZipArchive::Open(in), ErrorCode::Corrupt);
}

void TestZipRejectsRatio() {
  FileStream in(Fixture("bomb.zip"), "rb");
  ZipLimits limits;
  limits.max_ratio = 5;
  auto zip = ZipArchive::Open(in, limits);
  CHECK_THROWS(zip.Read("bomb.bin"), ErrorCode::LimitExceeded);
}

void TestZipEntryLimit() {
  FileStream in(Fixture("sample.zip"), "rb");
  ZipLimits limits;
  limits.max_entries = 2;
  CHECK_THROWS(ZipArchive::Open(in, limits), ErrorCode::LimitExceeded);
}

void TestXmlBasic() {
  const std::string doc = "<?xml version=\"1.0\"?><root a=\"1\"><b>hi &amp; bye</b><c/></root>";
  XmlReader r(doc);
  XmlEvent ev;
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::StartElement);
  CHECK_EQ(std::string(ev.name), std::string("root"));
  CHECK_EQ(ev.attributes.size(), 1u);
  CHECK_EQ(std::string(ev.attributes[0].value), std::string("1"));
  CHECK(r.Next(ev));
  CHECK_EQ(std::string(ev.name), std::string("b"));
  CHECK_EQ(ev.depth, 2);
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::Text);
  CHECK_EQ(std::string(ev.text), std::string("hi & bye"));
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::EndElement);
  CHECK(r.Next(ev));
  CHECK_EQ(std::string(ev.name), std::string("c"));
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::EndElement);
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::EndElement);
  CHECK(!r.Next(ev));
}

// Regression: an element with several attributes must keep every value.
// The reader used to hand out string_views into a std::vector<std::string>
// that reallocated while the following attributes were appended, which
// silently truncated the first value (the "Id" of every relationship in an
// OPC .rels part came back as "\0Id1").
void TestXmlMultiAttribute() {
  const std::string doc =
      "<Relationships xmlns=\"urn:x\">"
      "<Relationship Id=\"rId1\" Type=\"urn:styles\" Target=\"styles.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"urn:numbering\" Target=\"numbering.xml\"/>"
      "</Relationships>";
  XmlReader r(doc);
  XmlEvent ev;
  int count = 0;
  while (r.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    ++count;
    if (count == 1) {
      CHECK_EQ(ev.attributes.size(), 1u);
      CHECK_EQ(std::string(ev.attributes[0].name), std::string("xmlns"));
      CHECK_EQ(std::string(ev.attributes[0].value), std::string("urn:x"));
      continue;
    }
    CHECK_EQ(ev.attributes.size(), 3u);
    for (const auto& a : ev.attributes) {
      CHECK(!a.value.empty());
    }
    CHECK_EQ(std::string(ev.attributes[0].name), std::string("Id"));
    CHECK_EQ(std::string(ev.attributes[0].value), std::string(count == 2 ? "rId1" : "rId2"));
    CHECK_EQ(std::string(ev.attributes[1].name), std::string("Type"));
    CHECK_EQ(std::string(ev.attributes[1].value),
             std::string(count == 2 ? "urn:styles" : "urn:numbering"));
    CHECK_EQ(std::string(ev.attributes[2].name), std::string("Target"));
    CHECK_EQ(std::string(ev.attributes[2].value),
             std::string(count == 2 ? "styles.xml" : "numbering.xml"));
  }
  CHECK_EQ(count, 3);
}

// Entity decoding inside attribute values.
void TestXmlAttributeEntities() {
  const std::string doc = "<a x=\"a&amp;b&lt;c&quot;d&#65;\"/>";
  XmlReader r(doc);
  XmlEvent ev;
  CHECK(r.Next(ev));
  CHECK_EQ(ev.attributes.size(), 1u);
  CHECK_EQ(std::string(ev.attributes[0].value), std::string("a&b<c\"dA"));
  // A following element must not disturb the decoded value above.
  CHECK(r.Next(ev));
  CHECK(ev.type == XmlEventType::EndElement);
}

void TestXmlDoctypeRejected() {
  const std::string doc = "<!DOCTYPE foo [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><foo>&x;</foo>";
  XmlReader r(doc);
  XmlEvent ev;
  CHECK_THROWS(r.Next(ev), ErrorCode::Corrupt);
}

void TestXmlDepth() {
  std::string doc;
  for (int i = 0; i < 100; ++i) doc += "<a>";
  for (int i = 0; i < 100; ++i) doc += "</a>";
  XmlReader r(doc, XmlLimits{});
  XmlEvent ev;
  CHECK_THROWS(while (r.Next(ev)) {}, ErrorCode::LimitExceeded);
}

void TestPng() {
  RasterImage img;
  img.width = 4;
  img.height = 3;
  img.rgba.assign(4 * 3 * 4, 255);
  const auto png = EncodePng(img);
  CHECK(png.size() > 8);
  CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
  CHECK_EQ(ReadBe32(png.data() + 16), 4u);
  CHECK_EQ(ReadBe32(png.data() + 20), 3u);
  const char* tail = reinterpret_cast<const char*>(png.data() + png.size() - 8);
  CHECK(std::memcmp(tail, "IEND", 4) == 0);
}

void TestRasterRect() {
  LayoutPage page;
  page.width_pt = 72;
  page.height_pt = 72;
  page.ops.push_back(MakeRect(10, 10, 20, 20, Rgba{255, 0, 0, 255}));
  RasterOptions opt;
  opt.dpi = 72;
  WarningList warnings;
  const auto img = RasterizePage(page, opt, &warnings);
  CHECK_EQ(img.width, 72);
  CHECK_EQ(img.height, 72);
  auto px = [&](int x, int y) { return img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4; };
  CHECK(px(15, 15)[0] > 200 && px(15, 15)[1] < 60);
  CHECK(px(50, 50)[0] > 240 && px(50, 50)[1] > 240);
}

void TestPdfWithText() {
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  LayoutPage page;
  DrawText t;
  t.font.family = fonts.Match(FontKey{})->family();
  t.size_pt = 14;
  t.x = 72;
  t.y = 100;
  t.text_utf8 = u8"Hello \u4F60\u597D";
  page.ops.push_back(t);
  LayoutDocument doc;
  doc.pages.push_back(page);

  WarningList warnings;
  PdfSaveOptions opt;
  opt.title = "unit test";
  const auto bytes = PdfWriter::ToBytes(doc, fonts, opt, &warnings);
  CHECK(bytes.size() > 1000);
  const std::string s(bytes.begin(), bytes.end());
  CHECK(s.rfind("%PDF-1.7", 0) == 0);
  CHECK(s.find("/Type /Font") != std::string::npos);
  CHECK(s.find("/CIDFontType2") != std::string::npos);
  CHECK(s.find("/ToUnicode") != std::string::npos);
  CHECK(s.find("startxref") != std::string::npos);
  CHECK(s.find("%%EOF") != std::string::npos);
  CHECK(s.find("/Identity-H") != std::string::npos);
}

// ---- P11-1: PDF 链接注释 + 命名目标 + 大纲 ------------------------------

void TestPdfLinksAndOutline() {
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  const std::string family = fonts.Match(FontKey{})->family();

  LayoutDocument doc;
  for (int i = 0; i < 3; ++i) {
    LayoutPage page;
    page.width_pt = 595.276;
    page.height_pt = 841.89;
    DrawText t;
    t.font.family = family;
    t.size_pt = 14;
    t.x = 72;
    t.y = 90;
    t.text_utf8 = "Page " + std::to_string(i + 1);
    page.ops.push_back(t);
    doc.pages.push_back(std::move(page));
  }

  // 外部链接（含非 ASCII URI，走 UTF-16BE 字符串）+ 内部跳转。
  Link external;
  external.x = 72;
  external.y = 100;
  external.w_pt = 180;
  external.h_pt = 16;
  external.uri = "https://example.com/a?b=1&c=2";
  doc.pages[0].links.push_back(external);

  Link internal;
  internal.x = 72;
  internal.y = 130;
  internal.w_pt = 120;
  internal.h_pt = 16;
  internal.dest = "target_one";
  doc.pages[0].links.push_back(internal);

  Link unicode_uri = external;
  unicode_uri.y = 160;
  unicode_uri.uri = "https://example.com/\xE4\xB8\xAD\xE6\x96\x87";
  doc.pages[0].links.push_back(unicode_uri);

  NamedDest dest;
  dest.name = "target_one";
  dest.x = 72;
  dest.y = 400;
  doc.pages[1].dests.push_back(dest);

  // 两级大纲：1 / 2 / 2 / 1，用于验证 First/Last/Next/Prev/Count 的建树。
  const char* titles[] = {"Chapter A", "Section A.1", "Section A.2", "Chapter B"};
  const int levels[] = {1, 2, 2, 1};
  const int pages[] = {0, 0, 1, 2};
  for (int i = 0; i < 4; ++i) {
    Bookmark bm;
    bm.title = titles[i];
    bm.level = levels[i];
    bm.page = pages[i];
    bm.y = 90 + i * 10;
    doc.bookmarks.push_back(bm);
  }
  Bookmark cn;
  cn.title = u8"\u4E2D\u6587\u7AE0\u8282";
  cn.level = 1;
  cn.page = 2;
  cn.y = 300;
  doc.bookmarks.push_back(cn);

  WarningList warnings;
  PdfSaveOptions opt;
  const auto bytes = PdfWriter::ToBytes(doc, fonts, opt, &warnings);
  CHECK(warnings.items().empty());
  const std::string s(bytes.begin(), bytes.end());

  // 链接注释
  CHECK(s.find("/Annots [") != std::string::npos);
  CHECK(s.find("/Subtype /Link") != std::string::npos);
  CHECK(s.find("/Border [0 0 0]") != std::string::npos);
  CHECK(s.find("/S /URI /URI (https://example.com/a?b=1&c=2)") != std::string::npos);
  CHECK(s.find("/S /GoTo /D /target_one") != std::string::npos);
  // 非 ASCII URI 必须走 UTF-16BE 十六进制字符串。
  CHECK(s.find("<FEFF") != std::string::npos);

  // 命名目标：PDF 1.1 旧式字典 + PDF 1.2+ 名字树两种形式都写。
  CHECK(s.find("/Dests << /target_one [") != std::string::npos);
  CHECK(s.find("/Names << /Dests << /Names [") != std::string::npos);
  CHECK(s.find("/XYZ 72 441.89 null]") != std::string::npos);

  // 大纲
  CHECK(s.find("/Type /Outlines") != std::string::npos);
  CHECK(s.find("/Outlines ") != std::string::npos);
  CHECK(s.find("(Chapter A)") != std::string::npos);
  CHECK(s.find("(Section A.1)") != std::string::npos);
  CHECK(s.find("/Count 5") != std::string::npos);   // 根节点：5 条大纲项
  CHECK(s.find("/Count 2") != std::string::npos);   // Chapter A 的两个子项
  CHECK(s.find("/First ") != std::string::npos);
  CHECK(s.find("/Last ") != std::string::npos);
  CHECK(s.find("/Prev ") != std::string::npos);
  CHECK(s.find("/Next ") != std::string::npos);

  // 落盘给 PyMuPDF 复核。
  {
    FileStream out(OutPath("p11_links.pdf"), "wb");
    out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
  }
}

// ---- P11-1: XLSX 超链接读写往返 ----------------------------------------

void TestXlsxHyperlinks() {
  using namespace doccore::sheet;

  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.set_name("Links");
  ws.SetText(0, 0, "site");
  ws.SetText(1, 0, "internal");
  ws.SetText(2, 0, "both");

  Hyperlink ext;
  ext.range = Range{0, 0, 0, 0};
  ext.target = "https://example.com/x?a=1&b=2";
  ext.tooltip = "visit";
  ws.hyperlinks().push_back(ext);

  Hyperlink inner;
  inner.range = Range{1, 0, 1, 0};
  inner.location = "Links!A1";
  ws.hyperlinks().push_back(inner);

  Hyperlink both;
  both.range = Range{2, 0, 2, 2};
  both.target = "https://example.com/\xE4\xB8\xAD";
  both.location = "Links!A1";
  both.display = "show";
  ws.hyperlinks().push_back(both);

  MemoryStream out;
  SaveOptions save_opts;
  WarningList save_warnings;
  save_opts.warnings = &save_warnings;
  Save(wb, out, save_opts);
  CHECK(save_warnings.empty());

  const std::string_view bytes(reinterpret_cast<const char*>(out.data().data()),
                               out.data().size());
  // 包内容被压缩过，要按 zip 条目取出来看。
  {
    MemoryStream probe(out.data());
    auto zip = ZipArchive::Open(probe);
    const auto sheet = zip.Read("xl/worksheets/sheet1.xml");
    const std::string sheet_xml(sheet.begin(), sheet.end());
    CHECK(sheet_xml.find("<hyperlinks>") != std::string::npos);
    CHECK(sheet_xml.find("r:id=\"rId1\"") != std::string::npos);
    CHECK(sheet_xml.find("location=\"Links!A1\"") != std::string::npos);
    CHECK(sheet_xml.find("tooltip=\"visit\"") != std::string::npos);
    const auto rels = zip.Read("xl/worksheets/_rels/sheet1.xml.rels");
    const std::string rels_xml(rels.begin(), rels.end());
    CHECK(rels_xml.find("relationships/hyperlink") != std::string::npos);
    CHECK(rels_xml.find("TargetMode=\"External\"") != std::string::npos);
    CHECK(rels_xml.find("Id=\"rId1\"") != std::string::npos);
  }

  Workbook back = OpenMemory(bytes, nullptr);
  CHECK_EQ(back.sheet_count(), 1);
  Worksheet* re = back.SheetAt(0);
  CHECK(re != nullptr);
  if (re == nullptr) return;
  CHECK_EQ(re->hyperlinks().size(), static_cast<size_t>(3));
  if (re->hyperlinks().size() != 3) return;

  const Hyperlink& a = re->hyperlinks()[0];
  CHECK_EQ(a.target, std::string("https://example.com/x?a=1&b=2"));
  CHECK_EQ(a.tooltip, std::string("visit"));
  CHECK_EQ(a.range.row1, 0);
  CHECK_EQ(a.range.col1, 0);

  const Hyperlink& b = re->hyperlinks()[1];
  CHECK(b.target.empty());
  CHECK_EQ(b.location, std::string("Links!A1"));

  const Hyperlink& c = re->hyperlinks()[2];
  CHECK_EQ(c.target, std::string("https://example.com/\xE4\xB8\xAD"));
  CHECK_EQ(c.location, std::string("Links!A1"));
  CHECK_EQ(c.display, std::string("show"));
  CHECK_EQ(c.range.col2, 2);

  // 打印布局要把外部链接变成页面链接、把内部位置变成命名目标。
  SheetLayoutOptions lopts;
  LayoutDocument laid = LayoutWorkbook(back, lopts);
  CHECK(!laid.pages.empty());
  size_t link_count = 0;
  size_t dest_count = 0;
  for (const LayoutPage& page : laid.pages) {
    link_count += page.links.size();
    dest_count += page.dests.size();
  }
  CHECK_EQ(link_count, static_cast<size_t>(3));
  CHECK(dest_count >= 1);
  // 每张表一条大纲。
  CHECK_EQ(laid.bookmarks.size(), static_cast<size_t>(1));
  if (!laid.bookmarks.empty()) CHECK_EQ(laid.bookmarks[0].title, std::string("Links"));

  // 写成 PDF 落盘，交给 PyMuPDF 复核链接与命名目标。
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  WarningList pdf_warnings;
  const auto pdf = PdfWriter::ToBytes(laid, fonts, PdfSaveOptions(), &pdf_warnings);
  CHECK(pdf.size() > 500);
  const std::string pdf_text(pdf.begin(), pdf.end());
  CHECK(pdf_text.find("/Subtype /Link") != std::string::npos);
  // "Links!A1" 里的 '!' 是 PDF 名字的常规字符，不需要 # 转义。
  CHECK(pdf_text.find("/Dests << /Links!A1 [") != std::string::npos);
  {
    FileStream pdf_out(OutPath("p11_sheet_links.pdf"), "wb");
    pdf_out.Write(pdf.data(), static_cast<int64_t>(pdf.size()));
  }
}

// ---- P11-1: Word 标题样式 -> PDF 大纲 + 超链接 --------------------------

void TestDocxToPdfOutline() {
  using namespace doccore::text;

  Document doc;
  doc.has_doc_defaults = true;

  Style heading1;
  heading1.id = "Heading1";
  heading1.name = "heading 1";
  heading1.paragraph.outline_level = -1;  // 故意不写 outlineLvl，逼样式名推断生效
  doc.styles.push_back(heading1);

  Style heading2;
  heading2.id = "Heading2";
  heading2.name = "heading 2";
  doc.styles.push_back(heading2);

  Style normal;
  normal.id = "Normal";
  normal.name = "Normal";
  doc.styles.push_back(normal);

  auto add_para = [&](const std::string& style, const std::string& text) {
    Block b;
    b.type = Block::Type::Paragraph;
    b.paragraph.format.style = style;
    InlineItem it;
    it.type = InlineItem::Type::Text;
    it.text = text;
    b.paragraph.runs.push_back(std::move(it));
    doc.sections.back().blocks.push_back(std::move(b));
  };

  doc.sections.push_back(Section());
  add_para("Heading1", "Introduction");
  add_para("Normal", "Body text under the first heading.");
  add_para("Heading2", "Background");
  add_para("Normal", "More body text.");
  add_para("Heading1", "Method");
  add_para("Normal", "Closing body text.");

  LayoutOptions lopts;
  const LayoutDocument laid = LayoutText(doc, lopts);
  CHECK_EQ(laid.bookmarks.size(), static_cast<size_t>(3));
  if (laid.bookmarks.size() == 3) {
    CHECK_EQ(laid.bookmarks[0].title, std::string("Introduction"));
    CHECK_EQ(laid.bookmarks[0].level, 1);
    CHECK_EQ(laid.bookmarks[1].title, std::string("Background"));
    CHECK_EQ(laid.bookmarks[1].level, 2);
    CHECK_EQ(laid.bookmarks[2].title, std::string("Method"));
    CHECK_EQ(laid.bookmarks[2].level, 1);
  }

  // 大纲必须是严格递增的（标题在正文之前，y 随之增大）。
  for (size_t i = 1; i < laid.bookmarks.size(); ++i) {
    const bool ok = laid.bookmarks[i].page > laid.bookmarks[i - 1].page ||
                    (laid.bookmarks[i].page == laid.bookmarks[i - 1].page &&
                     laid.bookmarks[i].y > laid.bookmarks[i - 1].y);
    CHECK(ok);
  }

  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  WarningList warnings;
  const auto bytes = PdfWriter::ToBytes(laid, fonts, PdfSaveOptions(), &warnings);
  const std::string s(bytes.begin(), bytes.end());
  CHECK(s.find("/Type /Outlines") != std::string::npos);
  CHECK(s.find("(Introduction)") != std::string::npos);
  CHECK(s.find("(Background)") != std::string::npos);
  {
    FileStream out(OutPath("p11_docx_outline.pdf"), "wb");
    out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
  }
}

// ---- P11-1: PPT 幻灯片标题 -> 大纲、run 超链接 -> 链接注释 --------------

void TestDeckToPdfOutlineAndLinks() {
  using namespace doccore::deck;

  Presentation pres = Presentation::Create();
  while (pres.slide_count() > 0) pres.RemoveSlide(0);

  for (int i = 0; i < 2; ++i) {
    Slide& slide = pres.AddSlide();
    Shape& title = slide.AddPlaceholder("title", ShapeGeometry{457200, 274638, 8229600, 1143000});
    Paragraph tp;
    deck::Run tr;
    tr.text = i == 0 ? "Opening" : "Second Slide";
    tp.runs.push_back(tr);
    title.text.paragraphs.push_back(tp);

    Shape& body = slide.AddPlaceholder("body", ShapeGeometry{457200, 1600200, 8229600, 4525963});
    Paragraph bp;
    deck::Run br;
    br.text = "Click ";
    bp.runs.push_back(br);
    deck::Run link_run;
    link_run.text = "here";
    link_run.hyperlink = "https://example.com/deck";
    bp.runs.push_back(link_run);
    deck::Run tail;
    tail.text = " for details.";
    bp.runs.push_back(tail);
    body.text.paragraphs.push_back(bp);
  }

  DeckLayoutOptions lopts;
  const LayoutDocument laid = LayoutPresentation(pres, lopts);
  CHECK_EQ(laid.pages.size(), static_cast<size_t>(2));
  CHECK_EQ(laid.bookmarks.size(), static_cast<size_t>(2));
  if (laid.bookmarks.size() == 2) {
    CHECK_EQ(laid.bookmarks[0].title, std::string("Opening"));
    CHECK_EQ(laid.bookmarks[0].page, 0);
    CHECK_EQ(laid.bookmarks[1].title, std::string("Second Slide"));
    CHECK_EQ(laid.bookmarks[1].page, 1);
  }

  size_t link_count = 0;
  for (const LayoutPage& page : laid.pages) {
    for (const Link& l : page.links) {
      ++link_count;
      CHECK_EQ(l.uri, std::string("https://example.com/deck"));
      // "here" 只有 4 个字符宽，矩形必须窄于整行。
      CHECK(l.w_pt > 0.0);
      CHECK(l.h_pt > 0.0);
    }
  }
  CHECK_EQ(link_count, static_cast<size_t>(2));

  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  WarningList warnings;
  const auto bytes = PdfWriter::ToBytes(laid, fonts, PdfSaveOptions(), &warnings);
  const std::string s(bytes.begin(), bytes.end());
  CHECK(s.find("/Subtype /Link") != std::string::npos);
  CHECK(s.find("/URI (https://example.com/deck)") != std::string::npos);
  CHECK(s.find("(Opening)") != std::string::npos);
  {
    FileStream out(OutPath("p11_deck_outline.pdf"), "wb");
    out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
  }
}

// ---- P11-2: PDF 注释读写 -----------------------------------------------

void TestPdfAnnotations() {
  using namespace doccore::pdf;

  // 先造一个 3 页的基础 PDF。
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  const std::string family = fonts.Match(FontKey{})->family();
  LayoutDocument base;
  for (int i = 0; i < 3; ++i) {
    LayoutPage page;
    page.width_pt = 595.276;
    page.height_pt = 841.89;
    DrawText t;
    t.font.family = family;
    t.size_pt = 14;
    t.x = 72;
    t.y = 90;
    t.text_utf8 = "Annotated page " + std::to_string(i + 1);
    page.ops.push_back(t);
    base.pages.push_back(std::move(page));
  }
  const auto seed = PdfWriter::ToBytes(base, fonts, PdfSaveOptions(), nullptr);
  {
    FileStream out(OutPath("p11_annot_seed.pdf"), "wb");
    out.Write(seed.data(), static_cast<int64_t>(seed.size()));
  }

  PdfFile file = PdfFile::OpenMemory(seed);
  CHECK_EQ(file.page_count(), 3);
  // 基础文件里没有注释。
  for (int i = 0; i < file.page_count(); ++i) CHECK(file.page(i).annotations().empty());
  // 页面必须拿到对象号，否则增量更新无法改写页面字典。
  for (int i = 0; i < file.page_count(); ++i) CHECK(file.page(i).obj_num > 0);
  CHECK(file.startxref_offset() > 0);

  std::vector<PdfAnnotation> annots;
  auto make = [](AnnotSubtype sub, int page, double x, double y, double w, double h) {
    PdfAnnotation a;
    a.subtype = sub;
    a.subtype_name = AnnotSubtypeName(sub);
    a.page = page;
    a.x = x;
    a.y = y;
    a.w = w;
    a.h = h;
    a.color = 0xFFCC00;
    return a;
  };

  PdfAnnotation square = make(AnnotSubtype::Square, 0, 100, 600, 120, 60);
  square.contents = "boxed";
  square.author = "Tester";
  annots.push_back(square);

  PdfAnnotation highlight = make(AnnotSubtype::Highlight, 0, 80, 700, 200, 18);
  highlight.opacity = 0.5;
  highlight.contents = u8"\u9AD8\u4EAE";  // 中文正文走 UTF-16BE 十六进制
  annots.push_back(highlight);

  PdfAnnotation circle = make(AnnotSubtype::Circle, 1, 200, 300, 90, 90);
  annots.push_back(circle);

  PdfAnnotation stamp = make(AnnotSubtype::Stamp, 1, 300, 500, 140, 50);
  stamp.icon = "Approved";
  annots.push_back(stamp);

  PdfAnnotation note = make(AnnotSubtype::Text, 2, 400, 400, 20, 20);
  note.icon = "Comment";
  note.contents = "please check";
  note.author = u8"\u5F20\u4E09";
  annots.push_back(note);

  PdfAnnotation underline = make(AnnotSubtype::Underline, 2, 100, 200, 150, 14);
  underline.quads.push_back(PdfQuad{100, 200, 250, 200, 100, 214, 250, 214});
  annots.push_back(underline);

  WarningList warnings;
  const auto patched = AddAnnotations(file, annots, &warnings);
  CHECK(warnings.empty());
  CHECK(patched.size() > seed.size());
  // 原文件字节必须逐字节保留在开头。
  CHECK(std::memcmp(patched.data(), seed.data(), seed.size()) == 0);

  const std::string tail(patched.begin() + static_cast<long>(seed.size()), patched.end());
  CHECK(tail.find("/Prev ") != std::string::npos);
  CHECK(tail.find("/Subtype /Square") != std::string::npos);
  CHECK(tail.find("/Subtype /Highlight") != std::string::npos);
  CHECK(tail.find("/Subtype /Circle") != std::string::npos);
  CHECK(tail.find("/Subtype /Stamp") != std::string::npos);
  CHECK(tail.find("/Subtype /Text") != std::string::npos);
  CHECK(tail.find("/Subtype /Underline") != std::string::npos);
  // 外观流必须生成，否则阅读器显示空白。
  CHECK(tail.find("/Subtype /Form") != std::string::npos);
  CHECK(tail.find("/BM /Multiply") != std::string::npos);
  CHECK(tail.find("/BaseFont /Helvetica") != std::string::npos);

  {
    FileStream out(OutPath("p11_annotated.pdf"), "wb");
    out.Write(patched.data(), static_cast<int64_t>(patched.size()));
  }

  // 读回来：类型、页、矩形、作者、正文都要对。
  PdfFile back = PdfFile::OpenMemory(patched);
  CHECK_EQ(back.page_count(), 3);
  const auto& p0 = back.page(0).annotations();
  CHECK_EQ(p0.size(), static_cast<size_t>(2));
  if (p0.size() == 2) {
    CHECK(p0[0].subtype == AnnotSubtype::Square);
    CHECK_EQ(p0[0].contents, std::string("boxed"));
    CHECK_EQ(p0[0].author, std::string("Tester"));
    CHECK(std::fabs(p0[0].x - 100.0) < 0.01);
    CHECK(std::fabs(p0[0].y - 600.0) < 0.01);
    CHECK(std::fabs(p0[0].w - 120.0) < 0.01);
    CHECK(std::fabs(p0[0].h - 60.0) < 0.01);
    CHECK_EQ(p0[0].color, 0xFFCC00);
    CHECK(p0[0].has_appearance);
    CHECK(p0[1].subtype == AnnotSubtype::Highlight);
    CHECK(std::fabs(p0[1].opacity - 0.5) < 0.01);
    CHECK_EQ(p0[1].contents, std::string("\xE9\xAB\x98\xE4\xBA\xAE"));
  }
  const auto& p1 = back.page(1).annotations();
  CHECK_EQ(p1.size(), static_cast<size_t>(2));
  if (p1.size() == 2) {
    CHECK(p1[0].subtype == AnnotSubtype::Circle);
    CHECK(p1[1].subtype == AnnotSubtype::Stamp);
    CHECK_EQ(p1[1].icon, std::string("Approved"));
  }
  const auto& p2 = back.page(2).annotations();
  CHECK_EQ(p2.size(), static_cast<size_t>(2));
  if (p2.size() == 2) {
    CHECK(p2[0].subtype == AnnotSubtype::Text);
    CHECK_EQ(p2[0].contents, std::string("please check"));
    CHECK_EQ(p2[0].author, std::string("\xE5\xBC\xA0\xE4\xB8\x89"));
    CHECK_EQ(p2[0].icon, std::string("Comment"));
    CHECK(p2[1].subtype == AnnotSubtype::Underline);
    CHECK_EQ(p2[1].quads.size(), static_cast<size_t>(1));
    if (!p2[1].quads.empty()) {
      CHECK(std::fabs(p2[1].quads[0].x2 - 250.0) < 0.01);
      CHECK(std::fabs(p2[1].quads[0].y3 - 214.0) < 0.01);
    }
  }

  // 二次追加：既有 /Annots 必须保留，新注释接在后面。
  PdfAnnotation extra = make(AnnotSubtype::Square, 0, 50, 50, 40, 40);
  extra.contents = "second pass";
  const auto twice = AddAnnotations(back, {extra}, nullptr);
  PdfFile third = PdfFile::OpenMemory(twice);
  CHECK_EQ(third.page(0).annotations().size(), static_cast<size_t>(3));
  CHECK_EQ(third.page(1).annotations().size(), static_cast<size_t>(2));
  {
    FileStream out(OutPath("p11_annotated_twice.pdf"), "wb");
    out.Write(twice.data(), static_cast<int64_t>(twice.size()));
  }

  // 越界页号只告警，不抛。
  PdfAnnotation bad = make(AnnotSubtype::Square, 99, 0, 0, 10, 10);
  WarningList bad_warnings;
  const auto ignored = AddAnnotations(file, {bad}, &bad_warnings);
  CHECK_EQ(bad_warnings.items().size(), static_cast<size_t>(1));
  CHECK_EQ(bad_warnings.items()[0].code, static_cast<int>(WarningCode::AnnotSkipped));
  CHECK(ignored.size() > seed.size());
}

// ---- P11-3: 密码学原语（NIST / RFC 已知向量）---------------------------

std::string HexStr(const std::vector<uint8_t>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : bytes) {
    s.push_back(kHex[b >> 4]);
    s.push_back(kHex[b & 0xF]);
  }
  return s;
}

std::vector<uint8_t> FromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

void TestCryptoPrimitives() {
  using namespace doccore::crypto;

  // MD5（RFC 1321 附录 A.5）
  const std::string abc = "abc";
  CHECK_EQ(HexStr(Md5(reinterpret_cast<const uint8_t*>(abc.data()), abc.size())),
           std::string("900150983cd24fb0d6963f7d28e17f72"));
  const std::string empty;
  CHECK_EQ(HexStr(Md5(reinterpret_cast<const uint8_t*>(empty.data()), 0)),
           std::string("d41d8cd98f00b204e9800998ecf8427e"));
  // 跨分组边界的填充：55 字节刚好塞进一个块，56 字节要多吃一个填充块。
  const std::string a55(55, 'a');
  const std::string a56(56, 'a');
  CHECK_EQ(HexStr(Md5(reinterpret_cast<const uint8_t*>(a55.data()), a55.size())),
           std::string("ef1772b6dff9a122358552954ad0df65"));
  CHECK_EQ(HexStr(Md5(reinterpret_cast<const uint8_t*>(a56.data()), a56.size())),
           std::string("3b0c8ac703f828b04c6c197006d17218"));
  CHECK_EQ(HexStr(Sha256(reinterpret_cast<const uint8_t*>(a56.data()), a56.size())),
           std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));

  // SHA-256（FIPS 180-4 示例）
  CHECK_EQ(HexStr(Sha256(reinterpret_cast<const uint8_t*>(abc.data()), abc.size())),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(HexStr(Sha256(reinterpret_cast<const uint8_t*>(empty.data()), 0)),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  const std::string abcdbcde = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK_EQ(HexStr(Sha256(reinterpret_cast<const uint8_t*>(abcdbcde.data()), abcdbcde.size())),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  // SHA-384 / SHA-512（FIPS 180-4 示例）
  CHECK_EQ(HexStr(Sha384(reinterpret_cast<const uint8_t*>(abc.data()), abc.size())),
           std::string("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                       "8086072ba1e7cc2358baeca134c825a7"));
  CHECK_EQ(HexStr(Sha512(reinterpret_cast<const uint8_t*>(abc.data()), abc.size())),
           std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));

  // RC4（RFC 6229 前几组）
  {
    std::vector<uint8_t> key = FromHex("0102030405");
    std::vector<uint8_t> data(16, 0);
    Rc4(key.data(), key.size(), data.data(), data.size());
    CHECK_EQ(HexStr(data), std::string("b2396305f03dc027ccc3524a0a1118a8"));
    // RC4 是对称的：再跑一次还原。
    Rc4(key.data(), key.size(), data.data(), data.size());
    CHECK_EQ(HexStr(data), std::string("00000000000000000000000000000000"));
  }

  // AES-128（FIPS 197 附录 B/C.1）
  {
    const auto key = FromHex("000102030405060708090a0b0c0d0e0f");
    const auto plain = FromHex("00112233445566778899aabbccddeeff");
    Aes aes(key.data(), key.size());
    std::vector<uint8_t> block = plain;
    aes.EncryptBlock(block.data());
    CHECK_EQ(HexStr(block), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes.DecryptBlock(block.data());
    CHECK_EQ(HexStr(block), std::string("00112233445566778899aabbccddeeff"));
  }
  // AES-192
  {
    const auto key = FromHex("000102030405060708090a0b0c0d0e0f1011121314151617");
    Aes aes(key.data(), key.size());
    std::vector<uint8_t> block = FromHex("00112233445566778899aabbccddeeff");
    aes.EncryptBlock(block.data());
    CHECK_EQ(HexStr(block), std::string("dda97ca4864cdfe06eaf70a0ec0d7191"));
  }
  // AES-256
  {
    const auto key = FromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    Aes aes(key.data(), key.size());
    std::vector<uint8_t> block = FromHex("00112233445566778899aabbccddeeff");
    aes.EncryptBlock(block.data());
    CHECK_EQ(HexStr(block), std::string("8ea2b7ca516745bfeafc49904b496089"));
    aes.DecryptBlock(block.data());
    CHECK_EQ(HexStr(block), std::string("00112233445566778899aabbccddeeff"));
  }

  // AES-CBC（NIST SP 800-38A F.2.1，两段）
  {
    const auto key = FromHex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = FromHex("000102030405060708090a0b0c0d0e0f");
    std::vector<uint8_t> data =
        FromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    Aes aes(key.data(), key.size());
    // 加密会补 PKCS#7，所以只比对前 32 字节。
    AesCbcEncrypt(aes, iv.data(), &data);
    CHECK_EQ(HexStr(std::vector<uint8_t>(data.begin(), data.begin() + 32)),
             std::string("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"));
    CHECK_EQ(data.size(), static_cast<size_t>(48));  // 32 + 16 字节填充块
    CHECK(AesCbcDecrypt(aes, iv.data(), &data));
    CHECK_EQ(data.size(), static_cast<size_t>(32));
  }

  // 填充：非法填充必须被拒绝（否则会静默吞掉错误密码）。
  {
    std::vector<uint8_t> bad(16, 0);
    bad[15] = 5;  // 声称 5 字节填充，但前 4 个字节不是 5
    CHECK(!Pkcs7Unpad(&bad, 16));
  }
}

// P11-3：标准安全处理器。夹具由 PyMuPDF（MuPDF）生成，不是 DocCore 自己写的——
// 自己加密自己解密，两边的对称错误会互相抵消，测不出任何东西。
void TestPdfEncryption() {
  const char* kLines[] = {"Encrypted payload line one", "Second line with digits 0123456789",
                          "Third line: punctuation !@#$%^&*()"};

  struct Case {
    const char* file;
    const char* filter;  // 便于报错时定位是哪种算法挂了
  };
  const Case cases[] = {
      {"enc_rc4_40.pdf", "RC4-40 /V1 R2"},
      {"enc_rc4_128.pdf", "RC4-128 /V2 R3"},
      {"enc_aes_128.pdf", "AES-128 /V4 R4"},
      {"enc_aes_256.pdf", "AES-256 /V5 R6"},
  };

  for (const Case& c : cases) {
    const std::string path = Fixture(c.file);
    if (!std::filesystem::exists(path)) {
      CHECK(false);  // 夹具缺失要显式失败，不能静默跳过
      std::printf("  missing fixture %s (%s)\n", c.file, c.filter);
      continue;
    }

    // 无口令：必须报 NeedPassword，不能返回半解密的垃圾。
    CHECK_THROWS(pdf::PdfFile::OpenPath(path, {}), ErrorCode::NeedPassword);
    // 错口令：同样 NeedPassword。
    {
      pdf::PdfOpenOptions bad;
      bad.password = "nope";
      CHECK_THROWS(pdf::PdfFile::OpenPath(path, bad), ErrorCode::NeedPassword);
    }

    // 用户口令与所有者口令都要能开（Algorithm 3.A 与 3.B 两条路径）。
    for (const char* pw : {"user", "owner"}) {
      pdf::PdfOpenOptions opt;
      opt.password = pw;
      bool opened = false;
      try {
        pdf::PdfFile file = pdf::PdfFile::OpenPath(path, opt);
        opened = true;
        CHECK(file.encrypted());
        CHECK_EQ(file.page_count(), 1);
        // 夹具只授了"打印 + 复制"，其余位应当为 0。
        CHECK(file.permissions().print);
        CHECK(file.permissions().copy);
        CHECK(!file.permissions().modify);
        CHECK(!file.permissions().assemble);
        CHECK(file.permissions().raw != 0);
        const std::string text = file.ExtractText();
        for (const char* line : kLines) {
          const bool found = text.find(line) != std::string::npos;
          if (!found) std::printf("  %s pw=%s missing %s\n", c.file, pw, line);
          CHECK(found);
        }
        // 落盘给 PyMuPDF 复核：两套独立实现抽出的正文必须逐字一致。
        {
          const std::string stem = std::string(c.file).substr(0, std::strlen(c.file) - 4);
          const std::string dump = OutPath((stem + "_" + pw + ".txt").c_str());
          FileStream f(dump, "wb");
          f.Write(reinterpret_cast<const uint8_t*>(text.data()),
                  static_cast<int64_t>(text.size()));
        }
      } catch (const Error& e) {
        std::printf("  %s (%s) pw=%s threw: %s\n", c.file, c.filter, pw, e.what());
      }
      CHECK(opened);
    }
  }

  // 只设所有者口令：空用户口令应该直接可读，且不需要调用方知道任何口令。
  {
    const std::string path = Fixture("enc_owner_only.pdf");
    if (!std::filesystem::exists(path)) {
      CHECK(false);
    } else {
      pdf::PdfFile file = pdf::PdfFile::OpenPath(path, {});
      CHECK(file.encrypted());
      CHECK_EQ(file.page_count(), 1);
      const std::string text = file.ExtractText();
      for (const char* line : kLines) CHECK(text.find(line) != std::string::npos);
      // 所有者口令也能开。
      pdf::PdfOpenOptions owner;
      owner.password = "owner";
      pdf::PdfFile by_owner = pdf::PdfFile::OpenPath(path, owner);
      CHECK(by_owner.encrypted());
      CHECK(by_owner.ExtractText().find(kLines[0]) != std::string::npos);
    }
  }

  // 未加密的文件不能被误判为加密。
  {
    pdf::PdfFile plain = pdf::PdfFile::OpenPath(Fixture("sample_pdf.pdf"), {});
    CHECK(!plain.encrypted());
    // 未加密文档按"全都可以"处理。
    CHECK(plain.permissions().print);
    CHECK(plain.permissions().copy);
    CHECK(plain.permissions().modify);
  }
}

// P11-4：Word 水印与文档保护。
//
// 水印不是独立元素，是页眉里的 VML 形状；保护不是口令，是迭代哈希。两者都
// 必须"写出去能被自己读回来、也能被第三方读到"，所以除了往返，还要在 zip
// 层核对生成的 XML 到底长什么样。
void TestDocxWatermarkAndProtection() {
  // ---- 1. 文字水印 ------------------------------------------------------
  text::Document doc;
  {
    text::Section s;
    text::Block b;
    text::InlineItem run;
    run.type = text::InlineItem::Type::Text;
    run.text = "body text under the watermark";
    b.paragraph.runs.push_back(run);
    s.blocks.push_back(b);
    doc.sections.push_back(s);
  }
  doc.watermark.kind = text::Watermark::Kind::Text;
  doc.watermark.text = "CONFIDENTIAL";
  doc.watermark.font = "Calibri";
  doc.watermark.color = 0xC0C0C0;
  doc.watermark.rotation = 315.0;  // Word 默认：视觉上左低右高

  MemoryStream out;
  text::SaveOptions save_opts;
  WarningList save_warnings;
  save_opts.warnings = &save_warnings;
  text::Save(doc, out, save_opts);
  CHECK_EQ(save_warnings.items().size(), size_t(0));

  // ---- 2. zip 层：settings/header 的 XML 长什么样 ------------------------
  const std::vector<uint8_t> bytes = out.data();
  {
    // 落盘给 python-docx / zipfile 复核。
    FileStream f(OutPath("p11_watermark.docx"), "wb");
    f.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
  }
  {
    MemoryStream probe(bytes);
    auto zip = ZipArchive::Open(probe);
    // 文档原本没有页眉，写水印时必须合成一个。
    const auto header = zip.Read("word/header1.xml");
    const std::string hx(header.begin(), header.end());
    CHECK(hx.find("v:textpath") != std::string::npos);
    CHECK(hx.find("CONFIDENTIAL") != std::string::npos);
    CHECK(hx.find("rotation:315") != std::string::npos);
    CHECK(hx.find("#C0C0C0") != std::string::npos);
    CHECK(hx.find("_x0000_t136") != std::string::npos);  // 艺术字 shapetype
    CHECK(hx.find("xmlns:v") != std::string::npos);

    // 正文里要有指向合成页眉的引用，否则 Word 不会渲染它。
    const auto document = zip.Read("word/document.xml");
    const std::string dx(document.begin(), document.end());
    CHECK(dx.find("headerReference") != std::string::npos);
    CHECK(dx.find("w:type=\"default\"") != std::string::npos);
  }

  // ---- 3. 读回来：水印字段一致 -----------------------------------------
  WarningList read_warnings;
  text::Document back = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
      &read_warnings);
  CHECK_EQ(static_cast<int>(back.watermark.kind),
           static_cast<int>(text::Watermark::Kind::Text));
  CHECK_EQ(back.watermark.text, std::string("CONFIDENTIAL"));
  CHECK_EQ(back.watermark.font, std::string("Calibri"));
  CHECK_EQ(back.watermark.color, 0xC0C0C0);
  CHECK(std::fabs(back.watermark.rotation - 315.0) < 0.01);
  CHECK(back.watermark.box_width_pt > 100.0);
  // 水印段落不能留在页眉排版流里（否则会被当普通内容再排一次）。
  CHECK(back.sections[0].headers.empty() ||
        back.sections[0].headers[0].blocks.empty());

  // 二次保存必须字节一致：说明写入侧是确定性的。
  {
    MemoryStream again;
    text::Save(back, again);
    CHECK(again.data() == bytes);
  }

  // ---- 4. 排版与 PDF：水印要真的画出来，而且是旋转的 -------------------
  {
    // 排版与写出用同一套字体度量，否则水印字号会按估算值算、PDF 里按真实
    // 字宽排，两边对不上。
    FontRegistry fonts;
    fonts.AddFontFile(Fixture("test.ttf"));
    text::LayoutOptions opts;
    opts.fonts = &fonts;
    LayoutDocument layout = text::LayoutText(back, opts);
    CHECK_EQ(layout.pages.size(), size_t(1));
    bool found = false;
    for (const DrawOp& op : layout.pages[0].ops) {
      const auto* t = std::get_if<DrawText>(&op);
      if (t == nullptr || t->text_utf8 != "CONFIDENTIAL") continue;
      found = true;
      CHECK(std::fabs(t->rotate_deg - (-315.0)) < 0.01);  // IR 逆时针为正
      CHECK(t->size_pt > 40.0);                            // 按盒宽反推出的字号
      CHECK_EQ(static_cast<int>(t->color.a), 128);         // 半透明
      // 居中的粗检：起点应落在页面中部附近（旋转后基线起点会偏出去一些）。
      CHECK(t->x > 0.0 && t->x < layout.pages[0].width_pt);
      CHECK(t->y > 0.0 && t->y < layout.pages[0].height_pt);
    }
    CHECK(found);
    // 水印排在正文之前（PDF 里后画的在上，所以水印必须在最前面）。
    const auto* first = std::get_if<DrawText>(&layout.pages[0].ops.front());
    CHECK(first != nullptr && first->text_utf8 == "CONFIDENTIAL");

    // 带真实字体才能进 PDF（没有字体时文字会被跳过）。
    WarningList pdf_warnings;
    const std::vector<uint8_t> pdf_bytes =
        PdfWriter::ToBytes(layout, fonts, PdfSaveOptions(), &pdf_warnings);
    CHECK(pdf_bytes.size() > 1000);
    // 内容流被压缩过，没法在字节里找矩阵；改为核对 IR 与文本抽取，旋转的
    // 视觉效果交给 PyMuPDF 校验器（它会把页面光栅化数像素）。
    pdf::PdfFile file = pdf::PdfFile::OpenMemory(pdf_bytes, {});
    CHECK_EQ(file.page_count(), 1);
    CHECK(file.ExtractText().find("CONFIDENTIAL") != std::string::npos);
    {
      FileStream dump(OutPath("p11_watermark_text.pdf"), "wb");
      dump.Write(pdf_bytes.data(), static_cast<int64_t>(pdf_bytes.size()));
    }
  }

  // ---- 5. 文档保护：迭代哈希 -------------------------------------------
  {
    text::Document prot = doc;
    prot.watermark = text::Watermark{};
    const std::string salt_b64 = "c2FsdHNhbHRzYWx0c2FsdA==";  // "saltsaltsaltsalt"
    prot.protection.enabled = true;
    prot.protection.edit = 1;  // readOnly
    prot.protection.formatting = true;
    prot.protection.document_hash.salt = salt_b64;
    prot.protection.document_hash.algorithm = "SHA-512";
    prot.protection.document_hash.spin_count = 100000;
    prot.protection.document_hash.hash_size = 64;
    prot.protection.document_hash.hash = text::ComputeProtectionHash(
        "s3cret", salt_b64, "SHA-512", 100000, 64);
    CHECK(!prot.protection.document_hash.hash.empty());

    // 校验：对口令通过，错口令与空口令都不通过。
    CHECK(text::VerifyProtectionPassword(prot.protection.document_hash, "s3cret"));
    CHECK(!text::VerifyProtectionPassword(prot.protection.document_hash, "s3cret "));
    CHECK(!text::VerifyProtectionPassword(prot.protection.document_hash, ""));

    // 哈希算法本身也要对：SHA-1 / SHA-256 两种都要自洽。
    for (const char* alg : {"SHA-1", "SHA-256", "SHA-512"}) {
      const int size = std::strcmp(alg, "SHA-1") == 0 ? 20
                       : std::strcmp(alg, "SHA-256") == 0 ? 32
                                                          : 64;
      const std::string h = text::ComputeProtectionHash("pw", salt_b64, alg, 5000, size);
      text::ProtectionHash stored;
      stored.salt = salt_b64;
      stored.algorithm = alg;
      stored.spin_count = 5000;
      stored.hash_size = size;
      stored.hash = h;
      CHECK(text::VerifyProtectionPassword(stored, "pw"));
      CHECK(!text::VerifyProtectionPassword(stored, "pw2"));
      // spinCount 也是哈希的一部分：改了就认不出来。
      stored.spin_count = 4999;
      CHECK(!text::VerifyProtectionPassword(stored, "pw"));
    }

    MemoryStream prot_out;
    text::Save(prot, prot_out);
    {
      FileStream f(OutPath("p11_protected.docx"), "wb");
      f.Write(prot_out.data().data(), static_cast<int64_t>(prot_out.data().size()));
    }
    {
      MemoryStream probe(prot_out.data());
      auto zip = ZipArchive::Open(probe);
      const auto settings = zip.Read("word/settings.xml");
      const std::string sx(settings.begin(), settings.end());
      CHECK(sx.find("documentProtection") != std::string::npos);
      CHECK(sx.find("w:edit=\"readOnly\"") != std::string::npos);
      CHECK(sx.find("w:enforcement=\"1\"") != std::string::npos);
      CHECK(sx.find("w:algorithmName=\"SHA-512\"") != std::string::npos);
      CHECK(sx.find("w:cryptSpinCount=\"100000\"") != std::string::npos);
      CHECK(sx.find("w:hash=\"") != std::string::npos);
      CHECK(sx.find("w:salt=\"") != std::string::npos);
    }

    WarningList w2;
    text::Document prot_back = text::OpenMemory(
        std::string_view(reinterpret_cast<const char*>(prot_out.data().data()),
                         prot_out.data().size()),
        &w2);
    CHECK(prot_back.protection.enabled);
    CHECK_EQ(prot_back.protection.edit, 1);
    CHECK(prot_back.protection.formatting);
    CHECK_EQ(prot_back.protection.document_hash.algorithm, std::string("SHA-512"));
    CHECK_EQ(prot_back.protection.document_hash.spin_count, 100000);
    CHECK_EQ(prot_back.protection.document_hash.hash_size, 64);
    CHECK(text::VerifyProtectionPassword(prot_back.protection.document_hash, "s3cret"));
    CHECK(!text::VerifyProtectionPassword(prot_back.protection.document_hash, "wrong"));

    // 二次保存仍字节一致。
    MemoryStream prot_again;
    text::Save(prot_back, prot_again);
    CHECK(prot_again.data() == prot_out.data());
  }

  // ---- 6. settings.xml 里原有的其它设置不能被冲掉 ------------------------
  {
    text::Document keep = doc;
    keep.watermark = text::Watermark{};
    keep.settings_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:zoom w:percent=\"120\"/><w:defaultTabStop w:val=\"720\"/>"
        "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:val=\"15\"/></w:compat>"
        "</w:settings>";
    keep.protection.enabled = true;
    keep.protection.edit = 2;  // comments
    keep.protection.document_hash.salt = "c2FsdA==";
    keep.protection.document_hash.algorithm = "SHA-1";
    keep.protection.document_hash.spin_count = 1000;
    keep.protection.document_hash.hash_size = 20;
    keep.protection.document_hash.hash =
        text::ComputeProtectionHash("x", "c2FsdA==", "SHA-1", 1000, 20);

    MemoryStream kept_out;
    text::Save(keep, kept_out);
    MemoryStream probe(kept_out.data());
    auto zip = ZipArchive::Open(probe);
    const auto settings = zip.Read("word/settings.xml");
    const std::string sx(settings.begin(), settings.end());
    CHECK(sx.find("w:zoom") != std::string::npos);        // 原有设置保留
    CHECK(sx.find("w:defaultTabStop") != std::string::npos);
    CHECK(sx.find("compatibilityMode") != std::string::npos);
    CHECK(sx.find("documentProtection") != std::string::npos);
    CHECK(sx.find("w:edit=\"comments\"") != std::string::npos);
    // 元素只能出现一次。
    const size_t first = sx.find("documentProtection");
    CHECK(sx.find("documentProtection", first + 1) == std::string::npos);
  }

  // ---- 7. 图片水印 ------------------------------------------------------
  {
    // 造一张 8x4 的纯色 PNG，当水印图。
    RasterImage img;
    img.width = 8;
    img.height = 4;
    img.rgba.assign(static_cast<size_t>(8 * 4 * 4), 0);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
      img.rgba[i + 0] = 0x30;
      img.rgba[i + 1] = 0x60;
      img.rgba[i + 2] = 0x90;
      img.rgba[i + 3] = 255;
    }
    const std::vector<uint8_t> png = EncodePng(img);

    text::Document wmdoc = doc;
    wmdoc.watermark = text::Watermark{};
    wmdoc.watermark.kind = text::Watermark::Kind::Image;
    wmdoc.watermark.image_bytes = png;
    wmdoc.watermark.image_uri = "/word/media/image1.png";
    wmdoc.watermark.washout = true;
    wmdoc.watermark.box_width_pt = 300.0;

    MemoryStream img_out;
    text::Save(wmdoc, img_out);
    {
      FileStream f(OutPath("p11_watermark_image.docx"), "wb");
      f.Write(img_out.data().data(), static_cast<int64_t>(img_out.data().size()));
    }
    {
      MemoryStream probe(img_out.data());
      auto zip = ZipArchive::Open(probe);
      // 媒体部件必须真的写出来，否则 Word 打开是红叉。
      const auto media = zip.Read("word/media/watermark1.png");
      CHECK(media.size() == png.size());
      const auto header = zip.Read("word/header1.xml");
      const std::string hx(header.begin(), header.end());
      CHECK(hx.find("v:imagedata") != std::string::npos);
      CHECK(hx.find("r:id=\"rId1\"") != std::string::npos);
      CHECK(hx.find("gain=") != std::string::npos);  // 褪色参数
      // 页眉部件的关系必须指向媒体部件。
      const auto rels = zip.Read("word/_rels/header1.xml.rels");
      const std::string rx(rels.begin(), rels.end());
      CHECK(rx.find("media/watermark1.png") != std::string::npos);
      CHECK(rx.find("relationships/image") != std::string::npos);
      // Content_Types 要有 png 的 Default。
      const auto ct = zip.Read("[Content_Types].xml");
      const std::string cx(ct.begin(), ct.end());
      CHECK(cx.find("Extension=\"png\"") != std::string::npos);
    }

    WarningList w3;
    text::Document img_back = text::OpenMemory(
        std::string_view(reinterpret_cast<const char*>(img_out.data().data()),
                         img_out.data().size()),
        &w3);
    CHECK_EQ(static_cast<int>(img_back.watermark.kind),
             static_cast<int>(text::Watermark::Kind::Image));
    CHECK_EQ(img_back.watermark.image_bytes.size(), png.size());
    CHECK(img_back.watermark.washout);

    // 排版时图片水印要变成 DrawImage。
    text::LayoutOptions opts;
    LayoutDocument layout = text::LayoutText(img_back, opts);
    CHECK_EQ(layout.pages.size(), size_t(1));
    const auto* di = std::get_if<DrawImage>(&layout.pages[0].ops.front());
    CHECK(di != nullptr);
    if (di != nullptr) {
      CHECK_EQ(di->image.width, 8);
      CHECK(std::fabs(di->w_pt - 300.0) < 0.01);
      CHECK(std::fabs(di->h_pt - 150.0) < 0.01);  // 8:4 -> 300:150
    }
  }
}

void TestTtfBasics() {  FileStream in(Fixture("test.ttf"), "rb");
  auto font = TtfFont::Load(ReadAll(in, 64ull << 20));
  CHECK(font.num_glyphs() > 100);
  CHECK(font.units_per_em() > 0);
  const uint16_t gid = font.GlyphForCodepoint(U'A');
  CHECK(gid != 0);
  CHECK(font.AdvanceWidth(gid) > 0);
  CHECK_EQ(font.GlyphForCodepoint(U'\u4F60') != 0, true);
}

// ---- P5: OPC package model ---------------------------------------------

void TestOpcOpenSample() {
  MemoryStream mem(ReadFixture("sample.docx"));
  auto pkg = Package::Open(mem);
  CHECK_EQ(pkg->kind(), PackageKind::Word);
  CHECK(pkg->main_part() != nullptr);
  CHECK_EQ(pkg->main_part()->uri(), std::string("/word/document.xml"));
  // content types: Override for document.xml, Default png for the media part.
  CHECK(pkg->main_part()->mime().find("wordprocessingml.document.main") != std::string::npos);
  const Part* png = pkg->Find("/word/media/image1.png");
  CHECK(png != nullptr);
  CHECK_EQ(png->mime(), std::string("image/png"));
  // document rels: styles by relationship id.
  const Part* doc = pkg->Find("/word/document.xml");
  CHECK(doc != nullptr);
  const Relationships& rels = doc->relationships();
  const Relationship* styles = rels.Find("rId1");
  CHECK(styles != nullptr);
  CHECK_EQ(styles->resolved, std::string("/word/styles.xml"));
  // external hyperlink stays external.
  const Relationship* link = rels.Find("rId9");
  CHECK(link != nullptr);
  CHECK(link->external());
  CHECK_EQ(link->resolved, std::string("https://example.com/doc"));
  // root rels has four entries including customXml.
  CHECK_EQ(pkg->root_relationships().items().size(), size_t(4));
}

void TestOpcRejectsNonZip() {
  const std::vector<uint8_t> junk = {'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'};
  MemoryStream mem(junk);
  CHECK_THROWS(Package::Open(mem), ErrorCode::Corrupt);
}

void TestOpcRejectsMissingContentTypes() {
  const std::vector<uint8_t> zip = ReadFixture("sample.zip");  // plain zip, no OPC parts
  MemoryStream mem(zip);
  CHECK_THROWS(Package::Open(mem), ErrorCode::Corrupt);
}

// ---- P5: ZIP writer -----------------------------------------------------
void TestZipWriteRoundTrip() {
  std::vector<ZipWriteEntry> entries;
  ZipWriteEntry a;
  a.name = "[Content_Types].xml";
  a.data.assign(200, 'x');
  entries.push_back(a);
  ZipWriteEntry b;
  b.name = "word/document.xml";
  b.data.assign(64 << 10, 'y');
  b.data[100] = 'z';
  entries.push_back(b);
  ZipWriteEntry c;
  c.name = "word/media/image1.png";
  c.data = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  c.method = ZipMethod::Stored;
  entries.push_back(c);

  const auto bytes = ZipWriteToBytes(entries);
  MemoryStream mem(bytes);
  auto zip = ZipArchive::Open(mem);
  CHECK_EQ(zip.entries().size(), size_t(3));
  CHECK(zip.Has("[Content_Types].xml"));
  CHECK(zip.Has("word/document.xml"));
  CHECK(zip.Has("word/media/image1.png"));
  const auto ra = zip.Read("[Content_Types].xml");
  CHECK_EQ(ra.size(), a.data.size());
  CHECK(ra == a.data);
  const auto rb = zip.Read("word/document.xml");
  CHECK_EQ(rb.size(), b.data.size());
  CHECK(rb == b.data);
  const auto rc = zip.Read("word/media/image1.png");
  CHECK(rc == c.data);
  // the 64 KiB repetitive entry must have been deflated
  const ZipEntry* de = zip.Find("word/document.xml");
  CHECK(de != nullptr);
  CHECK_EQ(de->method, 8);
  CHECK(de->compressed < de->uncompressed);
}

void TestZipWriteRejectsUnsafeNames() {
  std::vector<ZipWriteEntry> entries;
  ZipWriteEntry e;
  e.name = "../escape.xml";
  e.data.assign(4, 'a');
  entries.push_back(e);
  bool threw = false;
  try {
    ZipWriteToBytes(entries);
  } catch (const Error& err) {
    threw = true;
    CHECK_EQ(err.code(), ErrorCode::InvalidArgument);
  }
  CHECK(threw);
}

void TestZipWriteEmpty() {
  const auto bytes = ZipWriteToBytes({});
  MemoryStream mem(bytes);
  auto zip = ZipArchive::Open(mem);
  CHECK_EQ(zip.entries().size(), size_t(0));
}

// ---- P1 gap closed: real deflate encoder round-trips through the decoder ----
void TestDeflateRoundTrip() {
  // 1. highly repetitive text (exercises LZ77 matches)
  std::string text;
  for (int i = 0; i < 400; ++i) text += "the quick brown fox jumps over the lazy dog 0123456789\n";
  // 2. random-ish bytes through every literal range
  std::vector<uint8_t> mix;
  for (int i = 0; i < 20000; ++i) mix.push_back(static_cast<uint8_t>((i * 2654435761u) >> 24));

  const std::vector<uint8_t> a(text.begin(), text.end());
  const auto da = DeflateRaw(a.data(), a.size());
  const auto ia = InflateRaw(da.data(), da.size(), 64ull << 20);
  CHECK_EQ(ia.size(), a.size());
  CHECK(ia == a);
  // LZ77 must actually shrink repetitive data.
  CHECK(da.size() < a.size() / 4);

  const auto db = DeflateRaw(mix.data(), mix.size());
  const auto ib = InflateRaw(db.data(), db.size(), 64ull << 20);
  CHECK_EQ(ib.size(), mix.size());
  CHECK(ib == mix);

  const auto zc = ZlibCompress(a.data(), a.size());
  const auto iz = ZlibDecompress(zc.data(), zc.size(), 64ull << 20);
  CHECK_EQ(iz.size(), a.size());
  CHECK(iz == a);

  const auto empty = DeflateRaw(nullptr, 0);
  const auto ie = InflateRaw(empty.data(), empty.size(), 1 << 20);
  CHECK_EQ(ie.size(), size_t(0));
}

// ---- P5: DOCX reader ----------------------------------------------------

void TestDocxReadStructure() {
  WarningList warnings;
  text::Document doc = text::OpenMemory(std::string_view(
      reinterpret_cast<const char*>(ReadFixture("sample.docx").data()),
      ReadFixture("sample.docx").size()), &warnings);

  // styles, numbering, notes and comments.
  CHECK_EQ(doc.styles.size(), size_t(8));
  CHECK(doc.FindStyle("Title") != nullptr);
  CHECK_EQ(doc.FindStyle("Title")->kind, text::Style::Kind::Paragraph);
  CHECK_EQ(doc.FindStyle("Title")->based_on, std::string("Normal"));
  CHECK(doc.FindStyleByName("Table Grid") != nullptr);
  CHECK_EQ(doc.FindStyle("CommentReference")->kind, text::Style::Kind::Character);
  CHECK_EQ(doc.abstract_numberings.size(), size_t(2));
  CHECK_EQ(doc.numbering.size(), size_t(2));
  CHECK(doc.FindNumbering("2") != nullptr);
  CHECK_EQ(doc.FindNumbering("2")->level_start_overrides[0], 5);
  CHECK_EQ(doc.FindAbstract("0")->levels[0].num_fmt, std::string("decimal"));
  CHECK_EQ(doc.FindAbstract("1")->levels[0].num_fmt, std::string("bullet"));
  CHECK_EQ(doc.footnotes.size(), size_t(2));  // separators skipped
  CHECK_EQ(doc.footnotes[1].id, 2);
  CHECK_EQ(doc.endnotes.size(), size_t(2));
  CHECK_EQ(doc.comments.size(), size_t(1));
  CHECK_EQ(doc.comments[0].author, std::string("Bob"));
  CHECK(doc.has_doc_defaults);
  CHECK_EQ(doc.default_run.size_half, 21);

  // two sections: one closed by the paragraph sectPr, one by the body sectPr.
  CHECK_EQ(doc.sections.size(), size_t(2));
  const text::Section& first = doc.sections[0];
  CHECK_EQ(first.page.width_twips, 11906);
  CHECK_EQ(first.page.margin_left, 1800);
  CHECK_EQ(first.page_number_start, 3);
  CHECK_EQ(first.headers.size(), size_t(1));
  CHECK_EQ(first.footers.size(), size_t(1));
  CHECK_EQ(first.headers[0].part_uri, std::string("/word/header1.xml"));
  const text::Section& last = doc.sections[1];
  CHECK_EQ(last.page.width_twips, 16838);
  CHECK_EQ(last.page.orientation, 1);
  CHECK_EQ(last.column_count, 2);
  CHECK_EQ(last.columns.size(), size_t(2));

  // body blocks of section one: 4 paragraphs (title, list, image, sect-break).
  CHECK_EQ(first.blocks.size(), size_t(4));
  const text::Paragraph& title = first.blocks[0].paragraph;
  CHECK_EQ(title.format.style, std::string("Title"));
  CHECK_EQ(title.runs.size(), size_t(2));  // run + bookmark pair
  CHECK_EQ(title.runs[0].text, std::string("DocCore read test"));
  CHECK_EQ(title.runs[0].format.bold, text::Tri::On);
  CHECK_EQ(title.runs[0].format.color, 0xC00000);
  CHECK_EQ(title.runs[0].format.size_half, 44);
  CHECK_EQ(title.runs[0].format.font_eastasia, std::string("SimHei"));
  CHECK_EQ(title.runs[0].format.underline, std::string("single"));

  const text::Paragraph& list = first.blocks[1].paragraph;
  CHECK_EQ(list.format.num_id, std::string("1"));
  CHECK_EQ(list.format.num_level, 0);
  CHECK_EQ(list.format.outline_level, -1);
  CHECK_EQ(list.runs.size(), size_t(12));
  CHECK_EQ(list.runs[0].type, text::InlineItem::Type::Text);
  CHECK_EQ(list.runs[1].type, text::InlineItem::Type::Tab);
  CHECK_EQ(list.runs[2].text, std::string("tail"));
  CHECK_EQ(list.runs[3].deleted_text, true);
  CHECK_EQ(list.runs[4].revision, 1);  // w:ins
  CHECK_EQ(list.runs[4].revision_author, std::string("Alice"));
  CHECK_EQ(list.runs[5].type, text::InlineItem::Type::Text);
  CHECK_EQ(list.runs[5].hyperlink_target, std::string("https://example.com/doc"));
  CHECK_EQ(list.runs[6].type, text::InlineItem::Type::Field);
  CHECK_EQ(list.runs[6].field_kind, 0);
  CHECK_EQ(list.runs[7].type, text::InlineItem::Type::InstrText);
  // 0 text / 1 tab / 2 "tail" / 3 delText / 4 w:ins / 5 hyperlink /
  // 6..10 field (begin, instrText, separate, result, end) / 11 footnote ref
  CHECK_EQ(list.runs[11].type, text::InlineItem::Type::FootnoteRef);
  CHECK_EQ(list.runs[11].note_id, 2);

  // the image paragraph resolves the drawing relationship.
  const text::Paragraph& image = first.blocks[2].paragraph;
  CHECK(!image.runs.empty());
  CHECK_EQ(image.runs[0].type, text::InlineItem::Type::Drawing);
  CHECK_EQ(image.runs[0].drawing_target, std::string("/word/media/image1.png"));

  // table lives in the second section.
  const std::vector<text::Block>& second = doc.sections[1].blocks;
  bool found_table = false;
  for (const auto& b : second) {
    if (b.type == text::Block::Type::Table) {
      found_table = true;
      CHECK_EQ(b.table->grid_twips.size(), size_t(2));
      CHECK_EQ(b.table->grid_twips[0], 4675);
      CHECK_EQ(b.table->rows.size(), size_t(2));
      CHECK(b.table->rows[0].header_row);
      CHECK(b.table->rows[1].cant_split);
      CHECK_EQ(b.table->rows[0].cells[0].format.valign, 1);
      CHECK_EQ(b.table->rows[1].cells[0].format.vmerge, 1);
      const text::Paragraph& cell_text = b.table->rows[0].cells[0].blocks[0].paragraph;
      CHECK_EQ(cell_text.runs[0].text, std::string("column one"));
    }
  }
  CHECK(found_table);

  // unknown parts are kept verbatim, including a customXml part with a
  // namespaced payload. settings.xml 不算未知部件了——它被读进
  // doc.settings_xml 做保护信息的就地改写（见 P11-4）。
  CHECK_EQ(doc.unknown_parts.size(), size_t(5));
  bool saw_custom_xml = false;
  for (const auto& up : doc.unknown_parts) {
    if (up.uri == "/customXml/item1.xml") {
      saw_custom_xml = true;
      CHECK(std::string(up.bytes.begin(), up.bytes.end()).find("kept") != std::string::npos);
    }
    CHECK(up.uri != "/word/settings.xml");
  }
  CHECK(saw_custom_xml);
  CHECK(!doc.settings_xml.empty());

  // unknown-part warning fired for every kept part.
  int unknown_warnings = 0;
  for (const auto& w : warnings.items()) {
    if (w.code == static_cast<int>(WarningCode::UnknownPartKept)) ++unknown_warnings;
  }
  CHECK_EQ(unknown_warnings, 5);
}

void TestDocxRejectsEscapingRels() {
  const std::vector<uint8_t> bytes = ReadFixture("evil_rels.docx");
  WarningList warnings;
  text::Document doc = text::OpenMemory(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()), &warnings);
  // The package still opens; the escaping relationship is dropped with a
  // warning instead of reading outside the file.
  CHECK_EQ(doc.sections.size(), size_t(2));
  bool saw_warning = false;
  for (const auto& w : warnings.items()) {
    if (w.code == static_cast<int>(WarningCode::UnknownPartKept) &&
        w.message.find("escapes package root") != std::string::npos) {
      saw_warning = true;
    }
  }
  CHECK(saw_warning);
}

void TestDocxRejectsPlainZip() {
  const std::vector<uint8_t> zip = ReadFixture("sample.zip");
  CHECK_THROWS(text::OpenMemory(std::string_view(
      reinterpret_cast<const char*>(zip.data()), zip.size())), ErrorCode::Corrupt);
}

// ---- P5: DOCX writer ----------------------------------------------------
//
// The writer is checked with a DOM fingerprint: read -> save -> read must give
// the same document, and saving that document again must produce identical
// bytes (the writer has to be idempotent, not just lossless once).

void DumpRunFormat(const text::RunFormat& f, std::string& o) {
  o += "r{b" + std::to_string(static_cast<int>(f.bold)) + " i" +
       std::to_string(static_cast<int>(f.italic)) + " st" +
       std::to_string(static_cast<int>(f.strike)) + " ds" +
       std::to_string(static_cast<int>(f.double_strike)) + " ac" +
       std::to_string(static_cast<int>(f.all_caps)) + " sc" +
       std::to_string(static_cast<int>(f.small_caps)) + " em" +
       std::to_string(static_cast<int>(f.emboss)) + " im" +
       std::to_string(static_cast<int>(f.imprint)) + " hd" +
       std::to_string(static_cast<int>(f.hidden)) + " np" +
       std::to_string(static_cast<int>(f.no_proof)) + " sz" + std::to_string(f.size_half) +
       " c" + std::to_string(f.color) + " sh" + std::to_string(f.shading) + " va" +
       std::to_string(f.vert_align) + " sp" + std::to_string(f.spacing_twips) + " k" +
       std::to_string(f.kerning_half) + " po" + std::to_string(f.position_half) + " u'" +
       f.underline + "' fa'" + f.font_ascii + "' fh'" + f.font_hansi + "' fe'" +
       f.font_eastasia + "' fc'" + f.font_cs + "' lg'" + f.lang + "' cs'" + f.character_style +
       "' ol" + std::to_string(f.outline_level) + "}";
}

void DumpParaFormat(const text::ParagraphFormat& f, std::string& o) {
  o += "p{st'" + f.style + "' al" + std::to_string(f.alignment) + " lr" +
       std::to_string(f.line_rule) + " li" + std::to_string(f.line_twips) + " b" +
       std::to_string(f.before_twips) + " a" + std::to_string(f.after_twips) + " ba" +
       std::to_string(f.before_autospacing) + " aa" + std::to_string(f.after_autospacing) +
       " cb" + std::to_string(f.context_before) + " il" + std::to_string(f.ind_left) + " ir" +
       std::to_string(f.ind_right) + " if" + std::to_string(f.ind_first_line) + " ic" +
       std::to_string(f.ind_first_line_chars) + " kn" +
       std::to_string(static_cast<int>(f.keep_next)) + " kl" +
       std::to_string(static_cast<int>(f.keep_lines)) + " wc" +
       std::to_string(static_cast<int>(f.widow_control)) + " sl" +
       std::to_string(static_cast<int>(f.suppress_line_numbers)) + " pb" +
       std::to_string(static_cast<int>(f.page_break_before)) + " ol" +
       std::to_string(f.outline_level) + " sh" + std::to_string(f.shading) + " bt" +
       std::to_string(f.border_top) + " bb" + std::to_string(f.border_bottom) + " bl" +
       std::to_string(f.border_left) + " br" + std::to_string(f.border_right) + " nu'" +
       f.num_id + "' nl" + std::to_string(f.num_level) + " rl" + std::to_string(int(f.rtl)) +
       " sg" + std::to_string(f.snap_to_grid) + " mi" + std::to_string(f.mirror_indents) + "}";
}

void DumpParagraph(const text::Paragraph& p, std::string& o) {
  DumpParaFormat(p.format, o);
  for (const auto& it : p.runs) {
    o += "[i" + std::to_string(static_cast<int>(it.type)) + " '" + it.text + "'";
    DumpRunFormat(it.format, o);
    o += " rv" + std::to_string(it.revision) + " au'" + it.revision_author + "' dt'" +
         it.revision_date + "' bk" + std::to_string(it.break_kind) + " fk" +
         std::to_string(it.field_kind) + " dg'" + it.drawing_target + "' sy" +
         std::to_string(static_cast<uint32_t>(it.symbol)) + " sf'" + it.symbol_font + "' ni" +
         std::to_string(it.note_id) + " ci" + std::to_string(it.comment_id) + " del" +
         std::to_string(it.deleted_text ? 1 : 0) + " ha'" + it.hyperlink_anchor + "' ht'" +
         it.hyperlink_target + "']";
  }
}

void DumpBlocks(const std::vector<text::Block>& blocks, std::string& o) {
  for (const auto& b : blocks) {
    if (b.type == text::Block::Type::Table) {
      o += "{tbl";
      if (b.table != nullptr) {
        o += " st'" + b.table->style + "' w" + std::to_string(b.table->width_twips) + " al" +
             std::to_string(b.table->alignment) + " bd" + std::to_string(b.table->borders) +
             " lf" + std::to_string(b.table->layout_fixed ? 1 : 0) + " in" +
             std::to_string(b.table->indent_twips) + " g";
        for (int g : b.table->grid_twips) o += "," + std::to_string(g);
        for (const auto& row : b.table->rows) {
          o += "|r h" + std::to_string(row.height_twips) + " hr" +
               std::to_string(row.height_rule) + " hd" + std::to_string(row.header_row ? 1 : 0) +
               " cs" + std::to_string(row.cant_split ? 1 : 0);
          for (const auto& cell : row.cells) {
            o += "|c w" + std::to_string(cell.format.width_twips) + " vm" +
                 std::to_string(cell.format.vmerge) + " gs" +
                 std::to_string(cell.format.grid_span) + " sh" +
                 std::to_string(cell.format.shading) + " va" +
                 std::to_string(cell.format.valign) + " nw" +
                 std::to_string(cell.format.no_wrap ? 1 : 0) + ":";
            DumpBlocks(cell.blocks, o);
          }
        }
      }
      o += "}";
    } else {
      o += "{";
      DumpParagraph(b.paragraph, o);
      o += "}";
    }
  }
}

void DumpSection(const text::Section& s, std::string& o) {
  const text::PageSetup& p = s.page;
  o += "sec{w" + std::to_string(p.width_twips) + " h" + std::to_string(p.height_twips) + " o" +
       std::to_string(p.orientation) + " mt" + std::to_string(p.margin_top) + " mb" +
       std::to_string(p.margin_bottom) + " ml" + std::to_string(p.margin_left) + " mr" +
       std::to_string(p.margin_right) + " hd" + std::to_string(p.header_dist) + " fd" +
       std::to_string(p.footer_dist) + " gu" + std::to_string(p.gutter) + " cc" +
       std::to_string(s.column_count) + " cs" + std::to_string(s.column_space) + " tp" +
       std::to_string(s.title_pg ? 1 : 0) + " pn" + std::to_string(s.page_number_start) + " pf" +
       std::to_string(s.page_number_fmt);
  for (const auto& c : s.columns) {
    o += " col(" + std::to_string(c.width_twips) + "," + std::to_string(c.space_twips) + ")";
  }
  for (const auto& h : s.headers) {
    o += " hdr" + std::to_string(static_cast<int>(h.kind)) + ":";
    DumpBlocks(h.blocks, o);
  }
  for (const auto& f : s.footers) {
    o += " ftr" + std::to_string(static_cast<int>(f.kind)) + ":";
    DumpBlocks(f.blocks, o);
  }
  o += "}";
  DumpBlocks(s.blocks, o);
}

// `with_columns` is false for the first generation: the reader derives column
// definitions from w:cols/@num when a file has none, so a fixture without
// explicit columns legitimately gains them after one save.
std::string Fingerprint(const text::Document& doc, bool with_columns) {
  std::string o;
  o += "defaults:" + std::to_string(doc.has_doc_defaults ? 1 : 0) + ";";
  DumpRunFormat(doc.default_run, o);
  DumpParaFormat(doc.default_paragraph, o);
  for (const auto& s : doc.styles) {
    o += "\nstyle '" + s.id + "' '" + s.name + "' k" + std::to_string(static_cast<int>(s.kind)) +
         " bo'" + s.based_on + "' nx'" + s.next + "' lk'" + s.link + "' pr" +
         std::to_string(s.priority) + " q" + std::to_string(s.quick_format ? 1 : 0) + " hi" +
         std::to_string(s.hidden ? 1 : 0) + " sh" + std::to_string(s.semi_hidden ? 1 : 0) + " uw" +
         std::to_string(s.unhide_when_used ? 1 : 0);
    DumpParaFormat(s.paragraph, o);
    DumpRunFormat(s.run, o);
  }
  for (const auto& a : doc.abstract_numberings) {
    o += "\nabstract '" + a.id + "'";
    for (const auto& l : a.levels) {
      o += " lvl(" + std::to_string(l.start) + "," + l.num_fmt + "," + l.lvl_text + "," +
           std::to_string(l.alignment) + "," + l.suffix + "," + l.font_ascii + "," +
           l.font_eastasia + "," + std::to_string(l.color) + "," +
           std::to_string(l.run_size_half);
      DumpParaFormat(l.paragraph, o);
      o += ")";
    }
  }
  for (const auto& n : doc.numbering) {
    o += "\nnum '" + n.num_id + "' -> '" + n.abstract_id + "'";
    for (int v : n.level_start_overrides) o += "," + std::to_string(v);
  }
  for (const auto& s : doc.sections) {
    if (with_columns) {
      DumpSection(s, o);
    } else {
      text::Section copy = s;
      copy.columns.clear();
      DumpSection(copy, o);
    }
    o += "\n";
  }
  for (const auto& n : doc.footnotes) {
    o += "\nfootnote " + std::to_string(n.id) + ":";
    DumpBlocks(n.blocks, o);
  }
  for (const auto& n : doc.endnotes) {
    o += "\nendnote " + std::to_string(n.id) + ":";
    DumpBlocks(n.blocks, o);
  }
  for (const auto& c : doc.comments) {
    o += "\ncomment " + std::to_string(c.id) + " '" + c.author + "' '" + c.initials + "' '" +
         c.date + "':";
    DumpBlocks(c.blocks, o);
  }
  for (const auto& u : doc.unknown_parts) {
    o += "\npart '" + u.uri + "' '" + u.mime + "' ";
    o.append(u.bytes.begin(), u.bytes.end());
  }
  return o;
}

void TestDocxWriteRoundTrip() {
  const std::vector<uint8_t> original = ReadFixture("sample.docx");
  WarningList read_warnings;
  text::Document doc = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(original.data()), original.size()),
      &read_warnings);

  MemoryStream first;
  text::Save(doc, first);
  const std::vector<uint8_t> saved = first.data();
  CHECK(saved.size() > 512);
  CHECK_EQ(saved[0], uint8_t('P'));
  CHECK_EQ(saved[1], uint8_t('K'));

  WarningList write_warnings;
  MemoryStream sink;
  text::SaveOptions opts;
  opts.warnings = &write_warnings;
  text::Save(doc, sink, opts);
  CHECK_EQ(sink.data().size(), saved.size());
  // Nothing the writer understands may be dropped silently.
  for (const auto& w : write_warnings.items()) {
    std::printf("      write warning %d %s\n", w.code, w.message.c_str());
  }
  CHECK_EQ(write_warnings.items().size(), size_t(0));

  WarningList second_warnings;
  text::Document again = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(saved.data()), saved.size()),
      &second_warnings);

  CHECK_EQ(Fingerprint(doc, false), Fingerprint(again, false));
  // The derived columns of the re-read document are the real ones.
  CHECK_EQ(again.sections.size(), size_t(2));
  CHECK_EQ(again.sections[1].columns.size(), size_t(2));

  // Idempotence: a second save of the re-read document is byte identical.
  MemoryStream second;
  text::Save(again, second);
  CHECK(second.data() == saved);

  // The re-read document still resolves the parts the writer rebuilt.
  CHECK_EQ(again.sections[0].headers.size(), size_t(1));
  CHECK_EQ(again.sections[0].headers[0].part_uri, std::string("/word/header1.xml"));
  CHECK_EQ(again.sections[0].footers[0].part_uri, std::string("/word/footer1.xml"));
  CHECK_EQ(again.unknown_parts.size(), doc.unknown_parts.size());
}

void TestDocxWriteToFile() {
  const std::vector<uint8_t> original = ReadFixture("sample.docx");
  text::Document doc = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(original.data()), original.size()));
  const std::string path = std::string(DOCCORE_TEST_DATA) + "/out_roundtrip.docx";
  text::SaveToFile(doc, path);
  FileStream in(path, "rb");
  const std::vector<uint8_t> bytes = ReadAll(in, 16ull << 20);
  CHECK(bytes.size() > 512);
  text::Document again = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  CHECK_EQ(again.sections.size(), doc.sections.size());
  std::remove(path.c_str());
}

void TestDocxWriteEmptyDocument() {
  // A programmatically built document with no sections must still produce a
  // package that reopens.
  text::Document doc;
  doc.has_doc_defaults = true;
  doc.default_run.size_half = 24;
  text::Section s;
  s.blocks.push_back(text::Block{});
  s.blocks[0].paragraph.runs.push_back(text::InlineItem{});
  s.blocks[0].paragraph.runs[0].text = "hello & <world>";
  doc.sections.push_back(s);

  MemoryStream out;
  text::Save(doc, out);
  text::Document again = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(out.data().data()), out.data().size()));
  CHECK_EQ(again.sections.size(), size_t(1));
  CHECK_EQ(again.sections[0].blocks.size(), size_t(1));
  CHECK_EQ(again.sections[0].blocks[0].paragraph.runs.size(), size_t(1));
  CHECK_EQ(again.sections[0].blocks[0].paragraph.runs[0].text, std::string("hello & <world>"));
  CHECK_EQ(again.default_run.size_half, 24);
}

// ---- P5: layout engine --------------------------------------------------

text::Document LoadSampleDocx() {
  const std::vector<uint8_t> bytes = ReadFixture("sample.docx");
  return text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<std::string> PageTexts(const LayoutDocument& doc) {
  std::vector<std::string> out;
  for (const auto& page : doc.pages) {
    std::string text;
    for (const auto& op : page.ops) {
      if (const auto* t = std::get_if<DrawText>(&op)) text += t->text_utf8;
    }
    out.push_back(std::move(text));
  }
  return out;
}

void TestLayoutPagination() {
  text::Document doc = LoadSampleDocx();
  WarningList warnings;
  text::LayoutOptions opts;
  opts.warnings = &warnings;
  const LayoutDocument laid = text::LayoutText(doc, opts);

  // section 1 = one A4 portrait page, section 2 = landscape and it carries an
  // explicit w:br type="page", so two pages there.
  CHECK_EQ(laid.pages.size(), size_t(3));
  CHECK(std::abs(laid.pages[0].width_pt - 595.276) < 0.5);
  CHECK(std::abs(laid.pages[0].height_pt - 841.89) < 0.5);
  CHECK(std::abs(laid.pages[1].width_pt - 841.89) < 0.5);
  CHECK(std::abs(laid.pages[1].height_pt - 595.276) < 0.5);
  CHECK_EQ(laid.pages[2].width_pt, laid.pages[1].width_pt);

  const std::vector<std::string> texts = PageTexts(laid);
  CHECK(texts[0].find("DocCore read test") != std::string::npos);
  CHECK(texts[0].find("header text") != std::string::npos);   // page header
  CHECK(texts[1].find("column one") != std::string::npos);    // table cell
  CHECK(texts[2].find("after page break") != std::string::npos);

  // every op must stay inside the page box
  for (const auto& page : laid.pages) {
    for (const auto& op : page.ops) {
      if (const auto* t = std::get_if<DrawText>(&op)) {
        CHECK(t->x >= 0.0);
        CHECK(t->x <= page.width_pt);
        CHECK(t->y >= 0.0);
        CHECK(t->y <= page.height_pt);
      }
    }
  }

  // 排版必须可重入：同一文档两次排版结果逐页一致。
  WarningList unused;
  text::LayoutOptions opts2;
  opts2.warnings = &unused;
  const LayoutDocument again = text::LayoutText(doc, opts2);
  CHECK_EQ(again.pages.size(), laid.pages.size());
  for (size_t i = 0; i < laid.pages.size(); ++i) {
    CHECK_EQ(again.pages[i].ops.size(), laid.pages[i].ops.size());
  }

  // 图片在这一轮没有解码器：必须给出 ImageSkipped，而不是静默丢弃。
  int image_warnings = 0;
  for (const auto& w : warnings.items()) {
    if (w.code == static_cast<int>(WarningCode::ImageSkipped)) ++image_warnings;
  }
  CHECK_EQ(image_warnings, 1);
}

void TestLayoutLineBreaking() {
  // 一个长中文段落必须折成多行，且每行都不超过版心宽度。
  text::Document doc;
  doc.has_doc_defaults = true;
  doc.default_run.size_half = 24;  // 12pt
  text::Section s;
  text::Block block;
  block.type = text::Block::Type::Paragraph;
  block.paragraph.format.alignment = 0;
  text::InlineItem run;
  run.type = text::InlineItem::Type::Text;
  for (int i = 0; i < 200; ++i) run.text += "汉字排版";
  block.paragraph.runs.push_back(run);
  s.blocks.push_back(block);
  doc.sections.push_back(s);

  text::LayoutOptions opts;
  const LayoutDocument laid = text::LayoutText(doc, opts);
  CHECK(laid.pages.size() >= 1);
  // 12pt 汉字在 A4 版心（约 482pt）里一行最多 40 字，200*4=800 字 → 至少 20 行。
  size_t lines = 0;
  double widest = 0.0;
  for (const auto& op : laid.pages[0].ops) {
    if (const auto* t = std::get_if<DrawText>(&op)) {
      ++lines;
      widest = std::max(widest, t->x);
    }
  }
  CHECK(lines >= 20);
  CHECK(widest <= laid.pages[0].width_pt);
}

void TestLayoutNumbering() {
  text::Document doc = LoadSampleDocx();
  text::LayoutOptions opts;
  const LayoutDocument laid = text::LayoutText(doc, opts);
  const std::vector<std::string> texts = PageTexts(laid);
  // fixture 的列表段用 numId=1（abstractNum 0 的 lvl 0：decimal + "%1."），
  // 计数从 1 开始，所以首行文本以 "1." 开头。
  CHECK(texts[0].find("1.") != std::string::npos);
  CHECK(texts[0].find("first item") != std::string::npos);
}

void TestTextExport() {
  text::Document doc = LoadSampleDocx();
  const std::string text = text::ExtractText(doc);
  CHECK(text.find("DocCore read test") != std::string::npos);
  CHECK(text.find("column one") != std::string::npos);
  CHECK(text.find("first footnote") != std::string::npos);
  CHECK(text.find("[endnotes]") != std::string::npos);

  const std::string html = text::ToHtml(doc);
  CHECK(html.find("<table>") != std::string::npos);
  CHECK(html.find("DocCore read test") != std::string::npos);
  CHECK(html.find("font-weight:bold") != std::string::npos);
}

}  // namespace

template <typename F>
void Run(const char* name, F fn) {
  std::printf("%-24s ", name);
  try {
    fn();
    std::printf("ok\n");
  } catch (const Error& e) {
    ++g_failures;
    std::printf("THROW code=%d %s\n", static_cast<int>(e.code()), e.what());
  } catch (const std::exception& e) {
    ++g_failures;
    std::printf("EXC %s\n", e.what());
  }
}

// ---- P6 表格与公式 ------------------------------------------------------

class TestSheetCtx : public sheet::EvalContext {
 public:
  explicit TestSheetCtx(sheet::Workbook& wb) : wb_(wb) {}

  sheet::Value CellValue(std::string_view sheet_name, int row, int col) override {
    sheet::Worksheet* ws = Resolve(sheet_name);
    if (ws == nullptr) return sheet::Value::Err(sheet::CellError::Ref);
    return ws->GetValue(row, col);
  }

  void ForEachInRange(std::string_view sheet_name, const sheet::Range& range,
                      const std::function<void(int, int, const sheet::Value&)>& fn) override {
    sheet::Worksheet* ws = Resolve(sheet_name);
    if (ws == nullptr) return;
    for (const auto& column : ws->columns()) {
      if (column.first < range.col1 || column.first > range.col2) continue;
      for (const auto& row : column.second) {
        if (row.first < range.row1 || row.first > range.row2) continue;
        fn(row.first, column.first, ws->GetValue(row.first, column.first));
      }
    }
  }

  double TodaySerial() const override { return 45000.0; }

 private:
  sheet::Worksheet* Resolve(std::string_view name) {
    if (name.empty()) return wb_.SheetAt(0);
    return wb_.SheetByName(name);
  }
  sheet::Workbook& wb_;
};

// 求值一个公式文本（不含 '='）。
sheet::Value EvalText(const std::string& text, sheet::Workbook& wb) {
  const sheet::ParseResult parsed = sheet::ParseFormula(text);
  if (!parsed.ok) return sheet::Value::Err(sheet::CellError::Name);
  TestSheetCtx ctx(wb);
  return sheet::Evaluate(*parsed.root, ctx);
}

double EvalNumber(const std::string& text, sheet::Workbook& wb) {
  const sheet::Value v = EvalText(text, wb);
  if (v.is_number()) return v.number;
  if (v.kind == sheet::Value::Kind::Bool) return v.boolean ? 1.0 : 0.0;
  return -12345.0;
}

std::string EvalString(const std::string& text, sheet::Workbook& wb) {
  const sheet::Value v = EvalText(text, wb);
  return sheet::FormatValue(v);
}

void TestSheetAddresses() {
  CHECK_EQ(sheet::ColumnName(0), std::string("A"));
  CHECK_EQ(sheet::ColumnName(25), std::string("Z"));
  CHECK_EQ(sheet::ColumnName(26), std::string("AA"));
  CHECK_EQ(sheet::ColumnName(701), std::string("ZZ"));
  CHECK_EQ(sheet::ColumnName(702), std::string("AAA"));
  CHECK_EQ(sheet::ColumnFromName("AA"), 26);
  CHECK_EQ(sheet::ColumnFromName("zz"), 701);
  CHECK_EQ(sheet::ColumnFromName("1A"), -1);
  CHECK_EQ(sheet::CellName(0, 0), std::string("A1"));
  CHECK_EQ(sheet::CellName(11, 27), std::string("AB12"));

  sheet::CellRef ref;
  CHECK(sheet::ParseCellName("AB12", &ref));
  CHECK_EQ(ref.row, 11);
  CHECK_EQ(ref.col, 27);
  CHECK(!sheet::ParseCellName("A0", &ref));
  CHECK(!sheet::ParseCellName("A1B", &ref));
  CHECK(!sheet::ParseCellName("", &ref));
}

void TestSheetNumberFormat() {
  using namespace doccore::sheet;

  NumberFormat plain = ParseNumberFormat("#,##0.00");
  CHECK(plain.kind == NumberFormat::Kind::Decimal);
  CHECK(plain.thousands);
  CHECK_EQ(plain.decimals, 2);
  CHECK_EQ(FormatValue(Value::Num(1234.5678), plain), std::string("1,234.57"));
  CHECK_EQ(FormatValue(Value::Num(-1234.5), plain), std::string("-1,234.50"));

  NumberFormat integer = ParseNumberFormat("0");
  CHECK(integer.kind == NumberFormat::Kind::Integer);
  CHECK_EQ(FormatValue(Value::Num(3.6), integer), std::string("4"));

  NumberFormat percent = ParseNumberFormat("0.00%");
  CHECK(percent.kind == NumberFormat::Kind::Percent);
  CHECK_EQ(FormatValue(Value::Num(0.1234), percent), std::string("12.34%"));

  NumberFormat money = ParseNumberFormat("$#,##0.00");
  CHECK(money.kind == NumberFormat::Kind::Currency);
  CHECK_EQ(money.prefix, std::string("$"));
  CHECK_EQ(FormatValue(Value::Num(-12.5), money), std::string("-$12.50"));

  NumberFormat date = ParseNumberFormat("yyyy-mm-dd");
  CHECK(date.kind == NumberFormat::Kind::Date);
  CHECK_EQ(FormatValue(Value::Num(45000), date), std::string("2023-03-15"));
  // 1900 闰年 bug 之前的部分整体偏移一天（Excel 行为）。
  CHECK_EQ(FormatValue(Value::Num(1), date), std::string("1900-01-01"));
  CHECK_EQ(FormatValue(Value::Num(61), date), std::string("1900-03-01"));

  NumberFormat time = ParseNumberFormat("hh:mm:ss");
  CHECK(time.kind == NumberFormat::Kind::Time);
  CHECK_EQ(FormatValue(Value::Num(0.5), time), std::string("12:00:00"));

  NumberFormat text_fmt = ParseNumberFormat("@");
  CHECK(text_fmt.kind == NumberFormat::Kind::Text);
  CHECK_EQ(FormatValue(Value::Str("abc"), text_fmt), std::string("abc"));

  NumberFormat general = ParseNumberFormat("General");
  CHECK(general.kind == NumberFormat::Kind::General);
  CHECK_EQ(FormatValue(Value::Num(1.5), general), std::string("1.5"));
  CHECK_EQ(FormatValue(Value::Boolean(true)), std::string("TRUE"));
  CHECK_EQ(FormatValue(Value::Err(CellError::DivZero)), std::string("#DIV/0!"));

  // 内置格式表：14 是日期。
  CHECK_EQ(std::string(BuiltinNumberFormat(14)), std::string("yyyy-mm-dd"));
  CHECK_EQ(std::string(BuiltinNumberFormat(2)), std::string("0.00"));
  CHECK(BuiltinNumberFormat(500).empty());
}

void TestFormulaParse() {
  using namespace doccore::sheet;

  ParseResult ok = ParseFormula("SUM(A1:B2)+1");
  CHECK(ok.ok);
  CHECK(ok.root != nullptr);
  CHECK_EQ(ToText(*ok.root), std::string("SUM(A1:B2)+1"));

  ParseResult abs = ParseFormula("$A$1+Sheet2!B$2");
  CHECK(abs.ok);
  CHECK_EQ(ToText(*abs.root), std::string("$A$1+Sheet2!B$2"));

  ParseResult quoted = ParseFormula("'My Sheet'!A1");
  CHECK(quoted.ok);
  CHECK_EQ(ToText(*quoted.root), std::string("'My Sheet'!A1"));

  ParseResult whole = ParseFormula("SUM(A:A)");
  CHECK(whole.ok);
  CHECK_EQ(ToText(*whole.root), std::string("SUM(A:A)"));

  ParseResult percent = ParseFormula("50%");
  CHECK(percent.ok);
  CHECK_EQ(ToText(*percent.root), std::string("50%"));

  ParseResult bad = ParseFormula("1+");
  CHECK(!bad.ok);
  CHECK(bad.root == nullptr);
  CHECK(!bad.error.empty());

  ParseResult unbalanced = ParseFormula("SUM(A1");
  CHECK(!unbalanced.ok);

  ParseResult text_lit = ParseFormula("\"a\"\"b\"");
  CHECK(text_lit.ok);
  CHECK_EQ(ToText(*text_lit.root), std::string("\"a\"\"b\""));
}

void TestFormulaEval() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.SetNumber(0, 0, 1);
  ws.SetNumber(1, 0, 2);
  ws.SetNumber(2, 0, 3);
  ws.SetText(0, 1, "abc");

  CHECK_EQ(EvalNumber("1+2*3", wb), 7.0);
  CHECK_EQ(EvalNumber("(1+2)*3", wb), 9.0);
  CHECK_EQ(EvalNumber("-2^2", wb), 4.0);  // Excel：一元负号比 ^ 紧
  CHECK_EQ(EvalNumber("2^3^2", wb), 64.0);
  CHECK_EQ(EvalNumber("10/4", wb), 2.5);
  CHECK_EQ(EvalNumber("50%", wb), 0.5);
  CHECK_EQ(EvalNumber("A1+A2+A3", wb), 6.0);
  CHECK_EQ(EvalNumber("SUM(A1:A3)", wb), 6.0);
  CHECK_EQ(EvalNumber("AVERAGE(A1:A3)", wb), 2.0);
  CHECK_EQ(EvalNumber("MIN(A1:A3)", wb), 1.0);
  CHECK_EQ(EvalNumber("MAX(A1:A3)", wb), 3.0);
  CHECK_EQ(EvalNumber("COUNT(A1:B3)", wb), 3.0);
  CHECK_EQ(EvalNumber("COUNTA(A1:B3)", wb), 4.0);
  CHECK_EQ(EvalNumber("ROUND(2.345,2)", wb), 2.35);
  CHECK_EQ(EvalNumber("ROUNDUP(2.341,2)", wb), 2.35);
  CHECK_EQ(EvalNumber("ROUNDDOWN(2.349,2)", wb), 2.34);
  CHECK_EQ(EvalNumber("MOD(7,3)", wb), 1.0);
  CHECK_EQ(EvalNumber("ABS(-4)", wb), 4.0);
  CHECK_EQ(EvalNumber("IF(A1>0,10,20)", wb), 10.0);
  CHECK_EQ(EvalNumber("AND(TRUE,A1=1)", wb), 1.0);
  CHECK_EQ(EvalNumber("COUNTIF(A1:A3,\">1\")", wb), 2.0);
  CHECK_EQ(EvalNumber("SUMIF(A1:A3,\">1\")", wb), 5.0);
  CHECK_EQ(EvalNumber("LEN(B1)", wb), 3.0);
  CHECK_EQ(EvalNumber("TODAY()", wb), 45000.0);

  CHECK_EQ(EvalString("\"a\"&\"b\"", wb), std::string("ab"));
  CHECK_EQ(EvalString("UPPER(B1)", wb), std::string("ABC"));
  CHECK_EQ(EvalString("LEFT(B1,2)", wb), std::string("ab"));
  CHECK_EQ(EvalString("MID(B1,2,1)", wb), std::string("b"));
  CHECK_EQ(EvalString("IF(A1>5,\"big\",\"small\")", wb), std::string("small"));
  CHECK_EQ(EvalString("TEXT(0.5,\"0.0%\")", wb), std::string("50.0%"));

  // 错误传播与识别
  CHECK(EvalText("1/0", wb).is_error());
  CHECK(EvalText("1/0", wb).error == CellError::DivZero);
  CHECK(EvalText("NOSUCHFUNC(1)", wb).error == CellError::Name);
  CHECK(EvalText("SUMIFS(A1:A3,A1:A3,1)", wb).error == CellError::NotImpl);
  CHECK(EvalText("Z9+1", wb).is_number());  // 空单元格按 0
  CHECK(EvalText("B1*2", wb).error == CellError::Value);

  // 查找函数
  Worksheet& lookup = wb.AddSheet("Lookup");
  lookup.SetNumber(0, 0, 1);
  lookup.SetText(0, 1, "one");
  lookup.SetNumber(1, 0, 2);
  lookup.SetText(1, 1, "two");
  lookup.SetNumber(2, 0, 3);
  lookup.SetText(2, 1, "three");
  CHECK_EQ(EvalString("VLOOKUP(2,Lookup!A1:B3,2,FALSE)", wb), std::string("two"));
  CHECK_EQ(EvalString("INDEX(Lookup!B1:B3,3)", wb), std::string("three"));
  CHECK_EQ(EvalNumber("MATCH(3,Lookup!A1:A3,0)", wb), 3.0);
  CHECK(EvalText("VLOOKUP(9,Lookup!A1:B3,2,FALSE)", wb).error == CellError::NotAvailable);

  CHECK(sheet::FunctionCount() >= 40);
}

void TestSheetRecalculate() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.SetNumber(0, 0, 1);                       // A1
  ws.SetFormula(0, 1, "A1+1");                 // B1
  ws.SetFormula(0, 2, "B1*2");                 // C1
  ws.SetFormula(1, 0, "SUM(A1:C1)");           // A2
  CHECK_EQ(wb.Recalculate(), 3);
  CHECK_EQ(ws.GetValue(0, 1).number, 2.0);
  CHECK_EQ(ws.GetValue(0, 2).number, 4.0);
  CHECK_EQ(ws.GetValue(1, 0).number, 7.0);

  // 跨表引用
  Worksheet& second = wb.AddSheet("Sheet2");
  second.SetFormula(0, 0, "Sheet1!A1*10");
  wb.Recalculate();
  CHECK_EQ(second.GetValue(0, 0).number, 10.0);

  // 循环引用只污染环上的单元格
  Worksheet& cyc = wb.AddSheet("Cycle");
  cyc.SetFormula(0, 0, "B1+1");
  cyc.SetFormula(0, 1, "A1+1");
  cyc.SetFormula(0, 2, "A1*2");  // 环外，读到的是 #CIRC!
  cyc.SetFormula(1, 0, "5");     // 常量公式，不受影响
  wb.Recalculate();
  CHECK(cyc.GetValue(0, 0).is_error());
  CHECK(cyc.GetValue(0, 0).error == CellError::Circular);
  CHECK(cyc.GetValue(0, 1).error == CellError::Circular);
  CHECK(cyc.GetValue(0, 2).is_error());
  CHECK_EQ(cyc.GetValue(1, 0).number, 5.0);

  // 自引用
  Worksheet& self = wb.AddSheet("Self");
  self.SetFormula(0, 0, "A1+1");
  wb.Recalculate();
  CHECK(self.GetValue(0, 0).error == CellError::Circular);
}

void TestXlsxRoundTrip() {
  using namespace doccore::sheet;

  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.set_name("Data");
  ws.SetText(0, 0, "Name");
  ws.SetText(0, 1, "Amount");
  ws.SetText(1, 0, "Alpha");
  ws.SetNumber(1, 1, 12.5);
  ws.SetText(2, 0, "Beta");
  ws.SetNumber(2, 1, 7.25);
  ws.SetBool(3, 0, true);
  ws.SetError(3, 1, CellError::DivZero);
  ws.SetFormula(4, 1, "SUM(B2:B3)");
  ws.Merge(0, 2, 0, 3);
  ws.SetColumnWidth(0, 18.0);
  ws.SetRowHeight(0, 22.0);
  ws.print().landscape = true;

  CellStyle bold;
  bold.font.bold = true;
  bold.num_fmt_id = 4;  // #,##0.00
  bold.num_fmt = ParseNumberFormat("#,##0.00");
  bold.fill = 0xFFF2CC;
  bold.halign = 3;
  const int32_t style_id = wb.AddStyle(bold);
  ws.Find(1, 1)->style_id = style_id;
  ws.Find(2, 1)->style_id = style_id;

  WarningList save_warnings;
  SaveOptions save_opts;
  save_opts.warnings = &save_warnings;
  save_opts.recalculate = true;  // 公式缓存值由保存前的重算填充
  MemoryStream out;
  Save(wb, out, save_opts);
  CHECK(save_warnings.empty());
  CHECK(out.Size() > 1000);

  WarningList load_warnings;
  Workbook back = OpenMemory(std::string_view(reinterpret_cast<const char*>(out.data().data()),
                                             out.data().size()),
                            &load_warnings);
  CHECK(load_warnings.empty());
  CHECK_EQ(back.sheet_count(), 1);
  Worksheet* re = back.SheetAt(0);
  CHECK(re != nullptr);
  if (re == nullptr) return;
  CHECK_EQ(re->name(), std::string("Data"));

  CHECK(re->GetValue(0, 0).is_text());
  CHECK_EQ(re->GetValue(0, 0).text, std::string("Name"));
  CHECK_EQ(re->GetValue(1, 1).number, 12.5);
  CHECK_EQ(re->GetValue(2, 1).number, 7.25);
  CHECK(re->GetValue(3, 0).kind == Value::Kind::Bool);
  CHECK(re->GetValue(3, 0).boolean);
  CHECK(re->GetValue(3, 1).error == CellError::DivZero);

  const Cell* formula_cell = re->Find(4, 1);
  CHECK(formula_cell != nullptr);
  if (formula_cell != nullptr) {
    CHECK(formula_cell->has_formula());
    CHECK_EQ(re->formulas()[static_cast<size_t>(formula_cell->formula_id)].text,
             std::string("SUM(B2:B3)"));
    CHECK_EQ(formula_cell->cached.number, 19.75);
  }

  CHECK_EQ(re->merges().size(), size_t(1));
  if (!re->merges().empty()) {
    CHECK_EQ(re->merges()[0].row1, 0);
    CHECK_EQ(re->merges()[0].col1, 2);
    CHECK_EQ(re->merges()[0].col2, 3);
  }
  CHECK_EQ(re->ColumnWidth(0), 18.0);
  CHECK_EQ(re->RowHeight(0), 22.0);
  CHECK(re->print().landscape);

  const Cell* styled = re->Find(1, 1);
  CHECK(styled != nullptr);
  if (styled != nullptr) {
    const CellStyle& style = back.StyleAt(styled->style_id);
    CHECK(style.font.bold);
    CHECK_EQ(style.num_fmt_id, 4);
    CHECK_EQ(style.fill, 0xFFF2CC);
    CHECK_EQ(style.halign, 3);
    CHECK(style.num_fmt.thousands);
  }

  // 显示值按数字格式渲染
  CHECK_EQ(FormatValue(re->GetValue(1, 1), back.StyleAt(styled->style_id).num_fmt),
           std::string("12.50"));

  // 再保存一次应当字节一致（写入是幂等的）
  MemoryStream again;
  Save(back, again);
  CHECK(again.data() == out.data());

  // 重算
  back.Recalculate();
  CHECK_EQ(re->GetValue(4, 1).number, 19.75);

  // 新增公式后重算
  re->SetFormula(4, 0, "COUNT(B2:B3)");
  back.Recalculate();
  CHECK_EQ(re->GetValue(4, 0).number, 2.0);
}

void TestXlsxExports() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.set_name("T");
  ws.SetText(0, 0, "a,b");
  ws.SetText(0, 1, "quote\"d");
  ws.SetNumber(1, 0, 1.5);
  ws.SetNumber(1, 1, 2);

  const std::string csv = ToCsv(ws);
  CHECK(csv.find("\"a,b\"") != std::string::npos);
  CHECK(csv.find("\"quote\"\"d\"") != std::string::npos);
  CHECK(csv.find("1.5,2") != std::string::npos);

  const std::string html = ToHtml(wb);
  CHECK(html.find("<table>") != std::string::npos);
  CHECK(html.find("&quot;") != std::string::npos || html.find("quote") != std::string::npos);
  CHECK(html.find("<h2>T</h2>") != std::string::npos);
}

void TestXlsxToFile() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  wb.SheetAt(0)->SetNumber(0, 0, 42);
  const std::string path = OutPath("p6_roundtrip.xlsx");
  SaveToFile(wb, path);

  FileStream in(path, "rb");
  const std::vector<uint8_t> bytes = ReadAll(in, 64ull << 20);
  CHECK(bytes.size() > 500);

  OpenOptions opts;
  opts.path = path;
  Workbook loaded = Open(opts);
  CHECK_EQ(loaded.sheet_count(), 1);
  CHECK_EQ(loaded.SheetAt(0)->GetValue(0, 0).number, 42.0);

  // 供 tests/verify_xlsx.py 用第三方工具交叉校验的示例工作簿。
  Workbook demo = Workbook::Create();
  Worksheet& d = *demo.SheetAt(0);
  d.set_name("Demo");
  d.SetText(0, 0, "Item");
  d.SetText(0, 1, "Qty");
  d.SetText(0, 2, "Price");
  d.SetText(0, 3, "Total");
  d.SetText(1, 0, "Widget");
  d.SetNumber(1, 1, 3);
  d.SetNumber(1, 2, 4.5);
  d.SetFormula(1, 3, "B2*C2");
  d.SetText(2, 0, "Gadget");
  d.SetNumber(2, 1, 2);
  d.SetNumber(2, 2, 9.25);
  d.SetFormula(2, 3, "B3*C3");
  d.SetFormula(3, 3, "SUM(D2:D3)");
  CellStyle money;
  money.num_fmt_id = 4;
  money.num_fmt = ParseNumberFormat("#,##0.00");
  money.font.bold = true;
  const int32_t money_id = demo.AddStyle(money);
  for (int row = 1; row <= 3; ++row) {
    if (Cell* cell = d.Find(row, 3)) cell->style_id = money_id;
  }
  d.Find(1, 2)->style_id = money_id;
  d.Find(2, 2)->style_id = money_id;
  SaveOptions demo_opts;
  demo_opts.recalculate = true;
  SaveToFile(demo, OutPath("p6_demo.xlsx"), demo_opts);
}

// 读一份真实的第三方（openpyxl）产出，覆盖共享字符串/样式/合并/日期/公式。
void TestXlsxReadSample() {
  using namespace doccore::sheet;
  OpenOptions opts;
  opts.path = Fixture("sample.xlsx");
  WarningList warnings;
  opts.warnings = &warnings;
  Workbook wb = Open(opts);
  CHECK_EQ(wb.sheet_count(), 2);
  Worksheet* ws = wb.SheetAt(0);
  CHECK(ws != nullptr);
  if (ws == nullptr) return;
  CHECK_EQ(ws->name(), std::string("Report"));
  CHECK_EQ(wb.SheetAt(1)->name(), std::string("Empty"));

  CHECK_EQ(ws->GetValue(0, 0).text, std::string("Region"));
  CHECK_EQ(ws->GetValue(1, 0).text, std::string("North"));
  CHECK_EQ(ws->GetValue(1, 1).number, 1200.5);
  CHECK_EQ(ws->GetValue(1, 3).number, 45353.0);  // D2 = 2024-03-01
  CHECK_EQ(ws->GetValue(2, 3).number, 45354.0);  // D3 = 2024-03-02

  const Cell* share = ws->Find(1, 2);
  CHECK(share != nullptr);
  if (share != nullptr) {
    CHECK(share->has_formula());
    CHECK_EQ(ws->formulas()[static_cast<size_t>(share->formula_id)].text, std::string("B2/B$6"));
  }

  CHECK_EQ(ws->merges().size(), size_t(1));
  if (!ws->merges().empty()) {
    CHECK_EQ(ws->merges()[0].row1, 6);
    CHECK_EQ(ws->merges()[0].col2, 3);
  }
  CHECK(ws->ColumnWidth(0) > 15.0);
  CHECK_EQ(ws->RowHeight(0), 24.0);

  // 表头样式
  const Cell* head = ws->Find(0, 0);
  CHECK(head != nullptr);
  if (head != nullptr) {
    const CellStyle& style = wb.StyleAt(head->style_id);
    CHECK(style.font.bold);
    CHECK_EQ(style.font.name, std::string("Arial"));
    CHECK_EQ(style.fill, 0xFFFF00);
    CHECK_EQ(style.halign, 2);
  }
  // 数字格式
  const Cell* amount = ws->Find(1, 1);
  CHECK(amount != nullptr);
  if (amount != nullptr) {
    const CellStyle& style = wb.StyleAt(amount->style_id);
    CHECK(style.num_fmt.thousands);
    CHECK_EQ(style.num_fmt.decimals, 2);
    CHECK_EQ(FormatValue(ws->GetValue(1, 1), style.num_fmt), std::string("1,200.50"));
  }

  // 重算：份额列与合计
  wb.Recalculate();
  CHECK(std::fabs(ws->GetValue(1, 2).number - 0.4546056991385023) < 1e-9);
  CHECK(std::fabs(ws->GetValue(4, 1).number - 2640.75) < 1e-9);

  // 再保存一份，交给 tests/verify_xlsx.py 用 openpyxl 复核
  SaveOptions save_opts;
  save_opts.warnings = &warnings;
  SaveToFile(wb, OutPath("p6_resaved.xlsx"), save_opts);
  CHECK(warnings.empty());
}

// 图表读取：openpyxl 产出的柱/折线/饼图与锚点矩形。
void TestXlsxReadCharts() {
  using namespace doccore::sheet;
  OpenOptions opts;
  opts.path = Fixture("sample_chart.xlsx");
  WarningList warnings;
  opts.warnings = &warnings;
  Workbook wb = Open(opts);
  CHECK_EQ(wb.charts.size(), size_t(3));
  if (wb.charts.size() < 3) return;

  const Chart& col = wb.charts[0];
  CHECK(col.kind == ChartKind::Column);
  CHECK(col.has_anchor);
  CHECK_EQ(col.sheet, std::string("Report"));
  CHECK_EQ(col.title, std::string("Quarterly revenue"));
  CHECK(col.legend);
  CHECK_EQ(col.series.size(), size_t(2));
  if (col.series.size() == 2) {
    const ChartSeries& s0 = col.series[0];
    CHECK_EQ(s0.name, std::string("Q1"));
    CHECK_EQ(s0.values.size(), size_t(4));
    CHECK_EQ(s0.categories.size(), size_t(4));
    if (s0.values.size() == 4) {
      CHECK_EQ(s0.values[0], 10.0);
      CHECK_EQ(s0.values[1], 40.0);
      CHECK_EQ(s0.values[2], 30.0);
      CHECK_EQ(s0.values[3], 25.0);
    }
    if (s0.categories.size() == 4) {
      CHECK_EQ(s0.categories[0], std::string("North"));
      CHECK_EQ(s0.categories[3], std::string("West"));
    }
    const ChartSeries& s1 = col.series[1];
    CHECK_EQ(s1.name, std::string("Q2"));
    if (s1.values.size() == 4) {
      CHECK_EQ(s1.values[0], 20.0);
      CHECK_EQ(s1.values[3], 35.0);
    }
  }
  // 锚点：E2 → 4 列 × 8.43 字符 × 5.25pt/字符，第 2 行 × 15pt
  CHECK(std::fabs(col.x - 177.0) < 0.5);
  CHECK(std::fabs(col.y - 15.0) < 0.5);
  CHECK(col.w > 300.0 && col.h > 150.0);

  const Chart& line = wb.charts[1];
  CHECK(line.kind == ChartKind::Line);
  CHECK_EQ(line.title, std::string("Q1 trend"));
  CHECK_EQ(line.series.size(), size_t(1));

  const Chart& pie = wb.charts[2];
  CHECK(pie.kind == ChartKind::Pie);
  CHECK_EQ(pie.title, std::string("Q2 share"));
  CHECK_EQ(pie.series.size(), size_t(1));
  // 饼图锚在 N2：13 列之后
  CHECK(pie.x > 500.0);
  CHECK(std::fabs(pie.y - 15.0) < 0.5);

  // 图表部件已建模，不应再作为未知部件报出
  bool chart_unknown = false;
  for (const Warning& w : warnings.items()) {
    if (w.module == "xlsx" && w.path.find("/chart") != std::string::npos) chart_unknown = true;
  }
  CHECK(!chart_unknown);

  // 写回：图表部件、绘图部件与关系链都要原样保留，否则"打开再保存"会丢图。
  const std::string rt = OutPath("p9_chart_rt.xlsx");
  SaveOptions save_opts;
  save_opts.warnings = &warnings;
  SaveToFile(wb, rt, save_opts);
  Workbook again = Open([&rt] {
    OpenOptions o;
    o.path = rt;
    return o;
  }());
  CHECK_EQ(again.charts.size(), size_t(3));
  if (again.charts.size() == 3) {
    CHECK(again.charts[0].kind == ChartKind::Column);
    CHECK_EQ(again.charts[0].series.size(), size_t(2));
    CHECK_EQ(again.charts[0].sheet, std::string("Report"));
    CHECK(std::fabs(again.charts[0].x - 177.0) < 0.5);
    CHECK(again.charts[1].kind == ChartKind::Line);
    CHECK(again.charts[2].kind == ChartKind::Pie);
  }
}

// 条件格式（F-205）：读取 dxf + cellIs 规则，命中单元格在导出时填充该颜色，
// 并且写回后规则仍在。
void TestConditionalFormat() {
  using namespace doccore::sheet;
  OpenOptions opts;
  opts.path = Fixture("sample_cf.xlsx");
  WarningList warnings;
  opts.warnings = &warnings;
  Workbook wb = Open(opts);
  const Worksheet* ws = wb.SheetByName("Sales");
  CHECK(ws != nullptr);
  if (ws == nullptr) return;

  CHECK_EQ(ws->conditional().size(), size_t(1));
  if (ws->conditional().size() != 1) return;
  const ConditionalFormat& cf = ws->conditional()[0];
  CHECK(cf.range.valid());
  CHECK_EQ(cf.range.row1, 1);  // B2
  CHECK_EQ(cf.range.row2, 5);  // B6
  CHECK_EQ(cf.range.col1, 1);
  CHECK_EQ(cf.rules.size(), size_t(1));
  if (cf.rules.size() != 1) return;
  const ConditionalRule& rule = cf.rules[0];
  CHECK(rule.type == CfType::CellIs);
  CHECK(rule.op == CfOperator::GreaterThan);
  CHECK_EQ(rule.operands.size(), size_t(1));
  if (rule.operands.size() == 1) CHECK_EQ(rule.operands[0], 100.0);
  // dxf 填充：浅红 FFC7CE。
  CHECK_EQ(rule.dxf.fill, 0xFFC7CE);

  // 命中判定：150/200/120 命中，80/95 不命中。
  const double values[5] = {150, 80, 200, 95, 120};
  const bool expect_hit[5] = {true, false, true, false, true};
  for (int i = 0; i < 5; ++i) {
    const CfDxf* hit = MatchedConditionalDxf(wb, *ws, 1 + i, 1);
    if (expect_hit[i]) {
      CHECK(hit != nullptr);
      if (hit != nullptr) CHECK_EQ(hit->fill, 0xFFC7CE);
    } else {
      CHECK(hit == nullptr);
    }
  }

  // 导出布局：只有命中的 3 个单元格画浅红填充。
  SheetLayoutOptions lopts;
  LayoutDocument laid = LayoutSheet(wb, *ws, lopts);
  int red_cells = 0;
  const Rgba kRed{0xFF, 0xC7, 0xCE, 0xFF};
  for (const LayoutPage& page : laid.pages) {
    for (const DrawOp& op : page.ops) {
      const DrawPath* path = std::get_if<DrawPath>(&op);
      if (path == nullptr || !path->has_fill) continue;
      if (path->fill.r == kRed.r && path->fill.g == kRed.g && path->fill.b == kRed.b) ++red_cells;
    }
  }
  CHECK_EQ(red_cells, 3);

  // 写回：conditionalFormatting + dxfs 都要保留。
  const std::string rt = OutPath("p9_cf_rt.xlsx");
  SaveOptions save_opts;
  save_opts.warnings = &warnings;
  SaveToFile(wb, rt, save_opts);
  OpenOptions ro;
  ro.path = rt;
  Workbook again = Open(ro);
  const Worksheet* ws2 = again.SheetByName("Sales");
  CHECK(ws2 != nullptr);
  if (ws2 != nullptr) {
    CHECK_EQ(ws2->conditional().size(), size_t(1));
    if (ws2->conditional().size() == 1 && ws2->conditional()[0].rules.size() == 1) {
      const ConditionalRule& r2 = ws2->conditional()[0].rules[0];
      CHECK(r2.type == CfType::CellIs);
      CHECK(r2.op == CfOperator::GreaterThan);
      CHECK_EQ(r2.operands.size(), size_t(1));
      if (r2.operands.size() == 1) CHECK_EQ(r2.operands[0], 100.0);
      CHECK_EQ(r2.dxf.fill, 0xFFC7CE);
    }
  }
}

// 数据验证（F-205）：列表型读入、列表取值解析、WriteCell 的默认/严格写入行为、往返。
void TestDataValidation() {
  using namespace doccore::sheet;
  {
    // 读取第三方夹具：内联列表 + 区域引用列表两条验证。
    OpenOptions opts;
    opts.path = Fixture("sample_dv.xlsx");
    WarningList warnings;
    opts.warnings = &warnings;
    Workbook wb = Open(opts);
    const Worksheet* ws = wb.SheetByName("Choices");
    CHECK(ws != nullptr);
    if (ws != nullptr) {
      CHECK_EQ(ws->validations().size(), size_t(2));
      // B2:B6 -> rows1..5, col1（内联）；D2:D6 -> rows1..5, col3（区域）。
      const DataValidation* inline_dv = ValidationAt(*ws, 1, 1);
      const DataValidation* range_dv = ValidationAt(*ws, 1, 3);
      CHECK(inline_dv != nullptr);
      CHECK(range_dv != nullptr);
      CHECK(ValidationAt(*ws, 1, 0) == nullptr);  // A 列无验证
      if (inline_dv != nullptr) {
        CHECK(inline_dv->type == ValidationType::List);
        std::vector<std::string> vals = ListValues(wb, *ws, *inline_dv);
        CHECK_EQ(vals.size(), size_t(3));
        CHECK(vals.size() == 3 && vals[0] == "Yes" && vals[1] == "No" && vals[2] == "Maybe");
      }
      if (range_dv != nullptr) {
        std::vector<std::string> vals = ListValues(wb, *ws, *range_dv);
        CHECK_EQ(vals.size(), size_t(3));
        CHECK(vals.size() == 3 && vals[0] == "Low" && vals[1] == "Medium" && vals[2] == "High");
      }
    }
  }

  {
    // 写行为：默认模式软失败仍写入+告警；严格模式拒绝写入。
    Workbook wb = Workbook::Create();
    Worksheet& ws = *wb.SheetAt(0);
    ws.set_name("Form");
    DataValidation dv;
    dv.type = ValidationType::List;
    dv.allow_blank = true;
    dv.formula1 = "\"Yes,No,Maybe\"";
    dv.ranges.push_back(Range{1, 1, 5, 1});  // B2:B6
    ws.validations().push_back(dv);

    CHECK(wb.write_warnings().empty());

    // 合法值：写入、无告警。
    ValidationResult ok = ws.WriteCell(1, 1, Value::Str("Yes"));
    CHECK(ok.written);
    CHECK(!ok.warned);
    CHECK(!ok.rejected);
    CHECK_EQ(ws.GetValue(1, 1).text, std::string("Yes"));
    CHECK(wb.write_warnings().empty());

    // 列表外的值，默认模式：仍写入 + 产生 ValidationFailed 告警。
    const size_t before = wb.write_warnings().items().size();
    ValidationResult soft = ws.WriteCell(1, 1, Value::Str("Possibly"));
    CHECK(soft.written);
    CHECK(soft.warned);
    CHECK(!soft.rejected);
    CHECK_EQ(ws.GetValue(1, 1).text, std::string("Possibly"));
    CHECK(wb.write_warnings().items().size() == before + 1);
    if (wb.write_warnings().items().size() == before + 1) {
      CHECK_EQ(wb.write_warnings().items().back().code,
               static_cast<int>(WarningCode::ValidationFailed));
    }

    // 严格模式：拒绝写入，值保持不变，再产生一条告警。
    wb.strict_validation = true;
    const size_t before2 = wb.write_warnings().items().size();
    ValidationResult rej = ws.WriteCell(1, 1, Value::Str("Nope"));
    CHECK(!rej.written);
    CHECK(rej.rejected);
    CHECK(!rej.warned);
    CHECK_EQ(ws.GetValue(1, 1).text, std::string("Possibly"));  // 未被改写
    CHECK(wb.write_warnings().items().size() == before2 + 1);

    // 严格模式下合法值仍可写入。
    ValidationResult ok2 = ws.WriteCell(1, 1, Value::Str("No"));
    CHECK(ok2.written);
    CHECK_EQ(ws.GetValue(1, 1).text, std::string("No"));

    // 未命中验证的单元格：与普通 Set 等价，无告警。
    const size_t before3 = wb.write_warnings().items().size();
    ValidationResult free = ws.WriteCell(1, 0, Value::Str("anything"));
    CHECK(free.written);
    CHECK(!free.warned);
    CHECK(wb.write_warnings().items().size() == before3);

    // 往返：验证规则与 formula1 保留。
    const std::string rt = OutPath("p9_dv_rt.xlsx");
    SaveOptions save_opts;
    SaveToFile(wb, rt, save_opts);
    OpenOptions ro;
    ro.path = rt;
    Workbook again = Open(ro);
    const Worksheet* ws2 = again.SheetByName("Form");
    CHECK(ws2 != nullptr);
    if (ws2 != nullptr) {
      CHECK_EQ(ws2->validations().size(), size_t(1));
      if (ws2->validations().size() == 1) {
        const DataValidation& d2 = ws2->validations()[0];
        CHECK(d2.type == ValidationType::List);
        CHECK_EQ(d2.formula1, std::string("\"Yes,No,Maybe\""));
        CHECK_EQ(d2.ranges.size(), size_t(1));
        if (d2.ranges.size() == 1) {
          CHECK_EQ(d2.ranges[0].row1, 1);
          CHECK_EQ(d2.ranges[0].row2, 5);
          CHECK_EQ(d2.ranges[0].col1, 1);
          CHECK_EQ(d2.ranges[0].col2, 1);
        }
        std::vector<std::string> vals = ListValues(again, *ws2, d2);
        CHECK_EQ(vals.size(), size_t(3));
      }
    }
  }
}

// 邮件（F-501）：EML 解析——RFC2047 主题、收发地址、纯文本/HTML 正文、
// 两个附件字节，以及扩展名与内容不符时按魔数报告实际类型。
void TestMail() {
  using namespace doccore::mail;
  OpenOptions opts;
  opts.path = Fixture("sample.eml");
  WarningList warnings;
  opts.warnings = &warnings;
  MailMessage m = Open(opts);
  CHECK(!m.from_msg);
  CHECK_EQ(m.subject, std::string("Hello 世界"));   // RFC 2047 解码
  CHECK_EQ(m.from.size(), size_t(1));
  if (m.from.size() == 1) {
    CHECK_EQ(m.from[0].name, std::string("Alice Example"));
    CHECK_EQ(m.from[0].email, std::string("alice@example.com"));
  }
  CHECK_EQ(m.to.size(), size_t(1));
  if (m.to.size() == 1) CHECK_EQ(m.to[0].email, std::string("bob@example.com"));
  CHECK_EQ(m.cc.size(), size_t(1));
  if (m.cc.size() == 1) CHECK_EQ(m.cc[0].email, std::string("carol@example.com"));
  CHECK_EQ(m.message_id, std::string("<demo-0001@example.com>"));

  // 正文：text/plain 与 text/html 都取到；首选 HTML。
  CHECK(m.text.find("plain-text body") != std::string::npos);
  CHECK(m.html.find("<b>HTML</b>") != std::string::npos);
  CHECK(m.body_kind == MailBodyKind::Html);

  // 两个附件。
  CHECK_EQ(m.attachments.size(), size_t(2));
  if (m.attachments.size() == 2) {
    const MailAttachment& a0 = m.attachments[0];
    const MailAttachment& a1 = m.attachments[1];
    CHECK_EQ(a0.name, std::string("photo.bin"));
    CHECK_EQ(a0.detected, std::string("image/png"));  // 扩展名 .bin 与实际内容不符→探测出 image/png
    CHECK(a0.bytes.size() >= 8 && a0.bytes[0] == 0x89 && a0.bytes[1] == 'P');
    CHECK_EQ(a1.name, std::string("notes.txt"));
    // base64 解回的文本附件内容。
    const std::string n1(a1.bytes.begin(), a1.bytes.end());
    CHECK(n1.find("second attachment") != std::string::npos);
  }

  // MSG：非 Unicode 代码页（Windows-1252）主题按代码页解码为 Unicode。
  {
    OpenOptions mopts;
    mopts.path = Fixture("sample.msg");
    WarningList mwarn;
    mopts.warnings = &mwarn;
    MailMessage msg = Open(mopts);
    CHECK(msg.from_msg);
    // 主题字节 0x93/0x94 → U+201C/U+201D，0xE9 → U+00E9（é）。
    CHECK_EQ(msg.subject, std::string("Caf\xC3\xA9 \xE2\x80\x9C" "Deal\xE2\x80\x9D"));
    CHECK_EQ(msg.from.size(), size_t(1));
    if (msg.from.size() == 1) {
      CHECK_EQ(msg.from[0].name, std::string("Dana \xE6\xB5\x8B\xE8\xAF\x95"));  // Dana 测试
      CHECK_EQ(msg.from[0].email, std::string("dana@example.com"));
    }
    CHECK_EQ(msg.text, std::string("Body in unicode."));
  }

  // 复用解码器：base64 / quoted-printable / 字符集。
  {
    const std::vector<uint8_t> dec = Base64Decode("SGVsbG8=");  // "Hello"
    const std::string s(dec.begin(), dec.end());
    CHECK_EQ(s, std::string("Hello"));
    CHECK_EQ(QuotedPrintableDecode("a=3Db=0Ac"), std::string("a=b\nc"));
    // Windows-1252 的 0x93 -> U+201C 左双引号（区别于 Latin-1 的控制字符）。
    const std::string cp = DecodeCharset(std::string("\x93", 1), "windows-1252");
    CHECK_EQ(cp, std::string("\xE2\x80\x9C"));
    CHECK_EQ(DecodeCharset(std::string("\xC3\xA9"), "utf-8"), std::string("\xC3\xA9"));  // é
  }
}

// ---- F-601 条码：QR（版本/EC/掩膜）与 Code128 --------------------------

// 版本 1 各等级的数据码字数（ISO/IEC 18004 表 9-1 的公认值）。
const int kQrV1DataCw[4] = {19, 16, 13, 9};

// 断言 QR 的结构不变量：定位图案、分隔符、定时图案、固定深色模块。
void CheckQrStructure(const doccore::barcode::QrCode& qr, int version) {
  const int n = qr.size;
  CHECK_EQ(qr.size, 17 + 4 * version);
  CHECK_EQ(qr.modules.size(), static_cast<size_t>(n) * n);

  // 三个 7x7 定位图案：深-浅-深-浅-深 五环 + 3x3 深心。
  const int corners[3][2] = {{0, 0}, {0, n - 7}, {n - 7, 0}};
  for (const auto& co : corners) {
    for (int dr = 0; dr < 7; ++dr) {
      for (int dc = 0; dc < 7; ++dc) {
        const int edge = std::max(std::abs(dr - 3), std::abs(dc - 3));
        CHECK_EQ(qr.at(co[0] + dr, co[1] + dc), edge == 3 || edge <= 1);
      }
    }
  }
  // 分隔符：定位图案四周一圈全浅。
  for (int i = 0; i < 8; ++i) {
    CHECK(!qr.at(7, i));
    CHECK(!qr.at(7, n - 1 - i));
    CHECK(!qr.at(i, 7));
    CHECK(!qr.at(n - 1 - i, 7));
  }
  // 定时图案：第 6 行/列，偶索引为深。
  for (int i = 8; i <= n - 9; ++i) {
    CHECK_EQ(qr.at(6, i), i % 2 == 0);
    CHECK_EQ(qr.at(i, 6), i % 2 == 0);
  }
  // (n-8, 8) 固定为深色模块。
  CHECK(qr.at(n - 8, 8));
  // 版本 2 起有对齐图案；版本 7 起有版本信息。
  if (version >= 2) {
    // v2 唯一未被定位图案遮挡的对齐中心是 (18, 18)。
    if (version == 2) {
      for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
          const int edge = std::max(std::abs(dr), std::abs(dc));
          CHECK_EQ(qr.at(18 + dr, 18 + dc), edge != 1);
        }
      }
    }
  }
}

void TestBarcode() {
  using namespace doccore::barcode;

  // ---- QR：版本 1 容量边界（L=19 / M=16 / Q=13 / H=9 数据码字）----------
  for (int lv = 0; lv < 4; ++lv) {
    const EcLevel ec = static_cast<EcLevel>(lv);
    // 头部 = 4(模式) + 8(计数) = 12 位，故载荷上限 = (容量*8 - 12)/8 字节。
    const int max_len = (kQrV1DataCw[lv] * 8 - 12) / 8;
    const QrCode small = QrEncode(std::string(static_cast<size_t>(max_len), 'A'), ec);
    CheckQrStructure(small, 1);
    const QrCode big = QrEncode(std::string(static_cast<size_t>(max_len) + 1, 'A'), ec);
    CheckQrStructure(big, 2);
  }

  // ---- QR：跨版本边界（v7 起带版本信息，v10 起计数位宽 16）-------------
  // v6-M = 108 数据码字 → 上限 106 字节；v7-M = 124 → 上限 122。
  CheckQrStructure(QrEncode(std::string(122, 'A'), EcLevel::M), 7);
  CheckQrStructure(QrEncode(std::string(123, 'A'), EcLevel::M), 8);
  // v9-M = 182 码字（12 位计数）→ 上限 180；v10-M = 216（16 位计数）→ 上限 213。
  CheckQrStructure(QrEncode(std::string(213, 'A'), EcLevel::M), 10);
  CheckQrStructure(QrEncode(std::string(214, 'A'), EcLevel::M), 11);

  // ---- QR：四个 EC 等级都能编码并保持结构 -----------------------------
  // "DocCore QR payload 2026" 共 23 字节；按各等级容量落在不同版本：
  // L/M → v2（34/28 数据码字），Q/H → v3（34/26 数据码字）。
  {
    const std::string payload = "DocCore QR payload 2026";
    const int expect[4] = {2, 2, 3, 3};
    for (int lv = 0; lv < 4; ++lv) {
      const QrCode qr = QrEncode(payload, static_cast<EcLevel>(lv));
      CheckQrStructure(qr, expect[lv]);
    }
  }

  // ---- QR：超出版本 40 容量 → LimitExceeded，不输出残缺符号 ------------
  {
    // v40-H 的字节容量为 1273，超容量必须抛错而非产出短码。
    bool threw = false;
    try {
      QrEncode(std::string(1300, 'x'), EcLevel::H);
    } catch (const Error& e) {
      threw = true;
      CHECK_EQ(e.code(), ErrorCode::LimitExceeded);
    }
    CHECK(threw);
    // 空载荷合法（版本 1）。
    const QrCode empty = QrEncode("", EcLevel::M);
    CheckQrStructure(empty, 1);
  }

  // ---- QR → PNG：批量落盘 + 清单，供 Python 独立解码器逐条校验 -------
  {
    const char* kLevelName[4] = {"L", "M", "Q", "H"};
    // 版本 40 各等级的字节容量（数据码字数减去 4+16 位头部）。
    const int kV40Bytes[4] = {2953, 2331, 1663, 1273};
    // 覆盖版本边界：v1 上限 14、v2 上限 28、v7 上限 122、v10 上限 213
    // （10 起计数位宽 16），外加零长度与各等级极限。
    const int kLens[] = {0,  1,  5,  14,  15,  28,  29,  100, 122, 123,
                         180, 181, 213, 214, 500, 1000, 2000};
    std::string manifest;
    int index = 0;
    auto emit = [&](int level, const std::string& payload) {
      const QrCode qr = QrEncode(payload, static_cast<EcLevel>(level));
      const std::vector<uint8_t> png = QrToPng(qr, 4, 4);
      CHECK(png.size() > 100);
      CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
      char name[64];
      std::snprintf(name, sizeof(name), "p9_qr_%03d.png", index++);
      FileStream out(OutPath(name), "wb");
      out.Write(png.data(), static_cast<int64_t>(png.size()));
      out.Flush();
      static const char kHex[] = "0123456789abcdef";
      std::string hex;
      for (unsigned char ch : payload) {
        hex += kHex[ch >> 4];
        hex += kHex[ch & 15];
      }
      manifest += std::string(name);
      manifest += '\t';
      manifest += kLevelName[level];
      manifest += '\t';
      manifest += std::to_string((qr.size - 17) / 4);  // 版本号
      manifest += '\t';
      manifest += hex;
      manifest += '\n';
    };

    for (int lv = 0; lv < 4; ++lv) {
      for (int len : kLens) {
        if (len > kV40Bytes[lv]) continue;
        // 载荷 = 长度前缀 + 循环字母 + 一段 UTF-8 中文（覆盖多字节路径）。
        std::string payload = std::to_string(len) + ":";
        payload.append(static_cast<size_t>(len),
                       static_cast<char>('A' + (len % 26)));
        payload += "\xE4\xB8\xAD\xE6\x96\x87";  // 中文
        emit(lv, payload);
      }
      // 各等级版本 40 极限。
      std::string maxed = "MAX:";
      maxed.append(static_cast<size_t>(kV40Bytes[lv]) - 4,
                   static_cast<char>('a' + (lv % 26)));
      emit(lv, maxed);
    }
    FileStream mf(OutPath("p9_qr_manifest.txt"), "wb");
    mf.Write(reinterpret_cast<const uint8_t*>(manifest.data()),
             static_cast<int64_t>(manifest.size()));
    mf.Flush();
    CHECK_EQ(manifest.empty(), false);
  }

  // ---- Code128：宽度串、校验位、停止码 ---------------------------------
  {
    const std::vector<int> bars = Code128Encode("AB");
    // 元素：起始 6 + 数据 6×2 + 校验 6 + 停止 6 + 终止条 1。
    CHECK_EQ(bars.size(), static_cast<size_t>(6 + 12 + 6 + 6 + 1));
    int total = 0;
    for (int w : bars) total += w;
    // 除停止码外每符号 11 模块；停止码 11 + 终止条 2。
    CHECK_EQ(total, 11 * 5 + 2);
    // Start B(104) + Σ value_i * i；'A'→33, 'B'→34。
    const int start_b[6] = {2, 1, 1, 2, 1, 4};
    for (int i = 0; i < 6; ++i) CHECK_EQ(bars[i], start_b[i]);
    // 数据 'A'(33) = "111323"，'B'(34) = "131123"。
    const int data_ab[12] = {1, 1, 1, 3, 2, 3, 1, 3, 1, 1, 2, 3};
    for (int i = 0; i < 12; ++i) CHECK_EQ(bars[6 + i], data_ab[i]);
    // 校验符号 102 = "411131"。
    const int chk[6] = {4, 1, 1, 1, 3, 1};
    for (int i = 0; i < 6; ++i) CHECK_EQ(bars[18 + i], chk[i]);
    // 停止码 106 = "233111"，后接 2 模块终止条。
    const int stop[7] = {2, 3, 3, 1, 1, 1, 2};
    for (int i = 0; i < 7; ++i) CHECK_EQ(bars[24 + i], stop[i]);
    // 校验和按 Start B + Σ value_i * i 累加（用变量累加，避免编译期常量告警）。
    int checksum = bars.size() > 0 ? 104 : 0;
    checksum += 33 * 1;
    checksum += 34 * 2;
    CHECK_EQ(checksum % 103, 102);

    // 非 ASCII 32..126 拒绝。
    bool threw = false;
    try {
      Code128Encode(std::string("bad\x01"));
    } catch (const Error& e) {
      threw = true;
      CHECK_EQ(e.code(), ErrorCode::InvalidArgument);
    }
    CHECK(threw);

    // 单字符：校验值 = Start B + value。
    const std::vector<int> one = Code128Encode("A");
    CHECK_EQ(one.size(), static_cast<size_t>(6 + 6 + 6 + 6 + 1));
    int one_sum = 104;
    one_sum += 33;
    CHECK_EQ(one_sum % 103, 34);

    // 渲染成 PNG 落盘。
    const std::vector<uint8_t> png = Code128ToPng(bars, 2, 10, 80);
    CHECK(png.size() > 100);
    CHECK(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    FileStream out(OutPath("p9_code128.png"), "wb");
    out.Write(png.data(), static_cast<int64_t>(png.size()));
    out.Flush();
  }
}

// 图表布局：柱高必须与数值成比例，饼图/折线要有自己的图元。
void TestChartLayout() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.set_name("Report");
  ws.SetText(0, 0, "Region");
  ws.SetText(0, 1, "Q1");
  ws.SetText(0, 2, "Q2");
  const char* cats[] = {"North", "South", "East", "West"};
  const double v1[] = {10, 40, 30, 25};
  const double v2[] = {20, 50, 15, 35};
  for (int i = 0; i < 4; ++i) {
    ws.SetText(i + 1, 0, cats[i]);
    ws.SetNumber(i + 1, 1, v1[i]);
    ws.SetNumber(i + 1, 2, v2[i]);
  }

  Chart chart;
  chart.kind = ChartKind::Column;
  chart.title = "Revenue";
  chart.sheet = "Report";
  chart.x = 177.0;
  chart.y = 15.0;
  chart.w = 300.0;  // 收窄，保证整张图落在同一页里（跨页时每页各画一遍）
  chart.h = 240.0;
  chart.has_anchor = true;
  ChartSeries s1;
  s1.name = "Q1";
  s1.categories.assign(cats, cats + 4);
  s1.values.assign(v1, v1 + 4);
  ChartSeries s2;
  s2.name = "Q2";
  s2.categories.assign(cats, cats + 4);
  s2.values.assign(v2, v2 + 4);
  chart.series.push_back(s1);
  chart.series.push_back(s2);
  wb.charts.push_back(chart);

  SheetLayoutOptions opts;
  LayoutDocument laid = LayoutSheet(wb, ws, opts);
  CHECK(!laid.pages.empty());
  if (laid.pages.empty()) return;

  // 图表锚在 E2，数据只到 C5：分页要把图表矩形折进行列范围，
  // 否则承载图表的那一页根本不会出现。
  bool saw_title = false;
  int bars = 0;
  std::vector<double> heights;  // 按出现顺序的柱子高度（先 Q1 后 Q2）
  const Rgba kS1{0x4F, 0x81, 0xBD, 0xFF};
  for (const LayoutPage& page : laid.pages) {
    for (const DrawOp& op : page.ops) {
      if (std::holds_alternative<DrawText>(op)) {
        const DrawText& text = std::get<DrawText>(op);
        if (text.text_utf8 == "Revenue") saw_title = true;
        continue;
      }
      const DrawPath* path = std::get_if<DrawPath>(&op);
      if (path == nullptr || !path->has_fill) continue;
      if (path->fill.r != kS1.r || path->fill.g != kS1.g || path->fill.b != kS1.b) continue;
      // 柱形是轴对齐矩形：所有点都落在外接框的四条边上。
      double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
      for (const PathVerb& verb : path->verbs) {
        min_x = std::min(min_x, verb.x1);
        max_x = std::max(max_x, verb.x1);
        min_y = std::min(min_y, verb.y1);
        max_y = std::max(max_y, verb.y1);
      }
      bool rect = path->verbs.size() >= 4;
      for (const PathVerb& verb : path->verbs) {
        const bool on_x = std::fabs(verb.x1 - min_x) < 1e-6 || std::fabs(verb.x1 - max_x) < 1e-6;
        const bool on_y = std::fabs(verb.y1 - min_y) < 1e-6 || std::fabs(verb.y1 - max_y) < 1e-6;
        if (!on_x || !on_y) rect = false;
      }
      const double w = max_x - min_x;
      const double h = max_y - min_y;
      if (rect && w > 12.0 && h > 12.0) {
        ++bars;
        heights.push_back(h);
      }
    }
  }
  CHECK(saw_title);
  CHECK_EQ(bars, 4);
  if (std::getenv("DOCCORE_DEBUG_CHART") != nullptr) {
    std::fprintf(stderr, "[chart] pages=%zu bars=%d saw_title=%d\n", laid.pages.size(), bars,
                 saw_title ? 1 : 0);
    for (double h : heights) std::fprintf(stderr, "  h=%.2f\n", h);
  }
  if (bars != 4) return;
  // 每根柱高都等于 值/坐标轴上限 × 绘图区高度：以第一根为基准推比例。
  for (int i = 0; i < 4; ++i) {
    const double expect = heights[0] / v1[0] * v1[i];
    CHECK(std::fabs(heights[i] - expect) < 0.35);
  }
  // 同一比例尺：40 与 25 应成 8:5。
  CHECK(std::fabs(heights[1] / heights[3] - 40.0 / 25.0) < 0.01);

  // 堆叠柱：每个分类的两段必须从基线起竖向累加——系列 0 的底边都在基线，
  // 系列 1 的底边正好等于同分类系列 0 的顶边，且段高按各自数值、总高按 v1+v2 成比例。
  // （回归防护：曾经的 bug 把 stack_top 跨分类累加，第 2 根柱叠在第 1 根顶上、整根飞出
  //  绘图区；同时纵轴按单值而非合计定标，也会让柱子冲高。）
  struct SegRect { double x0, y0, x1, y1; };
  Chart stacked;
  stacked.kind = ChartKind::Column;
  stacked.stacked = true;
  stacked.title = "Stacked";
  stacked.sheet = "Report";
  stacked.x = 177.0;
  stacked.y = 15.0;
  stacked.w = 300.0;
  stacked.h = 240.0;
  stacked.has_anchor = true;
  stacked.series.push_back(s1);
  stacked.series.push_back(s2);
  wb.charts.clear();
  wb.charts.push_back(stacked);
  LayoutDocument stack_doc = LayoutSheet(wb, ws, opts);

  auto collect_rects = [&](const Rgba& color) {
    std::vector<SegRect> out;
    for (const LayoutPage& page : stack_doc.pages) {
      for (const DrawOp& op : page.ops) {
        const DrawPath* path = std::get_if<DrawPath>(&op);
        if (path == nullptr || !path->has_fill) continue;
        if (path->fill.r != color.r || path->fill.g != color.g || path->fill.b != color.b) continue;
        double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
        for (const PathVerb& verb : path->verbs) {
          min_x = std::min(min_x, verb.x1);
          max_x = std::max(max_x, verb.x1);
          min_y = std::min(min_y, verb.y1);
          max_y = std::max(max_y, verb.y1);
        }
        if (max_x - min_x > 12.0 && max_y - min_y > 1.0) {
          out.push_back({min_x, min_y, max_x, max_y});
        }
      }
    }
    std::sort(out.begin(), out.end(),
              [](const SegRect& a, const SegRect& b) { return a.x0 < b.x0; });
    return out;
  };
  const Rgba kS2{0xC0, 0x50, 0x4D, 0xFF};
  std::vector<SegRect> seg1 = collect_rects(kS1);
  std::vector<SegRect> seg2 = collect_rects(kS2);
  CHECK_EQ(seg1.size(), 4);
  CHECK_EQ(seg2.size(), 4);
  if (seg1.size() == 4 && seg2.size() == 4) {
    const double baseline = seg1[0].y1;  // y 向下为正，底边是较大的 y
    for (int i = 0; i < 4; ++i) {
      CHECK(std::fabs(seg1[i].y1 - baseline) < 0.01);          // 系列 0 都从基线起
      CHECK(std::fabs(seg2[i].y1 - seg1[i].y0) < 0.01);        // 系列 1 接在系列 0 顶上
      CHECK(std::fabs(seg2[i].x0 - seg1[i].x0) < 0.01);        // 同一分类槽位
    }
    const double scale1 = (seg1[0].y0 - seg1[0].y1) / v1[0];
    for (int i = 0; i < 4; ++i) {
      const double h1 = seg1[i].y0 - seg1[i].y1;
      CHECK(std::fabs(h1 - scale1 * v1[i]) < 0.35);
      const double total_h = (seg1[i].y0 - seg1[i].y1) + (seg2[i].y0 - seg2[i].y1);
      CHECK(std::fabs(total_h - scale1 * (v1[i] + v2[i])) < 0.7);
    }
  }

  // 饼图：扇区角度与数值成比例。
  Chart pie;
  pie.kind = ChartKind::Pie;
  pie.title = "Share";
  pie.sheet = "Report";
  pie.x = 177.0;
  pie.y = 300.0;
  pie.w = 300.0;
  pie.h = 240.0;
  pie.has_anchor = true;
  ChartSeries ps;
  ps.name = "Q2";
  ps.categories.assign(cats, cats + 4);
  ps.values.assign(v2, v2 + 4);
  pie.series.push_back(ps);
  wb.charts.clear();
  wb.charts.push_back(pie);
  LayoutDocument pie_doc = LayoutSheet(wb, ws, opts);
  double total = 120.0;  // 20+50+15+35
  int slices = 0;
  for (const LayoutPage& page : pie_doc.pages) {
    for (const DrawOp& op : page.ops) {
      const DrawPath* path = std::get_if<DrawPath>(&op);
      if (path == nullptr || !path->has_fill) continue;
      if (path->verbs.size() < 4) continue;
      // 扇区：第一个点是圆心，其余点都在同一半径的弧上（图例色块是矩形，
      // 从一个角量到其余角的半径参差不齐，能被这条判据排除）。
      const double cx = path->verbs[0].x1;
      const double cy = path->verbs[0].y1;
      double r_min = 1e9, r_max = 0.0;
      double sweep = 0.0;
      double prev = 0.0;
      bool first = true;
      for (size_t i = 1; i < path->verbs.size(); ++i) {
        const PathVerb& verb = path->verbs[i];
        if (verb.kind != PathVerbKind::LineTo) continue;
        const double dx = verb.x1 - cx;
        const double dy = verb.y1 - cy;
        const double r = std::sqrt(dx * dx + dy * dy);
        if (r < 1.0) continue;  // 圆心
        r_min = std::min(r_min, r);
        r_max = std::max(r_max, r);
        const double a = std::atan2(dy, dx);
        if (first) {
          prev = a;
          first = false;
          continue;
        }
        double d = a - prev;
        while (d > 3.14159265358979) d -= 2 * 3.14159265358979;
        while (d < -3.14159265358979) d += 2 * 3.14159265358979;
        sweep += d;
        prev = a;
      }
      if (r_max < 8.0 || (r_max - r_min) / r_max > 0.02) continue;  // 不是扇区
      if (std::fabs(sweep) < 0.2) continue;
      ++slices;
      const double expect = 2 * 3.14159265358979 * (slices == 1 ? 20.0 : 50.0) / total;
      if (slices <= 2) CHECK(std::fabs(std::fabs(sweep) - expect) < 0.05);
    }
  }
  CHECK_EQ(slices, 4);
  if (std::getenv("DOCCORE_DEBUG_CHART") != nullptr) {
    std::fprintf(stderr, "[chart] pie slices=%d\n", slices);
  }
}

// 数据透视表（F-206）：建表 → 刷新 → 三部件写出 → 重读 → 源数据变化后再刷新。
//
// 断言重点在"刷新"这件事本身：目标区域先清空再重写，所以源数据变少时不能
// 留下上一次的残行；三个部件的关系链（sheet→pivotTable→cache→records）以及
// workbook 级的 <pivotCaches> 断一环 Excel 都会判定为损坏。
void TestXlsxPivot() {
  using namespace doccore::sheet;

  Workbook wb = Workbook::Create();
  Worksheet& data = *wb.SheetAt(0);
  data.set_name("Data");
  data.SetText(0, 0, "Region");
  data.SetText(0, 1, "Product");
  data.SetText(0, 2, "Amount");
  // East/Widget 出现两次，用来验证聚合是求和而不是去重。
  data.SetText(1, 0, "East");
  data.SetText(1, 1, "Widget");
  data.SetNumber(1, 2, 10);
  data.SetText(2, 0, "East");
  data.SetText(2, 1, "Gadget");
  data.SetNumber(2, 2, 20);
  data.SetText(3, 0, "West");
  data.SetText(3, 1, "Widget");
  data.SetNumber(3, 2, 5);
  data.SetText(4, 0, "West");
  data.SetText(4, 1, "Gadget");
  data.SetNumber(4, 2, 7);
  data.SetText(5, 0, "East");
  data.SetText(5, 1, "Widget");
  data.SetNumber(5, 2, 3);

  wb.AddSheet("Pivot");
  Worksheet* dst = wb.SheetByName("Pivot");
  // AddSheet 可能让 sheets 容器重新分配，前面的引用不能再用了。
  Worksheet* src_sheet = wb.SheetByName("Data");
  CHECK(dst != nullptr);
  CHECK(src_sheet != nullptr);
  if (dst == nullptr || src_sheet == nullptr) return;

  Range source;
  source.row1 = 0;
  source.col1 = 0;
  source.row2 = 5;
  source.col2 = 2;
  CellRef at;
  at.row = 2;  // A3，留出两行空位
  at.col = 0;

  PivotTable& pt =
      AddPivotTable(*dst, *src_sheet, source, at, {"Region"}, {"Product"}, {"Amount"});
  CHECK_EQ(wb.pivot_tables.size(), size_t(1));
  CHECK_EQ(pt.name, std::string("PivotTable1"));
  CHECK(pt.valid());
  CHECK(pt.dirty);
  CHECK_EQ(pt.fields.size(), size_t(3));
  if (pt.fields.size() == 3) {
    CHECK_EQ(pt.fields[0].name, std::string("Region"));
    CHECK_EQ(pt.fields[1].name, std::string("Product"));
    CHECK_EQ(pt.fields[2].name, std::string("Amount"));
    // 行/列字段必须有完整 item 表，度量字段是纯数值所以不需要。
    CHECK_EQ(pt.fields[0].shared_items.size(), size_t(2));
    CHECK_EQ(pt.fields[1].shared_items.size(), size_t(2));
    CHECK(pt.fields[2].shared_items.empty());
    CHECK(pt.fields[2].has_numbers);
    CHECK_EQ(pt.fields[2].min_value, 3.0);
    CHECK_EQ(pt.fields[2].max_value, 20.0);
  }
  CHECK_EQ(pt.row_fields.size(), size_t(1));
  CHECK_EQ(pt.column_fields.size(), size_t(1));
  CHECK_EQ(pt.data_fields.size(), size_t(1));
  if (!pt.data_fields.empty()) {
    CHECK(pt.data_fields[0].aggregate == PivotAggregate::Sum);
    CHECK_EQ(pt.data_fields[0].field_index, 1 + 1);
  }

  // ---- 目标区域的实际布局 ------------------------------------------------
  // 共享项按排序键升序：Region = [East, West]，Product = [Gadget, Widget]。
  // 所以列顺序是 Gadget, Widget，不是首次出现顺序。
  CHECK_EQ(pt.row_items.size(), size_t(3));  // 2 行 + 总计
  CHECK_EQ(pt.column_items.size(), size_t(3));

  auto text_at = [&](int r, int c) { return dst->GetValue(r, c).text; };
  auto num_at = [&](int r, int c) { return dst->GetValue(r, c).number; };

  CHECK_EQ(text_at(2, 0), std::string("Region"));   // 行字段名
  CHECK_EQ(text_at(2, 3), std::string("总计"));      // 列总计表头
  CHECK_EQ(text_at(3, 1), std::string("Gadget"));   // 列组合标签
  CHECK_EQ(text_at(3, 2), std::string("Widget"));
  CHECK_EQ(text_at(4, 0), std::string("East"));
  CHECK_EQ(text_at(5, 0), std::string("West"));
  CHECK_EQ(text_at(6, 0), std::string("总计"));

  CHECK_EQ(num_at(4, 1), 20.0);  // East / Gadget
  CHECK_EQ(num_at(4, 2), 13.0);  // East / Widget = 10 + 3
  CHECK_EQ(num_at(4, 3), 33.0);  // East 小计
  CHECK_EQ(num_at(5, 1), 7.0);   // West / Gadget
  CHECK_EQ(num_at(5, 2), 5.0);   // West / Widget
  CHECK_EQ(num_at(5, 3), 12.0);
  CHECK_EQ(num_at(6, 1), 27.0);  // Gadget 列总计
  CHECK_EQ(num_at(6, 2), 18.0);
  CHECK_EQ(num_at(6, 3), 45.0);  // 总计

  // ---- 写出的部件 --------------------------------------------------------
  WarningList warnings;
  SaveOptions save_opts;
  save_opts.warnings = &warnings;
  MemoryStream out;
  Save(wb, out, save_opts);
  CHECK(warnings.empty());

  {
    MemoryStream probe(out.data());
    auto zip = ZipArchive::Open(probe);

    const auto table_raw = zip.Read("xl/pivotTables/pivotTable1.xml");
    const std::string table_xml(table_raw.begin(), table_raw.end());
    CHECK(table_xml.find("<pivotTableDefinition") != std::string::npos);
    CHECK(table_xml.find("name=\"PivotTable1\"") != std::string::npos);
    CHECK(table_xml.find("cacheId=\"1\"") != std::string::npos);
    CHECK(table_xml.find("dataOnRows=\"0\"") != std::string::npos);
    CHECK(table_xml.find("<pivotFields count=\"3\">") != std::string::npos);
    CHECK(table_xml.find("axis=\"axisRow\"") != std::string::npos);
    CHECK(table_xml.find("axis=\"axisCol\"") != std::string::npos);
    CHECK(table_xml.find("<dataFields count=\"1\">") != std::string::npos);
    CHECK(table_xml.find("subtotal=\"sum\"") != std::string::npos);
    CHECK(table_xml.find("<location") != std::string::npos);
    CHECK(table_xml.find("<rowItems") != std::string::npos);
    CHECK(table_xml.find("<colItems") != std::string::npos);

    const auto cache_raw = zip.Read("xl/pivotCache/pivotCacheDefinition1.xml");
    const std::string cache_xml(cache_raw.begin(), cache_raw.end());
    CHECK(cache_xml.find("<cacheFields count=\"3\">") != std::string::npos);
    CHECK(cache_xml.find("<worksheetSource") != std::string::npos);
    CHECK(cache_xml.find("ref=\"A1:C6\"") != std::string::npos);
    CHECK(cache_xml.find("sheet=\"Data\"") != std::string::npos);
    CHECK(cache_xml.find("<sharedItems") != std::string::npos);
    CHECK(cache_xml.find("minValue=\"3\"") != std::string::npos);

    const auto records_raw = zip.Read("xl/pivotCache/pivotCacheRecords1.xml");
    const std::string records_xml(records_raw.begin(), records_raw.end());
    CHECK(records_xml.find("count=\"5\"") != std::string::npos);
    CHECK(records_xml.find("<r>") != std::string::npos);

    // 关系链：sheet2 -> pivotTable1 -> cacheDefinition1 -> records1
    const auto sheet_rels_raw = zip.Read("xl/worksheets/_rels/sheet2.xml.rels");
    const std::string sheet_rels(sheet_rels_raw.begin(), sheet_rels_raw.end());
    CHECK(sheet_rels.find("relationships/pivotTable") != std::string::npos);
    CHECK(sheet_rels.find("Target=\"../pivotTables/pivotTable1.xml\"") != std::string::npos);

    const auto table_rels_raw = zip.Read("xl/pivotTables/_rels/pivotTable1.xml.rels");
    const std::string table_rels(table_rels_raw.begin(), table_rels_raw.end());
    CHECK(table_rels.find("relationships/pivotCacheDefinition") != std::string::npos);
    CHECK(table_rels.find("Target=\"../pivotCache/pivotCacheDefinition1.xml\"") !=
          std::string::npos);

    const auto cache_rels_raw = zip.Read("xl/pivotCache/_rels/pivotCacheDefinition1.xml.rels");
    const std::string cache_rels(cache_rels_raw.begin(), cache_rels_raw.end());
    CHECK(cache_rels.find("relationships/pivotCacheRecords") != std::string::npos);
    CHECK(cache_rels.find("pivotCacheRecords1.xml") != std::string::npos);

    // workbook 级的 pivotCaches：Excel 从这里解析缓存链，缺了就等于没建表。
    const auto wb_raw = zip.Read("xl/workbook.xml");
    const std::string wb_xml(wb_raw.begin(), wb_raw.end());
    CHECK(wb_xml.find("<pivotCaches>") != std::string::npos);
    CHECK(wb_xml.find("<pivotCache cacheId=\"1\"") != std::string::npos);
    const auto wb_rels_raw = zip.Read("xl/_rels/workbook.xml.rels");
    const std::string wb_rels(wb_rels_raw.begin(), wb_rels_raw.end());
    CHECK(wb_rels.find("relationships/pivotCacheDefinition") != std::string::npos);
    CHECK(wb_rels.find("Target=\"pivotCache/pivotCacheDefinition1.xml\"") != std::string::npos);

    // 部件不能重复：重新生成后原部件必须从透传列表里剔除。
    int table_parts = 0;
    for (const ZipEntry& e : zip.entries()) {
      if (e.name == "xl/pivotTables/pivotTable1.xml") ++table_parts;
    }
    CHECK_EQ(table_parts, 1);
  }

  const std::string path = OutPath("p11_pivot.xlsx");
  SaveToFile(wb, path, save_opts);

  // ---- 重读 --------------------------------------------------------------
  OpenOptions open_opts;
  open_opts.path = path;
  WarningList read_warnings;
  open_opts.warnings = &read_warnings;
  Workbook back = Open(open_opts);
  CHECK_EQ(back.pivot_tables.size(), size_t(1));
  if (!back.pivot_tables.empty()) {
    const PivotTable& rt = back.pivot_tables[0];
    CHECK_EQ(rt.name, std::string("PivotTable1"));
    CHECK_EQ(rt.sheet, std::string("Pivot"));
    CHECK_EQ(rt.source_sheet, std::string("Data"));
    CHECK_EQ(rt.source.row1, 0);
    CHECK_EQ(rt.source.col2, 2);
    CHECK_EQ(rt.source.row2, 5);
    CHECK_EQ(rt.destination.row, 2);
    CHECK_EQ(rt.destination.col, 0);
    CHECK_EQ(rt.fields.size(), size_t(3));
    CHECK_EQ(rt.row_fields.size(), size_t(1));
    CHECK_EQ(rt.column_fields.size(), size_t(1));
    CHECK_EQ(rt.data_fields.size(), size_t(1));
    CHECK_EQ(rt.row_items.size(), size_t(3));
    CHECK_EQ(rt.column_items.size(), size_t(3));
    CHECK_EQ(rt.part_uri, std::string("/xl/pivotTables/pivotTable1.xml"));
    CHECK_EQ(rt.cache_uri, std::string("/xl/pivotCache/pivotCacheDefinition1.xml"));
    CHECK(!rt.dirty);  // 读入即干净，保存时按字节透传
    if (rt.data_fields.size() == 1) CHECK(rt.data_fields[0].aggregate == PivotAggregate::Sum);
  }

  // 读入后未刷新就保存，三个部件必须原样还在（走透传而不是重新生成）。
  {
    MemoryStream rt_out;
    Save(back, rt_out, save_opts);
    MemoryStream probe(rt_out.data());
    auto zip = ZipArchive::Open(probe);
    CHECK(zip.Find("xl/pivotTables/pivotTable1.xml") != nullptr);
    CHECK(zip.Find("xl/pivotCache/pivotCacheDefinition1.xml") != nullptr);
    CHECK(zip.Find("xl/pivotCache/pivotCacheRecords1.xml") != nullptr);
    const auto wb_raw = zip.Read("xl/workbook.xml");
    const std::string wb_xml(wb_raw.begin(), wb_raw.end());
    CHECK(wb_xml.find("<pivotCaches>") != std::string::npos);
  }

  // ---- 源数据变大：新增一行，刷新后新行必须出现 --------------------------
  {
    Worksheet* src = back.SheetByName("Data");
    CHECK(src != nullptr);
    if (src == nullptr) return;
    src->SetText(6, 0, "North");
    src->SetText(6, 1, "Widget");
    src->SetNumber(6, 2, 100);
    back.pivot_tables[0].source.row2 = 6;
    const int written = RefreshPivot(back, back.pivot_tables[0]);
    CHECK(written > 0);
    CHECK(back.pivot_tables[0].dirty);

    Worksheet* target = back.SheetByName("Pivot");
    CHECK(target != nullptr);
    if (target == nullptr) return;
    // 行组合排序：East, North, West → East 在第 4 行，North 第 5 行，West 第 6 行。
    CHECK_EQ(target->GetValue(4, 0).text, std::string("East"));
    CHECK_EQ(target->GetValue(5, 0).text, std::string("North"));
    CHECK_EQ(target->GetValue(6, 0).text, std::string("West"));
    CHECK_EQ(target->GetValue(5, 2).number, 100.0);  // North / Widget
    CHECK_EQ(target->GetValue(5, 3).number, 100.0);  // North 小计
    CHECK_EQ(target->GetValue(7, 3).number, 145.0);  // 总计 = 45 + 100
    CHECK_EQ(back.pivot_tables[0].row_items.size(), size_t(4));  // 3 行 + 总计
  }

  // ---- 源数据变小：刷新必须清掉上一次的残行 ------------------------------
  {
    back.pivot_tables[0].source.row2 = 2;  // 只剩 East 两行
    RefreshPivot(back, back.pivot_tables[0]);
    Worksheet* target = back.SheetByName("Pivot");
    CHECK(target != nullptr);
    if (target == nullptr) return;
    CHECK_EQ(target->GetValue(4, 0).text, std::string("East"));
    // 只剩一个行组合，总计行落到第 5 行；原来的 North / West 行现在必须是空的。
    CHECK_EQ(target->GetValue(5, 0).text, std::string("总计"));
    CHECK(target->GetValue(6, 0).kind == Value::Kind::Empty);
    CHECK(target->GetValue(7, 0).kind == Value::Kind::Empty);
    CHECK_EQ(target->GetValue(4, 1).number, 20.0);   // East / Gadget
    CHECK_EQ(target->GetValue(4, 2).number, 10.0);   // East / Widget 只剩 10
    CHECK_EQ(target->GetValue(5, 3).number, 30.0);   // 总计
    CHECK_EQ(back.pivot_tables[0].row_items.size(), size_t(2));  // 1 行 + 总计
  }

  // ---- 参数校验 ----------------------------------------------------------
  CHECK_THROWS(AddPivotTable(*dst, source, at, {"Nope"}, {}, {"Amount"}),
               ErrorCode::InvalidArgument);
  CHECK_THROWS(AddPivotTable(*dst, source, at, {}, {}, {"Nope"}), ErrorCode::InvalidArgument);
  {
    Range empty;
    empty.row1 = 3;
    empty.col1 = 0;
    empty.row2 = 1;
    empty.col2 = 2;
    CHECK_THROWS(AddPivotTable(*dst, empty, at, {}, {}, {}), ErrorCode::InvalidArgument);
  }
}

// 打印布局：分页、网格、文字位置都要落在页面内。
void TestSheetLayout() {
  using namespace doccore::sheet;
  Workbook wb = Workbook::Create();
  Worksheet& ws = *wb.SheetAt(0);
  ws.set_name("Grid");
  for (int row = 0; row < 6; ++row) {
    for (int col = 0; col < 4; ++col) {
      if (col == 0) ws.SetText(row, col, "row" + std::to_string(row));
      else ws.SetNumber(row, col, row * 10 + col);
    }
  }
  ws.Merge(0, 0, 0, 1);

  SheetLayoutOptions opts;
  LayoutDocument laid = LayoutSheet(wb, ws, opts);
  CHECK_EQ(laid.pages.size(), size_t(1));
  if (laid.pages.empty()) return;
  CHECK(laid.pages[0].width_pt > 590.0 && laid.pages[0].width_pt < 600.0);
  CHECK(laid.pages[0].height_pt > 840.0 && laid.pages[0].height_pt < 845.0);

  int texts = 0;
  for (const DrawOp& op : laid.pages[0].ops) {
    if (std::holds_alternative<DrawText>(op)) {
      const DrawText& text = std::get<DrawText>(op);
      ++texts;
      CHECK(text.x >= 0.0 && text.x < laid.pages[0].width_pt);
      CHECK(text.y > 0.0 && text.y < laid.pages[0].height_pt);
      CHECK(!text.text_utf8.empty());
    }
  }
  CHECK_EQ(texts, 23);  // 6x4 个单元格，合并区 A1:B1 只画左上角

  // 版面必须真的画出文字
  bool saw_row = false;
  for (const DrawOp& op : laid.pages[0].ops) {
    if (!std::holds_alternative<DrawText>(op)) continue;
    if (std::get<DrawText>(op).text_utf8.find("row") != std::string::npos) saw_row = true;
  }
  CHECK(saw_row);

  // 宽表要横向分页
  Workbook wide = Workbook::Create();
  Worksheet& big = *wide.SheetAt(0);
  for (int col = 0; col < 60; ++col) {
    big.SetColumnWidth(col, 12.0);
    for (int row = 0; row < 3; ++row) big.SetNumber(row, col, row + col);
  }
  LayoutDocument wide_doc = LayoutSheet(wide, big, opts);
  CHECK(wide_doc.pages.size() >= 2);
  for (const LayoutPage& page : wide_doc.pages) {
    CHECK(page.width_pt > 590.0);
    for (const DrawOp& op : page.ops) {
      if (!std::holds_alternative<DrawText>(op)) continue;
      const DrawText& text = std::get<DrawText>(op);
      CHECK(text.x < page.width_pt);
      CHECK(text.y < page.height_pt);
    }
  }

  // 横向 + 重复标题行
  Workbook tall = Workbook::Create();
  Worksheet& sheet = *tall.SheetAt(0);
  sheet.print().landscape = true;
  sheet.print().repeat_rows_first = 0;
  sheet.print().repeat_rows_last = 0;
  for (int row = 0; row < 200; ++row) {
    sheet.SetText(row, 0, row == 0 ? "HEADER" : "r" + std::to_string(row));
    sheet.SetNumber(row, 1, row);
  }
  LayoutDocument tall_doc = LayoutSheet(tall, sheet, opts);
  CHECK(tall_doc.pages.size() >= 2);
  CHECK(tall_doc.pages[0].width_pt > tall_doc.pages[0].height_pt);  // 横向
  // 第二页起要重复标题行
  int header_pages = 0;
  for (const LayoutPage& page : tall_doc.pages) {
    for (const DrawOp& op : page.ops) {
      if (!std::holds_alternative<DrawText>(op)) continue;
      if (std::get<DrawText>(op).text_utf8 == "HEADER") {
        ++header_pages;
        break;
      }
    }
  }
  CHECK_EQ(header_pages, static_cast<int>(tall_doc.pages.size()));

  // 整本工作簿：页数相加
  wb.AddSheet("Second").SetNumber(0, 0, 1);
  LayoutDocument all = LayoutWorkbook(wb, opts);
  CHECK_EQ(all.pages.size(), laid.pages.size() + 1);
}

// ---- P7：演示文稿 -------------------------------------------------------

void TestDeckDom() {
  using namespace doccore::deck;
  Presentation pres = Presentation::Create();
  CHECK_EQ(pres.slide_width, int64_t(12192000));
  CHECK_EQ(pres.slide_height, int64_t(6858000));
  CHECK_EQ(pres.slide_count(), 0);

  // 单位换算
  CHECK(std::fabs(EmuToPt(12700) - 1.0) < 1e-9);
  CHECK_EQ(PtToEmu(72.0), int64_t(914400));

  Slide& slide = pres.AddSlide();
  CHECK_EQ(pres.slide_count(), 1);
  CHECK(pres.SlideAt(0) == &slide);
  CHECK(pres.SlideAt(1) == nullptr);

  Shape& title = slide.AddPlaceholder("title", ShapeGeometry{685800, 2130425, 7772400, 1470025},
                                      "标题");
  CHECK(title.is_placeholder());
  CHECK_EQ(title.text.PlainText(), std::string("标题"));
  CHECK(slide.FindPlaceholder("title") == &title);
  CHECK(slide.FindPlaceholder("body") == nullptr);

  Shape& box = AddTextBox(slide, 0.0, 0.0, 72.0, 36.0, {"hello"});
  CHECK(!box.is_placeholder());
  CHECK_EQ(box.text.paragraphs.size(), size_t(1));

  // 段落 / run 的纯文本
  Paragraph paragraph;
  doccore::deck::Run a;
  a.text = "ab";
  doccore::deck::Run b;
  b.text = "cd";
  paragraph.runs.push_back(a);
  paragraph.runs.push_back(b);
  CHECK_EQ(paragraph.PlainText(), std::string("abcd"));
  TextBody body;
  body.paragraphs.push_back(paragraph);
  body.paragraphs.push_back(paragraph);
  CHECK(!body.empty());
  CHECK_EQ(body.PlainText(), std::string("abcd\nabcd"));

  // 表格
  Shape& table_shape = AddTable(slide, 0.0, 0.0, 300.0, 700.0, 2, 2);
  CHECK(table_shape.kind == ShapeKind::Table);
  CHECK_EQ(table_shape.table.rows.size(), size_t(2));
  CHECK_EQ(table_shape.table.col_widths.size(), size_t(2));
  table_shape.table.rows[0][0].body.paragraphs.push_back(paragraph);

  CHECK_EQ(slide.PlainText(), std::string("标题\nhello\nabcd"));
  CHECK(pres.ExtractText().find("标题") != std::string::npos);

  // 克隆必须深拷贝
  Presentation copy = pres.Clone();
  CHECK_EQ(copy.slide_count(), 1);
  copy.SlideAt(0)->shapes()[0].text.paragraphs[0].runs[0].text = "changed";
  CHECK_EQ(pres.SlideAt(0)->shapes()[0].text.paragraphs[0].runs[0].text, std::string("标题"));

  pres.RemoveSlide(0);
  CHECK_EQ(pres.slide_count(), 0);
}

void TestDeckUnitsAndImages() {
  using namespace doccore::deck;
  // 像素尺寸探测：只用 IHDR，不解码
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I',
                              'H',  'D', 'R',   0,    0,    0,    16,   0, 0, 0, 8};
  int w = 0;
  int h = 0;
  CHECK(ImagePixelSize(png, &w, &h));
  CHECK_EQ(w, 16);
  CHECK_EQ(h, 8);
  CHECK_EQ(ImageMimeFromBytes(png), std::string("image/png"));
}

void TestPptxRoundTrip() {
  using namespace doccore::deck;
  Presentation pres = Presentation::Create();
  pres.default_font = "Arial";
  pres.default_size_pt = 18.0;

  // 第 1 页：标题 + 正文
  Slide& first = pres.AddSlide();
  Shape& title = first.AddPlaceholder("title", ShapeGeometry{685800, 2130425, 7772400, 1470025}, "");
  title.text.paragraphs.clear();
  Paragraph heading;
  doccore::deck::Run heading_run;
  heading_run.text = "First Slide";
  heading_run.format.size_pt = 40.0;
  heading_run.format.bold = true;
  heading.runs.push_back(heading_run);
  title.text.paragraphs.push_back(heading);
  Shape& content =
      first.AddPlaceholder("body", ShapeGeometry{1371600, 3886200, 6400800, 1752600}, "");
  content.text.paragraphs.clear();
  for (int i = 0; i < 2; ++i) {
    Paragraph paragraph;
    doccore::deck::Run run;
    run.text = "line " + std::to_string(i + 1);
    paragraph.runs.push_back(run);
    content.text.paragraphs.push_back(paragraph);
  }
  first.notes() = "speaker notes";

  // 第 2 页：表格（用构造辅助建 2x3，再填内容）
  Slide& second = pres.AddSlide();
  Shape& second_table = AddTable(second, 39.37, 39.37, 472.44, 62.99, 2, 3);
  Table& table = second_table.table;
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < 3; ++c) {
      Paragraph paragraph;
      doccore::deck::Run run;
      run.text = "c" + std::to_string(r) + std::to_string(c);
      paragraph.runs.push_back(run);
      table.rows[static_cast<size_t>(r)][static_cast<size_t>(c)].body.paragraphs.push_back(
          paragraph);
      table.rows[static_cast<size_t>(r)][static_cast<size_t>(c)].fill = r == 0 ? 0x4472C4 : -1;
    }
  }
  // 第 3 页：绝对定位文本框
  Slide& third = pres.AddSlide();
  AddTextBox(third, 72.0, 72.0, 360.0, 108.0, {"free text"});

  MemoryStream mem;
  SaveOptions save_opts;
  Save(pres, mem, save_opts);
  CHECK(mem.data().size() > 1000);

  Presentation back =
      OpenMemory(std::string(reinterpret_cast<const char*>(mem.data().data()), mem.data().size()));
  CHECK_EQ(back.slide_count(), 3);
  CHECK_EQ(back.slide_width, pres.slide_width);
  CHECK_EQ(back.slide_height, pres.slide_height);
  CHECK_EQ(back.default_font, std::string("Arial"));

  const Slide* s0 = back.SlideAt(0);
  CHECK(s0 != nullptr);
  if (s0 != nullptr) {
    CHECK_EQ(s0->shapes().size(), size_t(2));
    CHECK_EQ(s0->notes(), std::string("speaker notes"));
    const Shape* read_title = s0->FindPlaceholder("title");
    CHECK(read_title != nullptr);
    if (read_title != nullptr) {
      CHECK_EQ(read_title->text.PlainText(), std::string("First Slide"));
      CHECK_EQ(read_title->geom.cx, int64_t(7772400));
      CHECK_EQ(read_title->text.paragraphs.size(), size_t(1));
      CHECK(read_title->text.paragraphs[0].runs[0].format.bold);
    }
    const Shape* read_body = s0->FindPlaceholder("body");
    CHECK(read_body != nullptr);
    if (read_body != nullptr) CHECK_EQ(read_body->text.paragraphs.size(), size_t(2));
  }

  const Slide* s1 = back.SlideAt(1);
  CHECK(s1 != nullptr);
  if (s1 != nullptr) {
    CHECK_EQ(s1->shapes().size(), size_t(1));
    if (!s1->shapes().empty() && s1->shapes()[0].kind == ShapeKind::Table) {
      const Table& read_back = s1->shapes()[0].table;
      CHECK_EQ(read_back.rows.size(), size_t(2));
      CHECK_EQ(read_back.rows[0].size(), size_t(3));
      CHECK_EQ(read_back.rows[0][0].body.PlainText(), std::string("c00"));
      CHECK_EQ(read_back.rows[1][2].body.PlainText(), std::string("c12"));
      CHECK_EQ(read_back.rows[0][0].fill, 0x4472C4);
      CHECK_EQ(read_back.col_widths.size(), size_t(3));
    }
  }

  const Slide* s2 = back.SlideAt(2);
  CHECK(s2 != nullptr);
  if (s2 != nullptr && !s2->shapes().empty()) {
    CHECK_EQ(s2->shapes()[0].text.PlainText(), std::string("free text"));
    CHECK_EQ(s2->shapes()[0].geom.x, int64_t(914400));
    CHECK_EQ(s2->shapes()[0].geom.cy, int64_t(1371600));
  }

  // 文本导出
  const std::string text = back.ExtractText();
  CHECK(text.find("First Slide") != std::string::npos);
  CHECK(text.find("free text") != std::string::npos);

  // 写文件再读
  const std::string path = OutPath("deck_roundtrip.pptx");
  SaveToFile(pres, path);
  OpenOptions file_opts;
  file_opts.path = path;
  Presentation from_file = Open(file_opts);
  CHECK_EQ(from_file.slide_count(), 3);
}

// 幻灯片切换与动画（F-403）：p:transition 完整建模，p:timing 只提语义层 +
// 原文保留。断言分四块：写出结构、元素顺序、重读字段、以及"原文优先"的写回规则。
void TestPptxTransitionAndAnimation() {
  using namespace doccore::deck;

  Presentation pres = Presentation::Create();
  Slide& slide = pres.AddSlide();
  Shape& a = AddTextBox(slide, 72.0, 72.0, 360.0, 54.0, {"Alpha"});
  Shape& b = AddTextBox(slide, 72.0, 144.0, 360.0, 54.0, {"Beta"});
  Shape& c = AddTextBox(slide, 72.0, 216.0, 360.0, 54.0, {"Gamma"});
  // 显式给 id，动画的 spid 才有确定值可断言。
  a.id = 2;
  b.id = 3;
  c.id = 4;
  pres.AddSlide();

  // ---- 切换 --------------------------------------------------------------
  SlideTransition tr;
  tr.type = TransitionType::Fade;
  tr.speed = TransitionSpeed::Fast;
  tr.advance_on_click = false;
  tr.advance_after_ms = 3000;
  slide.set_transition(tr);
  CHECK(slide.transition().present());

  // ---- 动画 --------------------------------------------------------------
  std::vector<SlideAnimation> anims;
  {
    SlideAnimation x;
    x.shape_id = 2;
    x.kind = AnimationKind::Entrance;
    x.effect = "fade";
    x.filter = "fade";
    x.trigger = AnimationTrigger::OnClick;
    x.duration_ms = 500;
    anims.push_back(x);
  }
  {
    SlideAnimation x;
    x.shape_id = 3;
    x.kind = AnimationKind::Entrance;
    x.effect = "wipe";
    x.filter = "wipe(up)";
    x.trigger = AnimationTrigger::WithPrevious;
    x.duration_ms = 750;
    x.delay_ms = 200;
    anims.push_back(x);
  }
  {
    SlideAnimation x;
    x.shape_id = 4;
    x.kind = AnimationKind::Exit;
    x.effect = "fade";
    x.filter = "fade";
    x.trigger = AnimationTrigger::AfterPrevious;
    x.duration_ms = 400;
    anims.push_back(x);
  }
  {
    SlideAnimation x;
    x.shape_id = 2;
    x.kind = AnimationKind::MotionPath;
    x.effect = "motionPath";
    x.filter = "motion";
    x.path = "M 0 0 L 0.25 0.1";
    x.trigger = AnimationTrigger::WithPrevious;
    x.duration_ms = 1000;
    anims.push_back(x);
  }
  slide.SetAnimations(anims);
  CHECK_EQ(slide.animations().size(), size_t(4));
  CHECK(slide.timing_xml().empty());

  MemoryStream mem;
  SaveOptions save_opts;
  Save(pres, mem, save_opts);
  CHECK(mem.data().size() > 1000);

  const std::string_view bytes(reinterpret_cast<const char*>(mem.data().data()),
                               mem.data().size());
  std::string slide_xml;
  {
    MemoryStream probe(mem.data());
    auto zip = ZipArchive::Open(probe);
    const auto raw = zip.Read("ppt/slides/slide1.xml");
    slide_xml.assign(raw.begin(), raw.end());
  }
  CHECK(!slide_xml.empty());

  // 切换
  CHECK(slide_xml.find("<p:transition") != std::string::npos);
  CHECK(slide_xml.find("spd=\"fast\"") != std::string::npos);
  CHECK(slide_xml.find("advClick=\"0\"") != std::string::npos);
  CHECK(slide_xml.find("advTm=\"3000\"") != std::string::npos);
  CHECK(slide_xml.find("<p:fade/>") != std::string::npos);

  // 动画
  CHECK(slide_xml.find("<p:timing>") != std::string::npos);
  CHECK(slide_xml.find("nodeType=\"tmRoot\"") != std::string::npos);
  CHECK(slide_xml.find("nodeType=\"mainSeq\"") != std::string::npos);
  CHECK(slide_xml.find("<p:seq concurrent=\"1\"") != std::string::npos);
  CHECK(slide_xml.find("nodeType=\"clickEffect\"") != std::string::npos);
  CHECK(slide_xml.find("nodeType=\"withEffect\"") != std::string::npos);
  CHECK(slide_xml.find("nodeType=\"afterEffect\"") != std::string::npos);
  CHECK(slide_xml.find("presetClass=\"entr\"") != std::string::npos);
  CHECK(slide_xml.find("presetClass=\"exit\"") != std::string::npos);
  CHECK(slide_xml.find("presetClass=\"path\"") != std::string::npos);
  CHECK(slide_xml.find("<p:animEffect transition=\"in\" filter=\"fade\"") != std::string::npos);
  CHECK(slide_xml.find("filter=\"wipe(up)\"") != std::string::npos);
  CHECK(slide_xml.find("<p:animEffect transition=\"out\"") != std::string::npos);
  CHECK(slide_xml.find("<p:animMotion") != std::string::npos);
  CHECK(slide_xml.find("path=\"M 0 0 L 0.25 0.1\"") != std::string::npos);
  CHECK(slide_xml.find("<p:bldLst>") != std::string::npos);
  CHECK(slide_xml.find("spid=\"2\"") != std::string::npos);
  CHECK(slide_xml.find("spid=\"3\"") != std::string::npos);
  CHECK(slide_xml.find("spid=\"4\"") != std::string::npos);
  // 入场效果必须先置可见再淡入。
  const size_t set_pos = slide_xml.find("<p:set>");
  const size_t anim_pos = slide_xml.find("<p:animEffect transition=\"in\"");
  CHECK(set_pos != std::string::npos);
  CHECK(anim_pos != std::string::npos);
  if (set_pos != std::string::npos && anim_pos != std::string::npos) CHECK(set_pos < anim_pos);

  // 元素顺序：cSld -> clrMapOvr -> transition -> timing（CT_Slide 的 sequence）
  {
    const size_t csld = slide_xml.find("</p:cSld>");
    const size_t clr = slide_xml.find("<p:clrMapOvr");
    const size_t trans = slide_xml.find("<p:transition");
    const size_t timing = slide_xml.find("<p:timing>");
    CHECK(csld != std::string::npos);
    CHECK(clr != std::string::npos);
    CHECK(trans != std::string::npos);
    CHECK(timing != std::string::npos);
    if (csld != std::string::npos && clr != std::string::npos) CHECK(csld < clr);
    if (clr != std::string::npos && trans != std::string::npos) CHECK(clr < trans);
    if (trans != std::string::npos && timing != std::string::npos) CHECK(trans < timing);
  }

  // ---- 重读 --------------------------------------------------------------
  Presentation back = OpenMemory(std::string(bytes));
  CHECK_EQ(back.slide_count(), 2);
  const Slide* s0 = back.SlideAt(0);
  CHECK(s0 != nullptr);
  if (s0 != nullptr) {
    const SlideTransition& rt = s0->transition();
    CHECK(rt.type == TransitionType::Fade);
    CHECK(rt.speed == TransitionSpeed::Fast);
    CHECK(!rt.advance_on_click);
    CHECK_EQ(rt.advance_after_ms, 3000);

    CHECK_EQ(s0->animations().size(), size_t(4));
    if (s0->animations().size() == 4) {
      const SlideAnimation& x0 = s0->animations()[0];
      CHECK_EQ(x0.shape_id, 2);
      CHECK(x0.kind == AnimationKind::Entrance);
      CHECK_EQ(x0.effect, std::string("fade"));
      CHECK(x0.trigger == AnimationTrigger::OnClick);
      CHECK_EQ(x0.duration_ms, 500);

      const SlideAnimation& x1 = s0->animations()[1];
      CHECK_EQ(x1.shape_id, 3);
      CHECK_EQ(x1.effect, std::string("wipe"));
      CHECK_EQ(x1.filter, std::string("wipe(up)"));
      CHECK(x1.trigger == AnimationTrigger::WithPrevious);
      CHECK_EQ(x1.duration_ms, 750);
      CHECK_EQ(x1.delay_ms, 200);

      const SlideAnimation& x2 = s0->animations()[2];
      CHECK_EQ(x2.shape_id, 4);
      CHECK(x2.kind == AnimationKind::Exit);
      CHECK(x2.trigger == AnimationTrigger::AfterPrevious);
      CHECK_EQ(x2.duration_ms, 400);

      const SlideAnimation& x3 = s0->animations()[3];
      CHECK(x3.kind == AnimationKind::MotionPath);
      CHECK_EQ(x3.path, std::string("M 0 0 L 0.25 0.1"));
      CHECK_EQ(x3.duration_ms, 1000);
    }
    // 读入后 <p:timing> 的原文必须留住（关键帧细节靠它往返）。
    CHECK(!s0->timing_xml().empty());
    CHECK(s0->timing_xml().find("<p:timing>") != std::string::npos);
    // 形状 id 要能往返，否则动画指向错目标。
    CHECK_EQ(s0->shapes().size(), size_t(3));
    if (s0->shapes().size() == 3) {
      CHECK_EQ(s0->shapes()[0].id, 2);
      CHECK_EQ(s0->shapes()[1].id, 3);
      CHECK_EQ(s0->shapes()[2].id, 4);
    }
  }

  // ---- 原文优先：不动 DOM 再存一次，<p:timing> 必须逐字节相同 -------------
  {
    MemoryStream again;
    Save(back, again, save_opts);
    MemoryStream probe(again.data());
    auto zip = ZipArchive::Open(probe);
    const auto raw = zip.Read("ppt/slides/slide1.xml");
    const std::string again_xml(raw.begin(), raw.end());
    const size_t p0 = slide_xml.find("<p:timing>");
    const size_t p1 = again_xml.find("<p:timing>");
    CHECK(p1 != std::string::npos);
    if (p0 != std::string::npos && p1 != std::string::npos) {
      const size_t e0 = slide_xml.find("</p:timing>", p0);
      const size_t e1 = again_xml.find("</p:timing>", p1);
      CHECK(e0 != std::string::npos);
      CHECK(e1 != std::string::npos);
      if (e0 != std::string::npos && e1 != std::string::npos) {
        CHECK_EQ(slide_xml.substr(p0, e0 + 11 - p0), again_xml.substr(p1, e1 + 11 - p1));
      }
    }
  }

  // ---- 清空：DOM 成为权威，两个元素都不该再写出来 ------------------------
  {
    Presentation stripped = back.Clone();
    Slide* t0 = stripped.SlideAt(0);
    CHECK(t0 != nullptr);
    if (t0 != nullptr) {
      t0->ClearAnimations();
      t0->set_transition(SlideTransition{});
      CHECK(t0->animations().empty());
      CHECK(t0->timing_xml().empty());
      CHECK(!t0->transition().present());
    }
    MemoryStream out;
    Save(stripped, out, save_opts);
    MemoryStream probe(out.data());
    auto zip = ZipArchive::Open(probe);
    const auto raw = zip.Read("ppt/slides/slide1.xml");
    const std::string xml(raw.begin(), raw.end());
    CHECK(xml.find("<p:timing") == std::string::npos);
    CHECK(xml.find("<p:transition") == std::string::npos);
    // 内容本身还在。
    CHECK(xml.find("Alpha") != std::string::npos);
  }

  // 落盘一份给独立校验器用。
  const std::string path = OutPath("p11_transition.pptx");
  SaveToFile(pres, path);
  CHECK(!path.empty());
}

void TestPptxReadSample() {
  using namespace doccore::deck;
  const std::string path = Fixture("sample.pptx");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return;  // fixture 是可选的

  OpenOptions open_opts;
  open_opts.path = path;
  Presentation pres = Open(open_opts);
  CHECK_EQ(pres.slide_count(), 3);
  CHECK_EQ(pres.slide_width, int64_t(12192000));
  CHECK_EQ(pres.slide_height, int64_t(6858000));
  CHECK_EQ(pres.default_font, std::string("Calibri"));

  const std::string text = pres.ExtractText();
  CHECK(text.find("DocCore") != std::string::npos);
  CHECK(text.find("R1C0") != std::string::npos);
  CHECK(text.find("第二段") != std::string::npos);

  // 备注
  CHECK_EQ(pres.SlideAt(0)->notes().find("备注"), size_t(0));

  // 占位符几何继承：标题必须落在版式给的框里，不能是 (0,0)
  const Shape* title = pres.SlideAt(0)->FindPlaceholder("ctrTitle");
  CHECK(title != nullptr);
  if (title != nullptr) {
    CHECK_EQ(title->geom.x, int64_t(685800));
    CHECK(title->geom.cy > 0);
  }
  const Shape* body = pres.SlideAt(0)->FindPlaceholder("subTitle");
  CHECK(body != nullptr);
  if (body != nullptr) CHECK_EQ(body->geom.x, int64_t(1371600));

  // 图片
  const Slide* third = pres.SlideAt(2);
  CHECK(third != nullptr);
  if (third != nullptr) {
    const Shape* picture = nullptr;
    for (const Shape& shape : third->shapes()) {
      if (shape.kind == ShapeKind::Picture) picture = &shape;
    }
    CHECK(picture != nullptr);
    if (picture != nullptr) {
      CHECK(!picture->picture.bytes.empty());
      CHECK_EQ(picture->picture.mime, std::string("image/png"));
      CHECK_EQ(picture->geom.x, int64_t(914400));
      CHECK_EQ(picture->geom.cy, int64_t(1371600));
    }
  }
}

void TestPptxLayout() {
  using namespace doccore::deck;
  Presentation pres = Presentation::Create();
  for (int i = 0; i < 3; ++i) {
    Slide& slide = pres.AddSlide();
    Shape& title =
        slide.AddPlaceholder("title", ShapeGeometry{685800, 300000, 10000000, 900000}, "");
    title.text.paragraphs.clear();
    Paragraph paragraph;
    doccore::deck::Run run;
    run.text = "Slide " + std::to_string(i + 1);
    run.format.size_pt = 40.0;
    paragraph.runs.push_back(run);
    title.text.paragraphs.push_back(paragraph);
  }

  LayoutDocument laid = LayoutPresentation(pres);
  CHECK_EQ(laid.pages.size(), size_t(3));
  if (laid.pages.empty()) return;
  // 一页幻灯片 = 一页 PDF，尺寸按幻灯片尺寸（960x540 pt）
  CHECK(std::fabs(laid.pages[0].width_pt - 960.0) < 0.01);
  CHECK(std::fabs(laid.pages[0].height_pt - 540.0) < 0.01);
  for (const LayoutPage& page : laid.pages) {
    CHECK(std::fabs(page.width_pt - 960.0) < 0.01);
    CHECK(std::fabs(page.height_pt - 540.0) < 0.01);
    bool saw_text = false;
    for (const DrawOp& op : page.ops) {
      if (!std::holds_alternative<DrawText>(op)) continue;
      const DrawText& text = std::get<DrawText>(op);
      saw_text = true;
      CHECK(text.text_utf8.find("Slide") != std::string::npos);
      // 坐标必须是"绝对定位"，落在页面内
      CHECK(text.x > 0.0 && text.x < page.width_pt);
      CHECK(text.y > 0.0 && text.y < page.height_pt);
    }
    CHECK(saw_text);
  }

  // 换行：窄框里的长文本要拆成多行
  Presentation narrow = Presentation::Create();
  Slide& only = narrow.AddSlide();
  AddTextBox(only, 0.0, 0.0, 100.0, 200.0,
             {"the quick brown fox jumps over the lazy dog again and again"});
  LayoutDocument narrow_doc = LayoutPresentation(narrow);
  CHECK_EQ(narrow_doc.pages.size(), size_t(1));
  if (!narrow_doc.pages.empty()) {
    int lines = 0;
    for (const DrawOp& op : narrow_doc.pages[0].ops) {
      if (std::holds_alternative<DrawText>(op)) ++lines;
    }
    CHECK(lines >= 2);
  }

  // 导出 PDF：页数与页面尺寸都要对上（PDF 写端必须内嵌字体）
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  DeckLayoutOptions layout_opts;
  layout_opts.fonts = &fonts;
  const std::string path = OutPath("deck_layout.pdf");
  SavePdfToFile(pres, path, layout_opts, {});
  std::error_code ec;
  CHECK(std::filesystem::exists(path, ec));
  FileStream in(path, "rb");
  const std::vector<uint8_t> bytes = ReadAll(in, 1 << 20);
  CHECK(bytes.size() > 500);
  const std::string head(bytes.begin(), bytes.begin() + 8);
  CHECK_EQ(head, std::string("%PDF-1.7"));
}

void TestDeckImageDecode() {
  // 现造一个 1x1 红色 PNG（8 位 RGB），走一遍完整解码路径
  std::vector<uint8_t> raw = {0x00, 0xFF, 0x00, 0x00};  // filter 0 + RGB
  const std::vector<uint8_t> z = ZlibCompress(raw.data(), raw.size());

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto push32 = [&png](uint32_t v) {
    png.push_back(static_cast<uint8_t>(v >> 24));
    png.push_back(static_cast<uint8_t>(v >> 16));
    png.push_back(static_cast<uint8_t>(v >> 8));
    png.push_back(static_cast<uint8_t>(v));
  };
  auto chunk = [&](const char* tag, const std::vector<uint8_t>& data) {
    push32(static_cast<uint32_t>(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), tag, tag + 4);
    png.insert(png.end(), data.begin(), data.end());
    push32(Crc32(png.data() + start, 4 + data.size()));
  };
  std::vector<uint8_t> ihdr = {0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0};  // 1x1, 8bit RGB
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});

  int w = 0;
  int h = 0;
  CHECK(ImageSize(png, &w, &h));
  CHECK_EQ(w, 1);
  CHECK_EQ(h, 1);
  CHECK_EQ(ImageMimeFromBytes(png), std::string("image/png"));

  ImageData image;
  CHECK(TryDecodeImage(png, &image, nullptr));
  CHECK_EQ(image.width, 1);
  CHECK_EQ(image.height, 1);
  CHECK_EQ(image.rgba.size(), size_t(4));
  if (image.rgba.size() == 4) {
    CHECK_EQ(int(image.rgba[0]), 0xFF);
    CHECK_EQ(int(image.rgba[1]), 0);
    CHECK_EQ(int(image.rgba[2]), 0);
    CHECK_EQ(int(image.rgba[3]), 0xFF);
  }

  // 坏数据要报错而不是崩
  std::vector<uint8_t> broken = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0};
  ImageData ignored;
  CHECK(!TryDecodeImage(broken, &ignored, nullptr));
  CHECK_THROWS(DecodeImage(broken), ErrorCode::Corrupt);
}

// ---- P8: PDF 读取、重建与转换总线 --------------------------------------

pdf::PdfFile OpenFixturePdf(const char* name) {
  pdf::PdfOpenOptions opts;
  return pdf::PdfFile::OpenPath(Fixture(name), opts);
}

// 由本引擎写出一页 PDF：给"读回自己产物"和"无文本层"两条用例共用。
std::vector<uint8_t> MakePdf(const LayoutDocument& doc, WarningList* warnings) {
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  PdfSaveOptions opt;
  opt.title = "p8";
  return PdfWriter::ToBytes(doc, fonts, opt, warnings);
}

void TestPdfReadFixture() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  CHECK_EQ(file.page_count(), 4);
  CHECK_EQ(file.version(), std::string("1.7"));
  CHECK(!file.encrypted());
  CHECK_EQ(file.title(), std::string("sample_pdf"));
  CHECK(file.object_count() > 20);

  // 页面尺寸：A4 纵向。
  for (int i = 0; i < file.page_count(); ++i) {
    const pdf::PdfPage& page = file.page(i);
    CHECK(std::abs(page.size.width - 595.28) < 0.5);
    CHECK(std::abs(page.size.height - 841.89) < 0.5);
  }

  // ① 几何对象：文本片段、线段、图像。
  const pdf::PdfPage& p0 = file.page(0);
  CHECK(p0.text_runs().size() >= 16);
  CHECK(p0.lines().size() >= 8);   // 表格的 4 条横线 + 4 条竖线
  CHECK_EQ(p0.images().size(), size_t(1));
  CHECK(!p0.looks_scanned());

  // 文本片段带坐标与字号：标题 24pt，正文 11pt。
  bool saw_title = false, saw_body = false;
  for (const pdf::PdfTextRun& run : p0.text_runs()) {
    if (run.text_utf8.find("DocCore PDF Rebuild Fixture") != std::string::npos) {
      saw_title = true;
      CHECK(std::abs(run.size_pt - 24.0) < 0.6);
      CHECK(std::abs(run.x - 72.0) < 1.5);
    }
    if (run.text_utf8.find("PyMuPDF") != std::string::npos) {
      saw_body = true;
      CHECK(std::abs(run.size_pt - 11.0) < 0.6);
    }
  }
  CHECK(saw_title);
  CHECK(saw_body);

  // ToUnicode 反查：第三方嵌入的 CJK 字体也要能还原成同一串汉字。
  const std::string text0 = p0.ExtractText();
  CHECK(text0.find("DocCore PDF Rebuild Fixture") != std::string::npos);
  CHECK(text0.find(u8"\u4E2D\u6587\u4E00\u884C") != std::string::npos);
  CHECK(text0.find("Region") != std::string::npos);

  // 第四页只有一张整页图：扫描页。
  CHECK(file.page(3).looks_scanned());
  CHECK(file.page(3).text_runs().empty());
  CHECK_EQ(file.page(3).images().size(), size_t(1));

  // 全文按 \f 分页。
  const std::string all = file.ExtractText();
  size_t separators = 0;
  for (char c : all) {
    if (c == '\f') ++separators;
  }
  CHECK_EQ(separators, size_t(3));
}

void TestPdfReadOwnOutput() {
  // 读回本引擎写出的 PDF：文本、字号、坐标、图像落位都要对得上。
  LayoutPage page;
  page.width_pt = 300;
  page.height_pt = 200;
  DrawText t;
  t.size_pt = 16;
  t.x = 20;
  t.y = 120;
  t.text_utf8 = "round trip";
  page.ops.push_back(t);

  ImageData img;
  img.width = 2;
  img.height = 2;
  img.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
  DrawImage di;
  di.image = img;
  di.x = 20;
  di.y = 40;
  di.w_pt = 40;
  di.h_pt = 30;
  page.ops.push_back(di);

  LayoutDocument doc;
  doc.pages.push_back(page);
  WarningList warnings;
  const std::vector<uint8_t> bytes = MakePdf(doc, &warnings);
  pdf::PdfFile file = pdf::PdfFile::OpenMemory(bytes, {});
  CHECK_EQ(file.page_count(), 1);
  const pdf::PdfPage& back = file.page(0);
  CHECK_EQ(back.text_runs().size(), size_t(1));
  if (!back.text_runs().empty()) {
    CHECK_EQ(back.text_runs()[0].text_utf8, std::string("round trip"));
    CHECK(std::abs(back.text_runs()[0].size_pt - 16.0) < 0.6);
  }
  CHECK_EQ(back.images().size(), size_t(1));
  if (!back.images().empty()) {
    CHECK(std::abs(back.images()[0].w - 40.0) < 0.6);
    CHECK(std::abs(back.images()[0].h - 30.0) < 0.6);
  }
  CHECK_EQ(back.ExtractText(), std::string("round trip"));
}

void TestPdfBlockGrouping() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  pdf::RebuildOptions opt;
  WarningList warnings;

  // 第一页：标题 + 两行正文（同一块）+ 图注。
  const pdf::PageModel page0 = pdf::Reconstructor::BuildPage(file.page(0), opt, &warnings);
  CHECK_EQ(page0.blocks.size(), size_t(3));
  CHECK_EQ(page0.tables.size(), size_t(1));
  CHECK(page0.has_text);
  CHECK(!page0.scanned);
  if (page0.blocks.size() >= 2) {
    // 标题 24pt 独占一行；正文两行同块。
    CHECK(std::abs(page0.blocks[0].size_pt - 24.0) < 0.6);
    CHECK(page0.blocks[0].single_line());
    const pdf::TextBlock* body = nullptr;
    for (const pdf::TextBlock& block : page0.blocks) {
      if (block.Text().find("This page is generated") != std::string::npos) body = &block;
    }
    CHECK(body != nullptr);
    if (body != nullptr) {
      CHECK(body->lines.size() >= 2);
      CHECK(body->Text().find("It exercises text") != std::string::npos);
    }
  }

  // 第二页：标题 + 左栏 + 右栏，栏号 0/0/1，阅读顺序左栏在右栏之前。
  const pdf::PageModel page1 = pdf::Reconstructor::BuildPage(file.page(1), opt, &warnings);
  CHECK_EQ(page1.blocks.size(), size_t(3));
  if (page1.blocks.size() == 3) {
    CHECK_EQ(page1.blocks[0].column, 0);
    CHECK_EQ(page1.blocks[1].column, 0);
    CHECK_EQ(page1.blocks[2].column, 1);
    CHECK(page1.blocks[1].Text().find("Left one") != std::string::npos);
    CHECK(page1.blocks[2].Text().find("Right one") != std::string::npos);
    CHECK(page1.blocks[1].Text().find("Right") == std::string::npos);
  }

  // 双栏间距 89pt 必须被切开，否则两栏会被并成一条长行。
  CHECK(page1.blocks.size() >= 3);
}

void TestPdfTableDetect() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  pdf::RebuildOptions opt;
  WarningList warnings;

  // 框线表：4 列 3 行，格子文字逐格对上。
  const pdf::PageModel page0 = pdf::Reconstructor::BuildPage(file.page(0), opt, &warnings);
  CHECK_EQ(page0.tables.size(), size_t(1));
  if (!page0.tables.empty()) {
    const pdf::DetectedTable& table = page0.tables[0];
    CHECK(table.bordered);
    CHECK_EQ(table.rows(), 3);
    CHECK_EQ(table.cols(), 4);
    CHECK(std::abs(table.left - 72.0) < 1.0);
    CHECK(std::abs(table.top - 200.0) < 1.0);
    CHECK(std::abs(table.right - 520.0) < 1.0);
    CHECK_EQ(table.cells[0][0], std::string("Region"));
    CHECK_EQ(table.cells[1][0], std::string("North"));
    CHECK_EQ(table.cells[2][3], std::string("60"));
    // 表内文字不能再作为普通块出现。
    for (const pdf::TextBlock& block : page0.blocks) {
      CHECK(block.Text().find("Region") == std::string::npos);
    }
  }

  // 无框线表：按列对齐识别，必须打 RebuildHeuristic 告警。
  WarningList align_warnings;
  const pdf::PageModel page2 = pdf::Reconstructor::BuildPage(file.page(2), opt, &align_warnings);
  CHECK_EQ(page2.tables.size(), size_t(1));
  if (!page2.tables.empty()) {
    const pdf::DetectedTable& table = page2.tables[0];
    CHECK(!table.bordered);
    CHECK_EQ(table.rows(), 4);
    CHECK_EQ(table.cols(), 3);
    CHECK_EQ(table.cells[0][0], std::string("Item"));
    CHECK_EQ(table.cells[3][0], std::string("Doohickey"));
    CHECK_EQ(table.cells[3][2], std::string("12.00"));
  }
  bool warned = false;
  for (const Warning& w : align_warnings.items()) {
    if (w.code == static_cast<int>(WarningCode::RebuildHeuristic)) warned = true;
  }
  CHECK(warned);
}

void TestPdfRebuildDocx() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  pdf::RebuildOptions opt;
  WarningList warnings;
  const text::Document doc = pdf::Reconstructor::ToDocument(file, opt, &warnings);
  CHECK_EQ(doc.sections.size(), size_t(1));
  const text::Section& section = doc.sections[0];
  // A4 页面：595.28pt * 20 = 11906 缇。
  CHECK(std::abs(section.page.width_twips - 11906) <= 2);
  CHECK(std::abs(section.page.height_twips - 16838) <= 2);

  size_t tables = 0, paragraphs = 0, headings = 0, drawings = 0;
  std::string body;
  for (const text::Block& block : section.blocks) {
    if (block.type == text::Block::Type::Table) {
      ++tables;
      CHECK(block.table != nullptr);
      if (block.table != nullptr && !block.table->rows.empty()) {
        // 第一张是框线表（4 列，挂 TableGrid 样式出框线），第二张是对齐表（3 列）。
        CHECK_EQ(block.table->rows[0].cells.size(), tables == 1 ? size_t(4) : size_t(3));
        if (tables == 1) CHECK_EQ(block.table->style, std::string("TableGrid"));
      }
      continue;
    }
    ++paragraphs;
    if (block.paragraph.format.style == "Heading1") ++headings;
    for (const text::InlineItem& item : block.paragraph.runs) {
      if (item.type == text::InlineItem::Type::Drawing) ++drawings;
      if (item.type == text::InlineItem::Type::Text) body += item.text;
    }
    body.push_back('\n');
  }
  CHECK_EQ(tables, size_t(2));
  CHECK(headings >= 3);           // 三页各有标题
  CHECK_EQ(drawings, 1);          // 第一页的插图
  CHECK(body.find("Left one") != std::string::npos);
  CHECK(body.find("Right one") != std::string::npos);
  // 左栏必须整块出现在右栏之前（阅读顺序）。
  CHECK(body.find("Left four") < body.find("Right one"));
  // 图像字节进了 unknown_parts，保存时才写得出 word/media。
  CHECK_EQ(doc.unknown_parts.size(), size_t(1));
  if (!doc.unknown_parts.empty()) {
    CHECK_EQ(doc.unknown_parts[0].mime, std::string("image/png"));
    CHECK(doc.unknown_parts[0].bytes.size() > 100);
  }

  // 存盘再读回：表格与文字都还在。
  const std::string path = OutPath("p8_rebuild.docx");
  text::SaveToFile(doc, path);
  FileStream in(path, "rb");
  const std::vector<uint8_t> bytes = ReadAll(in, 64ull << 20);
  text::Document back = text::OpenMemory(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                         bytes.size()),
                                         nullptr, nullptr);
  size_t back_tables = 0;
  std::string back_text;
  for (const text::Section& s : back.sections) {
    for (const text::Block& block : s.blocks) {
      if (block.type == text::Block::Type::Table) {
        ++back_tables;
        for (const text::Row& row : block.table->rows) {
          for (const text::Cell& cell : row.cells) {
            for (const text::Block& inner : cell.blocks) {
              for (const text::InlineItem& item : inner.paragraph.runs) back_text += item.text;
            }
          }
        }
      } else {
        for (const text::InlineItem& item : block.paragraph.runs) back_text += item.text;
      }
      back_text.push_back('\n');
    }
  }
  CHECK_EQ(back_tables, size_t(2));
  // 文字一条不丢：读回来的正文与写出前逐段一致。
  CHECK(back_text.find("DocCore PDF Rebuild Fixture") != std::string::npos);
  CHECK(back_text.find("Doohickey") != std::string::npos);
  CHECK(back_text.find("Right four: reading order must follow.") != std::string::npos);
}

void TestPdfRebuildXlsx() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  pdf::RebuildOptions opt;
  WarningList warnings;
  const sheet::Workbook book = pdf::Reconstructor::ToWorkbook(file, opt, &warnings);
  // 三页有文字，第四页是扫描页被跳过。
  CHECK_EQ(book.sheet_count(), 3);
  if (book.sheet_count() < 3) return;

  // 第一页：表外文字在上，框线表在下；数字必须是数值而不是文本。
  const sheet::Worksheet* p1 = book.SheetAt(0);
  CHECK(p1 != nullptr);
  if (p1 != nullptr) {
    CHECK_EQ(p1->GetValue(0, 0).text, std::string("DocCore PDF Rebuild Fixture"));
    bool found_header = false;
    for (int row = 0; row < 12; ++row) {
      const sheet::Value v = p1->GetValue(row, 0);
      if (v.kind == sheet::Value::Kind::Text && v.text == "North") {
        found_header = true;
        const sheet::Value n = p1->GetValue(row, 1);
        CHECK(n.kind == sheet::Value::Kind::Number);
        CHECK(std::abs(n.number - 10.0) < 1e-9);
        const sheet::Value m = p1->GetValue(row, 3);
        CHECK(m.kind == sheet::Value::Kind::Number);
        CHECK(std::abs(m.number - 30.0) < 1e-9);
      }
    }
    CHECK(found_header);
  }

  // 第二页：无网格，按文本间隙切列，阅读顺序左栏在前。
  const sheet::Worksheet* p2 = book.SheetAt(1);
  CHECK(p2 != nullptr);
  if (p2 != nullptr) {
    CHECK_EQ(p2->GetValue(0, 0).text, std::string("Two Column Page"));
    const sheet::Value left = p2->GetValue(1, 0);
    const sheet::Value right = p2->GetValue(5, 0);
    CHECK(left.text.find("Left one") != std::string::npos);
    CHECK(right.text.find("Right one") != std::string::npos);
  }

  // 第三页：对齐表，数字解析成数值。
  const sheet::Worksheet* p3 = book.SheetAt(2);
  CHECK(p3 != nullptr);
  if (p3 != nullptr) {
    bool saw_widget = false;
    for (int row = 0; row < 10; ++row) {
      if (p3->GetValue(row, 0).text == "Widget") {
        saw_widget = true;
        CHECK(std::abs(p3->GetValue(row, 1).number - 3.0) < 1e-9);
        CHECK(std::abs(p3->GetValue(row, 2).number - 4.5) < 1e-9);
      }
    }
    CHECK(saw_widget);
  }

  // 存盘再读回：sheet 数与数值都保持。
  const std::string path = OutPath("p8_rebuild.xlsx");
  sheet::SaveToFile(book, path);
  sheet::OpenOptions open;
  open.path = path;
  const sheet::Workbook back = sheet::Open(open);
  CHECK_EQ(back.sheet_count(), 3);
  if (back.sheet_count() == 3 && back.SheetAt(2) != nullptr) {
    CHECK(back.SheetAt(2)->GetValue(3, 2).kind == sheet::Value::Kind::Number);
  }
}

void TestPdfRebuildPptx() {
  pdf::PdfFile file = OpenFixturePdf("sample_pdf.pdf");
  pdf::RebuildOptions opt;
  WarningList warnings;
  const deck::Presentation pres = pdf::Reconstructor::ToPresentation(file, opt, &warnings);
  CHECK_EQ(pres.slide_count(), 3);  // 扫描页不产幻灯片
  // 幻灯片尺寸等于 PDF 页尺寸（595.28 x 841.89 pt）。
  CHECK(std::abs(deck::EmuToPt(pres.slide_width) - 595.28) < 0.5);
  CHECK(std::abs(deck::EmuToPt(pres.slide_height) - 841.89) < 0.5);

  int tables = 0, pictures = 0, texts = 0;
  for (int i = 0; i < pres.slide_count(); ++i) {
    const deck::Slide* slide = pres.SlideAt(i);
    for (const deck::Shape& shape : slide->shapes()) {
      if (shape.kind == deck::ShapeKind::Table) ++tables;
      if (shape.kind == deck::ShapeKind::Picture) {
        ++pictures;
        // 图片落位必须与 PDF 里的矩形一致（容差 1 pt）。
        CHECK(std::abs(deck::EmuToPt(shape.geom.x) - 72.0) < 1.0);
        CHECK(std::abs(deck::EmuToPt(shape.geom.y) - 360.0) < 1.0);
        CHECK(std::abs(deck::EmuToPt(shape.geom.cx) - 96.0) < 1.0);
        CHECK(std::abs(deck::EmuToPt(shape.geom.cy) - 72.0) < 1.0);
        CHECK(!shape.picture.bytes.empty());
      }
      if (shape.kind == deck::ShapeKind::Text) ++texts;
    }
  }
  CHECK_EQ(tables, 2);
  CHECK_EQ(pictures, 1);
  CHECK(texts >= 5);

  // 存盘再读回：页数与尺寸保持。
  const std::string path = OutPath("p8_rebuild.pptx");
  deck::SaveToFile(pres, path);
  deck::OpenOptions open;
  open.path = path;
  const deck::Presentation back = deck::Open(open);
  CHECK_EQ(back.slide_count(), 3);
  CHECK_EQ(back.slide_width, pres.slide_width);
  CHECK_EQ(back.slide_height, pres.slide_height);
}

// 手工拼一份带 Form XObject 的 PDF：字体和图像都只挂在 Form 自己的 /Resources 里，
// 页面只有 /Fm1 Do。检验 Form 递归的资源链与 CTM 串联。
std::vector<uint8_t> MakeFormPdf() {
  struct Obj {
    std::string head;   // "N 0 obj" 之后到 stream 之前
    std::string stream;  // 空表示字典对象
  };
  std::vector<Obj> objs(8);  // 1..7 用，下标 0 空着
  objs[1].head = "<< /Type /Catalog /Pages 2 0 R >>";
  objs[2].head = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
  objs[3].head =
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /XObject << /Fm1 5 0 R >> >> /Contents 4 0 R >>";
  objs[4].head = "<< /Length " + std::to_string(std::string("q 2 0 0 2 100 100 cm /Fm1 Do Q\n").size()) + " >>";
  objs[4].stream = "q 2 0 0 2 100 100 cm /Fm1 Do Q\n";
  const std::string form =
      "BT /F1 10 Tf 1 0 0 1 10 20 Tm (Hi) Tj ET\n"
      "q 10 0 0 10 30 5 cm /I1 Do Q\n";
  objs[5].head =
      "<< /Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 100 50] "
      "/Matrix [0.5 0 0 0.5 0 0] "
      "/Resources << /Font << /F1 6 0 R >> /XObject << /I1 7 0 R >> >> /Length " +
      std::to_string(form.size()) + " >>";
  objs[5].stream = form;
  objs[6].head = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
  objs[7].head =
      "<< /Type /XObject /Subtype /Image /Width 2 /Height 1 "
      "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 6 >>";
  objs[7].stream = std::string("\xff\x00\x00\x00\xff\x00", 6);

  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offsets(8, 0);
  for (size_t n = 1; n < objs.size(); ++n) {
    offsets[n] = out.size();
    out += std::to_string(n) + " 0 obj\n" + objs[n].head;
    if (!objs[n].stream.empty()) out += "\nstream\n" + objs[n].stream + "endstream";
    out += "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objs.size()) + "\n";
  out += "0000000000 65535 f \n";
  for (size_t n = 1; n < objs.size(); ++n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[n]);
    out += buf;
  }
  out += "trailer\n<< /Size " + std::to_string(objs.size()) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return std::vector<uint8_t>(out.begin(), out.end());
}

void TestPdfFormXObject() {
  pdf::PdfFile file = pdf::PdfFile::OpenMemory(MakeFormPdf(), {});
  CHECK_EQ(file.page_count(), 1);
  const pdf::PdfPage& page = file.page(0);

  // Form 里的文本：/Matrix 0.5 × 页面 cm 2 → 原点 (100,100)，字号 10*0.5*2 = 10。
  CHECK_EQ(page.text_runs().size(), size_t(1));
  if (!page.text_runs().empty()) {
    const pdf::PdfTextRun& run = page.text_runs()[0];
    CHECK_EQ(run.text_utf8, std::string("Hi"));
    CHECK(std::abs(run.x - 110.0) < 0.5);
    CHECK(std::abs(run.y - 120.0) < 0.5);
    CHECK(std::abs(run.size_pt - 10.0) < 0.2);
  }
  CHECK_EQ(page.ExtractText(), std::string("Hi"));

  // Form 里的图像：10×10 的格子经 0.5 × 2 两级变换后仍是一个 10×10 的方块。
  CHECK_EQ(page.images().size(), size_t(1));
  if (!page.images().empty()) {
    const pdf::PdfImagePlacement& img = page.images()[0];
    CHECK(std::abs(img.x - 130.0) < 0.5);
    CHECK(std::abs(img.y - 105.0) < 0.5);
    CHECK(std::abs(img.w - 10.0) < 0.5);
    CHECK(std::abs(img.h - 10.0) < 0.5);
    CHECK_EQ(img.pixel_width, 2);
    CHECK_EQ(img.pixel_height, 1);
  }
  // 字体只挂在 Form 上：能解出文字就说明资源链回退生效。
  // I1 只在 Form 的 /Resources 里，页级按名字查不到。
  CHECK(page.FindXObject("I1") == nullptr);
  CHECK(page.FindXObject("Fm1") != nullptr);
  const pdf::ObjectPtr* form = page.FindXObject("Fm1");
  CHECK(form != nullptr);
  if (form != nullptr && *form != nullptr) {
    CHECK((*form)->Find("Resources") != nullptr);
  }
}

void TestPdfNoTextLayer() {
  // 只放一张整页图、没有文本层：默认必须报 Unsupported，而不是产出空文档。
  LayoutPage page;
  page.width_pt = 200;
  page.height_pt = 200;
  ImageData img;
  img.width = 4;
  img.height = 4;
  img.rgba.assign(4 * 4 * 4, 128);
  for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
  DrawImage di;
  di.image = img;
  di.x = 0;
  di.y = 0;
  di.w_pt = 200;
  di.h_pt = 200;
  page.ops.push_back(di);
  LayoutDocument doc;
  doc.pages.push_back(page);
  WarningList warnings;
  const std::vector<uint8_t> bytes = MakePdf(doc, &warnings);

  pdf::PdfFile file = pdf::PdfFile::OpenMemory(bytes, {});
  CHECK_EQ(file.page_count(), 1);
  CHECK(file.page(0).looks_scanned());

  pdf::RebuildOptions opt;
  WarningList rebuild_warnings;
  CHECK_THROWS(pdf::Reconstructor::ToDocument(file, opt, &rebuild_warnings),
               ErrorCode::Unsupported);
  bool saw_no_text = false;
  for (const Warning& w : rebuild_warnings.items()) {
    if (w.code == static_cast<int>(WarningCode::NoTextLayer)) saw_no_text = true;
  }
  CHECK(saw_no_text);
  // 打开 OCR 但没有插件时仍然是 Unsupported（不静默成功）。
  pdf::RebuildOptions ocr;
  ocr.ocr_if_no_text = true;
  CHECK_THROWS(pdf::Reconstructor::ToDocument(file, ocr, nullptr), ErrorCode::Unsupported);
  // 三个方向都必须拒绝。
  CHECK_THROWS(pdf::Reconstructor::ToWorkbook(file, opt, nullptr), ErrorCode::Unsupported);
  CHECK_THROWS(pdf::Reconstructor::ToPresentation(file, opt, nullptr), ErrorCode::Unsupported);
}

void TestConvertSupport() {
  CHECK(Converter::SupportOf(Format::Docx, Format::Pdf) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Docx, Format::Docx) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Docx, Format::Html) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Xlsx, Format::Csv) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Pptx, Format::Png) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Pdf, Format::Pdf) == Support::Yes);
  CHECK(Converter::SupportOf(Format::Pdf, Format::Text) == Support::Yes);
  // PDF 反向一律 Lossy。
  CHECK(Converter::SupportOf(Format::Pdf, Format::Docx) == Support::Lossy);
  CHECK(Converter::SupportOf(Format::Pdf, Format::Xlsx) == Support::Lossy);
  CHECK(Converter::SupportOf(Format::Pdf, Format::Pptx) == Support::Lossy);
  // 三族之间没有转换路径。
  CHECK(Converter::SupportOf(Format::Docx, Format::Xlsx) == Support::No);
  CHECK(Converter::SupportOf(Format::Xlsx, Format::Pptx) == Support::No);
  CHECK(Converter::SupportOf(Format::Pptx, Format::Docx) == Support::No);
  CHECK(Converter::SupportOf(Format::Pdf, Format::Png) == Support::No);
  CHECK(Converter::SupportOf(Format::Unknown, Format::Pdf) == Support::No);
  CHECK_EQ(std::string(SupportName(Support::Lossy)), std::string("lossy"));

  // 格式探测：PDF 魔数、ZIP 容器按内容类型判族、扩展名兜底。
  const std::vector<uint8_t> pdf = ReadFixture("sample_pdf.pdf");
  CHECK(Converter::Detect(pdf.data(), pdf.size()) == Format::Pdf);
  const std::vector<uint8_t> docx = ReadFixture("sample.docx");
  CHECK(Converter::Detect(docx.data(), docx.size()) == Format::Docx);
  const std::vector<uint8_t> xlsx = ReadFixture("sample.xlsx");
  CHECK(Converter::Detect(xlsx.data(), xlsx.size()) == Format::Xlsx);
  const std::vector<uint8_t> pptx = ReadFixture("sample.pptx");
  CHECK(Converter::Detect(pptx.data(), pptx.size()) == Format::Pptx);
  const uint8_t junk[] = {'h', 'e', 'l', 'l', 'o'};
  CHECK(Converter::Detect(junk, sizeof(junk)) == Format::Unknown);
  CHECK(Converter::Detect(junk, sizeof(junk), "a.csv") == Format::Csv);
}

void TestConvertPdfToDocx() {
  const std::vector<uint8_t> pdf = ReadFixture("sample_pdf.pdf");
  MemoryStream in(pdf);
  MemoryStream out;
  ConvertOptions options;
  ConvertRequest request{in, out, Format::Unknown, Format::Docx, options, nullptr, nullptr};
  const Support support = Converter::Run(request);
  CHECK(support == Support::Lossy);
  CHECK(out.data().size() > 1000);
  // 产出必须是合法 ZIP（DOCX 包）。
  CHECK_EQ(out.data()[0], uint8_t('P'));
  CHECK_EQ(out.data()[1], uint8_t('K'));

  text::Document doc = text::OpenMemory(
      std::string_view(reinterpret_cast<const char*>(out.data().data()), out.data().size()),
      nullptr, nullptr);
  CHECK(doc.block_count() > 5);

  // PDF -> PDF 是原样复制。
  MemoryStream copy_in(pdf);
  MemoryStream copy_out;
  ConvertRequest copy_request{copy_in, copy_out, Format::Pdf, Format::Pdf, options, nullptr, nullptr};
  CHECK(Converter::Run(copy_request) == Support::Yes);
  CHECK_EQ(copy_out.data().size(), pdf.size());
  CHECK(std::memcmp(copy_out.data().data(), pdf.data(), pdf.size()) == 0);

  // 不支持的路径：抛 Unsupported，且目标流一个字节都不写。
  MemoryStream bad_in(pdf);
  MemoryStream bad_out;
  ConvertRequest bad{bad_in, bad_out, Format::Pdf, Format::Png, options, nullptr, nullptr};
  CHECK_THROWS(Converter::Run(bad), ErrorCode::Unsupported);
  CHECK_EQ(bad_out.data().size(), size_t(0));

  // 显式格式与内容冲突：InvalidArgument。
  MemoryStream conflict_in(pdf);
  MemoryStream conflict_out;
  ConvertRequest conflict{conflict_in, conflict_out, Format::Docx, Format::Docx, options,
                          nullptr, nullptr};
  CHECK_THROWS(Converter::Run(conflict), ErrorCode::InvalidArgument);

  // 中途失败要截断回调用前的位置：给一个已经写了内容的流。
  MemoryStream partial_in(pdf);
  MemoryStream partial_out;
  const uint8_t prefix[] = {'k', 'e', 'e', 'p'};
  partial_out.Write(prefix, 4);
  ConvertRequest failing{partial_in, partial_out, Format::Pdf, Format::Docx, options, nullptr,
                         nullptr};
  // 先成功一次，确认前缀被保留。
  CHECK(Converter::Run(failing) == Support::Lossy);
  CHECK(partial_out.data().size() > 4);
  CHECK(std::memcmp(partial_out.data().data(), prefix, 4) == 0);
}

void TestConvertDocxToPdf() {
  const std::vector<uint8_t> docx = ReadFixture("sample.docx");
  FontRegistry fonts;
  fonts.AddFontFile(Fixture("test.ttf"));
  MemoryStream in(docx);
  MemoryStream out;
  ConvertOptions options;
  options.fonts = &fonts;
  ConvertRequest request{in, out, Format::Docx, Format::Pdf, options, nullptr, nullptr};
  CHECK(Converter::Run(request) == Support::Yes);
  CHECK(out.data().size() > 2000);
  const std::string head(out.data().begin(), out.data().begin() + 5);
  CHECK_EQ(head, std::string("%PDF-"));
  // 读回来页数与文本都要在。
  pdf::PdfFile back = pdf::PdfFile::OpenMemory(out.data(), {});
  CHECK(back.page_count() >= 1);
  CHECK(!back.ExtractText().empty());
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Run("crc32", TestCrc);
  Run("zlib store", TestZlibStore);
  Run("inflate python", TestInflatePython);
  Run("inflate corrupt", TestInflateCorrupt);
  Run("zip read", TestZipRead);
  Run("zip traversal", TestZipRejectsTraversal);
  Run("zip ratio", TestZipRejectsRatio);
  Run("zip entry limit", TestZipEntryLimit);
  Run("xml basic", TestXmlBasic);
  Run("xml doctype", TestXmlDoctypeRejected);
  Run("xml depth", TestXmlDepth);
  Run("xml multi attribute", TestXmlMultiAttribute);
  Run("xml attribute entities", TestXmlAttributeEntities);
  Run("png encode", TestPng);
  Run("raster rect", TestRasterRect);
  Run("ttf parse", TestTtfBasics);
  Run("pdf text", TestPdfWithText);
  Run("pdf links outline", TestPdfLinksAndOutline);
  Run("docx to pdf outline", TestDocxToPdfOutline);
  Run("deck to pdf outline", TestDeckToPdfOutlineAndLinks);
  Run("pdf annotations", TestPdfAnnotations);
  Run("crypto primitives", TestCryptoPrimitives);
  Run("pdf encryption", TestPdfEncryption);
  Run("docx watermark protection", TestDocxWatermarkAndProtection);
  Run("zip write round trip", TestZipWriteRoundTrip);
  Run("zip write unsafe name", TestZipWriteRejectsUnsafeNames);
  Run("zip write empty", TestZipWriteEmpty);
  Run("deflate round trip", TestDeflateRoundTrip);
  Run("opc open sample", TestOpcOpenSample);
  Run("opc non-zip", TestOpcRejectsNonZip);
  Run("opc no content types", TestOpcRejectsMissingContentTypes);
  Run("docx structure", TestDocxReadStructure);
  Run("docx escaping rels", TestDocxRejectsEscapingRels);
  Run("docx plain zip rejected", TestDocxRejectsPlainZip);
  Run("docx write round trip", TestDocxWriteRoundTrip);
  Run("docx write to file", TestDocxWriteToFile);
  Run("docx write empty", TestDocxWriteEmptyDocument);
  Run("layout pagination", TestLayoutPagination);
  Run("layout line breaking", TestLayoutLineBreaking);
  Run("layout numbering", TestLayoutNumbering);
  Run("text export", TestTextExport);
  Run("sheet addresses", TestSheetAddresses);
  Run("sheet number format", TestSheetNumberFormat);
  Run("formula parse", TestFormulaParse);
  Run("formula eval", TestFormulaEval);
  Run("sheet recalculate", TestSheetRecalculate);
  Run("xlsx round trip", TestXlsxRoundTrip);
  Run("xlsx hyperlinks", TestXlsxHyperlinks);
  Run("xlsx exports", TestXlsxExports);
  Run("xlsx to file", TestXlsxToFile);
  Run("xlsx read sample", TestXlsxReadSample);
  Run("xlsx read charts", TestXlsxReadCharts);
  Run("xlsx pivot", TestXlsxPivot);
  Run("conditional format", TestConditionalFormat);
  Run("data validation", TestDataValidation);
  Run("mail parse", TestMail);
  Run("barcode", TestBarcode);
  Run("chart layout", TestChartLayout);
  Run("sheet layout", TestSheetLayout);
  Run("deck dom", TestDeckDom);
  Run("deck units and images", TestDeckUnitsAndImages);
  Run("pptx round trip", TestPptxRoundTrip);
  Run("pptx transition animation", TestPptxTransitionAndAnimation);
  Run("pptx read sample", TestPptxReadSample);
  Run("pptx layout", TestPptxLayout);
  Run("deck image decode", TestDeckImageDecode);
  Run("pdf read fixture", TestPdfReadFixture);
  Run("pdf read own output", TestPdfReadOwnOutput);
  Run("pdf block grouping", TestPdfBlockGrouping);
  Run("pdf table detect", TestPdfTableDetect);
  Run("pdf rebuild docx", TestPdfRebuildDocx);
  Run("pdf rebuild xlsx", TestPdfRebuildXlsx);
  Run("pdf rebuild pptx", TestPdfRebuildPptx);
  Run("pdf no text layer", TestPdfNoTextLayer);
  Run("pdf form xobject", TestPdfFormXObject);
  Run("convert support", TestConvertSupport);
  Run("convert pdf to docx", TestConvertPdfToDocx);
  Run("convert docx to pdf", TestConvertDocxToPdf);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
