#include "doccore/xml.h"

namespace doccore {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

XmlReader::XmlReader(std::string_view utf8, XmlLimits limits) : src_(utf8), limits_(limits) {
  // Strip a UTF-8 BOM if present.
  if (src_.size() >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF &&
      static_cast<unsigned char>(src_[1]) == 0xBB && static_cast<unsigned char>(src_[2]) == 0xBF) {
    src_.remove_prefix(3);
  }
}

std::string XmlReader::DecodeEntities(std::string_view raw, bool attribute) const {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c != '&') {
      out.push_back(c);
      continue;
    }
    const size_t semi = raw.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      throw Error(ErrorCode::Corrupt, "xml: unterminated entity");
    }
    const std::string_view ent = raw.substr(i + 1, semi - i - 1);
    if (ent == "lt") {
      out.push_back('<');
    } else if (ent == "gt") {
      out.push_back('>');
    } else if (ent == "amp") {
      out.push_back('&');
    } else if (ent == "quot") {
      out.push_back('"');
    } else if (ent == "apos") {
      out.push_back('\'');
    } else if (!ent.empty() && ent[0] == '#') {
      uint32_t cp = 0;
      bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
      const size_t start = hex ? 2 : 1;
      if (start >= ent.size()) throw Error(ErrorCode::Corrupt, "xml: bad character reference");
      for (size_t k = start; k < ent.size(); ++k) {
        const char d = ent[k];
        uint32_t v = 0;
        if (d >= '0' && d <= '9') {
          v = static_cast<uint32_t>(d - '0');
        } else if (hex && d >= 'a' && d <= 'f') {
          v = static_cast<uint32_t>(d - 'a' + 10);
        } else if (hex && d >= 'A' && d <= 'F') {
          v = static_cast<uint32_t>(d - 'A' + 10);
        } else {
          throw Error(ErrorCode::Corrupt, "xml: bad character reference");
        }
        cp = cp * (hex ? 16u : 10u) + v;
        if (cp > 0x10FFFF) throw Error(ErrorCode::Corrupt, "xml: character reference out of range");
      }
      if (cp == 0) throw Error(ErrorCode::Corrupt, "xml: nul character reference");
      const std::string enc = U32ToUtf8(std::u32string(1, static_cast<char32_t>(cp)));
      out += enc;
    } else {
      // Only the five predefined entities are legal; anything else means the
      // document relies on a DTD, which we refuse (docs/01 F-010).
      throw Error(ErrorCode::Corrupt, "xml: unknown entity (DTD required)");
    }
    i = semi;
  }
  (void)attribute;
  return out;
}

void XmlReader::SkipMisc() {
  for (;;) {
    while (pos_ < src_.size() && IsSpace(src_[pos_])) ++pos_;
    if (pos_ + 1 >= src_.size()) return;
    if (src_[pos_] != '<') return;
    if (src_.compare(pos_, 4, "<!--") == 0) {
      const size_t end = src_.find("-->", pos_ + 4);
      if (end == std::string_view::npos) throw Error(ErrorCode::Corrupt, "xml: unterminated comment");
      pos_ = end + 3;
      continue;
    }
    if (src_.compare(pos_, 2, "<?") == 0) {
      const size_t end = src_.find("?>", pos_ + 2);
      if (end == std::string_view::npos) throw Error(ErrorCode::Corrupt, "xml: unterminated processing instruction");
      pos_ = end + 2;
      continue;
    }
    if (src_.compare(pos_, 9, "<!DOCTYPE") == 0) {
      // Refuse outright: no DTD, therefore no entity expansion and no XXE.
      throw Error(ErrorCode::Corrupt, "xml: DOCTYPE is not allowed");
    }
    return;
  }
}

