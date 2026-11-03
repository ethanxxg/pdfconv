"""Regenerates tests/data/sample.pptx with python-pptx (third-party fixture).

Kept deliberately small but covering everything P7 must handle:
  slide 1  title + body placeholders (two paragraphs, bold run, alignment)
  slide 2  a 3x3 table with a filled header row
  slide 3  a picture plus a rotated-ish text box, and speaker notes on slide 1

Run:  python make_sample_pptx.py
"""
import os
import struct
import zlib

from pptx import Presentation
from pptx.util import Emu, Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data", "sample.pptx")


def solid_png(width, height, rgb):
    """Minimal 8-bit RGB PNG, no external dependencies."""

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def build():
    prs = Presentation()
    prs.slide_width = Emu(12192000)   # 13.333 in, 16:9
    prs.slide_height = Emu(6858000)   # 7.5 in

    title_layout = prs.slide_layouts[0]
    content_layout = prs.slide_layouts[1]
    blank_layout = prs.slide_layouts[6]

    # ---- slide 1: title + body
    slide = prs.slides.add_slide(title_layout)
    slide.shapes.title.text = "DocCore 演示文稿"
    body = slide.placeholders[1].text_frame
    body.text = "第一页正文：演示 PPTX 读取与导出。"
    para = body.add_paragraph()
    run = para.add_run()
    run.text = "加粗的一段"
    run.font.bold = True
    run.font.size = Pt(20)
    para.alignment = PP_ALIGN.CENTER
    slide.notes_slide.notes_text_frame.text = "备注：这一页用来验证备注页读写。"

    # ---- slide 2: table
    slide = prs.slides.add_slide(content_layout)
    slide.shapes.title.text = "表格页"
    rows, cols = 3, 3
    table_shape = slide.shapes.add_table(
        rows, cols, Inches(0.8), Inches(1.8), Inches(8.0), Inches(2.4)
    )
    table = table_shape.table
    for c in range(cols):
        cell = table.cell(0, c)
        cell.text = "列 %d" % (c + 1)
        cell.fill.solid()
        cell.fill.fore_color.rgb = RGBColor(0x44, 0x72, 0xC4)
    for r in range(1, rows):
        for c in range(cols):
            table.cell(r, c).text = "R%dC%d" % (r, c)

    # ---- slide 3: picture + text box
    slide = prs.slides.add_slide(blank_layout)
    slide.shapes.add_picture(
        _write_png(solid_png(64, 48, (0xED, 0x7D, 0x31))),
        Inches(1.0),
        Inches(1.0),
        Inches(2.0),
        Inches(1.5),
    )
    box = slide.shapes.add_textbox(Inches(4.0), Inches(1.0), Inches(5.0), Inches(1.5))
    tf = box.text_frame
    tf.word_wrap = True
    tf.text = "绝对定位文本框：这一页没有占位符。"
    p2 = tf.add_paragraph()
    p2.text = "第二段，用来验证多段落与换行。"

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    prs.save(OUT)
    print("wrote", OUT)


def _write_png(data):
    import io

    return io.BytesIO(data)


if __name__ == "__main__":
    build()
