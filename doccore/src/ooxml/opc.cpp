#include "doccore/opc.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "doccore/xml.h"

namespace doccore {

namespace {

std::string ToLowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

std::string_view AsUtf8(const std::vector<uint8_t>& v) {
  return std::string_view(reinterpret_cast<const char*>(v.data()), v.size());
}

std::string_view Attr(const XmlEvent& ev, std::string_view name) {
  for (const auto& a : ev.attributes) {
    if (a.name == name) return a.value;
  }
  return {};
}

// Core of the URI normalization: split on '/', resolve "." and "..", keep a
// leading '/'. Rejects anything that climbs above the package root.
std::string NormalizeSegments(std::string_view path, bool* ok) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view raw = path.substr(start, end - start);
    if (raw.empty() || raw == ".") {
      // keep
    } else if (raw == "..") {
      if (out.empty()) {
        *ok = false;
        return {};
      }
      out.pop_back();
    } else {
      out.push_back(raw);
    }
    if (end == path.size()) break;
    start = end + 1;
  }
  *ok = true;
  std::string res;
  for (const auto& seg : out) {
    res.push_back('/');
    res.append(seg);
  }
  return res;
}

// "_rels/..." or ".../_rels/....rels": relationship parts are plumbing, not
// content, and are exposed through Part::relationships() instead.
bool IsRelsPart(std::string_view name) {
  if (name.size() >= 6 && name.compare(0, 6, "_rels/") == 0) return true;
  const size_t pos = name.find("/_rels/");
  if (pos == std::string_view::npos) return false;
  return name.size() >= 5 && name.compare(name.size() - 5, 5, ".rels") == 0;
}

}  // namespace

bool ResolvePartUri(std::string_view base_dir, std::string_view target, std::string* out,
                    std::string* reason) {
  auto fail = [&](const char* why) {
    if (reason != nullptr) *reason = why;
    return false;
  };
  if (target.empty()) return fail("empty target");
  if (target.find('\\') != std::string_view::npos) return fail("backslash in target");
  if (target.size() >= 2 && target[1] == ':' && target.find("://") == std::string_view::npos) {
    return fail("drive letter in target");
  }
  std::string combined;
  if (target[0] == '/') {
    combined.assign(target);
  } else {
    combined = std::string(base_dir);
    if (!combined.empty() && combined.back() != '/') combined.push_back('/');
    combined.append(target);
  }
  bool ok = false;
  std::string norm = NormalizeSegments(combined, &ok);
  if (!ok || norm.empty()) return fail("target escapes package root");
  *out = norm;
  return true;
}

std::string PartDirectory(std::string_view part_uri) {
  const size_t slash = part_uri.rfind('/');
  if (slash == std::string_view::npos) return {};
  if (slash == 0) return {};  // "/a.xml" lives at the package root
  return std::string(part_uri.substr(0, slash));
}

std::string RelsPathFor(std::string_view part_uri) {
  const std::string dir = PartDirectory(part_uri);
  const std::string name(part_uri.substr(dir.size() + 1));
  std::string out;
  out.append(dir);
  out.append("/_rels/");
  out.append(name);
  out.append(".rels");
  return out;
}

Relationships Relationships::Parse(std::string_view utf8, std::string_view base_dir,
                                   WarningSink* warnings) {
  Relationships rels;
  XmlReader xml(utf8);
  XmlEvent ev;
  while (xml.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (ev.name != "Relationship") continue;
    Relationship r;
    r.id = std::string(Attr(ev, "Id"));
    r.type = std::string(Attr(ev, "Type"));
    r.target = std::string(Attr(ev, "Target"));
    r.target_mode = std::string(Attr(ev, "TargetMode"));
    if (r.id.empty() || r.type.empty() || r.target.empty()) {
      if (warnings != nullptr) {
        Warning w;
        w.code = static_cast<int>(WarningCode::UnknownPartKept);
        w.module = "opc";
        w.message = "relationship with missing Id/Type/Target skipped";
        warnings->Add(w);
      }
      continue;
    }
    if (r.target_mode.empty()) r.target_mode = "Internal";
    if (r.external()) {
      r.resolved = r.target;
    } else {
      std::string resolved;
      std::string reason;
      if (!ResolvePartUri(base_dir, r.target, &resolved, &reason)) {
        if (warnings != nullptr) {
          Warning w;
          w.code = static_cast<int>(WarningCode::UnknownPartKept);
          w.module = "opc";
          w.message = "relationship target rejected (" + reason + "): " + r.target;
          warnings->Add(w);
        }
        continue;
      }
      r.resolved = std::move(resolved);
    }
    rels.items_.push_back(std::move(r));
  }
  return rels;
}

