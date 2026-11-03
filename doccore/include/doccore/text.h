// DocCore - 字处理 DOM（中性命名 text，见 docs/01 §7 / docs/02 §3.3.1）。
//
// 本节只做"数据容器"：读 DOCX 得到 Document，保存时再序列化回去。
// 排版（Layout）与读写解析在 doccore/text；DOM 不依赖任何文件格式。
//
// 与 OOXML 的映射见 docs/02 §3.3.3 表；样式叠加规则见 §3.3.2。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace text {

// 三态：Unset 表示"继承样式链"，On/Off 表示显式开关（docs/02 §3.3.2）。
enum class Tri { Unset, On, Off };

// ---- 直接格式 ---------------------------------------------------------

// w:rPr 中与本引擎相关的部分。-1/Unset = 未指定。
struct RunFormat {
  Tri bold = Tri::Unset;
  Tri italic = Tri::Unset;
  Tri strike = Tri::Unset;
  Tri double_strike = Tri::Unset;
  Tri all_caps = Tri::Unset;
  Tri small_caps = Tri::Unset;
  Tri emboss = Tri::Unset;
  Tri imprint = Tri::Unset;
  Tri hidden = Tri::Unset;
  Tri no_proof = Tri::Unset;
  int size_half = -1;  // w:sz（半点值），-1 = 继承
  int color = -1;      // w:color val，0xRRGGBB，-1 = 未指定
  int highlight = -1;  // w:highlight val，-1 = 未指定
  int shading = -1;     // w:shd fill，0xRRGGBB，-1 = 未指定
  int vert_align = -1;  // w:vertAlign：0 baseline / 1 superscript / 2 subscript
  int spacing_twips = 0;  // w:spacing字符间距
  int kerning_half = 0;   // w:kern
  int position_half = 0; // w:position（相对基线升降）
  std::string underline;  // w:u val：single / double / dotted ...
  std::string font_ascii;   // w:rFonts w:ascii
  std::string font_hansi;   // w:rFonts w:hAnsi
  std::string font_eastasia;  // w:rFonts w:eastAsia
  std::string font_cs;        // w:rFonts w:cs
  std::string lang;           // w:lang w:val
  std::string character_style;  // w:rStyle val（样式 id）
  int outline_level = -1;      // w:outlineLvl（0..8）
};

// w:pPr 中与本引擎相关的部分。
struct ParagraphFormat {
  std::string style;  // w:pStyle val（样式 id）
  int alignment = -1;   // 0 left / 1 center / 2 right / 3 justify / 4..
  int line_rule = -1;   // w:spacing w:lineRule：0 auto / 1 atLeast / 2 exact
  int line_twips = -1;  // w:spacing w:line（240 份之一）
  int before_twips = -1;
  int after_twips = -1;
  int before_autospacing = -1;
  int after_autospacing = -1;
  int context_before = -1;  // w:contextualSpacing（与段前同时为 0）
  int ind_left = -1;        // w:ind w:left（缇）
  int ind_right = -1;
  int ind_first_line = -1;  // w:ind w:firstLine（正）或 w:hanging（负）
  int ind_first_line_chars = -1;
  Tri keep_next = Tri::Unset;
  Tri keep_lines = Tri::Unset;
  Tri widow_control = Tri::Unset;
  Tri suppress_line_numbers = Tri::Unset;
  Tri page_break_before = Tri::Unset;
  int outline_level = -1;
  int shading = -1;    // w:shd fill
  int border_top = -1;  // 段落边框，仅记录存在性与颜色
  int border_bottom = -1;
  int border_left = -1;
  int border_right = -1;
  std::string num_id;   // w:numPr/w:numId val，空 = 无编号
  int num_level = -1;   // w:numPr/w:ilvl val
  int tabs = -1;        // 制表位数量（详细位置未建模，置位即保留语义）
  int frame_props = -1;  // w:framePr（文本框定位），仅保留
  bool rtl = false;     // w:bidi
  int snap_to_grid = -1;
  int text_direction = -1;  // w:textDirection
  int outline_lvl_visible = -1;
  int mirror_indents = -1;
  int word_wrap = -1;
  int overflow_punct = -1;
  int kinsoku = -1;
  int line_height_exact = -1;
};

