// DocCore - 电子表格 DOM（中性命名 sheet，见 docs/02 §3.4.1）。
//
// 稀疏存储：列号 -> 行号 -> 单元格。空单元格不占内存，所以 50 万单元格的
// 工作簿只需要 50 万个节点，而不是一张二维表。
//
// 数值与显示分离：单元格存"值"（数字/文本/布尔/错误）加"数字格式"，
// 格式化只发生在导出与显示时，不改动存储值（docs/02 §3.4.1）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace sheet {

// ---- 值与错误 -----------------------------------------------------------

// OOXML 的错误文本一一对应；后两个是本引擎自己的（未实现函数、循环引用）。
enum class CellError : int16_t {
  Null = 0,     // #NULL!
  DivZero = 1,  // #DIV/0!
  Value = 2,    // #VALUE!
  Ref = 3,      // #REF!
  Name = 4,     // #NAME?
  Num = 5,      // #NUM!
  NotAvailable = 6,  // #N/A
  NotImpl = 7,       // #NIMPL!
  Circular = 8,      // #CIRC!
};

const char* CellErrorText(CellError code) noexcept;
CellError CellErrorFromText(std::string_view text);

// 运行期值。公式求值、函数调用、显示都走这一个类型。
struct Value {
  enum class Kind : uint8_t { Empty, Number, Text, Bool, Error };

  Kind kind = Kind::Empty;
  double number = 0.0;
  bool boolean = false;
  CellError error = CellError::Value;
  std::string text;

  static Value Empty() { return Value(); }
  static Value Num(double v);
  static Value Str(std::string v);
  static Value Boolean(bool v);
  static Value Err(CellError e);

  bool is_empty() const noexcept { return kind == Kind::Empty; }
  bool is_number() const noexcept { return kind == Kind::Number; }
  bool is_error() const noexcept { return kind == Kind::Error; }
  bool is_text() const noexcept { return kind == Kind::Text; }
};

// ---- 数字格式 -----------------------------------------------------------

// P0 支持的格式子集：整数、小数（含千分位）、百分号、货币前后缀、
// 日期/时间、文本。识别不了的格式保留原文，显示时退回通用格式并告警。
struct NumberFormat {
  enum class Kind : uint8_t {
    General,
    Integer,
    Decimal,
    Percent,
    Currency,
    Date,
    Time,
    DateTime,
    Text,
  };

  Kind kind = Kind::General;
  int decimals = 2;
  bool thousands = false;
  bool negative_parens = false;
  std::string prefix;  // 货币符号等字面量
  std::string suffix;
  std::string pattern;  // 原始格式串，保存时原样写回

  bool is_date() const noexcept { return kind == Kind::Date || kind == Kind::DateTime; }
  bool is_time() const noexcept { return kind == Kind::Time || kind == Kind::DateTime; }
};

// 内置格式 id（1..49）到格式串；未知 id 返回空串。
std::string_view BuiltinNumberFormat(int id);

// 解析格式串；无法识别时返回 General 并保持 pattern 原样。
NumberFormat ParseNumberFormat(std::string_view pattern);

// 按格式把值渲染成显示文本。日期用 1900 日期系统（date1904 为真时用 1904）。
std::string FormatValue(const Value& value, const NumberFormat& fmt, bool date1904 = false);

// 单元格文本的默认显示（General）。
std::string FormatValue(const Value& value, bool date1904 = false);

// ---- 样式 ---------------------------------------------------------------

struct CellFont {
  std::string name = "Calibri";
  double size_pt = 11.0;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  int color = -1;  // 0xRRGGBB，-1 = 默认
};

struct CellStyle {
  CellFont font;
  int num_fmt_id = 0;
  NumberFormat num_fmt;  // 已解析
  int fill = -1;         // 0xRRGGBB，-1 = 无填充
  int halign = -1;       // -1 未设置 / 0 常规 / 1 左 / 2 中 / 3 右
  int valign = -1;       // -1 未设置 / 0 上 / 1 中 / 2 下
  bool wrap = false;
  int border_mask = 0;  // bit0 上 / 1 右 / 2 下 / 3 左
};

// ---- 单元格 -------------------------------------------------------------

enum class CellKind : uint8_t { Empty, Number, Text, Bool, Error, Formula };

