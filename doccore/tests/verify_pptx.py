"""Independent verifier for DocCore's PPTX reader/writer and its PDF export.

Deliberately shares no code with the C++ implementation: the package is parsed
with zipfile + ElementTree, the semantic checks use python-pptx, and the PDF is
parsed by verify_output.py (a from-scratch PDF parser).

    python verify_pptx.py all <source.pptx> <exported.pdf> <roundtrip.pptx>
    python verify_pptx.py package <file.pptx>
    python verify_pptx.py pdf <file.pdf> <source.pptx>
    python verify_pptx.py roundtrip <source.pptx> <roundtrip.pptx>

Exit code 0 on success, AssertionError (traceback) otherwise.
"""
import io
import re
import sys
import zipfile
from xml.etree import ElementTree as ET

HERE = __file__.rsplit("/", 1)[0].rsplit("\\", 1)[0]
sys.path.insert(0, HERE)

from verify_output import Pdf  # noqa: E402

NS = {
    "a": "http://schemas.openxmlformats.org/drawingml/2006/main",
    "p": "http://schemas.openxmlformats.org/presentationml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
    "ct": "http://schemas.openxmlformats.org/package/2006/content-types",
    "rel": "http://schemas.openxmlformats.org/package/2006/relationships",
}
EMU_PER_PT = 12700
TOLERANCE_PT = 2.0


# ---- 1. package structure ------------------------------------------------


def rels_path_for(part):
    head, _, tail = part.rpartition("/")
    return (head + "/" if head else "") + "_rels/" + tail + ".rels"


def check_package(path):
    """Zip integrity, XML well-formedness, content types, relationship targets."""
    zf = zipfile.ZipFile(path)
    bad = zf.testzip()
    assert bad is None, "corrupt zip entry: %s" % bad
    names = set(zf.namelist())

    # every XML part must parse
    for name in sorted(names):
        if name.endswith((".xml", ".rels")):
            try:
                ET.fromstring(zf.read(name))
            except ET.ParseError as exc:
                raise AssertionError("%s is not well-formed XML: %s" % (name, exc))

    # [Content_Types].xml must cover every part
    types = ET.fromstring(zf.read("[Content_Types].xml"))
    defaults = {
        e.get("Extension").lower()
        for e in types.findall("ct:Default", NS)
    }
    overrides = {e.get("PartName").lstrip("/") for e in types.findall("ct:Override", NS)}
    for name in sorted(names):
        if name == "[Content_Types].xml" or name.endswith("/"):
            continue
        ext = name.rsplit(".", 1)[-1].lower()
        assert name in overrides or ext in defaults, "no content type for %s" % name

    # every r:id / r:embed must resolve to an existing part
    unresolved = []
    for name in sorted(names):
        if not name.endswith(".xml"):
            continue
        body = zf.read(name).decode("utf-8", "replace")
        ids = set(re.findall(r'r:(?:id|embed|link)="([^"]+)"', body))
        if not ids:
            continue
        rels = rels_path_for(name)
        assert rels in names, "%s references %s but %s is missing" % (name, ids, rels)
        targets = {}
        for rel in ET.fromstring(zf.read(rels)).findall("rel:Relationship", NS):
            if rel.get("TargetMode") == "External":
                continue
            targets[rel.get("Id")] = rel.get("Target")
        for rid in sorted(ids):
            if rid not in targets:
                unresolved.append((name, rid))
            elif targets[rid].startswith("../") or targets[rid].startswith("/"):
                base = name.rsplit("/", 1)[0]
                resolved = resolve(base, targets[rid])
                if resolved not in names:
                    unresolved.append((name, rid, resolved))
    assert not unresolved, "unresolved relationships: %s" % unresolved[:5]

    # the three parts PowerPoint always needs
    pres = ET.fromstring(zf.read("ppt/presentation.xml"))
    size = pres.find("p:sldSz", NS)
    assert size is not None, "presentation.xml has no sldSz"
    assert pres.find("p:sldMasterIdLst", NS) is not None, "no slide master listed"
    ids = pres.findall("p:sldIdLst/p:sldId", NS)
    assert ids, "no slides listed"

    # theme must have all four 3-entry style lists or PowerPoint "repairs" the file
    theme_name = [n for n in names if n.startswith("ppt/theme/")][0]
    theme = ET.fromstring(zf.read(theme_name))
    fmt = theme.find("a:themeElements/a:fmtScheme", NS)
    assert fmt is not None, "theme has no fmtScheme"
    for list_name, child in (
        ("a:fillStyleLst", "a:solidFill"),
        ("a:lnStyleLst", "a:ln"),
        ("a:effectStyleLst", "a:effectStyle"),
        ("a:bgFillStyleLst", "a:solidFill"),
    ):
        lst = fmt.find(list_name, NS)
        assert lst is not None, "theme fmtScheme has no %s" % list_name
        assert len(list(lst)) == 3, "%s needs exactly 3 entries, has %d" % (
            list_name,
            len(list(lst)),
        )
    for name in ("a:clrScheme", "a:fontScheme"):
        assert theme.find("a:themeElements/" + name, NS) is not None, "theme has no " + name

    print(
        "  package ok: parts=%d slides=%d size=%sx%s"
        % (
            len(names),
            len(ids),
            int(size.get("cx")) // EMU_PER_PT,
            int(size.get("cy")) // EMU_PER_PT,
        )
    )
    return zf


