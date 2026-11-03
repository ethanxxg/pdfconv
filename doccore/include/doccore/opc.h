// DocCore - OPC package model (ECMA-376 Part 2): content types, relationships
// and parts. Design refs: docs/02 §3.1.3.
//
// Model:
//   Package  = root relationships + set of parts
//   Part     = Part-URI (e.g. "/word/document.xml") + content type + content
//   Relationships = id / type / target, resolved to Part-URIs at open time
//
// Only reading is implemented in this slice. Parts the family decoder never
// touches stay in the package and are handed back as UnknownPart on save
// (docs/02 §3.1.3 item 6).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "doccore/core.h"
#include "doccore/stream.h"
#include "doccore/zip.h"

namespace doccore {

enum class PackageKind { Unknown, Word, Excel, PowerPoint };

// Normalizes a relationship target against the base part and returns a
// Part-URI (leading '/', no "." / ".." segments, forward slashes).
// Returns false when the result escapes the package.
bool ResolvePartUri(std::string_view base_dir, std::string_view target, std::string* out,
                    std::string* reason = nullptr);

// Directory of a Part-URI: "/word/document.xml" -> "/word", "/[a].xml" -> "".
std::string PartDirectory(std::string_view part_uri);

// Relationship-part path for a part: "/word/document.xml"
// -> "word/_rels/document.xml.rels".
std::string RelsPathFor(std::string_view part_uri);

struct Relationship {
  std::string id;
  std::string type;          // full relationship-type URI
  std::string target;        // raw Target attribute
  std::string resolved;      // Part-URI for internal targets, raw target otherwise
  std::string target_mode;   // "Internal" (default) or "External"

  bool external() const noexcept { return target_mode == "External"; }
};

class Relationships {
 public:
  static Relationships Parse(std::string_view utf8, std::string_view base_dir,
                             WarningSink* warnings = nullptr);

  const std::vector<Relationship>& items() const noexcept { return items_; }
  const Relationship* Find(std::string_view id) const;
  const Relationship* FindByType(std::string_view type) const;
  std::vector<const Relationship*> ByType(std::string_view type) const;

 private:
  std::vector<Relationship> items_;
};

// Content-type map from [Content_Types].xml: Default by extension, Override by
// part name. Lookup is case-insensitive on both.
class ContentTypes {
 public:
  static ContentTypes Parse(std::string_view utf8);

  void SetDefault(std::string ext, std::string mime);
  void SetOverride(std::string part_name, std::string mime);

  std::string For(std::string_view part_uri) const;

 private:
  std::vector<std::pair<std::string, std::string>> defaults_;   // ext -> mime
  std::vector<std::pair<std::string, std::string>> overrides_;  // part -> mime
};

class Package;

class Part {
 public:
  Part(Package* owner, std::string uri, std::string mime)
      : owner_(owner), uri_(std::move(uri)), mime_(std::move(mime)) {}

  // Part-URI with leading slash.
  const std::string& uri() const noexcept { return uri_; }
  const std::string& mime() const noexcept { return mime_; }
  std::string extension() const;                    // lower case, no dot
  const std::vector<uint8_t>& content();             // lazily read from the zip
  const Relationships& relationships() const;        // from _rels/<part>.rels
  bool external_content() const noexcept { return false; }

 private:
  friend class Package;
  void Load();

  Package* owner_;
  std::string uri_;
  std::string mime_;
  std::vector<uint8_t> bytes_;
  Relationships rels_;
  // Content and rels of one part are loaded together, exactly once. The flag is
  // mutable so the const relationships() accessor can trigger the load.
  mutable std::once_flag load_once_;
};

class Package {
 public:
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;

  // Throws Error: Corrupt (not a zip / no [Content_Types].xml / bad XML),
  // LimitExceeded (entry budgets), Io.
  static std::unique_ptr<Package> Open(Stream& in, WarningSink* warnings = nullptr,
                                       const ZipLimits& limits = {});

  PackageKind kind() const noexcept { return kind_; }
  const ZipArchive& zip() const noexcept { return *zip_; }
  ZipArchive& zip_mutable() noexcept { return *zip_; }
  WarningSink* warnings() const noexcept { return warnings_; }

  // Package-root relationships (_rels/.rels).
  const Relationships& root_relationships() const noexcept { return root_rels_; }
  // The part the officeDocument root relationship points at.
  Part* main_part();
  // Every part in the package, in zip order.
  const std::vector<std::unique_ptr<Part>>& parts() const noexcept { return parts_; }
  Part* Find(std::string_view part_uri);
  Part* FindByMime(std::string_view mime_suffix);  // e.g. ".main+xml"

 private:
  Package() = default;

  std::unique_ptr<ZipArchive> zip_;
  WarningSink* warnings_ = nullptr;
  ContentTypes content_types_;
  Relationships root_rels_;
  PackageKind kind_ = PackageKind::Unknown;
  std::vector<std::unique_ptr<Part>> parts_;
  std::map<std::string, size_t> index_;  // key: lower-cased Part-URI
  Part* main_part_ = nullptr;
};

}  // namespace doccore