struct Cell {
  CellKind kind = CellKind::Empty;
  double number = 0.0;
  bool boolean = false;
  int32_t text_id = -1;   // 共享字符串池下标
  CellError error = CellError::Value;
  int32_t style_id = -1;
  int32_t formula_id = -1;  // 索引 Worksheet::formulas，-1 = 无公式
  Value cached;             // 上次保存或上次重算的缓存值
  bool dirty = true;        // 需要重算

  bool empty() const noexcept { return kind == CellKind::Empty && formula_id < 0; }
  bool has_formula() const noexcept { return formula_id >= 0; }
};

// 公式：文本 + 解析后的 AST（AST 定义在 formula.h，这里用不透明指针）。
struct Formula {
  std::string text;  // 不含前导 '='
  std::shared_ptr<const void> ast;
};

// A1 风格的单元格地址。
struct CellRef {
  int row = 0;  // 0 基
  int col = 0;
  bool absolute_row = false;
  bool absolute_col = false;
};

std::string ColumnName(int col);          // 0 -> "A", 26 -> "AA"
int ColumnFromName(std::string_view name);  // "AA" -> 26，非法返回 -1
std::string CellName(int row, int col);   // (0,0) -> "A1"
bool ParseCellName(std::string_view name, CellRef* out);  // "B3" -> {2,1}

struct Range {
  int row1 = 0, col1 = 0, row2 = 0, col2 = 0;  // 闭区间，0 基
  bool valid() const noexcept { return row2 >= row1 && col2 >= col1; }
  int64_t cell_count() const noexcept {
    return static_cast<int64_t>(row2 - row1 + 1) * (col2 - col1 + 1);
  }
};

// ---- 条件格式（F-205）---------------------------------------------------

enum class CfType : uint8_t { CellIs, Expression, Unknown };

enum class CfOperator : uint8_t {
  None, LessThan, LessEqual, Equal, NotEqual, GreaterEqual, GreaterThan,
  Between, NotBetween
};

const char* CfOperatorName(CfOperator op) noexcept;  // OOXML 运算符文本

// 条件命中后叠加的"差异格式"，对应 OOXML 的 dxf。字段为 -1/false 表示不改。
struct CfDxf {
  int fill = -1;        // 0xRRGGBB，-1 = 不改填充
  int font_color = -1;  // 0xRRGGBB
  bool bold = false;
};

struct ConditionalRule {
  CfType type = CfType::CellIs;
  CfOperator op = CfOperator::None;
  std::vector<double> operands;  // cellIs 的阈值（1~2 个，按运算符取值）
  std::string formula;           // expression 类型的公式（不含 '=', 相对区域左上角）
  CfDxf dxf;
  int dxf_id = -1;         // 写回时在 Workbook::dxfs 里的下标（-1 = 写回时再分配）
  int priority = 1;              // 数字小者优先
};

struct ConditionalFormat {
  Range range;
  std::vector<ConditionalRule> rules;
};

// ---- 数据验证（F-205）---------------------------------------------------

enum class ValidationType : uint8_t {
  List, Whole, Decimal, Date, TextLength, Custom, Unknown
};

enum class ValidationOp : uint8_t {
  None, Between, NotBetween, Equal, NotEqual, LessThan, LessEqual, GreaterThan, GreaterEqual
};

const char* ValidationTypeName(ValidationType t) noexcept;
const char* ValidationOpName(ValidationOp op) noexcept;

// 对应 OOXML 工作表里的 <dataValidation>。 ranges 由 sqref 拆出（可能多段）。
// 列表型（type=List）的合法值来源：formula1 若是引号包裹的逗号列表（如
// "Yes,No,Maybe"）则直接拆解；若是区域引用（如 Sheet1!$A$1:$A$3 或 $A$1:$A$3）
// 则在 Workbook 上解析出该区域每个单元格的文本值。
struct DataValidation {
  ValidationType type = ValidationType::List;
  ValidationOp op = ValidationOp::None;
  std::string formula1;
  std::string formula2;
  bool allow_blank = true;
  bool show_error = true;
  int error_style = 1;  // 1=stop, 2=warning, 3=information（Excel 语义）
  std::string error_title, error_message, prompt_title, prompt_message;
  std::vector<Range> ranges;
};

