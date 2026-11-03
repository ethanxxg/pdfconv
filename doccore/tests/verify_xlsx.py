"""Independent verifier for DocCore's XLSX output.

Checks the package with three unrelated toolchains so a bug in our own reader
cannot hide a bug in our writer:

  zipfile    - container integrity
  ElementTree- every XML part is well formed
  openpyxl   - a third-party reader accepts the workbook and sees our values

Usage:  python tests/verify_xlsx.py <file.xlsx> [--expect-json <path>]
"""
import json
import posixpath
import sys
import zipfile
import xml.etree.ElementTree as ET

NS_CT = "{http://schemas.openxmlformats.org/package/2006/content-types}"
NS_MAIN = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
NS_REL = "{http://schemas.openxmlformats.org/package/2006/relationships}"

failures = []


def check(cond, message):
    if not cond:
        failures.append(message)
        print("FAIL " + message)


def verify(path):
    zf = zipfile.ZipFile(path)
    bad = zf.testzip()
    check(bad is None, "zip container corrupt at %s" % bad)
    names = set(zf.namelist())
    check("[Content_Types].xml" in names, "[Content_Types].xml missing")
    check("_rels/.rels" in names, "_rels/.rels missing")

    # every XML part must parse
    xml_parts = [n for n in names if n.endswith(".xml") or n.endswith(".rels")]
    check(len(xml_parts) >= 5, "too few XML parts: %d" % len(xml_parts))
    for name in xml_parts:
        try:
            ET.fromstring(zf.read(name))
        except ET.ParseError as exc:
            check(False, "%s is not well formed: %s" % (name, exc))

    # content types must cover every part
    ct = ET.fromstring(zf.read("[Content_Types].xml"))
    defaults = {e.get("Extension").lower(): e.get("ContentType") for e in ct.findall(NS_CT + "Default")}
    overrides = {e.get("PartName"): e.get("ContentType") for e in ct.findall(NS_CT + "Override")}
    for name in names:
        if name == "[Content_Types].xml" or name.endswith("/"):
            continue
        part = "/" + name
        ext = name.rsplit(".", 1)[-1].lower()
        check(part in overrides or ext in defaults, "no content type for %s" % part)

    # relationships must resolve
    for name in [n for n in names if n.endswith(".rels")]:
        rels = ET.fromstring(zf.read(name))
        base = name.rsplit("_rels/", 1)[0]
        for rel in rels.findall(NS_REL + "Relationship"):
            if rel.get("TargetMode") == "External":
                continue
            target = rel.get("Target")
            # Target 里允许出现 ../，必须先规范化再比对，否则 pivotTable ->
            # ../pivotCache/... 这类跨目录关系会被误判成"目标不存在"。
            if target.startswith("/"):
                resolved = posixpath.normpath(target.lstrip("/"))
            else:
                resolved = posixpath.normpath(base + target)
            check(resolved in names, "%s -> missing target %s" % (name, resolved))

    # third-party reader
    try:
        import openpyxl
    except ImportError:
        print("note: openpyxl not available, skipping value checks")
        return
    wb = openpyxl.load_workbook(path)
    check(len(wb.sheetnames) >= 1, "openpyxl sees no sheets")
    return wb


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    wb = verify(path)
    expect = None
    if "--expect-json" in sys.argv:
        with open(sys.argv[sys.argv.index("--expect-json") + 1], encoding="utf-8") as fh:
            expect = json.load(fh)
    if expect is not None and wb is not None:
        ws = wb[expect["sheet"]]
        for cell, value in expect.get("cells", {}).items():
            got = ws[cell].value
            if isinstance(value, float) and isinstance(got, (int, float)):
                ok = abs(float(got) - value) < 1e-9
            else:
                ok = got == value
            check(ok, "%s: expected %r, got %r" % (cell, value, got))
        for cell, formula in expect.get("formulas", {}).items():
            got = ws[cell].value
            check(got == formula, "%s: expected formula %r, got %r" % (cell, formula, got))
        if expect.get("formula_values"):
            # 缓存值要另开一次 data_only 的读取：openpyxl 不会自己算公式。
            import openpyxl
            cached = openpyxl.load_workbook(path, data_only=True)[expect["sheet"]]
            for cell, value in expect["formula_values"].items():
                got = cached[cell].value
                if isinstance(value, float) and isinstance(got, (int, float)):
                    ok = abs(float(got) - value) < 1e-9
                else:
                    ok = got == value
                check(ok, "%s cached: expected %r, got %r" % (cell, value, got))
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("verify_xlsx: ok (%s)" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
