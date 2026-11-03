// DocCore - 内部辅助：把 XmlReader 包装成"按子树走"的事件流。
// 仅供 src/ooxml 下的族解析器使用（不属于公开头文件）。
#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/xml.h"

namespace doccore {
namespace ooxml {

// 显式维护开元素计数：XmlEvent::depth 对自闭合元素差一层，不能直接用。
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

// `start` 必须是已消费的 Start 事件。把它的每个直接子元素交给 on_child；
// 子处理器若不消费该子树，这里会跳过。返回时该元素的 End 已被消费。
template <class F>
void WalkChildren(XmlWalker& w, const XmlEvent& start, F on_child) {
  const int mine = w.open();
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) return;
    if (ev.type == XmlEventType::StartElement && w.open() == mine + 1) on_child(ev);
  }
  throw Error(ErrorCode::Corrupt, "ooxml: unterminated <" + std::string(start.name) + ">");
}

// 叶子元素的字符数据（消费整个元素）。
inline std::string ElemText(XmlWalker& w, const XmlEvent& start) {
  (void)start;  // 只用 w 的当前位置，参数保留是为了调用处可读
  const int mine = w.open();
  std::string out;
  XmlEvent ev;
  while (w.Next(ev)) {
    if (ev.type == XmlEventType::EndElement && w.open() == mine - 1) break;
    if (ev.type == XmlEventType::Text) out.append(ev.text);
  }
  return out;
}

inline std::string_view LocalName(std::string_view name) {
  const size_t pos = name.rfind(':');
  return pos == std::string_view::npos ? name : name.substr(pos + 1);
}

inline bool NameIs(const XmlEvent& ev, std::string_view local) {
  return LocalName(ev.name) == local;
}

inline const std::string_view* Attr(const XmlEvent& ev, std::string_view name) {
  for (const XmlAttribute& a : ev.attributes) {
    if (a.name == name) return &a.value;
  }
  return nullptr;
}

inline std::string AttrOr(const XmlEvent& ev, std::string_view name, const std::string& fallback) {
  const std::string_view* v = Attr(ev, name);
  return v == nullptr ? fallback : std::string(*v);
}

inline int AttrInt(const XmlEvent& ev, std::string_view name, int fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr || v->empty()) return fallback;
  return std::atoi(std::string(*v).c_str());
}

inline int64_t AttrInt64(const XmlEvent& ev, std::string_view name, int64_t fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr || v->empty()) return fallback;
  return std::strtoll(std::string(*v).c_str(), nullptr, 10);
}

inline double AttrDouble(const XmlEvent& ev, std::string_view name, double fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr || v->empty()) return fallback;
  return std::strtod(std::string(*v).c_str(), nullptr);
}

inline bool AttrBool(const XmlEvent& ev, std::string_view name, bool fallback) {
  const std::string_view* v = Attr(ev, name);
  if (v == nullptr) return fallback;
  return *v == "1" || *v == "true";
}

// "FF00FF00" / "00FF00" -> 0x00FF00；主题色等非 rgb 属性返回 -1。
inline int ParseRgb(const std::string_view* rgb) {
  if (rgb == nullptr || rgb->size() < 6) return -1;
  const std::string hex(rgb->substr(rgb->size() - 6));
  char* end = nullptr;
  const long value = std::strtol(hex.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') return -1;
  return static_cast<int>(value & 0xFFFFFF);
}

inline std::string AsUtf8(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace ooxml
}  // namespace doccore
