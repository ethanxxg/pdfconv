"""Vector-figure preservation: embed SVG figures with a PNG fallback.

Aspose keeps some vector content as vectors; rasterising every figure was the
last big fidelity gap.  Word 2016+ renders SVG natively when the picture's
blip carries an ``asvg:svgBlip`` extension next to the regular PNG blip, and
older consumers silently fall back to the PNG.  So a vector-only figure is
shipped as both: crisp geometry (SVG, text converted to paths -- no font
dependency) plus the 200-dpi PNG we always made.

The engine role stays the same as everywhere else: MuPDF turns a clipped page
region into SVG; everything above it -- deciding *which* region, wiring the
part into the OOXML package, the fallback chain -- is this project's code.
"""
import re
import zipfile

# python-docx dedupes images by content hash and hands out one rId per part;
# one entry here per picture we augmented: blip rId -> svg bytes
_SVG_REFS = {}


def reset():
    _SVG_REFS.clear()


def has_raster(page, rect):
    """True when a bitmap image sits inside *rect*.

    A region containing a raster is already pixel-based; wrapping it in SVG
    would just base64 the pixels.  Only genuinely vector regions benefit.
    """
    r = (rect.x0 - 2, rect.y0 - 2, rect.x1 + 2, rect.y1 + 2)
    try:
        d = page.get_text("dict", clip=r)
    except Exception:
        return False
    return any(b.get("type") == 1 for b in d.get("blocks", ()))


def export_svg(side_doc, pno0, rect, max_bytes=4_000_000):
    """SVG bytes for a page region, or None.

    ``side_doc`` is a private handle on the same PDF: exporting needs the
    page's crop box moved, and mutating the working document mid-flight would
    corrupt every later crop/render on it.  Text becomes paths, so the SVG
    carries no font dependency.  Oversized output (> *max_bytes*, a few dense
    drawings explode into megabytes of paths) falls back to PNG.
    """
    try:
        pg = side_doc[pno0]
        keep = pg.cropbox
        pg.set_cropbox(rect)
        svg = pg.get_svg_image(text_as_path=True)
        pg.set_cropbox(keep)
        if not svg or len(svg.encode("utf8")) > max_bytes:
            return None
        return svg.encode("utf8")
    except Exception:
        return None


def register(run, svg_bytes):
    """Remember the SVG for the picture this run just added."""
    try:
        m = re.search(r'r:embed="(rId\d+)"', run._r.xml)
    except Exception:
        return
    if m:
        _SVG_REFS[m.group(1)] = svg_bytes


_BLIP = re.compile(r'(<a:blip\b[^>]*r:embed="(rId\d+)"[^>]*?)(/>|></a:blip>)')
_EXT = ('<a:extLst><a:ext uri="{96DAC541-7B7A-43D3-8B79-37D633B846F1}">'
        '<asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/'
        'drawing/2016/SVG/main" r:embed="%s"/></a:ext></a:extLst>')


def apply(docx_path):
    """Rewrite the saved package: add SVG parts and hook them into their blips.

    PNG stays the primary blip; the SVG rides in an ``svgBlip`` extension, so
    consumers that do not know the extension simply render the PNG.
    """
    if not _SVG_REFS:
        return 0
    with zipfile.ZipFile(docx_path) as z:
        parts = {n: z.read(n) for n in z.namelist()}

    rels = parts["word/_rels/document.xml.rels"].decode("utf8")
    body = parts["word/document.xml"].decode("utf8")
    ctypes = parts["[Content_Types].xml"].decode("utf8")

    # target of each registered blip rId, and the highest image index in use
    rid_target = dict(re.findall(
        r'<Relationship[^>]*Id="(rId\d+)"[^>]*Target="(media/[^"]+)"', rels))
    idx = [0]
    for mm in re.finditer(r'media/image(\d+)\.\w+', rels):
        idx.append(int(mm.group(1)))
    used_rids = [int(x) for x in re.findall(r'Id="rId(\d+)"', rels)]

    n_added = 0
    for blip_rid, svg in _SVG_REFS.items():
        if blip_rid not in rid_target:
            continue                        # picture part vanished; skip
        if blip_rid not in body:
            continue
        idx.append(idx[-1] + 1)
        svg_rid = "rId%d" % (max(used_rids) + 1)
        used_rids.append(max(used_rids) + 1)
        target = "media/image%d.svg" % idx[-1]

        rels = rels.replace("</Relationships>",
                            '<Relationship Id="%s" Type="http://schemas.'
                            'openxmlformats.org/officeDocument/2006/'
                            'relationships/image" Target="%s"/>'
                            "</Relationships>" % (svg_rid, target))
        parts["word/media/image%d.svg" % idx[-1]] = svg
        body = _BLIP.sub(
            lambda m: (m.group(1) + ">" + _EXT % svg_rid
                       + "</a:blip>") if m.group(2) == blip_rid
            else m.group(0), body, count=0)
        n_added += 1

    if 'Extension="svg"' not in ctypes:
        ctypes = ctypes.replace(
            "</Types>",
            '<Default Extension="svg" ContentType="image/svg+xml"/></Types>')

    parts["word/_rels/document.xml.rels"] = rels.encode("utf8")
    parts["word/document.xml"] = body.encode("utf8")
    parts["[Content_Types].xml"] = ctypes.encode("utf8")

    import os
    tmp = docx_path + ".svgtmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for n, data in parts.items():
            z.writestr(n, data)
    os.replace(tmp, docx_path)
    return n_added
