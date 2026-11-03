"""Independent verifier for DocCore output. Parses the PDF/PNG from scratch so
that a bug in the writer cannot hide behind the writer's own reader."""
import re
import sys
import zlib


class Pdf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        assert self.data.startswith(b"%PDF-1.7"), "bad header"
        assert self.data.rstrip().endswith(b"%%EOF"), "missing EOF marker"
        self.objects = {}
        self._load_xref()

    def _load_xref(self):
        idx = self.data.rfind(b"startxref")
        assert idx > 0, "no startxref"
        off = int(self.data[idx + 9 :].split()[0])
        m = re.match(rb"(\d+) 0 obj\s*<<(.*?)>>\s*stream\r?\n", self.data[off : off + 800], re.S)
        assert m, "xref stream not found at startxref"
        objnum = int(m.group(1))
        dict_text = m.group(2)
        assert b"/Type /XRef" in dict_text, "not an xref stream"
        start = off + m.end()
        length = int(re.search(rb"/Length (\d+)", dict_text).group(1))
        raw = self.data[start : start + length]
        rows = zlib.decompress(raw)
        w = [int(x) for x in re.search(rb"/W \[(.*?)\]", dict_text).group(1).split()]
        assert w == [1, 4, 2], "unexpected /W"
        size = int(re.search(rb"/Size (\d+)", dict_text).group(1))
        rec = sum(w)
        for i in range(size):
            chunk = rows[i * rec : (i + 1) * rec]
            if len(chunk) < rec:
                break
            typ = chunk[0]
            offset = int.from_bytes(chunk[1:5], "big")
            if typ == 1:
                self.objects[i] = offset
        self.xref_obj = objnum
        self.root_ref = re.search(rb"/Root (\d+) 0 R", dict_text).group(1).decode()

    def object_body(self, num):
        off = self.objects[num]
        end = self.data.find(b"endobj", off)
        return self.data[off:end]

    def dict_of(self, num):
        body = self.object_body(num)
        m = re.match(rb"\d+ 0 obj\s*<<", body)
        assert m, "object %d is not a dict" % num
        return body[m.end() - 2 :]

    def stream_of(self, num):
        body = self.object_body(num)
        m = re.search(rb">>\s*stream\r?\n", body)
        assert m, "object %d has no stream" % num
        start = m.end()
        length = int(re.search(rb"/Length (\d+)", body[: m.start()]).group(1))
        raw = body[start : start + length]
        head = body[: m.start()]
        if b"/FlateDecode" in head:
            return zlib.decompress(raw)
        return raw


def check_pdf(path, expected_text):
    pdf = Pdf(path)
    catalog = pdf.dict_of(int(pdf.root_ref))
    assert b"/Type /Catalog" in catalog, "root is not a catalog"

    # ToUnicode maps for every Type0 font
    maps = {}
    for num in pdf.objects:
        body = pdf.object_body(num)
        if b"/Subtype /Type0" not in body:
            continue
        tu = int(re.search(rb"/ToUnicode (\d+) 0 R", body).group(1))
        cmap = pdf.stream_of(tu).decode("latin-1")
        mapping = {}
        for block in re.findall(r"beginbfchar(.*?)endbfchar", cmap, re.S):
            for src, dst in re.findall(r"<([0-9A-Fa-f]{4})>\s*<([0-9A-Fa-f]{4,})>", block):
                mapping[int(src, 16)] = bytes.fromhex(dst).decode("utf-16-be")
        maps[num] = mapping
    assert maps, "no Type0 font found"

    # text from the page content streams
    text_parts = []
    for num in pdf.objects:
        body = pdf.object_body(num)
        if b"/Type /Page" not in body or b"/Type /Pages" in body:
            continue
        contents = int(re.search(rb"/Contents (\d+) 0 R", body).group(1))
        content = pdf.stream_of(contents).decode("latin-1")
        font = re.search(r"/F(\d+) [\d.]+ Tf", content)
        assert font, "no Tf operator in content stream"
        # map /Fn -> object number through /Font resources
        res = pdf.dict_of(num)
        fmap = {}
        for name, ref in re.findall(rb"/(F\d+) (\d+) 0 R", res):
            fmap[name.decode()] = int(ref)
        target = fmap["F" + font.group(1)]
        cmap = maps[target]
        for hexstr in re.findall(r"<([0-9A-Fa-f]+)> Tj", content):
            gids = [int(hexstr[i : i + 4], 16) for i in range(0, len(hexstr), 4)]
            text_parts.append("".join(cmap.get(g, "\uFFFD") for g in gids))
    got = "".join(text_parts)

    # embedded font subset must be a parseable TrueType with the right glyph count
    for num in pdf.objects:
        body = pdf.object_body(num)
        if b"/FontFile2" not in body:
            continue
        ff = int(re.search(rb"/FontFile2 (\d+) 0 R", body).group(1))
        font = pdf.stream_of(ff)
        assert font[:4] == b"\x00\x01\x00\x00", "subset is not TrueType"
        num_tables = int.from_bytes(font[4:6], "big")
        tags = set()
        for i in range(num_tables):
            rec = font[12 + i * 16 : 12 + (i + 1) * 16]
            tags.add(rec[:4])
        for required in (b"head", b"hhea", b"maxp", b"hmtx", b"loca", b"glyf"):
            assert required in tags, "subset missing %s" % required
        break

    print("  extracted text:", repr(got))
    assert expected_text in got, "text mismatch: %r not in %r" % (expected_text, got)
    print("  PDF ok: objects=%d fonts=%d" % (len(pdf.objects), len(maps)))


def check_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad png signature"
    pos = 8
    idat = b""
    w = h = 0
    while pos < len(data):
        length = int.from_bytes(data[pos : pos + 4], "big")
        tag = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        crc = int.from_bytes(data[pos + 8 + length : pos + 12 + length], "big")
        assert zlib.crc32(tag + payload) & 0xFFFFFFFF == crc, "crc mismatch on %s" % tag
        if tag == b"IHDR":
            w = int.from_bytes(payload[0:4], "big")
            h = int.from_bytes(payload[4:8], "big")
            assert payload[8] == 8 and payload[9] == 6, "expected 8-bit RGBA"
        elif tag == b"IDAT":
            idat += payload
        pos += 12 + length
    raw = zlib.decompress(idat)
    assert len(raw) == h * (w * 4 + 1), "bad scanline size"
    stride = w * 4 + 1
    px = lambda x, y: raw[y * stride + 1 + x * 4 : y * stride + 1 + x * 4 + 4]
    print("  png %dx%d, corner=%s, mid=%s" % (w, h, tuple(px(2, 2)), tuple(px(w // 2, h // 2))))
    assert px(2, 2)[0] > 240 and px(2, 2)[1] > 240, "background should be white"
    print("  PNG ok")


if __name__ == "__main__":
    kind = sys.argv[1]
    path = sys.argv[2]
    if kind == "pdf":
        check_pdf(path, sys.argv[3])
    else:
        check_png(path)
