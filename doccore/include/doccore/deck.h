// DocCore - 演示文稿 DOM（中性命名 deck，见 docs/02 §3.7）。
//
// 结构：Presentation -> Slide -> ShapeTree -> Shape。形状只有三类：文本框、
// 图片、表格。幻灯片是绝对定位的画布，没有分页，所以导出 PDF 时一页幻灯片
// 对应一个 LayoutPage，坐标直接用点（1 pt = 12700 EMU）。
//
// 坐标与尺寸全部用 EMU 存（和 OOXML 一致，避免来回换算丢精度）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doccore/core.h"

namespace doccore {
namespace deck {

// 1 pt = 12700 EMU，1 inch = 914400 EMU。
constexpr int64_t kEmuPerPoint = 12700;
constexpr int64_t kEmuPerInch = 914400;

inline double EmuToPt(int64_t emu) { return static_cast<double>(emu) / kEmuPerPoint; }
inline int64_t PtToEmu(double pt) { return static_cast<int64_t>(pt * kEmuPerPoint + 0.5); }

// ---- 文本 ---------------------------------------------------------------

struct RunFormat {
  std::string family;      // 空 = 继承主题/占位符
  double size_pt = 0.0;    // 0 = 继承
  bool bold = false;
  bool italic = false;
  bool underline = false;
  int color = -1;          // 0xRRGGBB，-1 = 继承
};

struct Run {
  std::string text;
  RunFormat format;
  std::string hyperlink;   // 空 = 无
};

struct Paragraph {
  std::vector<Run> runs;
  int align = -1;              // -1 继承 / 0 左 / 1 中 / 2 右 / 3 两端
  int level = 0;               // 缩进层级 0..8
  double space_before_pt = 0.0;
  double space_after_pt = 0.0;
  double line_spacing = 0.0;   // 0 = 单倍；>0 为倍数
  bool bullet = false;

  std::string PlainText() const;
};

// 文本体：段落 + 内边距 + 垂直锚点。
struct TextBody {
  std::vector<Paragraph> paragraphs;
  int anchor = -1;  // 0 顶 / 1 中 / 2 底；-1 = 源文件未写，按继承/顶部处理
  bool wrap = true;
  int64_t inset_left = 91440;    // 0.1 inch
  int64_t inset_top = 45720;     // 0.05 inch
  int64_t inset_right = 91440;
  int64_t inset_bottom = 45720;

  bool empty() const noexcept;
  std::string PlainText() const;
};

// ---- 形状 ---------------------------------------------------------------

enum class ShapeKind : uint8_t { Text, Picture, Table };

struct ShapeGeometry {
  int64_t x = 0, y = 0, cx = 0, cy = 0;
};

struct Picture {
  std::string part_uri;              // 媒体部件（读取时来自包内）
  std::string mime = "image/png";
  std::vector<uint8_t> bytes;        // 原图字节，保存时原样写回
  int pixel_width = 0;
  int pixel_height = 0;
};

struct TableCell {
  TextBody body;
  int fill = -1;      // 0xRRGGBB，-1 = 无
  int row_span = 1;
  int col_span = 1;
  bool merged = false;  // 被合并覆盖的从属单元格
};

struct Table {
  std::vector<std::vector<TableCell>> rows;
  std::vector<int64_t> col_widths;  // EMU，长度 = 列数
  std::vector<int64_t> row_heights; // EMU，长度 = 行数
  bool first_row_header = true;
  bool banded_rows = true;
  int border_color = 0xBFBFBF;
};

struct Shape {
  ShapeKind kind = ShapeKind::Text;
  // 形状 id（`p:cNvPr/@id`）。动画的 p:spTgt/@spid 指的就是它，所以必须能稳定
  // 往返；0 表示调用方没指定，写出时按位置分配。
  int id = 0;
  std::string name;
  ShapeGeometry geom;
  int64_t rotation = 0;       // 1/60000 度
  int fill = -1;              // 纯色填充
  int line_color = -1;        // 边框颜色
  double line_width_pt = 0.0;
  std::string placeholder;    // "title" / "body" / "ctrTitle" / "subTitle"，空 = 普通形状
  int placeholder_idx = -1;   // p:ph/@idx，-1 表示源文件没写（母版/版式继承要用）
  TextBody text;
  Picture picture;
  Table table;