def resolve(base, target):
    if target.startswith("/"):
        return target.lstrip("/")
    parts = base.split("/") if base else []
    for piece in target.split("/"):
        if piece == "..":
            parts.pop()
        elif piece and piece != ".":
            parts.append(piece)
    return "/".join(parts)


# ---- 2. semantic checks through python-pptx ------------------------------


def load_pptx(path):
    from pptx import Presentation

    return Presentation(path)


def shape_rects(slide):
    out = []
    for shape in slide.shapes:
        if shape.left is None or shape.width is None:
            continue
        out.append(
            (
                shape.name,
                shape.left / EMU_PER_PT,
                shape.top / EMU_PER_PT,
                shape.width / EMU_PER_PT,
                shape.height / EMU_PER_PT,
            )
        )
    return out


def check_roundtrip(source, roundtrip):
    """Same slide count, size, text and geometry after a DocCore save."""
    a = load_pptx(source)
    b = load_pptx(roundtrip)
    assert len(a.slides) == len(b.slides), "slide count %d -> %d" % (
        len(a.slides),
        len(b.slides),
    )
    assert (a.slide_width, a.slide_height) == (b.slide_width, b.slide_height), "slide size changed"

    for index, (sa, sb) in enumerate(zip(a.slides, b.slides), 1):
        ta = "".join(s.text_frame.text for s in sa.shapes if s.has_text_frame)
        tb = "".join(s.text_frame.text for s in sb.shapes if s.has_text_frame)
        ta = re.sub(r"\s+", "", ta)
        tb = re.sub(r"\s+", "", tb)
        assert ta == tb, "slide %d text changed:\n  %r\n  %r" % (index, ta, tb)

        ra = shape_rects(sa)
        rb = shape_rects(sb)
        assert len(ra) == len(rb), "slide %d shape count %d -> %d" % (index, len(ra), len(rb))
        for (na, xa, ya, wa, ha), (nb, xb, yb, wb, hb) in zip(ra, rb):
            for label, va, vb in (("x", xa, xb), ("y", ya, yb), ("w", wa, wb), ("h", ha, hb)):
                assert abs(va - vb) <= TOLERANCE_PT, (
                    "slide %d shape %r %s moved %.2f -> %.2f" % (index, na, label, va, vb)
                )

        ta_tables = [s for s in sa.shapes if s.has_table]
        tb_tables = [s for s in sb.shapes if s.has_table]
        assert len(ta_tables) == len(tb_tables), "slide %d table count changed" % index
        for xa, xb in zip(ta_tables, tb_tables):
            ga, gb = xa.table, xb.table
            assert (len(ga.rows), len(ga.columns)) == (len(gb.rows), len(gb.columns)), (
                "slide %d table shape changed" % index
            )
            for r in range(len(ga.rows)):
                for c in range(len(ga.columns)):
                    assert ga.cell(r, c).text == gb.cell(r, c).text, (
                        "slide %d cell (%d,%d) changed: %r -> %r"
                        % (index, r, c, ga.cell(r, c).text, gb.cell(r, c).text)
                    )

        na = sa.notes_slide.notes_text_frame.text if sa.has_notes_slide else ""
        nb = sb.notes_slide.notes_text_frame.text if sb.has_notes_slide else ""
        assert na.strip() == nb.strip(), "slide %d notes changed: %r -> %r" % (index, na, nb)

    print("  roundtrip ok: %d slides, text/geometry/tables/notes preserved" % len(a.slides))


# ---- 3. PDF export -------------------------------------------------------


def page_boxes(pdf):
    boxes = []
    for num in sorted(pdf.objects):
        body = pdf.object_body(num)
        if b"/Type /Page" not in body or b"/Type /Pages" in body:
            continue
        box = re.search(rb"/MediaBox \[([^\]]*)\]", body)
        assert box, "page %d has no MediaBox" % num
        boxes.append(tuple(float(v) for v in box.group(1).split()))
    return boxes


def page_contents(pdf):
    out = []
    for num in sorted(pdf.objects):
        body = pdf.object_body(num)
        if b"/Type /Page" not in body or b"/Type /Pages" in body:
            continue
        contents = int(re.search(rb"/Contents (\d+) 0 R", body).group(1))
        out.append(pdf.stream_of(contents).decode("latin-1"))
    return out


