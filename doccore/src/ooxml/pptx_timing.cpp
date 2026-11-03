// DocCore - 幻灯片切换与动画（docs/02 §3.7）。
//
// 读：`p:transition` 完整建模；`p:timing` 只把"哪个形状、什么效果、什么时候、
// 多长"提到语义层，同时把原文整段留住——时间线里的关键帧（p:anim 的 attrName、
// tavLst、p:animMotion 的 path 编辑模式）建模既无必要也无收益，重新序列化只会
// 丢东西。写：原文优先，没有原文才按模型生成。
//
// 生成的时序树比 PowerPoint 自己写的浅一层（PowerPoint 在 seq 与效果之间多套一
// 层 p:par 做"点击组"的容器），但 CT_TimeNodeList 允许 p:par 直接出现，所以浅
// 结构同样是合法文档，PowerPoint 能正常播放。
#include "pptx_timing.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "xml_walk.h"

namespace doccore {
namespace ooxml {
namespace {

// ---- 输出小工具 ---------------------------------------------------------

std::string Esc(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string OpenTag(const char* name, const std::vector<std::pair<const char*, std::string>>& attrs) {
  std::string out = "<";
  out += name;
  for (const auto& kv : attrs) {
    if (kv.second.empty()) continue;
    out += ' ';
    out += kv.first;
    out += "=\"";
    out += Esc(kv.second);
    out += '"';
  }
  out += '>';
  return out;
}

std::string Leaf(const char* name,
                 const std::vector<std::pair<const char*, std::string>>& attrs = {}) {
  std::string out = OpenTag(name, attrs);
  out.insert(out.size() - 1, "/");
  return out;
}

// ---- 读：p:transition ---------------------------------------------------

// `p:sndAc` → 嵌入音频名与 rId。
void ReadSoundAction(XmlWalker& w, const XmlEvent& start, deck::SlideTransition* out) {
  WalkChildren(w, start, [&](const XmlEvent& st) {
    if (!NameIs(st, "stSnd")) return;
    WalkChildren(w, st, [&](const XmlEvent& snd) {
      if (!NameIs(snd, "snd")) return;
      out->sound_name = AttrOr(snd, "name", out->sound_name);
      // r:embed 带前缀，Attr 是按完整 qname 比的，两种写法都试一遍。
      std::string rid = AttrOr(snd, "r:embed", std::string());
      if (rid.empty()) rid = AttrOr(snd, "embed", std::string());
      out->sound_rid = rid;
    });
  });
}

void ParseTransition(XmlWalker& w, const XmlEvent& start, deck::Slide* slide,
                     std::vector<std::string>* unmapped) {
  deck::SlideTransition t;
  t.speed = deck::TransitionSpeedFromName(AttrOr(start, "spd", "med"));
  t.advance_on_click = AttrBool(start, "advClick", true);
  t.advance_after_ms = AttrInt(start, "advTm", -1);
  bool mapped = false;
  bool saw_unknown_type = false;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "sndAc") {
      ReadSoundAction(w, child, &t);
      return;
    }
    if (name == "extLst") return;  // 扩展容器，忽略
    const deck::TransitionType type = deck::TransitionTypeFromName(name);
    if (type == deck::TransitionType::None) {
      saw_unknown_type = true;
      return;
    }
    t.type = type;
    mapped = true;
    t.direction = AttrOr(child, "dir", std::string());
    t.orient = AttrOr(child, "orient", std::string());
    t.spokes = AttrInt(child, "spokes", 0);
  });
  if (mapped) {
    slide->set_transition(t);
  } else if (saw_unknown_type && unmapped != nullptr) {
    // 只告警、不保留原文：未知类型多半来自 p14/p15 扩展（例如 morph），把它
    // 原样搬到根下会引用未声明的命名空间前缀，反而把文件写坏。
    unmapped->push_back("transition type not modelled");
  }
}

// ---- 读：p:timing -------------------------------------------------------

// `p:cond/@delay` 有两种取值：整数毫秒，或 "indefinite"（等点击）。
int ParseDelay(std::string_view text, bool* indefinite) {
  if (indefinite != nullptr) *indefinite = false;
  if (text == "indefinite") {
    if (indefinite != nullptr) *indefinite = true;
    return -1;
  }
  return std::atoi(std::string(text).c_str());
}

int ParseDuration(std::string_view text) {
  if (text.empty() || text == "indefinite") return 0;
  return std::atoi(std::string(text).c_str());
}

struct TimingCtx {
  int delay_ms = 0;
  int duration_ms = 0;
  int shape_id = 0;
  deck::AnimationTrigger trigger = deck::AnimationTrigger::OnClick;
  std::string node_type;
};

// filter 串 → 规范效果名。filter 可能带参数（"wipe(up)"），取括号前那一段。
std::string EffectFromFilter(const std::string& filter) {
  const size_t paren = filter.find('(');
  const std::string base = paren == std::string::npos ? filter : filter.substr(0, paren);
  if (base == "fade") return "fade";
  if (base == "wipe") return "wipe";
  if (base == "slide" || base == "fly") return "flyIn";
  if (base == "zoom") return "zoom";
  if (base == "split") return "split";
  if (base == "blinds") return "blinds";
  if (base == "dissolve") return "dissolve";
  if (base == "wheel") return "wheel";
  if (base == "checkerboard" || base == "checker") return "checker";
  if (base == "barn") return "barn";
  if (base == "circle") return "circle";
  if (base == "diamond") return "diamond";
  if (base == "plus") return "plus";
  if (base == "strips") return "strips";
  if (base == "wedge") return "wedge";
  if (base == "randombar" || base == "randomBar") return "randomBar";
  if (base == "stretch") return "stretch";
  if (base == "box") return "box";
  return base;
}

void WalkTiming(XmlWalker& w, const XmlEvent& el, TimingCtx ctx,
                std::vector<deck::SlideAnimation>* out);

// `p:cTn`：先取自身属性，再按文档顺序走子节点（stCondLst 在 childTnLst 之前，
// 所以 delay 能在进入子树前就位）。
void WalkTimeNode(XmlWalker& w, const XmlEvent& ctn, TimingCtx ctx,
                  std::vector<deck::SlideAnimation>* out) {
  TimingCtx self = ctx;
  self.duration_ms = ParseDuration(AttrOr(ctn, "dur", std::string()));
  const std::string node_type = AttrOr(ctn, "nodeType", std::string());
  if (!node_type.empty()) self.node_type = node_type;
  if (node_type == "clickEffect" || node_type == "clickPar") {
    self.trigger = deck::AnimationTrigger::OnClick;
  } else if (node_type == "withEffect" || node_type == "withGroup") {
    self.trigger = deck::AnimationTrigger::WithPrevious;
  } else if (node_type == "afterEffect" || node_type == "afterGroup") {
    self.trigger = deck::AnimationTrigger::AfterPrevious;
  }
  WalkChildren(w, ctn, [&](const XmlEvent& child) {
    const std::string_view name = LocalName(child.name);
    if (name == "stCondLst") {
      WalkChildren(w, child, [&](const XmlEvent& cond) {
        if (!NameIs(cond, "cond")) return;
        bool indefinite = false;
        const int delay = ParseDelay(AttrOr(cond, "delay", std::string()), &indefinite);
        self.delay_ms = delay < 0 ? 0 : delay;
      });
      return;
    }
    WalkTiming(w, child, self, out);
  });
}

// 取 p:cBhvr/p:cTn/@dur 与 p:cBhvr/p:tgtEl/p:spTgt/@spid —— 行为节点的时长和
// 目标形状。两者都在子树里，所以必须先扫完再发条目（扫一次即消费整棵子树）。
struct BehaviorInfo {
  int shape_id = 0;
  int duration_ms = 0;
};

BehaviorInfo ScanBehavior(XmlWalker& w, const XmlEvent& behavior) {
  BehaviorInfo info;
  WalkChildren(w, behavior, [&](const XmlEvent& child) {
    if (!NameIs(child, "cBhvr")) return;
    WalkChildren(w, child, [&](const XmlEvent& c) {
      const std::string_view n = LocalName(c.name);
      if (n == "cTn") {
        const int v = ParseDuration(AttrOr(c, "dur", std::string()));
        if (v > 0) info.duration_ms = v;
        return;
      }
      if (n == "tgtEl") {
        WalkChildren(w, c, [&](const XmlEvent& t) {
          if (NameIs(t, "spTgt")) info.shape_id = AttrInt(t, "spid", info.shape_id);
        });
      }
    });
  });
  return info;
}

void WalkTiming(XmlWalker& w, const XmlEvent& el, TimingCtx ctx,
                std::vector<deck::SlideAnimation>* out) {
  const std::string_view name = LocalName(el.name);
  if (name == "cTn") {
    WalkTimeNode(w, el, ctx, out);
    return;
  }
  if (name == "animEffect" || name == "animMotion") {
    const BehaviorInfo info = ScanBehavior(w, el);
    deck::SlideAnimation a;
    a.shape_id = info.shape_id > 0 ? info.shape_id : ctx.shape_id;
    a.trigger = ctx.trigger;
    a.delay_ms = ctx.delay_ms;
    a.duration_ms = ctx.duration_ms > 0 ? ctx.duration_ms : info.duration_ms;
    if (a.duration_ms <= 0) a.duration_ms = 500;
    if (name == "animMotion") {
      a.kind = deck::AnimationKind::MotionPath;
      a.effect = "motionPath";
      a.filter = "motion";
      a.path = AttrOr(el, "path", std::string());
    } else {
      a.filter = AttrOr(el, "filter", "fade");
      const std::string transition = AttrOr(el, "transition", std::string());
      if (transition == "in") {
        a.kind = deck::AnimationKind::Entrance;
      } else if (transition == "out") {
        a.kind = deck::AnimationKind::Exit;
      } else {
        a.kind = deck::AnimationKind::Emphasis;
      }
      a.effect = EffectFromFilter(a.filter);
    }
    out->push_back(std::move(a));
    return;
  }
  // p:set（入场前的可见性置位）、p:anim/p:animRot/p:animScale 等关键帧行为不
  // 单独出条目——它们已经隐含在对应的 animEffect 里，原文又整段保留着。
  WalkChildren(w, el, [&](const XmlEvent& child) { WalkTiming(w, child, ctx, out); });
}

void ParseTiming(XmlWalker& w, const XmlEvent& start, deck::Slide* slide) {
  std::vector<deck::SlideAnimation> list;
  TimingCtx ctx;
  WalkChildren(w, start, [&](const XmlEvent& child) {
    if (NameIs(child, "tnLst")) {
      WalkChildren(w, child, [&](const XmlEvent& node) { WalkTiming(w, node, ctx, &list); });
    }
  });
  if (!list.empty()) slide->SetAnimations(std::move(list));
}

// 把某个元素的原始字节从整份 XML 里抠出来（含起止标签）。找不到返回空串。
//
// 需要它是因为 XmlWalker 不暴露偏移量，而 <p:timing> 里满是没建模的属性与关键帧，
// 重新序列化必然丢东西。前缀安全性由调用方判断。
std::string ExtractElementRaw(const std::string& xml, std::string_view local) {
  const std::string needle = ":" + std::string(local);
  size_t pos = 0;
  while (true) {
    pos = xml.find(needle, pos);
    if (pos == std::string::npos) return std::string();
    // 往前找 '<'，确认紧邻的是元素名（而不是属性值里恰好出现的同名字样）。
    const size_t lt = xml.rfind('<', pos);
    if (lt == std::string::npos) return std::string();
    if (xml.find('>', lt) < pos) {  // 中间夹了 '>'，说明是属性值
      pos += needle.size();
      continue;
    }
    const size_t name_start = lt + 1;
    if (xml[name_start] == '/') {  // 结束标签，继续找下一个
      pos += needle.size();
      continue;
    }
    const std::string qname = xml.substr(name_start, pos + needle.size() - name_start);
    const size_t open_end = xml.find('>', pos);
    if (open_end == std::string::npos) return std::string();
    if (xml[open_end - 1] == '/') return xml.substr(lt, open_end - lt + 1);
    int depth = 1;
    size_t scan = open_end + 1;
    while (scan < xml.size() && depth > 0) {
      const size_t next = xml.find('<', scan);
      if (next == std::string::npos) return std::string();
      if (xml.compare(next + 1, qname.size(), qname) == 0) {
        const char after = next + 1 + qname.size() < xml.size() ? xml[next + 1 + qname.size()] : '\0';
        if (after == '>') {
          ++depth;  // 同名的开始标签
          scan = next + 2 + qname.size();
          continue;
        }
        if (after == ' ' || after == '\t' || after == '\n' || after == '\r') {
          // 可能自闭合
          const size_t e = xml.find('>', next);
          if (e == std::string::npos) return std::string();
          if (xml[e - 1] != '/') ++depth;
          scan = e + 1;
          continue;
        }
      }
      if (xml.compare(next + 1, qname.size() + 1, "/" + qname) == 0) {
        --depth;
        // next 指向 '<'，next+1 是 '/'，qname 从 next+2 起，结束标签的 '>' 在
        // next+2+qname.size()。scan 要停在 '>' 之后一位，才能把 '>' 也切进去。
        scan = next + 3 + qname.size();
        if (depth == 0) return xml.substr(lt, scan - lt);
        continue;
      }
      scan = next + 1;
    }
    return std::string();
  }
}

// 原文能否安全地搬到幻灯片根下重发：只能出现根上已声明的前缀。
bool PrefixSafe(const std::string& raw) {
  for (size_t i = 0; i + 1 < raw.size(); ++i) {
    if (raw[i] != '<' && raw[i] != ' ' && raw[i] != '\t') continue;
    size_t j = i + 1;
    if (raw[j] == '/') ++j;
    const size_t colon = raw.find(':', j);
    if (colon == std::string::npos) continue;
    // 前缀必须是字母开头、且到冒号之间只有字母数字。
    if (colon == j) continue;
    std::string prefix = raw.substr(j, colon - j);
    if (!std::isalpha(static_cast<unsigned char>(prefix[0]))) continue;
    bool ok = true;
    for (char c : prefix) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') ok = false;
    }
    if (!ok) continue;
    if (prefix == "p" || prefix == "a" || prefix == "r") continue;
    return false;
  }
  return true;
}