  bool is_placeholder() const noexcept { return !placeholder.empty(); }
};

// ---- 切换与动画（F-403）-------------------------------------------------
//
// 这两块在 OOXML 里是幻灯片根下的两个兄弟元素：`p:transition`（页面切换）和
// `p:timing`（动画时间线）。前者是扁平的小结构，可以完整建模；后者是一棵任意
// 深的时序树（每个效果都有一串关键帧行为节点），完整建模既不可能也没必要。
//
// 所以策略分两层：
//   * 切换：22 种标准类型 + 速度 + 自动换片 + 音效，全部建模、双向映射；
//   * 动画：只把"哪个形状、什么效果、什么时候、多长"提到语义层（这正好是
//     Aspose `Effect` 的粒度），关键帧细节留在 `timing_xml` 原文里。
// 没动过的幻灯片，两个元素都按原文字节写回；一旦调用了 SetAnimations /
// set_transition，原文被清掉，改由模型生成——与透视表的脏标记是同一套思路。

enum class TransitionType : uint8_t {
  None, Blinds, Checker, Circle, Comb, Cover, Cut, Diamond, Dissolve, Fade,
  Newsflash, Plus, Pull, Push, Random, RandomBar, Split, Strips, Wedge,
  Wheel, Wipe, Zoom
};

const char* TransitionTypeName(TransitionType type) noexcept;
TransitionType TransitionTypeFromName(std::string_view name);

enum class TransitionSpeed : uint8_t { Slow, Medium, Fast };

const char* TransitionSpeedName(TransitionSpeed speed) noexcept;
TransitionSpeed TransitionSpeedFromName(std::string_view name);

struct SlideTransition {
  TransitionType type = TransitionType::None;
  TransitionSpeed speed = TransitionSpeed::Medium;
  // 各类型的方向参数原样保存，不做枚举化：cover/pull/push/wipe 用 `dir`
  // （l/u/r/d/lu/ru/ld/rd），split 用 `orient`（horz/vert）+ `dir`（in/out），
  // wheel 用 `spokes`（1/2/3/4/8），blinds/checker/comb 只用 horz/vert。
  // 枚举化只会把"这个实现不认识的合法值"变成丢失。
  std::string direction;
  std::string orient;
  int spokes = 0;
  bool advance_on_click = true;
  int advance_after_ms = -1;  // -1 = 不自动换片
  std::string sound_name;     // p:sndAc 的嵌入音频名；空 = 无音效
  std::string sound_rid;      // p:sndAc/p:stSnd/p:snd r:embed

  bool present() const noexcept { return type != TransitionType::None; }
};

enum class AnimationKind : uint8_t { Entrance, Emphasis, Exit, MotionPath };
enum class AnimationTrigger : uint8_t { OnClick, WithPrevious, AfterPrevious };

// 一条动画效果。`effect` 是规范名（"fade"/"wipe"/"flyIn"/…），`filter` 是
// OOXML 的原始 filter 串（可能带参数，如 "wipe(up)"、"barn(inVertical)"）。
// 两者都留：前者给调用方判断，后者保证写回时不改语义。
struct SlideAnimation {
  int shape_id = 0;  // p:spTgt/@spid；0 = 目标未知
  AnimationKind kind = AnimationKind::Entrance;
  std::string effect = "fade";
  std::string filter = "fade";
  AnimationTrigger trigger = AnimationTrigger::OnClick;
  int duration_ms = 500;
  int delay_ms = 0;
  int repeat = 1;
  std::string path;  // 仅 MotionPath：p:animMotion/@path
};

const char* AnimationKindName(AnimationKind kind) noexcept;
const char* AnimationTriggerName(AnimationTrigger trigger) noexcept;

// ---- 幻灯片 -------------------------------------------------------------

class Slide {
 public:
  Slide() = default;
  explicit Slide(std::string layout_uri) : layout_uri_(std::move(layout_uri)) {}

