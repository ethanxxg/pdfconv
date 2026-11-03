// DocCore - DOCX 读取入口。Design refs: docs/02 §3.3.1 / §3.3.3
//
//   text::Document doc = text::Open({.path = "in.docx"});
//
// 只读切片：未知部件原样进入 doc.unknown_parts（保存时写回，见 §3.1.3 第 6 条）。
#pragma once

#include <string>
#include <string_view>

#include "doccore/core.h"
#include "doccore/stream.h"
#include "doccore/text.h"

namespace doccore {
namespace text {

struct OpenOptions {
  std::string path;      // 文件路径（与 memory 二选一）
  CancelToken* cancel = nullptr;
  WarningSink* warnings = nullptr;
};

// 按内容探测打开：ZIP + wordprocessingml 内容类型 → DOCX 族。
// 抛 Error：Io（打不开）/ Corrupt（非 OOXML）/ Unsupported（不是 Word 族）。
Document Open(const OpenOptions& opts);

// 从内存打开（包小于 32 MB 时内部不落盘，docs/02 §3.1.2）。
Document OpenMemory(std::string_view bytes, WarningSink* warnings = nullptr,
                    CancelToken* cancel = nullptr);

struct SaveOptions {
  WarningSink* warnings = nullptr;
  CancelToken* cancel = nullptr;
};

// 序列化为 OOXML 包并写入流。未知部件原样写回（docs/02 §3.1.3 第 6 条），
// 读取器读到的每个部件在这里都有对应的写出分支。
void Save(const Document& doc, Stream& out, const SaveOptions& opts = {});

// 保存到文件（原子替换：同目录临时文件 + MoveFileEx，docs/02 §8.3）。
void SaveToFile(const Document& doc, const std::string& path, const SaveOptions& opts = {});

}  // namespace text
}  // namespace doccore