// ---- 写：p:transition ---------------------------------------------------

// filter/effect → PowerPoint 的 presetID 与 presetSubtype。presetID 只影响
// PowerPoint 里显示的效果名，真正的视觉由 p:animEffect/@filter 决定，所以映射
// 不到时给一个通用值即可，不会改变播放行为。
void PresetFor(deck::AnimationKind kind, const std::string& effect, int* id, int* subtype) {
  *subtype = 0;
  if (effect == "appear") { *id = 1; return; }
  if (effect == "flyIn") { *id = 2; return; }
  if (effect == "blinds") { *id = 3; return; }
  if (effect == "box") { *id = 4; return; }
  if (effect == "checker") { *id = 5; return; }
  if (effect == "circle") { *id = 7; return; }
  if (effect == "diamond") { *id = 8; return; }
  if (effect == "dissolve") { *id = 9; return; }
  if (effect == "fade") { *id = 10; return; }
  if (effect == "plus") { *id = 12; return; }
  if (effect == "split") { *id = 13; return; }
  if (effect == "strips") { *id = 18; return; }
  if (effect == "wedge") { *id = 19; return; }
  if (effect == "randomBar") { *id = 20; return; }
  if (effect == "wheel") { *id = 21; return; }
  if (effect == "wipe") { *id = 22; return; }
  if (effect == "zoom") { *id = 23; return; }
  if (effect == "stretch") { *id = 24; return; }
  if (effect == "barn") { *id = 4; return; }
  *id = kind == deck::AnimationKind::Emphasis ? 26 : 10;
}