  const std::string& layout_uri() const noexcept { return layout_uri_; }
  void set_layout_uri(std::string uri) { layout_uri_ = std::move(uri); }
  const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  bool hidden() const noexcept { return hidden_; }
  void set_hidden(bool hidden) { hidden_ = hidden; }
  int background() const noexcept { return background_; }
  void set_background(int rgb) { background_ = rgb; }
  const std::string& notes() const noexcept { return notes_; }
  std::string& notes() noexcept { return notes_; }

  // 页面切换。set_transition 会清掉读入时的原文，改由模型生成。
  const SlideTransition& transition() const noexcept { return transition_; }
  void set_transition(SlideTransition value);
  // 读入时无法映射到 TransitionType 的切换（例如 p14:flash 这类扩展），
  // 原文存在这里，写回时优先使用。
  const std::string& transition_xml() const noexcept { return transition_xml_; }
  void set_transition_xml(std::string xml) { transition_xml_ = std::move(xml); }

  const std::vector<SlideAnimation>& animations() const noexcept { return animations_; }
  void SetAnimations(std::vector<SlideAnimation> list);
  void ClearAnimations();
  // 读入时 <p:timing> 的原始字节（含 tnLst / bldLst）。没被 SetAnimations 覆盖
  // 就原样写回，保住关键帧与构建列表。
  const std::string& timing_xml() const noexcept { return timing_xml_; }
  void set_timing_xml(std::string xml) { timing_xml_ = std::move(xml); }

  std::vector<Shape>& shapes() noexcept { return shapes_; }
  const std::vector<Shape>& shapes() const noexcept { return shapes_; }

  // 按占位符类型找形状（"title"/"body"/...）。
  Shape* FindPlaceholder(std::string_view type);
  const Shape* FindPlaceholder(std::string_view type) const;
  // 追加一个占位符文本框（读母版/版式默认位置用）。
  Shape& AddPlaceholder(std::string type, ShapeGeometry geom, std::string prompt = {});

  std::string PlainText() const;

 private:
  std::string layout_uri_;
  std::string name_;
  std::string notes_;
  bool hidden_ = false;
  int background_ = -1;
  std::vector<Shape> shapes_;
  SlideTransition transition_;
  std::string transition_xml_;
  std::vector<SlideAnimation> animations_;
  std::string timing_xml_;
};

// ---- 演示文稿 -----------------------------------------------------------

struct UnknownPart {
  std::string uri;
  std::string mime;
  std::vector<uint8_t> bytes;
};

struct Presentation {
  // 16:9，与 PowerPoint 2013+ 默认一致。
  int64_t slide_width = 12192000;
  int64_t slide_height = 6858000;

  std::string default_font = "Calibri";
  double default_size_pt = 18.0;
  int default_text_color = 0x000000;
  int master_background = 0xFFFFFF;

  std::vector<std::unique_ptr<Slide>> slides;
  std::vector<UnknownPart> unknown_parts;  // 保存时原样写回

  static Presentation Create();

  int slide_count() const noexcept { return static_cast<int>(slides.size()); }
  Slide* SlideAt(int index);
  const Slide* SlideAt(int index) const;
  Slide& AddSlide();
  Slide& AddSlide(std::string layout_uri);
  void RemoveSlide(int index);

  // 纯文本导出（每页一段）。
  std::string ExtractText() const;

  Presentation Clone() const;
};

}  // namespace deck
}  // namespace doccore
