"""PDF editing operations for the job runner.

Six operations, one input PDF, one output PDF:

    watermark -> text (incl. {page}/{total} headers and footers), tiled or
                 single, with CJK support, opacity and diagonal rotation
    redact    -> TRUE redaction: the glyphs are removed from the content
                 stream, not covered by a black rectangle
    annotate  -> highlight / underline / strike / squiggly / sticky note /
                 free text, anchored to searched phrases or explicit boxes
    stamp     -> overlay a JPG/PNG (a seal, a signature) with real alpha
    form      -> fill AcroForm widgets and optionally bake them flat
    rewrite   -> literal find/replace inside the existing text boxes, reusing
                 pdfconv's in-place translation kernel so nothing moves

Runs in a process pool: importable with no side effects, only picklable
arguments, and every refusal is an EditError with a stable code.

``options`` is a plain validated dict rather than a dozen positional arguments
-- the six ops have almost no shared parameters beyond the page spec, and a
dict keeps the dispatch in jobs.py from growing an arm per operation.
"""
import io
import os
import re

import pymupdf as fitz
from PIL import Image

from pdfconv import measure
from pdfconv import pages as P
from pdfconv.translate_inplace import rewrite_pdf_inplace

MAX_PAGES = 5000

OPS = ("watermark", "redact", "annotate", "stamp", "form", "rewrite")
POSITIONS = ("tl", "tc", "tr", "ml", "mc", "mr", "bl", "bc", "br", "center")
ANNOT_KINDS = ("highlight", "underline", "strike", "squiggly", "note",
               "freetext")
ANGLES = (0, 30, 45, 60, 90)
_HEX = re.compile(r"^#?([0-9a-fA-F]{6})$")


