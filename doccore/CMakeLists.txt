cmake_minimum_required(VERSION 3.20)
project(doccore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(doccore STATIC
  src/kernel/core.cpp
  src/kernel/crypto.cpp
  src/kernel/stream.cpp
  src/kernel/deflate.cpp
  src/kernel/zip.cpp
  src/kernel/zip_write.cpp
  src/kernel/xml.cpp
  src/ooxml/opc.cpp
  src/ooxml/text.cpp
  src/ooxml/docx_read.cpp
  src/ooxml/docx_write.cpp
  src/ooxml/docx_protect.cpp
  src/ooxml/xlsx_read.cpp
  src/ooxml/xlsx_write.cpp
  src/ooxml/pptx_read.cpp
  src/ooxml/pptx_write.cpp
  src/ooxml/pptx_timing.cpp
  src/sheet/sheet.cpp
  src/sheet/formula.cpp
  src/sheet/recalc.cpp
  src/sheet/pivot.cpp
  src/mail/mail.cpp
  src/mail/msg.cpp
  src/ole/ole.cpp
  src/barcode/barcode.cpp
  src/deck/deck.cpp
  src/render/layout.cpp
  src/render/text_layout.cpp
  src/render/sheet_layout.cpp
  src/render/deck_layout.cpp
  src/render/image.cpp
  src/render/render.cpp
  src/pdf/pdf_read.cpp
  src/pdf/pdf_rebuild.cpp
  src/convert/convert.cpp
  src/pdf/ttf.cpp
  src/pdf/pdf_writer.cpp
  src/pdf/pdf_annotate.cpp
  src/pdf/pdf_crypt.cpp
)
target_include_directories(doccore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(doccore_cli src/cli/main.cpp)
target_link_libraries(doccore_cli PRIVATE doccore)
set_target_properties(doccore_cli PROPERTIES OUTPUT_NAME doccore)

add_executable(doccore_tests tests/test_main.cpp)
target_link_libraries(doccore_tests PRIVATE doccore)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/data" DOCCORE_TEST_DATA_DIR)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/out" DOCCORE_TEST_OUT_DIR)
target_compile_definitions(doccore_tests PRIVATE
  DOCCORE_TEST_DATA="${DOCCORE_TEST_DATA_DIR}"
  DOCCORE_TEST_OUT="${DOCCORE_TEST_OUT_DIR}")

enable_testing()
add_test(NAME unit COMMAND doccore_tests)