// WriteCell 的返回：描述这次写入与验证的交互结果。
struct ValidationResult {
  bool written = false;   // 值最终是否被写入单元格
  bool rejected = false;  // 严格模式下被拒绝（未写入）
  bool warned = false;    // 默认模式下写入但产生了验证警告
};

// ---- 超链接 -------------------------------------------------------------

// 对应 OOXML 工作表里的 <hyperlink ref="A1:B2" r:id="rId1"/>。
// target 为外部 URI（写在 _rels 里，TargetMode="External"）；location 为文档内
// 位置（"Sheet2!A1" 或定义名），两者可以同时存在（Excel 允许）。tooltip 是
// 悬停提示，display 是显示文本（省略时取单元格自身的值）。
struct Hyperlink {
  Range range;
  std::string target;
  std::string location;
  std::string display;
  std::string tooltip;

  bool empty() const noexcept { return target.empty() && location.empty(); }
};

// ---- 打印设置 -----------------------------------------------------------

struct PrintSetup {
  int paper_width_twips = 11906;   // A4
  int paper_height_twips = 16838;
  int margin_top = 720;
  int margin_bottom = 720;
  int margin_left = 720;
  int margin_right = 720;
  bool landscape = false;
  int fit_to_width = 0;   // 0 = 不缩放；>0 = 压到 N 页宽
  int fit_to_height = 0;
  int repeat_rows_first = -1;   // 重复标题行（0 基，闭区间）
  int repeat_rows_last = -1;
  // 默认必须是"无效区间"（row2 < row1），否则打印布局会把整表裁成 A1。
  Range print_area{0, 0, -1, -1};  // valid() 为假表示整表
};

// ---- 数据透视表 ---------------------------------------------------------
//
// 模型分三层，与 OOXML 的三个部件一一对应：
//   PivotTable          <-> xl/pivotTables/pivotTableN.xml     布局与字段归属
//   PivotCache          <-> xl/pivotCache/pivotCacheDefinition1.xml  字段与共享项
//   （缓存记录）        <-> xl/pivotCache/pivotCacheRecords1.xml     源数据的快照
//
// 刷新（RefreshPivot）走的是**工作表里的实时数据**而不是缓存记录——缓存的
// 意义是"上次刷新时的样子"，刷新的意义正是把它换成现在的样子。

// 聚合方式，对应 dataField/@subtotal。
enum class PivotAggregate {
  Sum,
  Count,       // 非空计数
  Average,
  Max,
  Min,
  Product,
  CountNumbers,  // 只数数值
  StdDev,
  StdDevP,
  Variance,
  VarianceP,
};

const char* PivotAggregateName(PivotAggregate a) noexcept;  // "sum" / "count" ...
PivotAggregate PivotAggregateFromName(std::string_view name);

// 轴归属，对应 pivotField/@axis。
enum class PivotAxis { None, Row, Column, Page, Data };

// 一个缓存字段（源数据的一列）。
struct PivotCacheField {
  std::string name;
  // 共享项：字符串字段是去重后的取值表（顺序即 Excel 的 item 下标）；
  // 数值字段留空，Excel 用 min/max 描述。
  std::vector<std::string> shared_items;
  bool has_numbers = false;
  bool has_strings = false;
  double min_value = 0;
  double max_value = 0;
};

// rowItems / colItems 的一项：一列（或一行）表头对应的各级 item 下标。
struct PivotItem {
  bool grand = false;            // t="grand"：总计行/列
  std::vector<int> indices;      // 每级字段的 item 下标，-1 表示该级不适用
};

struct PivotDataField {
  int field_index = -1;                       // 指向 PivotTable::fields
  std::string name;                           // 显示名（"求和项:金额"）
  PivotAggregate aggregate = PivotAggregate::Sum;
  std::string number_format;                  // numFmtId 的原文，空 = 常规
};

struct PivotField {
  std::string name;
  PivotAxis axis = PivotAxis::None;
  bool show_all = true;       // showAll="1"
  bool data_field = false;    // dataField="1"
  // 该字段在行/列/页方向上的 item 显示顺序（下标指向 PivotCacheField::shared_items）。
  // 空表示按缓存顺序。
  std::vector<int> item_order;
};

struct PivotTable {
  std::string name = "PivotTable1";
  std::string sheet;        // 所在工作表
  std::string source_sheet;  // 源数据工作表
  Range source;              // 源区域，首行是字段名
  CellRef destination;       // 目标左上角
  int cache_id = 1;

