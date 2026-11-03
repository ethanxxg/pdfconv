"""Embed the PDF's own TrueType/OpenType fonts into the output DOCX.

Why this is a post-processing pass
----------------------------------
python-docx has no API for font parts, and the part must be *obfuscated*, so the
cleanest route is to rewrite the finished .docx zip: add the font parts, the
relationships, the content-type default and the <w:font> entries in
fontTable.xml.  Rewriting after save also keeps the conversion path unchanged
when nothing is embeddable.

What can and cannot be embedded
-------------------------------
DOCX only carries sfnt fonts (TrueType / OpenType).  Type1 (PFA/PFB) and bare
CFF are not representable in the format at all -- Word has no consumer for
them -- so on a corpus like this one (54 pfa / 22 cff / 4 cid against 9 ttf)
most documents simply have nothing embeddable.  Aspose hits exactly the same
wall; that is a property of DOCX, not of the converter.

Obfuscation
-----------
ECMA-376 stores embedded fonts as "ODTTF": the first 32 bytes are XORed with
the 16-byte font key taken in reverse order, repeated twice.
"""
import os
import re
import uuid
import shutil
import zipfile
import tempfile

_SFNT_MAGIC = (b"\x00\x01\x00\x00", b"OTTO", b"true", b"ttcf")
_FONT_REL = ("http://schemas.openxmlformats.org/officeDocument/2006/"
             "relationships/font")
_ODTTF_CT = ("application/vnd.openxmlformats-officedocument."
             "obfuscatedFont")


def _is_sfnt(buf):
    return bool(buf) and buf[:4] in _SFNT_MAGIC


# ---------------------------------------------------------------------------
# Font-name resolution
#
# A PDF usually names a font that the reader does not have.  Word then
# substitutes something of its own choosing, and for CJK text that is almost
# always a *serif* -- so a report set in 思源黑体 comes out in 宋体 and the user
# rightly says the fonts do not match.  Only sfnt fonts can be embedded in a
# DOCX at all; a CID-keyed CFF font (what most Chinese PDFs carry) cannot, so
# the name has to be resolved to an installed equivalent of the same class.
# ---------------------------------------------------------------------------

# Sans-serif CJK families (思源黑体 / 黑体 / 苹方 / ゴシック ...)
_CJK_SANS = re.compile(
    r"(Source\s*Han\s*Sans|SourceHanSans|NotoSansCJK|Noto\s*Sans\s*(CJK|SC|TC|JP|KR)|"
    r"PingFang|Hiragino\s*Sans|Heiti|SimHei|Hei\b|黑体|雅黑|YaHei|"
    r"Gothic|Meiryo|DengXian|等线|STHeiti|Microsoft\s*JhengHei)", re.I)

# Serif / Song / Ming CJK families
_CJK_SERIF = re.compile(
    r"(Source\s*Han\s*Serif|SourceHanSerif|NotoSerifCJK|Noto\s*Serif\s*(CJK|SC)|"
    r"SimSun|NSimSun|Songti|STSong|Song\b|宋|Ming|Mincho|明朝|FangSong|仿宋|"
    r"Kai|楷|MS\s*Mincho|Baoli|STFangsong)", re.I)

_LATIN_SANS = re.compile(
    r"(Arial|Helvetica|Verdana|Tahoma|Calibri|Segoe|Roboto|Open\s*Sans|Lato|"
    r"Futura|Gill\s*Sans|Univers|Myriad|Trebuchet|Century\s*Gothic)", re.I)
_LATIN_SERIF = re.compile(
    r"(Times|Roman\b|Georgia|Garamond|Minion|Cambria|Palatino|Book\s*Antiqua|"
    r"Baskerville|Caslon|Century|Serif)", re.I)
_LATIN_MONO = re.compile(r"(Courier|Consol|Mono|Menlo|Inconsolata)", re.I)
_MATH = re.compile(
    r"(CMSY|CMEX|CMMI|CMSL|CMSY|MSBM|MTSY|MTMI|MTEX|MT\s*Extra|RBLMI|RMTMI|"
    r"BLEX|Symbol|Wingdings|Webdings|MathematicalPi|STIX|LatinModern|XITS|"
    r"Asana|Euclid|Dingbats)", re.I)

#: fallbacks, most specific first
FALLBACK = {
    "cjk-sans": "Microsoft YaHei",
    "cjk-serif": "SimSun",
    "sans": "Arial",
    "serif": "Times New Roman",
    "mono": "Courier New",
    "math": "Cambria Math",
}

_INSTALLED = {"names": None}


def installed_names():
    """Lower-cased family names installed on this machine (cached)."""
    if _INSTALLED["names"] is not None:
        return _INSTALLED["names"]
    names = set()
    try:
        import winreg
        key = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts"
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                k = winreg.OpenKey(hive, key)
            except OSError:
                continue
            i = 0
            while True:
                try:
                    val, data, _t = winreg.EnumValue(k, i)
                except OSError:
                    break
                i += 1
                names.add(re.sub(r"\s*\(.*?\)\s*$", "", str(val)).strip().lower())
                names.add(os.path.splitext(os.path.basename(str(data)))[0]
                          .strip().lower())
    except Exception:
        pass
    _INSTALLED["names"] = names
    return names


