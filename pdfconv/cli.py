"""Command-line interface.

Usage
-----
    python -m pdfconv <in.pdf> <out.docx|out.xlsx|out.pptx>
    python -m pdfconv <in.docx|in.xlsx> <out.pdf>
    python -m pdfconv --batch <docs_dir> <out_dir> [--from pdf|docx|xlsx] [--force]

The output format follows the output extension: .xlsx builds a workbook of
native tables, .pptx builds a slide deck (one slide per page), .pdf renders a
DOCX or an XLSX back to PDF, anything else builds the DOCX.
"""
import argparse
import os
import sys

from .convert import convert
from .docx2pdf import convert_docx_pdf
from .translate import (EchoTranslator, GlossaryTranslator, HttpTranslator,
                        translate_docx, translate_pdf)
from .pptx import convert_pptx
from .pptx2pdf import convert_pptx_pdf
from .xlsx import convert_xlsx
from .xlsx2pdf import convert_xlsx_pdf


def _batch(docs_dir, out_dir, force, ext=".docx", src="pdf"):
    os.makedirs(out_dir, exist_ok=True)
    want = "." + src.lstrip(".")
    files = sorted(f for f in os.listdir(docs_dir)
                   if f.lower().endswith(want))
    done = 0
    for f in files:
        path = os.path.join(docs_dir, f)
        out = os.path.join(out_dir, os.path.splitext(f)[0] + ext)
        if os.path.exists(out) and not force:
            continue
        try:
            if ext == ".xlsx":
                n, t = convert_xlsx(path, out)
                print(f"OK  {f}: sheets={n} tables={t}")
            elif ext == ".pptx":
                n, p, t = convert_pptx(path, out)
                print(f"OK  {f}: slides={n} pictures={p} tables={t}")
            elif ext == ".pdf":
                if src == "xlsx":
                    n, s, c = convert_xlsx_pdf(path, out)
                    print(f"OK  {f}: pages={n} sheets={s} cells={c}")
                elif src == "pptx":
                    n, s, t = convert_pptx_pdf(path, out)
                    print(f"OK  {f}: pages={n} shapes={s} tables={t}")
                else:
                    n, p, t = convert_docx_pdf(path, out)
                    print(f"OK  {f}: pages={n} paras={p} tables={t}")
            else:
                n, t, g = convert(path, out)
                print(f"OK  {f}: blocks={n} tables={t} figs={g}")
            done += 1
        except Exception as e:
            print(f"ERR {f}: {e!r}")
    print(f"batch done: {done}/{len(files)}")
    return 0


def _engine(name, glossary):
    if name == "echo":
        return EchoTranslator()
    if name == "glossary":
        return GlossaryTranslator(glossary)
    return HttpTranslator()