  std::vector<PivotCacheField> fields;
  std::vector<PivotField> pivot_fields;   // 与 fields 一一对应
  std::vector<int> row_fields;
  std::vector<int> column_fields;
  std::vector<int> page_fields;
  std::vector<PivotDataField> data_fields;

  bool show_row_grand_totals = true;
  bool show_column_grand_totals = true;
  std::string style_name = "PivotStyleLight16";

  // 布局描述（刷新时填，写出时用）。读入时保留原文，未刷新则原样写回。
  std::vector<PivotItem> row_items;
  std::vector<PivotItem> column_items;

  std::string part_uri;   // 读入时的部件路径，写回时定位用
  std::string cache_uri;  // 缓存定义部件路径；workbook.xml 的 pivotCaches 指向它
  // 读入的透视表默认不 dirty：保存时原样写回原部件（字节级往返）。调用过
  // RefreshPivot 或新建的表会置位，保存时按 DOM 重新生成三个部件。
  bool dirty = false;

  bool valid() const noexcept { return source.valid() && !fields.empty(); }
};

// ---- 工作表 -------------------------------------------------------------

class Workbook;

class Worksheet {
 public:
  Worksheet() = default;
  explicit Worksheet(std::string name) : name_(std::move(name)) {}

  const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }

  // 不存在返回 nullptr（读路径用，不制造空单元格）。
  Cell* Find(int row, int col);
  const Cell* Find(int row, int col) const;
  // 不存在则创建（写路径用）。
  Cell& CellAt(int row, int col);

  void SetNumber(int row, int col, double v);
  void SetText(int row, int col, std::string text);
  void SetBool(int row, int col, bool v);
  void SetError(int row, int col, CellError e);
  // 公式文本不带 '='；返回公式下标。
  int32_t SetFormula(int row, int col, std::string text);

  // 带数据验证的写入：若 (row,col) 命中列表型验证且值不在列表中，默认模式
  // 仍写入并往工作簿 write_warnings 追加 ValidationFailed 警告；严格模式
  // （Workbook::strict_validation 为真）拒绝写入并返回 rejected。命中且合法、
  // 或未命中验证时与普通 Set* 等价（written=true）。
  ValidationResult WriteCell(int row, int col, const Value& v);

  // 显示值：有公式取缓存/计算结果，否则取存储值。
  Value GetValue(int row, int col) const;
  // 该单元格的样式（越界或未设置时返回默认样式）。
  const CellStyle& StyleOf(int row, int col) const;

  Range UsedRange() const;
  void Merge(int row1, int col1, int row2, int col2);
  bool Merged(int row, int col) const;

  // 列宽用字符数（OOXML 约定），行高用点。
  double ColumnWidth(int col) const;
  void SetColumnWidth(int col, double chars);
  double RowHeight(int row) const;
  void SetRowHeight(int row, double pt);

  const std::map<int32_t, std::map<int32_t, Cell>>& columns() const noexcept { return columns_; }
  const std::map<int32_t, double>& col_widths() const noexcept { return col_widths_; }
  const std::map<int32_t, double>& row_heights() const noexcept { return row_heights_; }
  std::map<int32_t, std::map<int32_t, Cell>>& columns() noexcept { return columns_; }
  const std::vector<Range>& merges() const noexcept { return merges_; }
  std::vector<Range>& merges() noexcept { return merges_; }
  const std::vector<Formula>& formulas() const noexcept { return formulas_; }
  std::vector<Formula>& formulas() noexcept { return formulas_; }
  const std::vector<ConditionalFormat>& conditional() const noexcept { return conditional_; }
  std::vector<ConditionalFormat>& conditional() noexcept { return conditional_; }
  const std::vector<DataValidation>& validations() const noexcept { return validations_; }
  std::vector<DataValidation>& validations() noexcept { return validations_; }
  const std::vector<Hyperlink>& hyperlinks() const noexcept { return hyperlinks_; }
  std::vector<Hyperlink>& hyperlinks() noexcept { return hyperlinks_; }
  PrintSetup& print() noexcept { return print_; }
  const PrintSetup& print() const noexcept { return print_; }

  // 所属工作簿（由 Workbook::AddSheet / 读取器设置）：共享字符串池与样式表
  // 都在工作簿上，所以取显示值与样式需要它。为空时按空池处理。
  Workbook* owner() const noexcept { return owner_; }

 private:
  friend class Workbook;
  std::string name_ = "Sheet1";
  Workbook* owner_ = nullptr;
  std::map<int32_t, std::map<int32_t, Cell>> columns_;
  std::vector<Range> merges_;
  std::vector<Formula> formulas_;
  std::vector<ConditionalFormat> conditional_;  // F-205 条件格式
  std::vector<DataValidation> validations_;  // F-205 数据验证
  std::vector<Hyperlink> hyperlinks_;
  std::map<int32_t, double> col_widths_;   // 字符宽度
  std::map<int32_t, double> row_heights_;  // 点
  PrintSetup print_;
};

