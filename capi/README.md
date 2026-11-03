# pdfconv C ABI / C++ 接口

给 C / C++（以及 C#、Go 等能 FFI 的语言）同事用的**纯 C 接口**。共享库内部用
CPython C API 嵌入 Python 解释器，调用 `pdfconv.convert()`；调用方只看
`pdfconv_capi.h`，无需了解 Python。

## 接口

```c
int         pdfconv_convert(const char* in_pdf, const char* out_docx); // 0=成功
const char* pdfconv_last_error(void);                                  // 失败原因
```

C++ 用法（见 `example.cpp`）：

```cpp
#include "pdfconv_capi.h"
int rc = pdfconv_convert("in.pdf", "out.docx");
if (rc != 0) printf("%s\n", pdfconv_last_error());
```

## 构建

需要 **CPython 开发头文件 + libpython**（`python3-dev` / Windows 的 Python 安装自带）。
注意：**运行机器上必须装有** Python、`PyMuPDF`、`python-docx`，且 `pdfconv` 包可被导入。

```bash
cmake -S capi -B capi/build
cmake --build capi/build --config Release
```

运行（把**包含 `pdfconv/` 的目录**加入 `PYTHONPATH`）：

```bash
# Windows
set PYTHONPATH=C:\Users\TH\Desktop\dev\test\aspose
capi\build\pdfconv_example.exe docs\some.pdf out.docx

# Linux/macOS
export PYTHONPATH=/path/to/aspose
./capi/build/pdfconv_example docs/some.pdf out.docx
```

也可不用 CMake 手工编译：

```bash
cc -shared -fPIC -o libpdfconv.so pdfconv_capi.c \
   $(python3-config --includes) $(python3-config --ldflags)
c++ example.cpp -I. -L. -lpdfconv -o pdfconv_example
```

## 部署说明

- 这是"**嵌入 Python 的 C 接口**"，不是纯 C++ 实现。目标机器需带 Python 运行时
  （体积较大，可用 PyInstaller 打包或随程序分发 Python 运行时）。
- 每次调用会获取 GIL，多线程调用会被串行化；高并发建议用**多进程**或直接走 HTTP 服务。
- 若要"**零 Python 依赖**的原生库"，只能把 `pdfconv` 逻辑重写成 C++（工作量大，需要
  C++ 工具链）。当前环境没有 gcc/msvc，故本仓库提供的是嵌入式 C ABI 方案。

## 备选：pybind11

若更想要 C++ 风格（`pdfconv::convert(std::string, std::string)` 直接暴露为模块），可用
pybind11 写一个扩展模块，思路相同（内部调用 `pdfconv.convert`）。本仓库采用 CPython C API
是为了少一个第三方构建依赖。