def _class_of(name):
    if _MATH.search(name):
        return "math"
    if _CJK_SANS.search(name):
        return "cjk-sans"
    if _CJK_SERIF.search(name):
        return "cjk-serif"
    if _LATIN_MONO.search(name):
        return "mono"
    if _LATIN_SANS.search(name):
        return "sans"
    if _LATIN_SERIF.search(name):
        return "serif"
    return None


def resolve(name, embeddable=()):
    """The font name to write for *name*: itself when usable, else a class
    equivalent Word will actually have.

    Kept verbatim when the PDF's font can be embedded (then Word uses the real
    thing) or when the family is installed here.  Otherwise the name is mapped
    by class, because a missing CJK font silently falls back to a serif and the
    document stops looking like its source.
    """
    if not name:
        return name
    bare = name.split("+")[-1]
    if bare in embeddable or name in embeddable:
        return bare
    if bare.lower() in installed_names():
        return bare
    cls = _class_of(bare)
    return FALLBACK.get(cls, bare)


def resolve_map(names, embeddable=()):
    """{original: resolved} for every name in *names*."""
    out = {}
    for n in names:
        if not n:
            continue
        out[n] = resolve(n, embeddable)
    return out


def _obfuscate(data, guid):
    key = bytes.fromhex(guid.strip("{}").replace("-", ""))
    out = bytearray(data)
    for i in range(min(32, len(out))):
        out[i] ^= key[15 - (i % 16)]
    return bytes(out)


def collect(pdf, used_names):
    """Extract an embeddable font for each name we actually wrote.

    Names are kept exactly as the PDF reports them (subset prefix and all),
    because fontTable must agree with the names referenced by the runs; a
    mismatch makes Word silently ignore the embedded font.
    """
    if not used_names:
        return {}
    xref_for = {}
    for pno in range(len(pdf)):
        for xref, _ext, _sub, base, _res, _enc in pdf.get_page_fonts(pno):
            xref_for.setdefault(base, xref)
            bare = base.split("+")[-1]
            xref_for.setdefault(bare, xref)
    out = {}
    for name in used_names:
        xref = xref_for.get(name) or xref_for.get(name.split("+")[-1])
        if xref is None:
            continue
        try:
            _nm, _ext, _sub, buf = pdf.extract_font(xref)
        except Exception:
            continue
        if not _is_sfnt(buf):
            continue          # Type1 / bare CFF: not representable in DOCX
        out[name] = buf
    return out


def _xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def embed(docx_path, fonts):
    """Rewrite *docx_path* in place, adding embedded font parts. Returns count."""
    if not fonts:
        return 0
    with zipfile.ZipFile(docx_path) as z:
        entries = {n: z.read(n) for n in z.namelist()}

    rels_name = "word/_rels/document.xml.rels"
    table_name = "word/fontTable.xml"
    ct_name = "[Content_Types].xml"
    if table_name not in entries or rels_name not in entries or ct_name not in entries:
        return 0

    rels = entries[rels_name].decode("utf8")
    used_ids = set(re.findall(r'Id="([^"]+)"', rels))
    next_rid = 1
    while ("rId%d" % next_rid) in used_ids:
        next_rid += 1

    table = entries[table_name].decode("utf8")
    new_rels = []
    new_fonts = {}
    added = 0
    for i, (name, buf) in enumerate(sorted(fonts.items()), start=1):
        rid = "rId%d" % next_rid
        next_rid += 1
        while ("rId%d" % next_rid) in used_ids:
            next_rid += 1
        guid = "{%s}" % str(uuid.uuid4()).upper()
        part = "word/fonts/font%d.odttf" % i
        new_fonts[part] = _obfuscate(buf, guid)
        new_rels.append(
            '<Relationship Id="%s" Type="%s" Target="fonts/font%d.odttf"/>'
            % (rid, _FONT_REL, i))
        # only add a table entry when the name is not already declared
        if re.search(r'<w:font w:name="%s"' % re.escape(_xml_escape(name)), table):
            continue
        new_fonts_entry = (
            '<w:font w:name="%s"><w:embedRegular r:id="%s" w:fontKey="%s"/>'
            '</w:font>' % (_xml_escape(name), rid, guid))
        table = table.replace("</w:fonts>", new_fonts_entry + "</w:fonts>")
        added += 1

    if not new_fonts:
        return 0

    rels = rels.replace("</Relationships>", "".join(new_rels) + "</Relationships>")
    ct = entries[ct_name].decode("utf8")
    if 'Extension="odttf"' not in ct:
        ct = ct.replace(
            "<Types ", '<Types ', 1)
        ct = re.sub(r"(<Types[^>]*>)",
                    r'\1<Default Extension="odttf" ContentType="%s"/>' % _ODTTF_CT,
                    ct, count=1)

    entries[rels_name] = rels.encode("utf8")
    entries[table_name] = table.encode("utf8")
    entries[ct_name] = ct.encode("utf8")
    entries.update(new_fonts)

    fd, tmp = tempfile.mkstemp(suffix=".docx", dir=os.path.dirname(
        os.path.abspath(docx_path)))
    os.close(fd)
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as out:
            for n, data in entries.items():
                out.writestr(n, data)
        shutil.move(tmp, docx_path)
    finally:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
    return added
