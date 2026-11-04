"""PDF -> PDF/A-2b / PDF/A-3b for the job runner.

Why this is hand-rolled: PyMuPDF has no PDF/A writer, and neither Ghostscript
nor pikepdf exists on the deploy target, so the conformance work is done with
the primitives MuPDF *does* expose -- font subsetting, XMP metadata, and raw
xref edits for the colour output intent.

What ISO 19005 actually asks for, and what this module does about it:

  fonts embedded ....... ``subset_fonts()`` embeds and subsets every font whose
                         font file it can reach.  A base-14 font (Helvetica,
                         Courier, ...) has no font file anywhere, so it cannot
                         be embedded; those are **reported** rather than quietly
                         passed off as conformant.
  colour ............... a DeviceRGB document needs an OutputIntent, so an sRGB
                         ICC profile is embedded and referenced from the
                         catalog.  Without it VeraPDF rejects the file.
  metadata ............. XMP carrying ``pdfaid:part`` / ``pdfaid:conformance``.
  forbidden content .... JavaScript, Launch / Sound / Movie / Rendition actions
                         and form XFA are stripped; embedded files are dropped
                         for PDF/A-2 (PDF/A-3 exists precisely to allow them).
  no encryption ........ refused up front -- an encrypted file cannot conform.

Runs inside the process pool, so it must import cleanly with no side effects and
take only picklable arguments.
"""
import os
import re
import xml.sax.saxutils as sax

import pymupdf as fitz

MAX_PAGES = 5000
# Only level B (visual fidelity) is offered: level A additionally needs correct
# tagging and a logical structure tree, which a converter cannot invent for an
# arbitrary input PDF.
PARTS = {"2": "B", "3": "B"}

# Action types ISO 19005-2 forbids outright.  /GoTo, /GoToR, /URI and /Named
# stay: links and bookmarks are perfectly legal in an archival PDF.
_BAD_ACTION = {"/JavaScript", "/Launch", "/Sound", "/Movie", "/ResetForm",
               "/ImportData", "/SetOCGState", "/Rendition", "/Trans",
               "/GoTo3DView"}

_ANNOT_REF = re.compile(r"(\d+)\s+0\s+R")

_XMP_TMPL = """<?xpacket begin="\ufeff" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/"
    xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/">
   %s
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>"""


class PdfaError(Exception):
    """A refusal or failure with a stable code the API can branch on."""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message

    def __reduce__(self):
        # Raised in a pool worker, re-raised in the web process; without this
        # the code is lost to a TypeError during unpickling (see convert.py).
        return (self.__class__, (self.code, self.message))


# -- low-level object surgery ---------------------------------------------
def _xref_of(doc, xref, key):
    """Follow *key* to another object, or None when it is not an indirect ref."""
    try:
        kind, val = doc.xref_get_key(xref, key)
    except Exception:
        return None
    if kind != "xref":
        return None
    try:
        return int(val.split()[0])
    except (ValueError, IndexError):
        return None


def _drop(doc, xref, key):
    """Delete *key* from object *xref*; 'null' is how PyMuPDF removes a key."""
    try:
        if doc.xref_get_key(xref, key)[0] == "null":
            return False
        doc.xref_set_key(xref, key, "null")
        return True
    except Exception:
        # A malformed object is not worth failing the whole conversion over --
        # the audit at the end still reports whatever survived.
        return False


def _squeeze(doc, xref, key, dropped):
    """Rewrite a direct dict so the ``/dropped null`` litter is gone.

    Only the exact token just nulled is removed, so a string literal elsewhere
    in the dict that happens to contain the same text is never touched.  A dict
    that this empties entirely is deleted: ``<< /JavaScript null >>`` is legal
    (a null entry reads as absent) but it is litter in a file meant to last
    decades.
    """
    try:
        kind, val = doc.xref_get_key(xref, key)
    except Exception:
        return
    if kind != "dict" or not val:
        return
    token = "/%s null" % dropped
    if token not in val:
        return
    cleaned = val.replace(token, "")
    if not cleaned.replace("<<", "").replace(">>", "").strip():
        _drop(doc, xref, key)
        return
    try:
        doc.xref_set_key(xref, key, cleaned)
    except Exception:
        pass


