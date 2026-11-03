// Minimal C++ caller for the pdfconv C ABI.
//
//   pdfconv_example input.pdf output.docx
//
// This shows the entire C++ integration: one include + one call.
#include "pdfconv_capi.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <in.pdf> <out.docx>\n", argv[0]);
        return 2;
    }
    int rc = pdfconv_convert(argv[1], argv[2]);
    if (rc != 0) {
        std::printf("FAILED: %s\n", pdfconv_last_error());
        return 1;
    }
    std::printf("OK: %s -> %s\n", argv[1], argv[2]);
    return 0;
}
