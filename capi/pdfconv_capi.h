/*
 * pdfconv C ABI -- a plain C interface so C / C++ / C# / Go (via FFI) can call
 * the Python converter.  The shared library embeds a CPython interpreter and
 * invokes pdfconv.convert(); callers see only this C header.
 *
 *   int rc = pdfconv_convert("in.pdf", "out.docx");
 *   if (rc != 0) fprintf(stderr, "%s\n", pdfconv_last_error());
 *
 * Threading: each call acquires the GIL.  Calls are serialised by the GIL.
 */
#ifndef PDFCONV_CAPI_H
#define PDFCONV_CAPI_H

#ifdef _WIN32
#  ifdef PDFCONV_BUILD
#    define PDFCONV_API __declspec(dllexport)
#  else
#    define PDFCONV_API __declspec(dllimport)
#  endif
#else
#  define PDFCONV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Convert in_pdf -> out_docx.  Returns 0 on success, non-zero on failure. */
PDFCONV_API int pdfconv_convert(const char* in_pdf, const char* out_docx);

/* Message describing the most recent failure (valid until the next call). */
PDFCONV_API const char* pdfconv_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PDFCONV_CAPI_H */