def cmap_for(pdf, page_obj):
    """ToUnicode map of the first font used on a page."""
    res = pdf.dict_of(page_obj)
    fmap = {name.decode(): int(ref) for name, ref in re.findall(rb"/(F\d+) (\d+) 0 R", res)}
    first = sorted(fmap)[0]
    body = pdf.object_body(fmap[first])
    tu = int(re.search(rb"/ToUnicode (\d+) 0 R", body).group(1))
    cmap = pdf.stream_of(tu).decode("latin-1")
    mapping = {}
    for block in re.findall(r"beginbfchar(.*?)endbfchar", cmap, re.S):
        for src, dst in re.findall(r"<([0-9A-Fa-f]{4})>\s*<([0-9A-Fa-f]{4,})>", block):
            mapping[int(src, 16)] = bytes.fromhex(dst).decode("utf-16-be")
    return mapping


def page_text(pdf, content):
    """Decode <...> Tj strings with the page's ToUnicode map."""
    parts = []
    for hexstr in re.findall(r"<([0-9A-Fa-f]+)> Tj", content):
        gids = [int(hexstr[i : i + 4], 16) for i in range(0, len(hexstr), 4)]
        parts.append("".join(cmap_cache.get(g, "\uFFFD") for g in gids))
    return "".join(parts)


cmap_cache = {}


def check_pdf(pdf_path, source_path):
    pdf = Pdf(pdf_path)
    src = load_pptx(source_path)

    boxes = page_boxes(pdf)
    assert len(boxes) == len(src.slides), "PDF has %d pages, deck has %d slides" % (
        len(boxes),
        len(src.slides),
    )

    want_w = src.slide_width / EMU_PER_PT
    want_h = src.slide_height / EMU_PER_PT
    for index, (x0, y0, x1, y1) in enumerate(boxes, 1):
        assert abs(x1 - x0 - want_w) <= 0.5 and abs(y1 - y0 - want_h) <= 0.5, (
            "page %d is %.1fx%.1f pt, slide is %.1fx%.1f pt"
            % (index, x1 - x0, y1 - y0, want_w, want_h)
        )

    # text of every slide must come out of the PDF
    page_objs = [
        num
        for num in sorted(pdf.objects)
        if b"/Type /Page" in pdf.object_body(num)
        and b"/Type /Pages" not in pdf.object_body(num)
    ]
    assert len(page_objs) == len(boxes)
    for index, (obj, content) in enumerate(zip(page_objs, page_contents(pdf)), 1):
        cmap_cache.clear()
        cmap_cache.update(cmap_for(pdf, obj))
        got = re.sub(r"\s+", "", page_text(pdf, content))
        want = re.sub(
            r"\s+", "", "".join(s.text_frame.text for s in src.slides[index - 1].shapes if s.has_text_frame)
        )
        for chunk in split_chunks(want):
            assert chunk in got, "slide %d: %r missing from PDF page %d (%r)" % (
                index,
                chunk,
                index,
                got[:80],
            )
        print("  page %d text ok: %d chars" % (index, len(got)))

    # picture placement: compare the image cm matrix against the shape rect
    checked = 0
    for index, content in enumerate(page_contents(pdf), 1):
        if " Do" not in content:
            continue
        slide = src.slides[index - 1]
        pictures = [s for s in slide.shapes if s.shape_type == 13]  # MSO_SHAPE_TYPE.PICTURE
        if not pictures:
            continue
        matrices = [
            tuple(float(v) for v in m)
            for m in re.findall(r"([-\d.]+) 0 0 ([-\d.]+) ([-\d.]+) ([-\d.]+) cm", content)
        ]
        assert len(matrices) == len(pictures), "slide %d: %d images drawn, %d pictures" % (
            index,
            len(matrices),
            len(pictures),
        )
        for pic, (w, h, x, y) in zip(pictures, matrices):
            want_x = pic.left / EMU_PER_PT
            want_y = want_h - pic.top / EMU_PER_PT - pic.height / EMU_PER_PT
            want_w = pic.width / EMU_PER_PT
            want_hh = pic.height / EMU_PER_PT
            for label, got, want in (
                ("x", x, want_x),
                ("y", y, want_y),
                ("w", w, want_w),
                ("h", h, want_hh),
            ):
                assert abs(got - want) <= TOLERANCE_PT, (
                    "slide %d picture %s offset %.2f pt (got %.2f, want %.2f)"
                    % (index, label, abs(got - want), got, want)
                )
            checked += 1
    print("  pdf ok: %d pages, %d pictures placed within %.1f pt" % (len(boxes), checked, TOLERANCE_PT))


def split_chunks(text, size=8):
    """Compare in chunks so one long paragraph still gives a readable failure."""
    return [text[i : i + size] for i in range(0, len(text), size)]


# ---- entry point ---------------------------------------------------------


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    mode = argv[1]
    if mode == "package":
        check_package(argv[2])
    elif mode == "roundtrip":
        check_roundtrip(argv[2], argv[3])
    elif mode == "pdf":
        check_pdf(argv[2], argv[3])
    elif mode == "all":
        check_package(argv[2])
        check_roundtrip(argv[2], argv[4])
        check_pdf(argv[3], argv[2])
    else:
        print("unknown mode: %s" % mode)
        return 2
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