// ---- 工作簿 -------------------------------------------------------------

struct DefinedName {
  std::string name;
  std::string refers_to;  // 不含前导 '='，例如 "Sheet1!$A$1:$B$3"
};

struct UnknownRel {
  std::string id;
  std::string type;      // 完整关系类型 URI
  std::string target;    // 已解析成 Part-URI（内部目标）
  bool external = false; // 外部目标按 TargetMode="External" 写回
};

// 未识别部件：保存时连关系一起原样写回（图表、绘图这类部件只有关系链完整
// 才有效，光写部件本身 Excel 会直接忽略）。
struct UnknownPart {
  std::string uri;
  std::string mime;
  std::vector<uint8_t> bytes;
  std::vector<UnknownRel> rels;
};

// 工作表与绘图部件的对应关系：图表锚点放在绘图部件里，写回时要给工作表补
// 一条 drawing 关系，否则图表整条链都断了。
struct SheetDrawing {
  std::string sheet;      // 工作表名
  std::string part_uri;   // "/xl/drawings/drawingN.xml"
};

// ---- 图表 ---------------------------------------------------------------

// 只覆盖 P9 承诺的常用图型；其余图型读入时归 Unknown 并保留原部件。
enum class ChartKind { Column, Bar, Line, Pie, Area, Scatter, Doughnut, Unknown };

const char* ChartKindName(ChartKind kind) noexcept;

struct ChartSeries {
  std::string name;                     // c:tx
  std::vector<double> values;           // c:val 的 numCache（无缓存时按区域实取）
  std::vector<std::string> categories;  // c:cat 的 strCache/numCache
  int accent = -1;                      // srgbClr 覆盖；-1 = 用默认调色板
};

struct Chart {
  ChartKind kind = ChartKind::Column;
  std::string title;                 // c:title 的纯文本
  bool legend = true;                // c:legend 存在
  bool stacked = false;              // bar: c:grouping="stacked"
  bool smooth = false;               // line: c:smooth
  std::vector<ChartSeries> series;
  // 锚定矩形：工作表坐标（点），原点在 A1 左上角。未关联锚点时 has_anchor=false，
  // 由调用方给默认位置。
  double x = 0, y = 0, w = 0, h = 0;
  bool has_anchor = false;
  std::string sheet;        // 所属工作表名（drawing 挂在哪张表上）
  std::string part_uri;     // xl/charts/chartN.xml，写回时定位用
};

class Workbook {
 public:
  static Workbook Create();

  // 深拷贝：工作表、样式、共享字符串池、定义名称、未识别部件全部复制。
  Workbook Clone() const;

  int sheet_count() const noexcept { return static_cast<int>(sheets.size()); }
  Worksheet* SheetAt(int index);
  const Worksheet* SheetAt(int index) const;
  Worksheet* SheetByName(std::string_view name);
  const Worksheet* SheetByName(std::string_view name) const;
  Worksheet& AddSheet(std::string name);
  void RemoveSheet(int index);

  // 共享字符串池（读写都去重）。
  int32_t InternString(const std::string& text);
  const std::string& StringAt(int32_t id) const;

  const CellStyle& StyleAt(int32_t id) const;
  int32_t AddStyle(CellStyle style);
  const CellStyle& DefaultStyle() const;

  // 全表重算；返回参与计算的单元格数。循环引用只污染环上单元格。
  int Recalculate(CancelToken* cancel = nullptr);
  // 只重算指定工作表（导出时用）。
  int RecalculateSheet(Worksheet& sheet, CancelToken* cancel = nullptr);

