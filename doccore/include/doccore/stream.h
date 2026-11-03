// DocCore - byte streams. Design refs: docs/02 §3.1.2
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "doccore/core.h"

namespace doccore {

class Stream {
 public:
  virtual ~Stream() = default;
  // Returns bytes read; 0 at end of stream. Throws Error(Io) on failure.
  virtual int64_t Read(uint8_t* dst, int64_t bytes) = 0;
  virtual void Write(const uint8_t* src, int64_t bytes) = 0;
  virtual void Seek(int64_t pos) = 0;
  virtual int64_t Tell() const = 0;
  virtual int64_t Size() const = 0;
  // Default implementation throws Unsupported: only seekable sinks can roll back.
  virtual void Truncate(int64_t size);
};

class FileStream : public Stream {
 public:
  // mode: "rb" or "wb"
  FileStream(const std::string& path, const char* mode);
  ~FileStream() override;

  int64_t Read(uint8_t* dst, int64_t bytes) override;
  void Write(const uint8_t* src, int64_t bytes) override;
  void Seek(int64_t pos) override;
  int64_t Tell() const override;
  int64_t Size() const override;
  void Truncate(int64_t size) override;
  void Flush();

 private:
  std::FILE* fp_ = nullptr;
  std::string path_;
};

class MemoryStream : public Stream {
 public:
  MemoryStream() = default;
  explicit MemoryStream(std::vector<uint8_t> data) : data_(std::move(data)) {}

  int64_t Read(uint8_t* dst, int64_t bytes) override;
  void Write(const uint8_t* src, int64_t bytes) override;
  void Seek(int64_t pos) override;
  int64_t Tell() const override { return pos_; }
  int64_t Size() const override { return static_cast<int64_t>(data_.size()); }
  void Truncate(int64_t size) override;

  const std::vector<uint8_t>& data() const { return data_; }
  std::vector<uint8_t> TakeData() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
  int64_t pos_ = 0;
};

// Caps how many bytes may be read through it. Used so that a declared size in a
// hostile file can never cause a large allocation (docs/02 §3.1.2).
class LimitStream : public Stream {
 public:
  LimitStream(Stream& inner, int64_t budget) : inner_(inner), budget_(budget) {}

  int64_t Read(uint8_t* dst, int64_t bytes) override;
  void Write(const uint8_t* src, int64_t bytes) override { inner_.Write(src, bytes); }
  void Seek(int64_t pos) override { inner_.Seek(pos); }
  int64_t Tell() const override { return inner_.Tell(); }
  int64_t Size() const override { return inner_.Size(); }
  int64_t Consumed() const { return consumed_; }

 private:
  Stream& inner_;
  int64_t budget_;
  int64_t consumed_ = 0;
};

// Reads the whole stream, failing with LimitExceeded past `max_bytes`.
std::vector<uint8_t> ReadAll(Stream& in, int64_t max_bytes);

// Writes a file atomically: temp file in the same directory, then replace.
// docs/02 §8.3
void WriteFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes);

}  // namespace doccore
