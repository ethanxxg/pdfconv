#include "doccore/text.h"

namespace doccore {
namespace text {

const Style* Document::FindStyle(std::string_view id) const {
  if (id.empty()) return nullptr;
  for (const auto& s : styles) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

const Style* Document::FindStyleByName(std::string_view name) const {
  if (name.empty()) return nullptr;
  for (const auto& s : styles) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

const AbstractNumbering* Document::FindAbstract(std::string_view id) const {
  for (const auto& a : abstract_numberings) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

const NumberingInstance* Document::FindNumbering(std::string_view id) const {
  for (const auto& n : numbering) {
    if (n.num_id == id) return &n;
  }
  return nullptr;
}

size_t Document::block_count() const {
  size_t total = 0;
  for (const auto& sec : sections) total += sec.blocks.size();
  return total;
}

}  // namespace text
}  // namespace doccore