  // 值查询（跨表引用用）。
  Value ValueOf(const Worksheet& sheet, int row, int col) const;

  std::vector<std::unique_ptr<Worksheet>> sheets;
  std::vector<std::string> shared_strings;
  std::vector<CellStyle> styles;
  std::vector<DefinedName> names;
  std::vector<UnknownPart> unknown_parts;  // 保存时原样写回
  std::vector<SheetDrawing> drawings;      // 工作表 -> 绘图部件（图表锚点）
  std::vector<Chart> charts;               // F-205：图表 DOM（渲染与写回都用它）
  std::vector<PivotTable> pivot_tables;    // F-206：数据透视表
  std::vector<CfDxf> dxfs;                 // F-205：条件格式引用的差异格式表
  bool date1904 = false;
  std::string active_sheet = "Sheet1";

  // F-205 数据验证：严格模式开启后，列表型验证失败会拒绝写入（WriteCell 返回
  // rejected 且不落盘）；默认 false（仅警告仍写入）。
  bool strict_validation = false;
  // 数据验证软失败（默认模式）时累加的警告；读取/导出前可清空或检查。
  WarningList write_warnings_;

  WarningList& write_warnings() noexcept { return write_warnings_; }
  const WarningList& write_warnings() const noexcept { return write_warnings_; }

 private:
  const Worksheet* FindSheetByName(std::string_view name) const;

  // 共享字符串的哈希索引：shared_strings 是公开容器，读取器会直接 push_back，
  // 所以索引在尺寸对不上时整体重建（读取期一次，之后增量为 O(1)）。
  mutable std::unordered_map<std::string, int32_t> string_index_;
};

// 单元格 (row,col) 命中的条件格式叠加格式；未命中返回 nullptr。
// 规则按 priority 从小到大取首个命中者（与 Excel 语义一致）。
const CfDxf* MatchedConditionalDxf(const Workbook& book, const Worksheet& ws, int row, int col);
// 单元格 (row,col) 命中的列表型验证（首个覆盖它的 dataValidation）；未命中返回 nullptr。
const DataValidation* ValidationAt(const Worksheet& ws, int row, int col);

// 解析列表型验证的合法取值集合：内联列表直接拆解，区域引用在 book 上取值。
std::vector<std::string> ListValues(const Workbook& book, const Worksheet& ws,
                                    const DataValidation& dv);

// ---- 数据透视表的操作 ---------------------------------------------------
// 放在 Workbook/Worksheet 之后：这两个函数要用完整类型。

// 刷新：按 source 区域的实时数据重算，把结果写进 destination 起的目标区域，
// 并重建 fields 的共享项与 row_items/column_items。
// 返回写入的单元格数。源区域为空或工作表找不到时抛 InvalidArgument。
int RefreshPivot(Workbook& book, PivotTable& table);

// 在 ws 上建一张透视表：按字段名指定行/列/数据字段（名字必须在表头里，否则
// 抛 InvalidArgument）。建好后自动刷新一次。返回工作簿里那份的引用。
//
// 源区域默认就在 ws 自己身上。Excel 里更常见的是"数据表 + 透视表"分开放，
// 这时用下面那个带 source_ws 的重载——源区域的行列号是按 source_ws 解释的。
PivotTable& AddPivotTable(Worksheet& ws, const Range& source, const CellRef& destination,
                          const std::vector<std::string>& row_fields,
                          const std::vector<std::string>& column_fields,
                          const std::vector<std::string>& data_fields,
                          const std::string& name = std::string());

PivotTable& AddPivotTable(Worksheet& ws, Worksheet& source_ws, const Range& source,
                          const CellRef& destination,
                          const std::vector<std::string>& row_fields,
                          const std::vector<std::string>& column_fields,
                          const std::vector<std::string>& data_fields,
                          const std::string& name = std::string());

// 按名字找字段下标；找不到返回 -1。
int PivotFieldIndex(const PivotTable& table, std::string_view name);

// 源区域的当前数据行（不含表头），每行按字段顺序给出 Value。写出缓存记录
// （pivotCacheRecords）时用它；刷新与写出因此看到同一份数据。
std::vector<std::vector<Value>> PivotSourceRows(const Workbook& book, const PivotTable& table);

}  // namespace sheet
}  // namespace doccore