def _drop_path(doc, xref, path):
    """Delete *path* such as ``Names/JavaScript`` from object *xref*.

    Real PDFs use both layouts: an indirect dict (delete the key from that
    object -- exact) and a direct one (PyMuPDF's key path, which leaves
    ``/Key null`` behind, hence the squeeze).
    """
    head, _, tail = path.partition("/")
    if not tail:
        return _drop(doc, xref, path)
    sub = _xref_of(doc, xref, head)
    if sub is not None:
        return _drop_path(doc, sub, tail)
    try:
        if doc.xref_get_key(xref, path)[0] == "null":
            return False
        doc.xref_set_key(xref, path, "null")
    except Exception:
        return False
    _squeeze(doc, xref, head, tail)
    return True


def _action_subtype(doc, xref, path):
    """The /S of the action at *path* ('/JavaScript', '/GoTo', ...), or None."""
    try:
        kind, val = doc.xref_get_key(xref, path + "/S")
    except Exception:
        return None
    return val if kind == "name" else None


def _strip_forbidden(doc, allow_embedded_files):
    """Remove everything ISO 19005 refuses.  Returns a list of what went."""
    removed = []
    cat = doc.pdf_catalog()

    # Document-level scripting.  A plain /OpenAction destination ("open at page
    # 3") is legal and is left alone -- only a forbidden action goes.
    if _action_subtype(doc, cat, "OpenAction") in _BAD_ACTION:
        if _drop_path(doc, cat, "OpenAction"):
            removed.append("OpenAction")
    _drop_path(doc, cat, "AA")

    if _drop_path(doc, cat, "Names/JavaScript"):
        removed.append("JavaScript")
    if not allow_embedded_files and _drop_path(doc, cat, "Names/EmbeddedFiles"):
        removed.append("EmbeddedFiles")

    # XFA is forbidden in every part; NeedAppearances is a viewer hint that
    # archival readers are not required to honour, so it must not be set.
    if _drop_path(doc, cat, "AcroForm/XFA"):
        removed.append("XFA")
    _drop_path(doc, cat, "AcroForm/NeedAppearances")

    for pno in range(doc.page_count):
        px = doc.page_xref(pno)
        _drop(doc, px, "AA")
        kind, val = doc.xref_get_key(px, "Annots")
        if kind != "array":
            continue
        for ref in _ANNOT_REF.findall(val or ""):
            ax = int(ref)
            _drop(doc, ax, "AA")
            sub = _action_subtype(doc, ax, "A")
            if sub in _BAD_ACTION:
                _drop(doc, ax, "A")
                name = sub.lstrip("/")
                if name not in removed:
                    removed.append(name)

    return removed


_ICC = None


def _icc_profile():
    """sRGB ICC bytes, generated once per worker process.

    Pillow is already a dependency for image compression, and lcms builds a
    compact but valid sRGB profile from it -- no 3 MB ICC file to ship.
    """
    global _ICC
    if _ICC is None:
        from PIL import ImageCms
        _ICC = ImageCms.ImageCmsProfile(
            ImageCms.createProfile("sRGB")).tobytes()
    return _ICC


def _add_output_intent(doc):
    """Embed an sRGB profile and point the catalog at it.

    This is the step most hand-rolled "PDF/A" writers skip, and it is the first
    thing a validator complains about: without /OutputIntents a document that
    uses DeviceRGB has no defined colour space.
    """
    icc = _icc_profile()

    prof = doc.get_new_xref()
    doc.update_object(prof, "<<>>")
    doc.update_stream(prof, icc, compress=True)
    doc.xref_set_key(prof, "N", "3")
    doc.xref_set_key(prof, "Alternate", "/DeviceRGB")

    intent = doc.get_new_xref()
    doc.update_object(intent, "<<>>")
    doc.xref_set_key(intent, "Type", "/OutputIntent")
    doc.xref_set_key(intent, "S", "/GTS_PDFA1")
    doc.xref_set_key(intent, "OutputConditionIdentifier",
                     "(sRGB IEC61966-2.1)")
    doc.xref_set_key(intent, "Info", "(sRGB IEC61966-2.1)")
    doc.xref_set_key(intent, "RegistryName", "(http://www.color.org)")
    doc.xref_set_key(intent, "DestOutputProfile", "%d 0 R" % prof)
    doc.xref_set_key(doc.pdf_catalog(), "OutputIntents", "[%d 0 R]" % intent)


def _xmp(doc, part, conf):
    meta = doc.metadata or {}
    title = (meta.get("title") or "").strip()
    bits = [
        "<pdfaid:part>%s</pdfaid:part>" % part,
        "<pdfaid:conformance>%s</pdfaid:conformance>" % conf,
        "<dc:format>application/pdf</dc:format>",
    ]
    if title:
        bits.append('<dc:title><rdf:Alt><rdf:li xml:lang="x-default">%s'
                    "</rdf:li></rdf:Alt></dc:title>" % sax.escape(title))
    return _XMP_TMPL % "\n   ".join(bits)


