"""Machine-readable capability catalog -- the contract every client reads.

``tools.py`` is the browser-facing registry (labels, paths, icons).  This module
adds the part an agent needs but a page does not: a JSON Schema for each tool's
``options`` object, and its file-count/shape contract.  Together they are the
single source of truth the Web grid, the CLI and (later) the MCP server all
render from -- so adding a tool means one row here, not three copies in three
clients.

The HTTP API remains the only executor.  Every capability maps to a ``task``
(and optionally an ``op``) understood by ``POST /api/v1/jobs``; that route owns
validation, quota, plan gating and the HMAC download.  Clients never call the
engine directly, which is why a capability invoked from the CLI obeys exactly
the same free/Pro rules as one invoked from a browser.
"""
from . import tools as T

# JSON Schema fragment types, kept tiny on purpose.  Field names match the
# keys ``main.create_job`` reads from the request's ``options`` object.
def _enum(values, default):
    return {"type": "string", "enum": list(values), "default": default}


def _int(lo, hi, default):
    return {"type": "integer", "minimum": lo, "maximum": hi, "default": default}


def _bool(default=True):
    return {"type": "boolean", "default": default}


def _str(max_len=200):
    return {"type": "string", "maxLength": max_len}


# Per-tool options, keyed by tool id.  A tool absent from this table takes no
# options beyond ``name``.  ``task``/``op`` tell a client how to build the job.
SPEC = {
    "merge": {"task": "merge", "min_files": 2},
    "compress": {"task": "compress", "props": {
        "level": _enum(["extreme", "recommended", "less"], "recommended")}},
    "pdf-to-word": {"task": "convert", "props": {
        "ocr": _bool(False)}},
    "pdf-to-excel": {"task": "convert", "props": {
        "ocr": _bool(False)}},
    "pdf-to-ppt": {"task": "convert", "props": {
        "ocr": _bool(False)}},
    "word-to-pdf": {"task": "convert"},
    "excel-to-pdf": {"task": "convert"},
    "ppt-to-pdf": {"task": "convert"},
    "split": {"task": "pages", "op": "split", "props": {
        "ranges": _str(200), "every": _int(1, 5000, 1)}},
    "extract": {"task": "pages", "op": "extract", "props": {
        "pages": _str(200)}},
    "delete": {"task": "pages", "op": "delete", "props": {
        "pages": _str(200)}},
    "rotate": {"task": "pages", "op": "rotate", "props": {
        "angle": _enum(["90", "180", "270"], "90"), "pages": _str(200)}},
    "pdf-to-image": {"task": "media", "op": "pdf2img", "props": {
        "fmt": _enum(["png", "jpg"], "png"),
        "dpi": _enum(["72", "150", "300"], "150")}},
    "image-to-pdf": {"task": "media", "op": "img2pdf"},
    "scan": {"task": "scan", "props": {
        "fmt": _enum(["pdf", "images"], "pdf"),
        "mode": _enum(["color", "gray", "bw"], "color"),
        "dpi": _enum(["150", "200", "300"], "200"),
        "crop": _bool(True), "deskew": _bool(True), "flatten": _bool(True),
        "denoise": _bool(True), "sharpen": _bool(True)}},
    "idcard": {"task": "scan", "op": "idcard", "max_files": 2, "props": {
        "mode": _enum(["color", "gray", "bw"], "color"),
        "dpi": _enum(["150", "200", "300"], "200")}},
    "image-to-text": {"task": "ocr", "props": {
        "fmt": _enum(["txt", "docx"], "txt")}},
    "pdfa": {"task": "pdfa", "props": {
        "part": _enum(["2", "3"], "2")}},
    "watermark": {"task": "edit", "op": "watermark", "props": {
        "text": _str(120), "position": _str(40), "size": _int(6, 96, 36),
        "opacity": {"type": "number", "minimum": 0.05, "maximum": 1,
                    "default": 0.3}, "angle": _enum(["0", "30", "45", "60", "90"], "0"),
        "color": _str(20), "tile": _bool(False), "pages": _str(200)}},
    "redact": {"task": "edit", "op": "redact", "props": {
        "keywords": _str(500), "color": _str(20), "pages": _str(200)}},
    "annotate": {"task": "edit", "op": "annotate", "props": {
        "kind": _enum(["highlight", "underline", "strike", "squiggly",
                       "note", "freetext"], "highlight"),
        "keywords": _str(500), "note": _str(500), "author": _str(60),
        "color": _str(20), "pages": _str(200)}},
    "stamp": {"task": "edit", "op": "stamp", "min_files": 2, "max_files": 2,
              "props": {
                  "position": _str(40),
                  "scale": {"type": "number", "minimum": 0.05, "maximum": 0.6,
                            "default": 0.18},
                  "rotation": _enum(["0", "90", "180", "270"], "0"),
                  "opacity": {"type": "number", "minimum": 0.1, "maximum": 1,
                              "default": 1}, "pages": _str(200)}},
    "form": {"task": "edit", "op": "form", "props": {
        "fields": {"type": "object", "description": "AcroForm field name -> value"},
        "flatten": _bool(True), "pages": _str(200)}},
    "rewrite": {"task": "edit", "op": "rewrite", "props": {
        "pairs": {"type": "array", "items": {"type": "object",
                  "properties": {"find": _str(200), "replace": _str(200)},
                  "required": ["find"]}, "maxItems": 20},
        "pages": _str(200)}},
}


def _schema(spec):
    props = dict(spec.get("props") or {})
    props["name"] = _str(120)
    required = [k for k, v in (spec.get("props") or {}).items()
                if k in ("text", "pages", "keywords", "pairs", "fields")]
    s = {"type": "object", "properties": props, "additionalProperties": False}
    if required:
        s["required"] = required
    return s


def describe(name):
    """One capability as a plain dict, or None."""
    tool = T.get(name)
    spec = SPEC.get(name)
    if tool is None or spec is None:
        return None
    return {
        "id": name,
        "task": spec["task"],
        "op": spec.get("op"),
        "label": tool["label"],
        "description": tool.get("sub") or tool["title"],
        "src": tool["src"],
        "dst": tool["dst"],
        "accept": tool.get("accept") or [".pdf"],
        "pro": bool(tool.get("pro")),
        "min_files": spec.get("min_files", 1),
        "max_files": spec.get("max_files", 20),
        "input_schema": _schema(spec),
    }


def describe_all():
    return [describe(i) for i in T.ORDER if describe(i) is not None]


def get(name):
    return describe(name)