bool XmlReader::ParseStartTag(XmlEvent& ev) {
  const size_t start = pos_;
  size_t p = pos_ + 1;
  size_t name_end = p;
  while (name_end < src_.size() && !IsSpace(src_[name_end]) && src_[name_end] != '>' && src_[name_end] != '/') {
    ++name_end;
  }
  if (name_end == p) throw Error(ErrorCode::Corrupt, "xml: empty element name");
  if (name_end - p > limits_.max_name) throw Error(ErrorCode::LimitExceeded, "xml: element name too long");

  ev.type = XmlEventType::StartElement;
  ev.name = src_.substr(p, name_end - p);
  ev.attributes.clear();
  attr_values_.clear();

  bool self_closing = false;
  p = name_end;
  for (;;) {
    while (p < src_.size() && IsSpace(src_[p])) ++p;
    if (p >= src_.size()) throw Error(ErrorCode::Corrupt, "xml: unterminated start tag");
    if (src_[p] == '>') {
      ++p;
      break;
    }
    if (src_[p] == '/') {
      if (p + 1 >= src_.size() || src_[p + 1] != '>') throw Error(ErrorCode::Corrupt, "xml: bad empty tag");
      p += 2;
      self_closing = true;
      break;
    }
    // attribute
    const size_t an = p;
    while (p < src_.size() && !IsSpace(src_[p]) && src_[p] != '=' && src_[p] != '>') ++p;
    if (p == an) throw Error(ErrorCode::Corrupt, "xml: bad attribute name");
    const std::string_view attr_name = src_.substr(an, p - an);
    while (p < src_.size() && IsSpace(src_[p])) ++p;
    if (p >= src_.size() || src_[p] != '=') throw Error(ErrorCode::Corrupt, "xml: attribute without value");
    ++p;
    while (p < src_.size() && IsSpace(src_[p])) ++p;
    if (p >= src_.size() || (src_[p] != '"' && src_[p] != '\'')) {
      throw Error(ErrorCode::Corrupt, "xml: unquoted attribute value");
    }
    const char quote = src_[p++];
    const size_t vs = p;
    while (p < src_.size() && src_[p] != quote) ++p;
    if (p >= src_.size()) throw Error(ErrorCode::Corrupt, "xml: unterminated attribute value");
    // Fast path: a value without '&' is a direct view into src_ (stable for the
    // reader's lifetime). Values that actually need entity decoding go through
    // attr_values_ (a deque: push_back never relocates existing elements, so
    // previously handed-out views stay valid).
    const std::string_view raw = src_.substr(vs, p - vs);
    std::string_view value;
    if (raw.find('&') == std::string_view::npos) {
      value = raw;
    } else {
      attr_values_.push_back(DecodeEntities(raw, true));
      value = attr_values_.back();
    }
    ++p;
    ev.attributes.push_back({attr_name, value});
  }

  pos_ = p;
  if (self_closing) {
    ev.depth = depth_ + 1;
    pending_end_ = true;
    pending_name_.assign(ev.name);
  } else {
    ++depth_;
    if (depth_ > limits_.max_depth) throw Error(ErrorCode::LimitExceeded, "xml: nesting too deep");
    ev.depth = depth_;
  }
  (void)start;
  return true;
}

bool XmlReader::ParseEndTag(XmlEvent& ev) {
  const size_t end = src_.find('>', pos_);
  if (end == std::string_view::npos) throw Error(ErrorCode::Corrupt, "xml: unterminated end tag");
  std::string_view name = src_.substr(pos_ + 2, end - pos_ - 2);
  while (!name.empty() && IsSpace(name.back())) name.remove_suffix(1);
  if (name.empty()) throw Error(ErrorCode::Corrupt, "xml: empty end tag");
  if (depth_ <= 0) throw Error(ErrorCode::Corrupt, "xml: unbalanced end tag");
  --depth_;
  ev.type = XmlEventType::EndElement;
  ev.name = name;
  ev.attributes.clear();
  ev.depth = depth_;
  pos_ = end + 1;
  return true;
}

bool XmlReader::Next(XmlEvent& ev) {
  ev.attributes.clear();
  if (pending_end_) {
    pending_end_ = false;
    ev.type = XmlEventType::EndElement;
    ev.name = pending_name_;
    ev.text = {};
    ev.depth = depth_;
    return true;
  }
  for (;;) {
    if (!started_) {
      started_ = true;
      SkipMisc();
    }
    if (pos_ >= src_.size()) {
      ev.type = XmlEventType::EndOfDocument;
      ev.name = {};
      ev.text = {};
      ev.depth = depth_;
      return false;
    }
    if (src_[pos_] == '<') {
      if (src_.compare(pos_, 2, "</") == 0) return ParseEndTag(ev);
      if (src_.compare(pos_, 9, "<![CDATA[") == 0) {
        const size_t end = src_.find("]]>", pos_ + 9);
        if (end == std::string_view::npos) throw Error(ErrorCode::Corrupt, "xml: unterminated CDATA");
        text_buf_.assign(src_.substr(pos_ + 9, end - pos_ - 9));
        pos_ = end + 3;
        ev.type = XmlEventType::Text;
        ev.text = text_buf_;
        ev.name = {};
        ev.depth = depth_;
        return true;
      }
      SkipMisc();
      if (pos_ >= src_.size()) continue;
      return ParseStartTag(ev);
    }
    // text run up to the next '<'
    const size_t start = pos_;
    size_t end = src_.find('<', pos_);
    if (end == std::string_view::npos) end = src_.size();
    const std::string_view raw = src_.substr(start, end - start);
    pos_ = end;
    text_buf_ = DecodeEntities(raw, false);
    if (text_buf_.size() > limits_.max_text_node) throw Error(ErrorCode::LimitExceeded, "xml: text node too large");
    text_bytes_ += text_buf_.size();
    if (text_bytes_ > limits_.max_total_text) throw Error(ErrorCode::LimitExceeded, "xml: total text too large");
    if (text_buf_.empty()) continue;  // whitespace-only or fully skipped
    ev.type = XmlEventType::Text;
    ev.text = text_buf_;
    ev.name = {};
    ev.depth = depth_;
    return true;
  }
}

}  // namespace doccore