# -- audit ----------------------------------------------------------------
def _embedded(doc, xref, ext):
    """Is this font's data actually inside the file?

    A Type0 font keeps its descriptor on the descendant CIDFont, so both the
    font object and its descendants are checked before falling back to the
    extension MuPDF reports ('n/a' means "not embedded").
    """
    seen = []
    for obj in (xref,):
        if not obj:
            continue
        seen.append(obj)
        desc = _xref_of(doc, obj, "DescendantFonts")
        if desc is not None:
            kind, val = doc.xref_get_key(desc, "DescendantFonts")
            if kind == "array":
                for ref in _ANNOT_REF.findall(val or ""):
                    seen.append(int(ref))

    for obj in seen:
        fd = _xref_of(doc, obj, "FontDescriptor")
        if fd is None:
            continue
        for k in ("FontFile", "FontFile2", "FontFile3"):
            if doc.xref_get_key(fd, k)[0] != "null":
                return True
        return False
    return str(ext or "n/a") not in ("n/a", "", "None")


def _font_report(doc):
    """(references, embedded references, [names that are not embedded])."""
    total = emb = 0
    missing = []
    for pno in range(doc.page_count):
        try:
            fonts = doc.get_page_fonts(pno, full=True)
        except Exception:
            continue
        for f in fonts:
            total += 1
            if _embedded(doc, f[0], f[1]):
                emb += 1
            elif f[3] not in missing:
                missing.append(f[3])
    return total, emb, missing


# -- task -----------------------------------------------------------------
def pdfa_task(src, dst, part="2", max_pages=None):
    """Convert *src* to a PDF/A file at *dst*.

    Returns a report the result page can render: the part/conformance, the font
    audit, what was stripped, and any warning worth showing the user.
    """
    part = str(part or "2").strip()
    if part not in PARTS:
        raise PdfaError("bad_part", "PDF/A 版本只能是 2b 或 3b")
    conf = PARTS[part]
    limit = int(max_pages or MAX_PAGES)

    doc = fitz.open(src)
    try:
        if doc.needs_pass:
            raise PdfaError("encrypted", "文件已加密，请先解除密码")
        pages = doc.page_count
        if pages == 0:
            raise PdfaError("empty", "文件没有任何页面")
        if pages > limit:
            raise PdfaError("too_many_pages",
                            "文件共 %d 页，最多支持 %d 页" % (pages, limit))

        removed = _strip_forbidden(doc, allow_embedded_files=(part == "3"))

        # Subsetting shrinks the file and proves the font data is reachable.
        # A font that cannot be loaded is left alone and shows up in the audit.
        try:
            doc.subset_fonts()
        except Exception:
            pass

        _add_output_intent(doc)
        doc.set_xml_metadata(_xmp(doc, part, conf))

        try:
            doc.save(dst, garbage=4, deflate=True, clean=True)
        except Exception:
            raise PdfaError("convert_failed", "写入 PDF/A 失败，请重试")
    finally:
        doc.close()

    if not os.path.exists(dst) or os.path.getsize(dst) == 0:
        raise PdfaError("convert_failed", "转换没有产生文件，请重试")

    # Audit the bytes the user will actually receive, not the in-memory doc:
    # garbage collection renumbers objects, so only the saved file is truth.
    out = fitz.open(dst)
    try:
        total, emb, missing = _font_report(out)
        has_intent = out.xref_get_key(
            out.pdf_catalog(), "OutputIntents")[0] != "null"
        meta = out.get_xml_metadata() or ""
    finally:
        out.close()

    warnings = []
    if missing:
        warnings.append("%d 个字体无法嵌入（%s），严格校验可能不通过"
                        % (len(missing), "、".join(missing[:3])))
    if not has_intent:
        warnings.append("色彩描述（OutputIntent）未能写入")
    if "pdfaid:part" not in meta:
        warnings.append("XMP 归档标识未能写入")

    return {
        "op": "pdfa",
        "pages": pages,
        "part": part,
        # Display form is lowercase (PDF/A-2b); the XMP value must stay the
        # uppercase "B" the standard specifies.
        "conformance": "PDF/A-%s%s" % (part, conf.lower()),
        "fonts_total": total,
        "fonts_embedded": emb,
        "fonts_missing": missing[:8],
        "removed": removed,
        "warnings": warnings,
    }
