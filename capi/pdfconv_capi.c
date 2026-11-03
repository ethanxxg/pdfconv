/*
 * C ABI wrapper: embed CPython and call pdfconv.convert().
 *
 * Build with CMake (see CMakeLists.txt) or e.g.:
 *   cc -shared -fPIC -o libpdfconv.so pdfconv_capi.c \
 *      $(python3-config --includes) $(python3-config --ldflags)
 *
 * Runtime: the directory containing the `pdfconv` package must be importable,
 * e.g. export PYTHONPATH=/path/to/project  (where pdfconv/ lives).
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdio.h>
#include <string.h>

#ifndef PDFCONV_BUILD
#define PDFCONV_BUILD 1
#endif
#include "pdfconv_capi.h"

static char g_err[2048] = {0};

const char* pdfconv_last_error(void) { return g_err; }

static void set_err(const char* msg) {
    if (!msg) msg = "";
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = '\0';
}

static void capture_py_error(void) {
    PyObject *t = NULL, *v = NULL, *tb = NULL, *s = NULL;
    PyErr_Fetch(&t, &v, &tb);
    PyErr_NormalizeException(&t, &v, &tb);
    if (v) s = PyObject_Str(v);
    if (s) {
        const char* c = PyUnicode_AsUTF8(s);
        set_err(c ? c : "unknown Python error");
    } else {
        set_err("unknown Python error");
    }
    Py_XDECREF(s);
    Py_XDECREF(t);
    Py_XDECREF(v);
    Py_XDECREF(tb);
}

int pdfconv_convert(const char* in_pdf, const char* out_docx) {
    if (!in_pdf || !out_docx) { set_err("null argument"); return -1; }

    if (!Py_IsInitialized()) {
        Py_Initialize();
    }
    PyGILState_STATE gil = PyGILState_Ensure();

    int rc = -1;
    PyObject *mod = NULL, *fn = NULL, *a_in = NULL, *a_out = NULL;
    PyObject *args = NULL, *res = NULL;

    mod = PyImport_ImportModule("pdfconv");
    if (!mod) { capture_py_error(); goto done; }

    fn = PyObject_GetAttrString(mod, "convert");
    if (!fn) { capture_py_error(); goto done; }
    if (!PyCallable_Check(fn)) { set_err("pdfconv.convert is not callable"); goto done; }

    /* decode filesystem paths so non-ASCII (e.g. Chinese) names work */
    a_in = PyUnicode_DecodeFSDefault(in_pdf);
    a_out = PyUnicode_DecodeFSDefault(out_docx);
    if (!a_in || !a_out) { capture_py_error(); goto done; }

    args = PyTuple_Pack(2, a_in, a_out);
    if (!args) { capture_py_error(); goto done; }

    res = PyObject_CallObject(fn, args);
    if (!res) { capture_py_error(); goto done; }

    rc = 0;

done:
    Py_XDECREF(res);
    Py_XDECREF(args);
    Py_XDECREF(a_in);
    Py_XDECREF(a_out);
    Py_XDECREF(fn);
    Py_XDECREF(mod);
    PyGILState_Release(gil);
    return rc;
}