// ---- 样式与编号 -------------------------------------------------------

struct Style {
  enum class Kind { Paragraph, Character, Table, Numbering };
  std::string id;    // w:styleId
  std::string name;  // w:name w:val
  Kind kind = Kind::Paragraph;
  std::string based_on;  // w:basedOn val
  std::string next;      // w:next val
  std::string link;      // w:link val（关联样式）
  bool quick_format = false;
  bool hidden = false;
  bool semi_hidden = false;
  bool unhide_when_used = false;
  int priority = -1;
  ParagraphFormat paragraph;
  RunFormat run;
};

struct NumberingLevel {
  int start = 1;
  std::string num_fmt = "decimal";  // decimal / bullet / lowerLetter ...
  std::string lvl_text = "%1.";      // w:lvlText
  int alignment = -1;
  std::string suffix = "tab";  // w:suff：tab / space / nothing
  ParagraphFormat paragraph;  // 缩进等继承属性
  std::string font_ascii;     // 编号使用的字体
  std::string font_eastasia;
  int color = -1;
  int run_size_half = -1;
};

struct AbstractNumbering {
  std::string id;  // w:abstractNumId
  std::vector<NumberingLevel> levels;  // 9 级
};

struct NumberingInstance {
  std::string num_id;      // w:numId
  std::string abstract_id;  // w:abstractNumId val
  std::vector<int> level_start_overrides;  // 每级 start 覆盖，-1 = 不覆盖
};

// ---- 行内元素 ---------------------------------------------------------

// 段落内容。一段文字、一个域、一张图、一个书签，都是 InlineItem。
struct InlineItem {
  enum class Type {
    Text,        // w:t / w:delText
    Tab,         // w:tab
    Break,       // w:br
    Field,        // w:fldChar begin/separate/end
    InstrText,   // w:instrText（域指令）
    Drawing,     // w:drawing / w:pict
    Symbol,      // w:sym
    BookmarkStart,   // w:bookmarkStart
    BookmarkEnd,     // w:bookmarkEnd
    FootnoteRef,     // w:footnoteReference
    EndnoteRef,      // w:endnoteReference
    CommentRef,      // w:commentReference
    Separator,       // w:separator / w:continuationSeparator（注释/脚注分隔线）
  };

  Type type = Type::Text;
  std::string text;               // Text/InstrText 的文本；BookmarkStart 的名字
  RunFormat format;               // w:rPr
  int revision = 0;               // 0 无 / 1 w:ins / 2 w:del
  std::string revision_author;    // w:ins/@w:author
  std::string revision_date;      // w:ins/@w:date
  int break_kind = 0;             // w:br/@w:type：0 text / 1 page / 2 column / 3 line
  int field_kind = 0;             // w:fldChar/@w:fldCharType：0 begin / 1 separate / 2 end
  std::string drawing_target;    // Drawing 的关系目标（Part-URI），经 rels 解析
  std::string drawing_doc_pr;     // wp:docPr/@name，保留供调试
  char32_t symbol = 0;           // w:sym/@w:char
  std::string symbol_font;        // w:sym/@w:font
  int note_id = 0;                // FootnoteRef/EndnoteRef/CommentRef 的 id
  int comment_id = 0;            // CommentRef 的 id（w:comment 索引）
  int tab_pos = -1;              // 自定义制表位（未建模时 -1）
  bool deleted_text = false;      // Text 来自 w:delText
  std::string hyperlink_anchor;    // w:hyperlink/@w:anchor（书签跳转）
  std::string hyperlink_target;   // 经 rels 解析后的外部 URL
};

// ---- 块级元素 ---------------------------------------------------------

struct Paragraph {
  ParagraphFormat format;
  std::vector<InlineItem> runs;  // 行内序列（名字沿用 OOXML 的 w:r）
  std::string pstyle_name;        // 调试用：样式 id 副本

  // 便捷查询
  bool empty_runs() const { return runs.empty(); }
};

// Block 先于 Table 定义：表格成员用 shared_ptr 打破 Cell↔Table 的循环依赖
//（shared_ptr 允许目标类型不完整，值成员做不到）。
struct Table;

struct Block {
  enum class Type { Paragraph, Table } type = Type::Paragraph;
  Paragraph paragraph;           // type == Paragraph 时有效
  std::shared_ptr<Table> table;  // type == Table 时有效
};