const char* PresetClass(deck::AnimationKind kind) {
  switch (kind) {
    case deck::AnimationKind::Entrance: return "entr";
    case deck::AnimationKind::Emphasis: return "emph";
    case deck::AnimationKind::Exit: return "exit";
    case deck::AnimationKind::MotionPath: return "path";
  }
  return "entr";
}

const char* NodeTypeFor(deck::AnimationTrigger trigger) {
  switch (trigger) {
    case deck::AnimationTrigger::OnClick: return "clickEffect";
    case deck::AnimationTrigger::WithPrevious: return "withEffect";
    case deck::AnimationTrigger::AfterPrevious: return "afterEffect";
  }
  return "clickEffect";
}

// 一条效果的行为子树：入场/出场是 p:set（置可见）+ p:animEffect，强调只有
// p:animEffect，动作路径是 p:animMotion。
std::string BehaviorXml(const deck::SlideAnimation& a, int* next_id) {
  const std::string spid = std::to_string(a.shape_id);
  const std::string dur = std::to_string(a.duration_ms);
  std::string out;
  if (a.kind == deck::AnimationKind::MotionPath) {
    const std::string path = a.path.empty() ? "M 0 0 L 0.25 0" : a.path;
    out += OpenTag("p:animMotion",
                   {{"origin", "layout"},
                    {"path", path},
                    {"pathEditMode", "relative"},
                    {"rAng", "0"},
                    {"ptsTypes", ""}});
    out += OpenTag("p:cBhvr", {});
    out += Leaf("p:cTn", {{"id", std::to_string((*next_id)++)}, {"dur", dur}, {"fill", "hold"}});
    out += OpenTag("p:tgtEl", {});
    out += Leaf("p:spTgt", {{"spid", spid}});
    out += "</p:tgtEl>";
    out += "</p:cBhvr>";
    out += "</p:animMotion>";
    return out;
  }
  const bool visibility = a.kind == deck::AnimationKind::Entrance ||
                          a.kind == deck::AnimationKind::Exit;
  if (visibility) {
    out += OpenTag("p:set", {});
    out += OpenTag("p:cBhvr", {});
    out += Leaf("p:cTn", {{"id", std::to_string((*next_id)++)}, {"dur", "1"}, {"fill", "hold"}});
    out += OpenTag("p:tgtEl", {});
    out += Leaf("p:spTgt", {{"spid", spid}});
    out += "</p:tgtEl>";
    out += OpenTag("p:attrNameLst", {});
    out += "<p:attrName>style.visibility</p:attrName>";
    out += "</p:attrNameLst>";
    out += "</p:cBhvr>";
    out += OpenTag("p:to", {});
    out += Leaf("p:strVal", {{"val", a.kind == deck::AnimationKind::Entrance ? "visible" : "hidden"}});
    out += "</p:to>";
    out += "</p:set>";
  }
  out += OpenTag("p:animEffect",
                 {{"transition", a.kind == deck::AnimationKind::Entrance ? "in"
                                 : a.kind == deck::AnimationKind::Exit     ? "out"
                                                                           : ""},
                  {"filter", a.filter.empty() ? a.effect : a.filter}});
  out += OpenTag("p:cBhvr", {});
  out += Leaf("p:cTn", {{"id", std::to_string((*next_id)++)}, {"dur", dur}});
  out += OpenTag("p:tgtEl", {});
  out += Leaf("p:spTgt", {{"spid", spid}});
  out += "</p:tgtEl>";
  out += "</p:cBhvr>";
  out += "</p:animEffect>";
  return out;
}

}  // namespace