class EditError(Exception):
    """A refusal or failure with a stable code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; the default
        # Exception pickle is cls(*self.args) == cls(message) -- one argument
        # short -- so without this the code is lost to TypeError.
        return (self.__class__, (self.code, self.message))


# -- shared validation ----------------------------------------------------

def _check_pdf(src, limit):
    try:
        doc = fitz.open(src)
    except Exception:
        raise EditError("bad_pdf", "文件无法读取，请确认是有效的 PDF")
    try:
        if doc.needs_pass:
            raise EditError("encrypted", "文件已加密，请先解除密码")
        total = doc.page_count
        if not total:
            raise EditError("empty", "文件没有任何页面")
        if total > limit:
            raise EditError("too_many_pages",
                            "文件共 %d 页，最多支持 %d 页" % (total, limit))
    finally:
        doc.close()
    return total


def _select(spec, total):
    """'1,3,5-7' -> sorted 0-based page numbers; empty means all pages."""
    s = str(spec or "").strip()
    if not s:
        return list(range(total))
    try:
        picked = P.parse_pages(s, total)
    except Exception:
        raise EditError("bad_pages", "页码格式不正确，例如 1,3,5-7")
    if not picked:
        raise EditError("bad_pages", "没有有效页码（1–%d）" % total)
    return [p - 1 for p in sorted(picked)]


def _color(value, default=(1.0, 0.0, 0.0)):
    if not value:
        return default
    m = _HEX.match(str(value).strip())
    if not m:
        raise EditError("bad_color", "颜色格式不正确，应为 #RRGGBB")
    h = m.group(1)
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def _number(value, lo, hi, default, code, label):
    try:
        v = float(value if value not in (None, "") else default)
    except (TypeError, ValueError):
        raise EditError(code, "%s必须是数字" % label)
    if not (lo <= v <= hi):
        raise EditError(code, "%s只能在 %s–%s 之间" % (label, lo, hi))
    return v


def _anchor(rect, position, w, h, margin=28.0):
    """Place a w x h box on a 3x3 grid inside *rect*."""
    # Two-letter positions: first char = row (t/m/b), second = column (l/m/r).
    # "center" is an alias for the middle cell.
    if position in POSITIONS and position != "center":
        row, col = position[0], position[1]
    else:
        row, col = "m", "m"
    x0, y0, x1, y1 = rect.x0, rect.y0, rect.x1, rect.y1
    if col == "l":
        x = x0 + margin
    elif col == "r":
        x = x1 - margin - w
    else:
        x = x0 + (rect.width - w) / 2.0
    if row == "t":
        y = y0 + margin
    elif row == "b":
        y = y1 - margin - h
    else:
        y = y0 + (rect.height - h) / 2.0
    return fitz.Rect(x, y, x + w, y + h)


def _font_for(text):
    """Droid Sans Fallback carries CJK and Latin; Helvetica is lighter."""
    return fitz.Font("china-s" if measure.has_cjk(text) else "helv")


def _box_page(box):
    """Which page a normalised box belongs to; 0 means every selected page."""
    try:
        return int(box.get("page") or 0)
    except (TypeError, ValueError):
        raise EditError("bad_rect", "框选区域的页码不正确")


def _rects_from_normalized(page, boxes):
    """Normalised [x, y, w, h] in 0..1 -> page-space Rects."""
    out = []
    r = page.rect
    for b in boxes or []:
        try:
            x, y, w, h = (float(b[k]) for k in ("x", "y", "w", "h"))
        except (KeyError, TypeError, ValueError):
            raise EditError("bad_rect", "框选区域格式不正确")
        if not (0 <= x <= 1 and 0 <= y <= 1 and 0 < w <= 1 and 0 < h <= 1):
            raise EditError("bad_rect", "框选区域超出页面范围")
        out.append(fitz.Rect(r.x0 + x * r.width, r.y0 + y * r.height,
                             r.x0 + (x + w) * r.width,
                             r.y0 + (y + h) * r.height))
    return out


def _save(doc, dst):
    try:
        doc.save(dst, garbage=3, deflate=True)
    finally:
        doc.close()


# -- watermark / page numbers ---------------------------------------------

def _watermark(doc, sel, o, total):
    text = str(o.get("text") or "").strip()
    if not text:
        raise EditError("bad_text", "请填写水印文字")
    if len(text) > 120:
        raise EditError("bad_text", "水印文字最多 120 个字符")
    position = str(o.get("position") or "center").lower()
    if position not in POSITIONS:
        raise EditError("bad_position", "水印位置无效")
    size = _number(o.get("size"), 6, 96, 36, "bad_size", "字号")
    opacity = _number(o.get("opacity"), 0.05, 1.0, 0.3,
                      "bad_opacity", "透明度")
    try:
        angle = int(o.get("angle") or 0)
    except (TypeError, ValueError):
        angle = -1
    if angle not in ANGLES:
        raise EditError("bad_angle", "水印角度只能是 0 / 30 / 45 / 60 / 90")
    color = _color(o.get("color"))
    tile = bool(o.get("tile"))

    for pno in sel:
        page = doc[pno]
        label = (text.replace("{page}", str(pno + 1))
                     .replace("{total}", str(total)))
        font = _font_for(label)
        tw = fitz.TextWriter(page.rect, color=color)
        if tile:
            # One writer, one rotation about the page centre.  The grid
            # over-covers the page so the rotated rows still reach the corners.
            step_x = max(150.0, font.text_length(label, fontsize=size) + 90)
            step_y = max(90.0, size * 4.0)
            r = page.rect
            xs = [r.x0 - r.width * 0.5 + i * step_x
                  for i in range(int(2 * r.width / step_x) + 3)]
            ys = [r.y0 - r.height * 0.5 + j * step_y
                  for j in range(int(2 * r.height / step_y) + 3)]
            for y in ys:
                for x in xs[:60]:
                    tw.append((x, y), label, font=font, fontsize=size)
            pivot = fitz.Point(r.x0 + r.width / 2, r.y0 + r.height / 2)
            morph = (pivot, fitz.Matrix(angle or 30))
        else:
            w = font.text_length(label, fontsize=size)
            box = _anchor(page.rect, position, w, size * 1.25)
            tw.append((box.x0, box.y0 + size * 0.82), label,
                      font=font, fontsize=size)
            pivot = fitz.Point(box.x0 + w / 2, box.y0 + size / 2)
            morph = (pivot, fitz.Matrix(angle)) if angle else None
        tw.write_text(page, color=color, opacity=opacity, morph=morph)
    return {"op": "watermark", "pages": len(doc), "touched": len(sel),
            "tile": tile}


# -- true redaction --------------------------------------------------------

def _redact(doc, sel, o):
    keywords = [str(k).strip() for k in (o.get("keywords") or []) if str(k).strip()]
    boxes = o.get("rects") or []
    if not keywords and not boxes:
        raise EditError("bad_keywords", "请填写要抹掉的文字或框选区域")
    if len(keywords) > 50 or len(boxes) > 200:
        raise EditError("bad_keywords", "一次最多 50 个关键词 / 200 个区域")
    fill = _color(o.get("color"), (0.0, 0.0, 0.0))
    areas = 0
    for pno in sel:
        page = doc[pno]
        hits = []
        for kw in keywords:
            try:
                hits.extend(page.search_for(kw) or [])
            except Exception:
                continue
        # Normalised boxes are grouped per page; a missing / zero page means
        # "every selected page", which is what a signature block needs.
        page_boxes = []
        for b in boxes:
            if _box_page(b) in (0, pno + 1):
                page_boxes.append(b)
        hits.extend(_rects_from_normalized(page, page_boxes))
        if not hits:
            continue
        for r in hits:
            page.add_redact_annot(r, fill=fill, cross_out=False)
        # fill=False would leave the glyphs in place -- that is the exact bug
        # that makes "black box" redaction tools leak the original text.
        page.apply_redactions(images=fitz.PDF_REDACT_IMAGE_NONE,
                              graphics=fitz.PDF_REDACT_LINE_ART_NONE)
        areas += len(hits)
    if not areas:
        raise EditError("no_match", "没有找到要抹掉的文字，请检查关键词")
    return {"op": "redact", "pages": len(doc), "areas": areas,
            "touched": len(sel)}


# -- annotations -----------------------------------------------------------

_ADDERS = {
    "highlight": "add_highlight_annot",
    "underline": "add_underline_annot",
    "strike": "add_strikeout_annot",
    "squiggly": "add_squiggly_annot",
}


def _annotate(doc, sel, o):
    kind = str(o.get("kind") or "highlight").lower()
    if kind not in ANNOT_KINDS:
        raise EditError("bad_kind", "未知的批注类型")
    keywords = [str(k).strip() for k in (o.get("keywords") or []) if str(k).strip()]
    note = str(o.get("note") or "").strip()
    if kind in ("note", "freetext") and not note:
        raise EditError("bad_note", "请填写批注内容")
    if kind not in ("note", "freetext") and not keywords and not (o.get("rects")):
        raise EditError("bad_keywords", "请填写要标注的文字或框选区域")
    color = _color(o.get("color"), (1.0, 0.85, 0.0))
    author = str(o.get("author") or "pdfkit")[:60]
    count = 0
    for pno in sel:
        page = doc[pno]
        hits = []
        for kw in keywords:
            try:
                hits.extend(page.search_for(kw) or [])
            except Exception:
                continue
        page_boxes = [b for b in (o.get("rects") or [])
                      if _box_page(b) in (0, pno + 1)]
        rects = _rects_from_normalized(page, page_boxes)
        if kind in _ADDERS:
            targets = hits + rects
            adder = getattr(page, _ADDERS[kind])
            for r in targets:
                a = adder(r)
                a.set_colors(stroke=color)
                if note:
                    a.set_info(content=note[:500], title=author)
                a.update()
                count += 1
        elif kind == "note":
            for r in (rects or hits[:1] or
                      [_anchor(page.rect, "tr", 20, 20)]):
                a = page.add_text_annot(fitz.Point(r.x0, r.y0), note[:500])
                a.set_info(title=author)
                a.update()
                count += 1
        else:  # freetext
            for r in (rects or [_anchor(page.rect, "tr", 220, 48)]):
                a = page.add_freetext_annot(
                    r, note[:500], fontsize=11, fontname="china-s",
                    text_color=(0, 0, 0), fill_color=(1, 1, 0.85))
                a.set_info(title=author)
                a.update()
                count += 1
    if not count:
        raise EditError("no_match", "没有找到要标注的文字，请检查关键词")
    return {"op": "annotate", "pages": len(doc), "annots": count,
            "touched": len(sel)}


# -- seal / image overlay --------------------------------------------------

def _stamp(doc, sel, o, image_path):
    if not image_path or not os.path.exists(image_path):
        raise EditError("bad_image", "请上传印章图片")
    try:
        with open(image_path, "rb") as f:
            raw = f.read()
        im = Image.open(io.BytesIO(raw))
        im.load()
    except Exception:
        raise EditError("bad_image", "印章图片无法读取，只支持 JPG / PNG")
    if im.format not in ("JPEG", "PNG"):
        raise EditError("bad_image", "印章图片只支持 JPG / PNG")
    position = str(o.get("position") or "br").lower()
    if position not in POSITIONS:
        raise EditError("bad_position", "盖章位置无效")
    scale = _number(o.get("scale"), 0.05, 0.6, 0.18, "bad_scale", "印章大小")
    opacity = _number(o.get("opacity"), 0.1, 1.0, 1.0,
                      "bad_opacity", "透明度")
    try:
        rotation = int(o.get("rotation") or 0)
    except (TypeError, ValueError):
        rotation = -1
    if rotation not in (0, 90, 180, 270):
        raise EditError("bad_angle", "印章角度只能是 0 / 90 / 180 / 270")

    rgba = im.convert("RGBA")
    if opacity < 1.0:
        alpha = rgba.getchannel("A").point(lambda a: int(a * opacity))
        rgba.putalpha(alpha)
    if rotation:
        rgba = rgba.rotate(rotation, expand=True)
    buf = io.BytesIO()
    rgba.save(buf, format="PNG")
    stream = buf.getvalue()
    ratio = rgba.height / float(rgba.width or 1)

    for pno in sel:
        page = doc[pno]
        w = page.rect.width * scale
        h = w * ratio
        box = _anchor(page.rect, position, w, h)
        page.insert_image(box, stream=stream, overlay=True,
                          keep_proportion=True)
    return {"op": "stamp", "pages": len(doc), "touched": len(sel)}


# -- AcroForm --------------------------------------------------------------

def form_fields(src):
    """Read-only widget inventory; used by the UI before it can render a form."""
    doc = fitz.open(src)
    try:
        if doc.needs_pass:
            raise EditError("encrypted", "文件已加密，请先解除密码")
        out = []
        for pno, page in enumerate(doc):
            for w in page.widgets() or ():
                out.append({
                    "name": w.field_name or "",
                    "type": int(w.field_type or 0),
                    "type_name": w.field_type_string or "",
                    "value": w.field_value,
                    "options": list(w.choice_values or []),
                    "page": pno + 1,
                })
        return out
    finally:
        doc.close()


def _form(doc, sel, o):
    fields = o.get("fields") or {}
    if not isinstance(fields, dict) or not fields:
        raise EditError("bad_fields", "请填写表单内容")
    if len(fields) > 200:
        raise EditError("bad_fields", "一次最多填写 200 个字段")
    flatten = bool(o.get("flatten", True))
    found, warnings, seen = 0, [], set()
    for pno, page in enumerate(doc):
        # Consume the widget generator directly: materialising annotation
        # handles into a list invalidates them in PyMuPDF (see
        # pdfconv/annots.py), and widgets behave the same way.
        for w in page.widgets() or ():
            name = w.field_name
            if not name:
                continue
            seen.add(name)
            if name not in fields or pno not in sel:
                continue
            value = fields[name]
            try:
                if w.field_type == fitz.PDF_WIDGET_TYPE_CHECKBOX:
                    w.field_value = (bool(value) if not isinstance(value, str)
                                     else value.strip().lower()
                                     in ("1", "true", "yes", "on", "是"))
                else:
                    w.field_value = str(value)
                w.update()
                found += 1
            except Exception as e:
                warnings.append("%s：%s" % (name, str(e)[:80]))
    if not found:
        raise EditError("no_fields", "没有找到可填写的表单域，请确认文件是 PDF 表单")
    missing = [k for k in fields if k not in seen]
    if flatten:
        # bake() turns widget appearances into ordinary page content, so the
        # values survive readers that ignore AcroForm and cannot be edited away.
        doc.bake()
    return {"op": "form", "pages": len(doc), "filled": found,
            "flattened": flatten, "missing": missing[:20],
            "warnings": warnings[:10]}


# -- in-place text rewrite -------------------------------------------------

def _rewrite(src, dst, sel, o):
    pairs = []
    for item in (o.get("pairs") or []):
        if not isinstance(item, dict):
            continue
        find = str(item.get("find") or "")
        repl = str(item.get("replace") or "")
        if not find:
            continue
        if len(find) > 200 or len(repl) > 200:
            raise EditError("bad_pairs", "单条替换内容最多 200 个字符")
        pairs.append((find, repl))
    if not pairs:
        raise EditError("bad_pairs", "请填写要查找和替换的文字")
    if len(pairs) > 20:
        raise EditError("bad_pairs", "一次最多 20 条替换规则")
    report, total = rewrite_pdf_inplace(src, dst, pairs, pages=sel)
    if not report["rewritten"]:
        if os.path.exists(dst):
            os.remove(dst)
        raise EditError("no_match", "没有找到要替换的文字，请检查原文")
    return {"op": "rewrite", "pages": total, "touched": len(sel),
            "rewritten": report["rewritten"], "boxes": report["boxes"],
            "overflow": report["overflow"],
            "warning": ("有 %d 处文字替换后放不进原来的文本框，已保留原文"
                        % report["overflow"]) if report["overflow"] else None}


# -- entry point -----------------------------------------------------------

def edit_task(paths, dst, op, options=None, max_pages=None):
    """Run one edit operation.  Returns a report dict.

    ``paths[0]`` is the source PDF; ``paths[1]``, when present, is the seal
    image for the stamp operation.  Uploads have no file extension, which is
    why the image is opened by content rather than by name.
    """
    if op not in OPS:
        raise EditError("bad_op", "未知的编辑操作")
    o = options or {}
    src = paths[0]
    total = _check_pdf(src, int(max_pages or MAX_PAGES))
    sel = _select(o.get("pages"), total)

    if op == "rewrite":
        return _rewrite(src, dst, sel, o)

    doc = fitz.open(src)
    try:
        if op == "watermark":
            report = _watermark(doc, sel, o, total)
        elif op == "redact":
            report = _redact(doc, sel, o)
        elif op == "annotate":
            report = _annotate(doc, sel, o)
        elif op == "stamp":
            report = _stamp(doc, sel, o,
                            paths[1] if len(paths) > 1 else None)
        else:
            report = _form(doc, sel, o)
    except EditError:
        doc.close()
        if os.path.exists(dst):
            try:
                os.remove(dst)
            except OSError:
                pass
        raise
    _save(doc, dst)
    return report