struct CellFormat {
  int width_twips = -1;
  int vmerge = 0;      // 0 无 / 1 continue / 2 restart
  int grid_span = 1;   // w:gridSpan
  int shading = -1;
  int valign = -1;     // 0 top / 1 center / 2 bottom
  int borders = -1;    // 边框存在性掩码
  bool no_wrap = false;
  std::string style;  // w:tcStyle
  bool hide_mark = false;
};

struct Cell {
  CellFormat format;
  std::vector<Block> blocks;
};

struct Row {
  int height_twips = -1;
  int height_rule = -1;  // 0 auto / 1 atLeast / 2 exact
  bool header_row = false;  // w:tblHeader
  bool cant_split = false;  // w:cantSplit
  std::vector<Cell> cells;
};

struct Table {
  std::vector<int> grid_twips;  // w:tblGrid/w:gridCol，缺失时重建（§3.3.3）
  std::vector<Row> rows;
  std::string style;  // w:tblStyle
  int width_twips = -1;
  int alignment = -1;  // w:jc：0 left / 1 center / 2 right
  int borders = -1;
  bool overlap = false;
  bool layout_fixed = false;
  int indent_twips = 0;
};

// ---- 节、页眉页脚 -----------------------------------------------------

struct PageSetup {
  int width_twips = 11906;   // A4 纵向
  int height_twips = 16838;
  int margin_top = 1440;
  int margin_bottom = 1440;
  int margin_left = 1440;
  int margin_right = 1440;
  int header_dist = 720;
  int footer_dist = 720;
  int gutter = 0;
  int orientation = 0;  // 0 portrait / 1 landscape
};

struct ColumnDef {
  int width_twips = 0;
  int space_twips = 0;
};

struct HeaderFooter {
  enum class Kind { Default, First, Even };
  Kind kind = Kind::Default;
  std::string part_uri;  // /word/header1.xml
  std::vector<Block> blocks;
};

struct Section {
  PageSetup page;
  std::vector<ColumnDef> columns;  // w:cols；为空表示由 width/space 推导
  int column_space = 720;          // w:cols/@w:space
  int column_count = 1;
  bool title_pg = false;  // w:titlePg
  int page_number_start = -1;  // w:pgNumType/@w:start
  int page_number_fmt = 0;    // 0 decimal / 1 upperRoman ...
  std::vector<HeaderFooter> headers;
  std::vector<HeaderFooter> footers;
  std::vector<Block> blocks;  // 本节包含的块（文档 = 各节依次拼接）
};

// ---- 脚注 / 尾注 / 批注 ----------------------------------------------

struct Note {
  int id = -1;
  std::vector<Block> blocks;
};

struct Comment {
  int id = -1;
  std::string author;
  std::string initials;
  std::string date;
  std::vector<Block> blocks;
};

// ---- 水印 -------------------------------------------------------------
//
// Word 的水印**不是独立元素**：它是页眉里的一个 VML 形状（`v:shape` 里挂
// `v:textpath` 或 `v:imagedata`，外加 `o:Watermark` 扩展属性）。所以读的时候
// 要从页眉里把它认出来，写的时候再重新生成页眉 VML。DOM 里单独建模，是因为
// 它是"整页装饰"，语义上不属于页眉内容，也不该参与页眉的排版流。
struct Watermark {
  enum class Kind { None, Text, Image };
  Kind kind = Kind::None;

  // 文字水印
  std::string text = "DRAFT";
  std::string font = "Calibri";
  int color = 0xC0C0C0;   // 0xRRGGBB，Word 默认银灰
  int size_pt = 0;        // 0 = 按页宽自动推算（Word 的做法）
  // 旋转角，**顺时针为正**，与 Word 的 `rotation:315` 同一约定（默认 315 即
  // 视觉上左低右高）。排版时转成 IR 的逆时针正角，取负即可。
  double rotation = 315.0;
  bool semitransparent = true;  // `v:fill/@opacity`
  bool bold = false;
  bool italic = false;
  // VML 形状盒（pt）。Word 把文字水印的 `v:textpath` 写成 `font-size:1pt` +
  // `fitshape="t"`，真正的字号是由形状盒拉伸出来的——所以盒宽才是权威尺寸。
  // 0 表示没有盒信息，由排版按页宽推算。
  double box_width_pt = 0;
  double box_height_pt = 0;

