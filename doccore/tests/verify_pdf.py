"""Independent verifier for wave P8 (PDF reading + PDF -> DOCX/XLSX/PPTX).

Nothing here uses DocCore code: text comes from PyMuPDF, DOCX from python-docx,
XLSX from openpyxl, PPTX from python-pptx. The point is to prove the reader and
the reconstructor against a second implementation, not to re-run our own checks.

Usage:
  python tests/verify_pdf.py [read|rebuild|scanned|all] [--exe <path>]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
FIXTURE = os.path.join(DATA, "sample_pdf.pdf")

failures = []
checks = 0


def check(cond, message):
    global checks
    checks += 1
    if not cond:
        failures.append(message)
        print("FAIL %s" % message)


def norm(text):
    """Collapse whitespace so run/line splitting differences do not matter."""
    return re.sub(r"\s+", "", text or "")


def run_cli(exe, args):
    proc = subprocess.run([exe] + args, capture_output=True)
    return proc.returncode, proc.stdout.decode("utf-8", "replace"), proc.stderr.decode(
        "utf-8", "replace"
    )


def import_fitz():
    try:
        import fitz
    except ImportError:
        print("PyMuPDF is required: pip install pymupdf")
        raise SystemExit(2)
    return fitz


# ---- ① PDF 读取 ---------------------------------------------------------


def verify_read(exe, workdir):
    fitz = import_fitz()
    doc = fitz.open(FIXTURE)

    code, out, err = run_cli(exe, ["pdf", FIXTURE, "--mode", "info"])
    check(code == 0, "pdf --mode info exit=%d (%s)" % (code, err.strip()))
    m = re.search(r'"pages":(\d+)', out)
    check(m is not None and int(m.group(1)) == doc.page_count,
          "page count: ours=%s pymupdf=%d" % (m.group(1) if m else "?", doc.page_count))

    # 页面尺寸逐页比对。
    for line in out.splitlines():
        hit = re.match(r"\s+\[(\d+)\] ([\d.]+)x([\d.]+) pt", line)
        if not hit:
            continue
        idx = int(hit.group(1))
        page = doc[idx]
        check(abs(float(hit.group(2)) - page.rect.width) <= 0.5,
              "page %d width ours=%s pymupdf=%.2f" % (idx, hit.group(2), page.rect.width))
        check(abs(float(hit.group(3)) - page.rect.height) <= 0.5,
              "page %d height ours=%s pymupdf=%.2f" % (idx, hit.group(3), page.rect.height))

    # 每页文本：PyMuPDF 抽出的每一行都必须出现在我们的文本里（去空白比较）。
    txt_path = os.path.join(workdir, "read.txt")
    code, _, err = run_cli(exe, ["pdf", FIXTURE, "--mode", "text", "--out", txt_path])
    check(code == 0, "pdf --mode text exit=%d (%s)" % (code, err.strip()))
    with open(txt_path, "r", encoding="utf-8") as fh:
        ours_pages = fh.read().split("\f")
    check(len(ours_pages) == doc.page_count,
          "text pages=%d expected=%d" % (len(ours_pages), doc.page_count))

    total_lines = 0
    matched = 0
    for i, page in enumerate(doc):
        ours = norm(ours_pages[i] if i < len(ours_pages) else "")
        for line in (page.get_text() or "").splitlines():
            key = norm(line)
            if not key:
                continue
            total_lines += 1
            if key in ours:
                matched += 1
            else:
                print("  page %d missing line: %r" % (i, line))
    ratio = matched / total_lines if total_lines else 1.0
    check(total_lines > 20, "expected a text-rich fixture, got %d lines" % total_lines)
    check(ratio >= 0.95, "line coverage %.3f < 0.95" % ratio)
    print("read: %d/%d lines matched (%.1f%%)" % (matched, total_lines, ratio * 100))

    # 扫描页判定：第 4 页只有整页图。
    check('"scanned_pages":1' in out, "expected exactly one scanned page in info output")
    return True


# ---- ② PDF -> DOCX ------------------------------------------------------


def verify_docx(exe, workdir):
    fitz = import_fitz()
    from docx import Document

    out_docx = os.path.join(workdir, "rebuild.docx")
    code, stdout, err = run_cli(
        exe, ["convert", FIXTURE, "--to", "docx", "--out", out_docx]
    )
    check(code == 0, "convert pdf->docx exit=%d (%s)" % (code, err.strip()))
    check('"support":"lossy"' in stdout, "pdf->docx must report lossy, got %s" % stdout.strip())
    check("1011" in err, "scanned page must raise NoTextLayer (1011)")

    doc = Document(out_docx)
    body = "\n".join(p.text for p in doc.paragraphs)
    for table in doc.tables:
        for row in table.rows:
            for cell in row.cells:
                body += "\n" + cell.text
    flat = norm(body)

    # 正文完整率：非扫描页的每一行都要在 DOCX 里。
    src = fitz.open(FIXTURE)
    total = 0
    matched = 0
    for i in range(3):  # 第 4 页是扫描页，不参与
        for line in (src[i].get_text() or "").splitlines():
            key = norm(line)
            if not key:
                continue
            total += 1
            if key in flat:
                matched += 1
    ratio = matched / total if total else 1.0
    check(ratio >= 0.95, "docx text coverage %.3f < 0.95" % ratio)
    print("docx: %d/%d lines preserved (%.1f%%)" % (matched, total, ratio * 100))

    # 简单表格仍然是 Word 表格，格子文字与 PDF 一致。
    check(len(doc.tables) >= 2, "expected >=2 Word tables, got %d" % len(doc.tables))
    if doc.tables:
        cells = [[c.text.strip() for c in row.cells] for row in doc.tables[0].rows]
        check(cells[0] == ["Region", "Q1", "Q2", "Q3"], "ruled table header %r" % (cells[0],))
        check(cells[1] == ["North", "10", "20", "30"], "ruled table row 1 %r" % (cells[1],))
        check(cells[2] == ["South", "40", "50", "60"], "ruled table row 2 %r" % (cells[2],))

    # 标题被识别成 Heading 1（字号 24 对正文 11）。
    headings = [p.text.strip() for p in doc.paragraphs if p.style and p.style.name == "Heading 1"]
    check("DocCore PDF Rebuild Fixture" in headings, "title not mapped to Heading 1: %r" % headings)

    # 图片仍可取出。
    import zipfile

    with zipfile.ZipFile(out_docx) as z:
        names = [n for n in z.namelist() if n.startswith("word/media/")]
    check(len(names) == 1, "expected 1 embedded image, got %r" % names)
    return True


# ---- ③ PDF -> XLSX ------------------------------------------------------


def verify_xlsx(exe, workdir):
    from openpyxl import load_workbook

    out = os.path.join(workdir, "rebuild.xlsx")
    code, _, err = run_cli(exe, ["convert", FIXTURE, "--to", "xlsx", "--out", out])
    check(code == 0, "convert pdf->xlsx exit=%d (%s)" % (code, err.strip()))

    book = load_workbook(out)
    check(len(book.worksheets) == 3,
          "expected 3 sheets (scanned page skipped), got %d" % len(book.worksheets))

    # 第一页：框线表的数字必须是数值类型。
    ws = book.worksheets[0]
    values = {}
    for row in ws.iter_rows():
        for cell in row:
            if cell.value is not None:
                values[str(cell.value)] = cell
    check("Region" in values, "sheet 1 missing table header")
    check("North" in values, "sheet 1 missing North row")
    if "North" in values:
        row = values["North"].row
        q1 = ws.cell(row=row, column=2).value
        q3 = ws.cell(row=row, column=4).value
        check(isinstance(q1, (int, float)) and abs(float(q1) - 10) < 1e-9,
              "North/Q1 should be numeric 10, got %r" % (q1,))
        check(isinstance(q3, (int, float)) and abs(float(q3) - 30) < 1e-9,
              "North/Q3 should be numeric 30, got %r" % (q3,))

    # 第二页：双栏正文按阅读顺序排（左栏四行在右栏之前）。
    ws2 = book.worksheets[1]
    col_a = [ws2.cell(row=r, column=1).value or "" for r in range(1, ws2.max_row + 1)]
    joined = "\n".join(str(v) for v in col_a)
    check("Left one" in joined and "Right four" in joined,
          "sheet 2 lost column text: %r" % col_a[:4])
    check(joined.find("Left four") < joined.find("Right one"),
          "sheet 2 reading order is wrong (right column interleaved)")

    # 第三页：无框线表按对齐识别，数字仍是数值。
    ws3 = book.worksheets[2]
    found = False
    for row in ws3.iter_rows():
        if row and row[0].value == "Widget":
            found = True
            check(abs(float(row[1].value) - 3) < 1e-9, "Widget qty %r" % (row[1].value,))
            check(abs(float(row[2].value) - 4.5) < 1e-9, "Widget price %r" % (row[2].value,))
    check(found, "sheet 3 missing aligned table row 'Widget'")
    return True


# ---- ④ PDF -> PPTX ------------------------------------------------------


def verify_pptx(exe, workdir):
    from pptx import Presentation
    from pptx.util import Emu

    out = os.path.join(workdir, "rebuild.pptx")
    code, _, err = run_cli(exe, ["convert", FIXTURE, "--to", "pptx", "--out", out])
    check(code == 0, "convert pdf->pptx exit=%d (%s)" % (code, err.strip()))

    pres = Presentation(out)
    check(len(pres.slides) == 3,
          "expected 3 slides (scanned page skipped), got %d" % len(pres.slides))
    # 幻灯片尺寸等于 PDF 页尺寸（595.28 x 841.89 pt）。
    check(abs(pres.slide_width / 12700.0 - 595.28) <= 0.5,
          "slide width %.2f pt" % (pres.slide_width / 12700.0))
    check(abs(pres.slide_height / 12700.0 - 841.89) <= 0.5,
          "slide height %.2f pt" % (pres.slide_height / 12700.0))

    texts = []
    tables = 0
    pictures = []
    for slide in pres.slides:
        for shape in slide.shapes:
            if shape.has_text_frame:
                texts.append(shape.text_frame.text)
            if getattr(shape, "has_table", False) and shape.has_table:
                tables += 1
                rows = [[c.text.strip() for c in r.cells] for r in shape.table.rows]
                if rows and rows[0][:1] == ["Region"]:
                    check(rows[0] == ["Region", "Q1", "Q2", "Q3"], "pptx table header %r" % (rows[0],))
            if shape.shape_type == 13:  # PICTURE
                pictures.append(
                    (shape.left / 12700.0, shape.top / 12700.0,
                     shape.width / 12700.0, shape.height / 12700.0)
                )
    check(tables >= 2, "expected >=2 slide tables, got %d" % tables)
    joined = norm("\n".join(texts))
    check("DocCorePDFRebuildFixture" in joined, "slide text lost the title")
    check("Rightfour:readingordermustfollow." in joined, "slide text lost the right column")

    # 图片落位偏差 <= 2 pt（PDF 里是 72,360 起、96x72）。
    check(len(pictures) == 1, "expected 1 picture, got %d" % len(pictures))
    if pictures:
        x, y, w, h = pictures[0]
        check(abs(x - 72) <= 2, "picture x=%.2f expected 72" % x)
        check(abs(y - 360) <= 2, "picture y=%.2f expected 360" % y)
        check(abs(w - 96) <= 2, "picture w=%.2f expected 96" % w)
        check(abs(h - 72) <= 2, "picture h=%.2f expected 72" % h)
    return True


# ---- ⑤ 扫描件必须拒绝 ---------------------------------------------------


def verify_scanned(exe, workdir):
    fitz = import_fitz()
    # 用 PyMuPDF 现场造一份"只有整页图"的 PDF（独立于 DocCore 的写出路径）。
    path = os.path.join(workdir, "scanned.pdf")
    doc = fitz.open()
    page = doc.new_page(width=595, height=842)
    pix = fitz.Pixmap(fitz.csGRAY, fitz.IRect(0, 0, 120, 170), False)
    samples = bytearray(pix.samples_mv)
    for i in range(len(samples)):
        samples[i] = 240 if (i // 120) % 30 else 20
    pix.samples_mv[:] = bytes(samples)
    page.insert_image(fitz.Rect(0, 0, 595, 842), stream=pix.tobytes("png"))
    doc.save(path)
    doc.close()

    for target in ("docx", "xlsx", "pptx"):
        out = os.path.join(workdir, "scanned.%s" % target)
        code, _, err = run_cli(exe, ["convert", path, "--to", target, "--out", out])
        check(code != 0, "scanned pdf -> %s must fail, exit=%d" % (target, code))
        check("1011" in err or "text layer" in err,
              "scanned pdf -> %s must report NoTextLayer, stderr=%r" % (target, err.strip()))
        check(not os.path.exists(out) or os.path.getsize(out) == 0,
              "scanned pdf -> %s must not write a half-made file" % target)
    print("scanned: rejected on all three targets")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", nargs="?", default="all",
                        choices=["read", "rebuild", "scanned", "all"])
    parser.add_argument("--exe", default=None)
    args = parser.parse_args()

    exe = args.exe
    if exe is None:
        for candidate in ("build/Release/doccore.exe", "build/doccore.exe",
                          "build/Release/doccore", "build/doccore"):
            path = os.path.join(os.path.dirname(HERE), candidate)
            if os.path.exists(path):
                exe = path
                break
    if exe is None or not os.path.exists(exe):
        print("doccore executable not found; pass --exe")
        return 2
    if not os.path.exists(FIXTURE):
        print("fixture missing: run python tests/make_sample_pdf.py first")
        return 2

    with tempfile.TemporaryDirectory() as workdir:
        if args.mode in ("read", "all"):
            verify_read(exe, workdir)
        if args.mode in ("rebuild", "all"):
            verify_docx(exe, workdir)
            verify_xlsx(exe, workdir)
            verify_pptx(exe, workdir)
        if args.mode in ("scanned", "all"):
            verify_scanned(exe, workdir)

    print("%d checks, %d failures" % (checks, len(failures)))
    if failures:
        for f in failures:
            print("  - %s" % f)
        return 1
    print("pdf ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
