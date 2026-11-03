// DocCore - pull XML reader. Design refs: docs/02 §3.1.4
// No DTD, no external entities, bounded depth/name/text sizes.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"

namespace doccore {

struct XmlLimits {
  int max_depth = 64;
  size_t max_name = 4096;
  uint64_t max_text_node = 64ull << 20;
  uint64_t max_total_text = 2ull << 30;
};

enum class XmlEventType { StartElement, EndElement, Text, EndOfDocument };

struct XmlAttribute {
  std::string_view name;
  std::string_view value;
};

struct XmlEvent {
  XmlEventType type = XmlEventType::EndOfDocument;
  std::string_view name;                    // element name
  std::string_view text;                    // decoded text for Text events
  std::vector<XmlAttribute> attributes;     // valid for StartElement
  int depth = 0;
};

class XmlReader {
 public:
  XmlReader(std::string_view utf8, XmlLimits limits = {});

  // Returns false when the document ends. Views stay valid until the next call.
  bool Next(XmlEvent& ev);

 private:
  void SkipMisc();
  bool ParseStartTag(XmlEvent& ev);
  bool ParseEndTag(XmlEvent& ev);
  std::string DecodeEntities(std::string_view raw, bool attribute) const;

  std::string_view src_;
  size_t pos_ = 0;
  XmlLimits limits_;
  int depth_ = 0;
  uint64_t text_bytes_ = 0;
  std::string text_buf_;
  // deque (not vector): push_back never relocates existing elements, so
  // views handed out for entity-decoded attribute values stay valid.
  std::deque<std::string> attr_values_;
  bool started_ = false;
  // A self-closing element emits Start then End, so consumers never have to
  // special-case empty elements.
  bool pending_end_ = false;
  std::string pending_name_;
};

}  // namespace doccore