def _translate(a):
    import json
    glossary = {}
    if a.glossary:
        with open(a.glossary, "r", encoding="utf-8") as f:
            glossary = json.load(f)
    eng = _engine(a.engine, glossary)
    if a.output.lower().endswith(".pdf"):
        mode = a.mode or ("bilingual" if a.bilingual else "reflow")
        st, pages = translate_pdf(a.input, a.output, eng, src=a.src_lang,
                                  tgt=a.to, glossary=glossary,
                                  bilingual=a.bilingual, mode=mode)
        extra = ""
        if mode == "inplace":
            extra = (" boxes=%d shrunk=%d overflow=%d"
                     % (st.boxes, st.shrunk, st.overflow))
        print("mode=%s pages=%d units=%d paragraphs=%d chars=%d->%d "
              "fallback=%d%s"
              % (mode, pages, st.units, st.paragraphs, st.chars_in,
                 st.chars_out, st.fallback, extra))
        return 0
    # DOCX out: a PDF source is converted first, then translated in place
    src = a.input
    if src.lower().endswith(".pdf"):
        convert(src, a.output)
        src = a.output
    st = translate_docx(src, a.output, eng, src=a.src_lang, tgt=a.to,
                        glossary=glossary, bilingual=a.bilingual)
    print("units=%d paragraphs=%d tables=%d chars=%d->%d fallback=%d"
          % (st.units, st.paragraphs, st.tables, st.chars_in,
             st.chars_out, st.fallback))
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--batch":
        # keep the original flag style: --batch <dir> <dir> [--from src] [--force]
        rest = argv[1:]
        force = "--force" in rest
        rest = [a for a in rest if a != "--force"]
        src = "pdf"
        if "--from" in rest:
            i = rest.index("--from")
            try:
                src = rest[i + 1].lstrip(".").lower()
            except IndexError:
                pass
            del rest[i:i + 2]
        if len(rest) < 2:
            print("usage: python -m pdfconv --batch <docs_dir> <out_dir> "
                  "[--from pdf|docx|xlsx] [--force]", file=sys.stderr)
            return 2
        ext = ".pdf" if src in ("docx", "xlsx", "pptx") else ".docx"
        return _batch(rest[0], rest[1], force, ext=ext, src=src)

    p = argparse.ArgumentParser(
        prog="pdfconv",
        description="Self-contained PDF <-> DOCX / XLSX / PPTX converter "
                    "(MuPDF reads the PDF; all reconstruction is ours).")
    p.add_argument("input", help="input PDF (or DOCX / XLSX when the output "
                                 "is .pdf)")
    p.add_argument("output", help="output path: .xlsx for a workbook of "
                                  "native tables, .pptx for a slide deck, "
                                  ".pdf to render a DOCX / XLSX, otherwise "
                                  "DOCX")
    p.add_argument("--to", metavar="LANG",
                   help="translate into LANG (zh/ja/en/...) on the way out; "
                        "PDF in, translated PDF or DOCX out")
    p.add_argument("--from", dest="src_lang", default="en", metavar="LANG",
                   help="source language (default: en)")
    p.add_argument("--bilingual", action="store_true",
                   help="keep the original paragraph above each translation")
    p.add_argument("--mode", default=None,
                   choices=("reflow", "inplace", "bilingual"),
                   help="how the translation lands in a PDF: reflow "
                        "(re-paginate, default), inplace (refill each text "
                        "box, page count unchanged), bilingual (source above "
                        "translation)")
    p.add_argument("--glossary", metavar="JSON",
                   help="term table {source: target} applied on top of the MT")
    p.add_argument("--engine", default="http",
                   choices=("http", "glossary", "echo"),
                   help="MT engine: http (OpenAI-compatible endpoint from "
                        "PDFCONV_MT_* env), glossary (offline term swap), "
                        "echo (identity, for tests)")
    a = p.parse_args(argv)
    if a.to:
        return _translate(a)
    if a.output.lower().endswith(".xlsx"):
        n, t = convert_xlsx(a.input, a.output)
        print("sheets=%d tables=%d" % (n, t))
        return 0
    if a.output.lower().endswith(".pptx"):
        n, p, t = convert_pptx(a.input, a.output)
        print("slides=%d pictures=%d tables=%d" % (n, p, t))
        return 0
    if a.output.lower().endswith(".pdf"):
        if a.input.lower().endswith((".xlsx", ".xlsm")):
            n, s, c = convert_xlsx_pdf(a.input, a.output)
            print("pages=%d sheets=%d cells=%d" % (n, s, c))
        elif a.input.lower().endswith((".pptx", ".pptm")):
            n, s, t = convert_pptx_pdf(a.input, a.output)
            print("pages=%d shapes=%d tables=%d" % (n, s, t))
        else:
            n, p, t = convert_docx_pdf(a.input, a.output)
            print("pages=%d paragraphs=%d tables=%d" % (n, p, t))
        return 0
    res = convert(a.input, a.output)
    print("blocks=%d tables=%d figures=%d" % res)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