  // 图片水印
  std::vector<uint8_t> image_bytes;  // 原样保存的图片字节（PNG/JPEG/GIF/BMP）
  std::string image_uri;             // 原 Part-URI，写回时用于生成媒体部件
  int image_scale = 100;             // 百分比
  bool washout = true;               // 褪色（Word 的 gain/blacklevel 效果）

  bool present() const noexcept { return kind != Kind::None; }
};

// ---- 文档保护 ---------------------------------------------------------
//
// `w:documentProtection` 限制"能改什么"，`w:writeProtection` 限制"能不能存"。
// 两者都只存口令的迭代哈希，口令本身不在文件里——所以校验要按 ISO 29500
// §17.15.1.29 的迭代算法重算，见 `VerifyProtectionPassword`。
struct ProtectionHash {
  std::string hash;        // base64，w:hash / w:hashValue
  std::string salt;        // base64，w:salt
  std::string algorithm;   // w:algorithmName，缺省按 SHA-1（老文件）
  int spin_count = 100000;  // w:cryptSpinCount
  int hash_size = 20;       // 哈希字节数：SHA-1=20 / SHA-256=32 / SHA-512=64

  bool present() const noexcept { return !hash.empty() && !salt.empty(); }
};

struct DocumentProtection {
  // w:documentProtection
  bool enabled = false;   // w:enforcement
  int edit = 0;           // 0 none / 1 readOnly / 2 comments / 3 trackedChanges / 4 forms
  bool formatting = false;  // w:formatting：连格式也锁
  ProtectionHash document_hash;

  // w:writeProtection
  bool write_protected = false;
  bool recommended = false;  // w:recommended：只提示，不强制
  ProtectionHash write_hash;
};

// 按 ISO 29500 §17.15.1.29 校验口令：
//   H0 = H(salt ‖ UTF-16LE(password))
//   Hi = H(H(i-1) ‖ uint32_le(i-1))   i = 1..spinCount
//   取 H(spinCount) 的前 hash_size 字节，base64 后与存储值比较。
bool VerifyProtectionPassword(const ProtectionHash& stored, const std::string& password);

// 反向：给定口令与盐，算出可写进 XML 的 base64 哈希（供写入方与测试使用）。
std::string ComputeProtectionHash(const std::string& password, const std::string& salt_base64,
                                  const std::string& algorithm, int spin_count, int hash_size);

// ---- 未识别部件 -------------------------------------------------------

struct UnknownPart {
  std::string uri;  // Part-URI
  std::string mime;
  std::vector<uint8_t> bytes;
};

// ---- Document ----------------------------------------------------------

struct Document {
  RunFormat default_run;            // w:docDefaults/w:rPrDefault
  ParagraphFormat default_paragraph;  // w:docDefaults/w:pPrDefault
  bool has_doc_defaults = false;
  std::vector<Style> styles;
  std::vector<AbstractNumbering> abstract_numberings;
  std::vector<NumberingInstance> numbering;
  std::vector<Section> sections;
  std::vector<Note> footnotes;
  std::vector<Note> endnotes;
  std::vector<Comment> comments;
  std::vector<UnknownPart> unknown_parts;  // 保存时原样写回（§3.1.3 第 6 条）

  Watermark watermark;
  DocumentProtection protection;
  // 原 settings.xml 原文。保护相关的元素在写回时从这里就地改写，其余设置
  // （兼容性开关、修订参数、语言等）一个字节都不动——比"重建一份 settings"
  // 安全得多。空表示原文件没有 settings.xml。
  std::string settings_xml;
  // 原 settings.xml 的 Part-URI（可能是 /word/settings.xml，也可能是别处）。
  std::string settings_uri = "/word/settings.xml";

  // 查询辅助
  const Style* FindStyle(std::string_view id) const;
  const AbstractNumbering* FindAbstract(std::string_view id) const;
  const NumberingInstance* FindNumbering(std::string_view id) const;
  const Style* FindStyleByName(std::string_view name) const;
  size_t block_count() const;
};

}  // namespace text
}  // namespace doccore
