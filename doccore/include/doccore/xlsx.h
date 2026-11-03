// DocCore - XLSX 读写入口。Design refs: docs/02 §3.4.1
//
//   sheet::Workbook book = sheet::Open({.path = "in.xlsx"});
//   book.SheetAt(0)->SetNumber(0, 0, 42);
//   sheet::Save(book, "out.xlsx");
//
// 读：共享字符串、样式与数字格式、单元格、公式、合并单元格、列宽行高、
// 打印设置；未识别部件原样保留，保存时写回（docs/02 §3.1.3 第 6 条）。
#pragma once

#include <string>

#include "doccore/core.h"
#include "doccore/sheet.h"
#include "doccore/stream.h"

namespace doccore {
namespace sheet {

struct OpenOptions {
  std::string path;  // 文件路径（与 memory 二选一）
  CancelToken* cancel = nullptr;
  WarningSink* warnings = nullptr;
};

// 抛 Error：Io / Corrupt（非 OOXML）/ Unsupported（不是电子表格族）。
Workbook Open(const OpenOptions& opts);
Workbook OpenMemory(std::string_view bytes, WarningSink* warnings = nullptr,
                    CancelToken* cancel = nullptr);

struct SaveOptions {
  WarningSink* warnings = nullptr;
  CancelToken* cancel = nullptr;
  // 保存前重算（默认不重算，与"打开看到上次缓存"一致，docs/02 §3.4.2）。
  bool recalculate = false;
};

void Save(const Workbook& book, Stream& out, const SaveOptions& opts = {});
void SaveToFile(const Workbook& book, const std::string& path, const SaveOptions& opts = {});

// ---- 导出 ---------------------------------------------------------------

// CSV：默认导出第一张表；逗号分隔，含分隔符/引号/换行的字段加引号。
std::string ToCsv(const Worksheet& sheet, char delimiter = ',');

// HTML：整本工作簿，一张表一个 <table>，合并单元格映射为 rowspan/colspan。
std::string ToHtml(const Workbook& book);

}  // namespace sheet
}  // namespace doccore