// ---- 对外接口 -----------------------------------------------------------

void ReadTransitionAndTiming(const std::string& xml, deck::Slide* slide,
                             std::vector<std::string>* unmapped) {
  if (slide == nullptr) return;
  XmlWalker w(xml);
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    const std::string_view name = LocalName(ev.name);
    if (name == "transition") {
      ParseTransition(w, ev, slide, unmapped);
    } else if (name == "timing") {
      ParseTiming(w, ev, slide);
      const std::string raw = ExtractElementRaw(xml, "timing");
      if (!raw.empty() && PrefixSafe(raw)) slide->set_timing_xml(raw);
    }
  }
}

std::string TransitionXml(const deck::Slide& slide) {
  if (!slide.transition_xml().empty()) return slide.transition_xml();
  const deck::SlideTransition& t = slide.transition();
  if (!t.present()) return std::string();

  std::vector<std::pair<const char*, std::string>> attrs;
  attrs.emplace_back("spd", deck::TransitionSpeedName(t.speed));
  if (!t.advance_on_click) attrs.emplace_back("advClick", "0");
  if (t.advance_after_ms >= 0) attrs.emplace_back("advTm", std::to_string(t.advance_after_ms));

  std::string out = OpenTag("p:transition", attrs);
  // CT_SlideTransition 的 sequence 是 sndAc 在前、切换类型在后。
  if (!t.sound_name.empty() || !t.sound_rid.empty()) {
    out += OpenTag("p:sndAc", {});
    out += OpenTag("p:stSnd", {});
    std::vector<std::pair<const char*, std::string>> snd;
    if (!t.sound_rid.empty()) snd.emplace_back("r:embed", t.sound_rid);
    if (!t.sound_name.empty()) snd.emplace_back("name", t.sound_name);
    out += Leaf("p:snd", snd);
    out += "</p:stSnd>";
    out += "</p:sndAc>";
  }
  const char* type = deck::TransitionTypeName(t.type);
  std::vector<std::pair<const char*, std::string>> type_attrs;
  if (!t.direction.empty()) type_attrs.emplace_back("dir", t.direction);
  if (!t.orient.empty()) type_attrs.emplace_back("orient", t.orient);
  if (t.spokes > 0) type_attrs.emplace_back("spokes", std::to_string(t.spokes));
  std::string tag = "p:";
  tag += type;
  out += Leaf(tag.c_str(), type_attrs);
  out += "</p:transition>";
  return out;
}

