cmake_minimum_required(VERSION 3.15)
project(pdfconv_capi LANGUAGES C CXX)

# Locate the CPython development files (headers + libpython).
find_package(Python3 COMPONENTS Development REQUIRED)

# Shared library exposing the C ABI.
add_library(pdfconv SHARED pdfconv_capi.c)
target_compile_definitions(pdfconv PRIVATE PDFCONV_BUILD=1)
target_link_libraries(pdfconv PRIVATE Python3::Python)
target_include_directories(pdfconv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Example C++ consumer.
add_executable(pdfconv_example example.cpp)
target_link_libraries(pdfconv_example PRIVATE pdfconv)

# ---------------------------------------------------------------------------
# Build:
#   cmake -S capi -B capi/build
#   cmake --build capi/build --config Release
#
# Run (the folder that CONTAINS the `pdfconv` package must be on PYTHONPATH):
#   Windows : set PYTHONPATH=C:\path\to\aspose
#   Linux   : export PYTHONPATH=/path/to/aspose
#   capi/build/pdfconv_example in.pdf out.docx
# ---------------------------------------------------------------------------