const Relationship* Relationships::Find(std::string_view id) const {
  const Relationship* found = nullptr;
  for (const auto& r : items_) {
    if (r.id == id) found = &r;  // later entries win
  }
  return found;
}

const Relationship* Relationships::FindByType(std::string_view type) const {
  for (const auto& r : items_) {
    if (r.type == type) return &r;
  }
  return nullptr;
}

std::vector<const Relationship*> Relationships::ByType(std::string_view type) const {
  std::vector<const Relationship*> out;
  for (const auto& r : items_) {
    if (r.type == type) out.push_back(&r);
  }
  return out;
}

ContentTypes ContentTypes::Parse(std::string_view utf8) {
  ContentTypes ct;
  XmlReader xml(utf8);
  XmlEvent ev;
  while (xml.Next(ev)) {
    if (ev.type != XmlEventType::StartElement) continue;
    if (ev.name == "Default") {
      const std::string_view ext = Attr(ev, "Extension");
      const std::string_view mime = Attr(ev, "ContentType");
      if (!ext.empty() && !mime.empty()) ct.SetDefault(ToLowerAscii(ext), std::string(mime));
    } else if (ev.name == "Override") {
      std::string_view part = Attr(ev, "PartName");
      const std::string_view mime = Attr(ev, "ContentType");
      if (part.empty() || mime.empty()) continue;
      if (part[0] != '/') {
        std::string with_slash;
        with_slash.push_back('/');
        with_slash.append(part);
        ct.SetOverride(ToLowerAscii(with_slash), std::string(mime));
      } else {
        ct.SetOverride(ToLowerAscii(part), std::string(mime));
      }
    }
  }
  return ct;
}

void ContentTypes::SetDefault(std::string ext, std::string mime) {
  for (auto& kv : defaults_) {
    if (kv.first == ext) {
      kv.second = std::move(mime);
      return;
    }
  }
  defaults_.emplace_back(std::move(ext), std::move(mime));
}

void ContentTypes::SetOverride(std::string part_name, std::string mime) {
  for (auto& kv : overrides_) {
    if (kv.first == part_name) {
      kv.second = std::move(mime);
      return;
    }
  }
  overrides_.emplace_back(std::move(part_name), std::move(mime));
}

std::string ContentTypes::For(std::string_view part_uri) const {
  std::string key = ToLowerAscii(part_uri);
  if (key.empty()) return "application/octet-stream";
  if (key[0] != '/') key.insert(key.begin(), '/');
  for (const auto& kv : overrides_) {
    if (kv.first == key) return kv.second;
  }
  const size_t dot = part_uri.rfind('.');
  if (dot == std::string_view::npos || dot + 1 >= part_uri.size()) {
    return "application/octet-stream";
  }
  const std::string ext = ToLowerAscii(part_uri.substr(dot + 1));
  for (const auto& kv : defaults_) {
    if (kv.first == ext) return kv.second;
  }
  return "application/octet-stream";
}

std::string Part::extension() const {
  const size_t dot = uri_.rfind('.');
  if (dot == std::string::npos) return {};
  return ToLowerAscii(uri_.substr(dot + 1));
}

const std::vector<uint8_t>& Part::content() {
  std::call_once(load_once_, [this] { Load(); });
  return bytes_;
}