std::string TimingXml(const deck::Slide& slide) {
  if (!slide.timing_xml().empty()) return slide.timing_xml();
  const std::vector<deck::SlideAnimation>& list = slide.animations();
  if (list.empty()) return std::string();

  // id 只需在整页内唯一。1 留给 tmRoot、2 留给 mainSeq，效果从 3 起编号——
  // 和 PowerPoint 自己的输出习惯一致，便于人工比对。
  int next_id = 3;
  std::string groups;   // 各点击组
  std::string build;    // bldLst
  std::vector<int> built;
  // 连续的同组触发归到一个 p:par 里：OnClick 必开新组，WithPrevious 并进当前组，
  // AfterPrevious 开一个"自动往下走"的新组。
  int group_open = -1;  // 当前组的触发类型，-1 = 还没开组
  for (size_t i = 0; i < list.size(); ++i) {
    const deck::SlideAnimation& a = list[i];
    const bool start_group =
        group_open < 0 || a.trigger == deck::AnimationTrigger::OnClick ||
        a.trigger == deck::AnimationTrigger::AfterPrevious;
    if (start_group) {
      if (group_open >= 0) {
        groups += "</p:childTnLst></p:cTn></p:par>";  // 收掉上一个组
      }
      const bool click = a.trigger == deck::AnimationTrigger::OnClick;
      groups += OpenTag("p:par", {});
      groups += OpenTag("p:cTn", {{"id", std::to_string(next_id++)}, {"fill", "hold"}});
      groups += OpenTag("p:stCondLst", {});
      groups += Leaf("p:cond", {{"delay", click ? "indefinite" : "0"}});
      groups += "</p:stCondLst>";
      groups += OpenTag("p:childTnLst", {});
      group_open = static_cast<int>(a.trigger);
    }
    int preset_id = 10;
    int preset_subtype = 0;
    PresetFor(a.kind, a.effect, &preset_id, &preset_subtype);
    groups += OpenTag("p:par", {});
    groups += OpenTag("p:cTn",
                      {{"id", std::to_string(next_id++)},
                       {"presetID", std::to_string(preset_id)},
                       {"presetClass", PresetClass(a.kind)},
                       {"presetSubtype", std::to_string(preset_subtype)},
                       {"fill", "hold"},
                       {"grpId", "0"},
                       {"nodeType", NodeTypeFor(a.trigger)}});
    groups += OpenTag("p:stCondLst", {});
    groups += Leaf("p:cond", {{"delay", std::to_string(std::max(0, a.delay_ms))}});
    groups += "</p:stCondLst>";
    groups += OpenTag("p:childTnLst", {});
    groups += BehaviorXml(a, &next_id);
    groups += "</p:childTnLst></p:cTn></p:par>";
    if (std::find(built.begin(), built.end(), a.shape_id) == built.end()) {
      built.push_back(a.shape_id);
    }
  }
  if (group_open >= 0) groups += "</p:childTnLst></p:cTn></p:par>";

  for (int spid : built) {
    build += Leaf("p:bldP", {{"spid", std::to_string(spid)}, {"grpId", "0"}});
  }

  std::string out;
  out += OpenTag("p:timing", {});
  out += OpenTag("p:tnLst", {});
  out += OpenTag("p:par", {});
  out += OpenTag("p:cTn", {{"id", "1"},
                           {"dur", "indefinite"},
                           {"restart", "never"},
                           {"nodeType", "tmRoot"}});
  out += OpenTag("p:childTnLst", {});
  out += OpenTag("p:seq", {{"concurrent", "1"}, {"nextAc", "seek"}});
  out += OpenTag("p:cTn", {{"id", "2"},
                           {"dur", "indefinite"},
                           {"nodeType", "mainSeq"}});
  out += OpenTag("p:childTnLst", {});
  out += groups;
  out += "</p:childTnLst></p:cTn>";
  // 上一项 / 下一项的动作来源：点击换页。
  out += OpenTag("p:prevCondLst", {});
  out += OpenTag("p:cond", {{"evt", "onPrev"}, {"delay", "0"}});
  out += OpenTag("p:tgtEl", {});
  out += Leaf("p:sldTgt");
  out += "</p:tgtEl></p:cond></p:prevCondLst>";
  out += OpenTag("p:nextCondLst", {});
  out += OpenTag("p:cond", {{"evt", "onNext"}, {"delay", "0"}});
  out += OpenTag("p:tgtEl", {});
  out += Leaf("p:sldTgt");
  out += "</p:tgtEl></p:cond></p:nextCondLst>";
  out += "</p:seq></p:childTnLst></p:cTn></p:par></p:tnLst>";
  out += OpenTag("p:bldLst", {});
  out += build;
  out += "</p:bldLst>";
  out += "</p:timing>";
  return out;
}

}  // namespace ooxml
}  // namespace doccore
