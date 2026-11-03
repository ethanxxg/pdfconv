"""Corpus-wide fidelity metrics for the report."""
import sys, os, glob, zipfile, re, collections
from docx import Document


def main(d):
    files = [p for p in glob.glob(os.path.join(d, "*.docx"))
             if not os.path.basename(p).startswith("~$")]
    fonts = set()
    colored = 0
    tables = gridspan = hyperlinks = headers = pagefields = 0
    paras = chars = 0
    docs_colored = 0
    for p in files:
        doc = Document(p)
        paras += len(doc.paragraphs)
        chars += sum(len(x.text) for x in doc.paragraphs)
        tables += len(doc.tables)
        got = False
        for para in doc.paragraphs:
            for r in para.runs:
                if r.font.name:
                    fonts.add(r.font.name)
                try:
                    c = r.font.color
                    if c is not None and c.rgb is not None and str(c.rgb) != "000000":
                        colored += 1
                        got = True
                except Exception:
                    pass
        if got:
            docs_colored += 1
        z = zipfile.ZipFile(p)
        names = z.namelist()
        body = z.read("word/document.xml").decode("utf8")
        gridspan += body.count("gridSpan")
        hyperlinks += body.count("<w:hyperlink")
        hdr = [n for n in names if re.match(r"word/header\d+\.xml", n)]
        if hdr:
            headers += 1
            for n in hdr:
                if "PAGE" in z.read(n).decode("utf8"):
                    pagefields += 1
                    break
    print(f"docs={len(files)}")
    print(f"paragraphs={paras} chars={chars}")
    print(f"distinct font names={len(fonts)}")
    print(f"colored runs={colored} (docs with colour={docs_colored})")
    print(f"tables={tables} gridSpan={gridspan}")
    print(f"hyperlinks={hyperlinks}")
    print(f"docs with header={headers} page-field={pagefields}")


if __name__ == "__main__":
    main(sys.argv[1])
