"""The tool registry -- one table that every page, route and test reads.

Adding a conversion means adding a row here, not another copy of the page.  The
formats below are exactly the ones `pdfconv` can actually do; nothing is listed
that would fail at runtime.

`src` / `dst` are file kinds, not extensions: "pdf" and the three OOXML kinds.
The engine dispatch, upload validation and download Content-Type are all keyed
on them.
"""

# What each kind looks like on the wire.
MAGIC = {
    "pdf": b"%PDF-",
    "docx": b"PK\x03\x04",
    "xlsx": b"PK\x03\x04",
    "pptx": b"PK\x03\x04",
}

# Download MIME types.  Getting these wrong means the browser saves a file that
# the target application refuses to open, which looks like a failed conversion.
MIME = {
    "pdf": "application/pdf",
    "docx": ("application/vnd.openxmlformats-officedocument"
             ".wordprocessingml.document"),
    "xlsx": ("application/vnd.openxmlformats-officedocument"
             ".spreadsheetml.sheet"),
    "pptx": ("application/vnd.openxmlformats-officedocument"
             ".presentationml.presentation"),
    "jpg": "image/jpeg",
    "jpeg": "image/jpeg",
    "png": "image/png",
}

EXT = {"pdf": ".pdf", "docx": ".docx", "xlsx": ".xlsx", "pptx": ".pptx"}

TOOLS = {
    "merge": {
        "id": "merge", "label": "合并 PDF", "path": "/",
        "title": "合并 PDF", "src": "pdf", "dst": "pdf",
        "sub": "把多个 PDF 按你指定的顺序合成一个，可逐份旋转。",
        "icon": "merge",
    },
    "compress": {
        "id": "compress", "label": "压缩 PDF", "path": "/compress",
        "title": "压缩文件", "src": "pdf", "dst": "pdf",
        "sub": "PDF、JPG、PNG 都能压：图片重采样、PDF 重排对象，画面与文字保持清晰。",
        "icon": "compress",
    },
    "pdf-to-word": {
        "id": "pdf-to-word", "label": "PDF 转 Word",
        "path": "/convert/pdf-to-word",
        "title": "PDF 转 Word", "src": "pdf", "dst": "docx",
        "sub": "转成可编辑的 .docx，保留版式、表格和图片。",
        "icon": "docx", "action": "转换为 Word", "result": "下载 Word 文档",
    },
    "pdf-to-excel": {
        "id": "pdf-to-excel", "label": "PDF 转 Excel",
        "path": "/convert/pdf-to-excel",
        "title": "PDF 转 Excel", "src": "pdf", "dst": "xlsx",
        "sub": "把 PDF 里的表格提取成可计算的 .xlsx 工作表。",
        "icon": "xlsx", "action": "转换为 Excel", "result": "下载 Excel 表格",
        # Extraction only finds *tables*.  A prose-only PDF produces an empty
        # workbook, so the page warns about that up front.
        "warn": "只提取表格：正文段落不会进入 Excel。没有表格的 PDF 会得到空表。",
    },
    "pdf-to-ppt": {
        "id": "pdf-to-ppt", "label": "PDF 转 PPT",
        "path": "/convert/pdf-to-ppt",
        "title": "PDF 转 PPT", "src": "pdf", "dst": "pptx",
        "sub": "每页生成一张幻灯片，图片与文字分别还原。",
        "icon": "pptx", "action": "转换为 PPT", "result": "下载 PPT 演示文稿",
    },
    "word-to-pdf": {
        "id": "word-to-pdf", "label": "Word 转 PDF",
        "path": "/convert/word-to-pdf",
        "title": "Word 转 PDF", "src": "docx", "dst": "pdf",
        "sub": "把 .docx 转成版式固定的 PDF，便于分发和打印。",
        "icon": "docx", "action": "转换为 PDF", "result": "下载 PDF",
        "accept": [".docx"], "mime": "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    },
    "excel-to-pdf": {
        "id": "excel-to-pdf", "label": "Excel 转 PDF",
        "path": "/convert/excel-to-pdf",
        "title": "Excel 转 PDF", "src": "xlsx", "dst": "pdf",
        "sub": "把 .xlsx 转成分页排好的 PDF。",
        "icon": "xlsx", "action": "转换为 PDF", "result": "下载 PDF",
        "accept": [".xlsx"], "mime": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    },
    "ppt-to-pdf": {
        "id": "ppt-to-pdf", "label": "PPT 转 PDF",
        "path": "/convert/ppt-to-pdf",
        "title": "PPT 转 PDF", "src": "pptx", "dst": "pdf",
        "sub": "把 .pptx 转成 PDF，一页一张幻灯片。",
        "icon": "pptx", "action": "转换为 PDF", "result": "下载 PDF",
        "accept": [".pptx"], "mime": "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    },
}

# Order matters: it is the nav order and the order of the grid.
ORDER = ["merge", "compress", "pdf-to-word", "pdf-to-excel", "pdf-to-ppt",
         "word-to-pdf", "excel-to-pdf", "ppt-to-pdf"]

# Conversions that go through the generic /convert page (everything else has a
# bespoke page because the interaction is genuinely different).
CONVERT_TOOLS = [t for t in ORDER if t.startswith(("pdf-to-", "word-to-",
                                                   "excel-to-", "ppt-to-"))]


def get(name):
    return TOOLS.get(name)


def is_pdf_source(tool):
    return tool["src"] == "pdf"


def describe_all():
    out = []
    for name in ORDER:
        t = TOOLS[name]
        out.append({
            "id": t["id"], "label": t["label"], "path": t["path"],
            "title": t["title"], "sub": t["sub"], "icon": t["icon"],
            "src": t["src"], "dst": t["dst"],
            "accept": t.get("accept") or [".pdf"],
        })
    return out