const Relationships& Part::relationships() const {
  std::call_once(load_once_, [this] { const_cast<Part*>(this)->Load(); });
  return rels_;
}

void Part::Load() {
  const ZipEntry* e = owner_->zip_mutable().Find(uri_.substr(1));
  if (e == nullptr) {
    throw Error(ErrorCode::Corrupt, "opc: part not present in zip: " + uri_);
  }
  bytes_ = owner_->zip_mutable().Read(*e);
  const std::string rels_path = RelsPathFor(uri_);
  const ZipEntry* re = owner_->zip_mutable().Find(rels_path.substr(1));
  if (re != nullptr) {
    rels_ = Relationships::Parse(AsUtf8(owner_->zip_mutable().Read(*re)), PartDirectory(uri_),
                                 owner_->warnings());
  }
}

Package::~Package() = default;

std::unique_ptr<Package> Package::Open(Stream& in, WarningSink* warnings, const ZipLimits& limits) {
  auto pkg = std::unique_ptr<Package>(new Package());
  pkg->warnings_ = warnings;
  pkg->zip_ = std::make_unique<ZipArchive>(ZipArchive::Open(in, limits, warnings));

  const ZipEntry* ct_entry = pkg->zip_->Find("[Content_Types].xml");
  if (ct_entry == nullptr) {
    throw Error(ErrorCode::Corrupt, "opc: [Content_Types].xml missing");
  }
  pkg->content_types_ = ContentTypes::Parse(AsUtf8(pkg->zip_->Read(*ct_entry)));

  const ZipEntry* rels_entry = pkg->zip_->Find("_rels/.rels");
  if (rels_entry == nullptr) {
    throw Error(ErrorCode::Corrupt, "opc: _rels/.rels missing");
  }
  pkg->root_rels_ = Relationships::Parse(AsUtf8(pkg->zip_->Read(*rels_entry)), "", warnings);

  for (const auto& e : pkg->zip_->entries()) {
    if (e.name == "[Content_Types].xml") continue;
    if (IsRelsPart(e.name)) continue;
    if (!IsSafePartName(e.name)) continue;  // zip layer already rejects; belt & braces

    const std::string uri = "/" + e.name;
    auto part = std::make_unique<Part>(pkg.get(), uri, pkg->content_types_.For(uri));
    pkg->index_[ToLowerAscii(uri)] = pkg->parts_.size();
    pkg->parts_.push_back(std::move(part));
  }

  // Kind comes from the officeDocument root relationship's content type.
  constexpr const char* kOfficeDocument =
      "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
  const Relationship* main = pkg->root_rels_.FindByType(kOfficeDocument);
  if (main != nullptr && !main->external()) {
    Part* mp = pkg->Find(main->resolved);
    if (mp != nullptr) {
      pkg->main_part_ = mp;
      const std::string& mime = mp->mime();
      auto has = [&](const char* needle) { return mime.find(needle) != std::string::npos; };
      if (has("wordprocessingml")) {
        pkg->kind_ = PackageKind::Word;
      } else if (has("spreadsheetml")) {
        pkg->kind_ = PackageKind::Excel;
      } else if (has("presentationml")) {
        pkg->kind_ = PackageKind::PowerPoint;
      }
    }
  }
  return pkg;
}

Part* Package::main_part() { return main_part_; }

Part* Package::Find(std::string_view part_uri) {
  std::string key = ToLowerAscii(part_uri);
  if (key.empty()) return nullptr;
  if (key[0] != '/') key.insert(key.begin(), '/');
  auto it = index_.find(key);
  if (it == index_.end()) return nullptr;
  return parts_[it->second].get();
}

Part* Package::FindByMime(std::string_view mime_suffix) {
  for (auto& p : parts_) {
    const std::string& mime = p->mime();
    if (mime.size() >= mime_suffix.size() &&
        mime.compare(mime.size() - mime_suffix.size(), mime_suffix.size(), mime_suffix) == 0) {
      return p.get();
    }
  }
  return nullptr;
}

}  // namespace doccore
