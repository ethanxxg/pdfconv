#include "doccore/stream.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace doccore {

void Stream::Truncate(int64_t) {
  throw Error(ErrorCode::Unsupported, "stream does not support truncate");
}

FileStream::FileStream(const std::string& path, const char* mode) : path_(path) {
  if (fopen_s(&fp_, path.c_str(), mode) != 0) fp_ = nullptr;
  if (fp_ == nullptr) {
    throw Error(ErrorCode::Io, std::string("cannot open file: ") + std::strerror(errno), path);
  }
}

FileStream::~FileStream() {
  if (fp_ != nullptr) std::fclose(fp_);
}

int64_t FileStream::Read(uint8_t* dst, int64_t bytes) {
  if (bytes <= 0) return 0;
  const size_t got = std::fread(dst, 1, static_cast<size_t>(bytes), fp_);
  if (got == 0 && std::ferror(fp_)) throw Error(ErrorCode::Io, "read failed", path_);
  return static_cast<int64_t>(got);
}

void FileStream::Write(const uint8_t* src, int64_t bytes) {
  if (bytes <= 0) return;
  if (std::fwrite(src, 1, static_cast<size_t>(bytes), fp_) != static_cast<size_t>(bytes)) {
    throw Error(ErrorCode::Io, "write failed", path_);
  }
}

void FileStream::Seek(int64_t pos) {
  if (fseek(fp_, static_cast<long>(pos), SEEK_SET) != 0) {
    throw Error(ErrorCode::Io, "seek failed", path_);
  }
}

int64_t FileStream::Tell() const { return static_cast<int64_t>(ftell(fp_)); }

int64_t FileStream::Size() const {
  const long cur = ftell(fp_);
  fseek(fp_, 0, SEEK_END);
  const long end = ftell(fp_);
  fseek(fp_, cur, SEEK_SET);
  return static_cast<int64_t>(end);
}

void FileStream::Truncate(int64_t size) {
  Flush();
  if (_chsize_s(_fileno(fp_), static_cast<__int64>(size)) != 0) {
    throw Error(ErrorCode::Io, "truncate failed", path_);
  }
}

void FileStream::Flush() {
  if (std::fflush(fp_) != 0) throw Error(ErrorCode::Io, "flush failed", path_);
}

int64_t MemoryStream::Read(uint8_t* dst, int64_t bytes) {
  const int64_t avail = static_cast<int64_t>(data_.size()) - pos_;
  if (avail <= 0 || bytes <= 0) return 0;
  const int64_t n = bytes < avail ? bytes : avail;
  std::memcpy(dst, data_.data() + pos_, static_cast<size_t>(n));
  pos_ += n;
  return n;
}

void MemoryStream::Write(const uint8_t* src, int64_t bytes) {
  if (bytes <= 0) return;
  const size_t end = static_cast<size_t>(pos_ + bytes);
  if (end > data_.size()) data_.resize(end);
  std::memcpy(data_.data() + pos_, src, static_cast<size_t>(bytes));
  pos_ = static_cast<int64_t>(end);
}

void MemoryStream::Seek(int64_t pos) {
  if (pos < 0) throw Error(ErrorCode::InvalidArgument, "negative seek");
  pos_ = pos;
}

void MemoryStream::Truncate(int64_t size) {
  if (size < 0) throw Error(ErrorCode::InvalidArgument, "negative truncate");
  data_.resize(static_cast<size_t>(size));
  if (pos_ > size) pos_ = size;
}

int64_t LimitStream::Read(uint8_t* dst, int64_t bytes) {
  if (bytes <= 0) return 0;
  if (consumed_ >= budget_) {
    throw Error(ErrorCode::LimitExceeded, "read budget exhausted");
  }
  const int64_t left = budget_ - consumed_;
  const int64_t n = inner_.Read(dst, bytes < left ? bytes : left);
  consumed_ += n;
  return n;
}

std::vector<uint8_t> ReadAll(Stream& in, int64_t max_bytes) {
  std::vector<uint8_t> out;
  in.Seek(0);
  uint8_t buf[64 * 1024];
  for (;;) {
    const int64_t n = in.Read(buf, sizeof(buf));
    if (n <= 0) break;
    if (static_cast<int64_t>(out.size()) + n > max_bytes) {
      throw Error(ErrorCode::LimitExceeded, "stream exceeds limit");
    }
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

void WriteFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes) {
  const std::string tmp = path + ".tmp";
  {
    FileStream out(tmp, "wb");
    if (!bytes.empty()) out.Write(bytes.data(), static_cast<int64_t>(bytes.size()));
    out.Flush();
  }
#ifdef _WIN32
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::remove(tmp.c_str());
    throw Error(ErrorCode::Io, "cannot replace target file", path);
  }
#else
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw Error(ErrorCode::Io, "cannot replace target file", path);
  }
#endif
}

}  // namespace doccore
