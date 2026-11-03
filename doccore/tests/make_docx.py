"""Builds DOCX fixtures for the DocCore unit tests (P5 word-processing slice).

Two files are produced:
  data/sample.docx    - a rich but tiny Word document exercising the whole DOM
  data/evil_rels.docx - same package with a relationship that escapes the root

Both are written by hand (deterministic bytes) so test expectations never drift.
"""
import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
os.makedirs(DATA, exist_ok=True)

W_NS = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
A_NS = "http://schemas.openxmlformats.org/drawingml/2006/main"
WP_NS = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
PIC_NS = "http://schemas.openxmlformats.org/drawingml/2006/picture"
PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
CT_NS = "http://schemas.openxmlformats.org/package/2006/content-types"


def one_px_png():
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    raw = b"\x00\xff\x00\x00\xff\xff"  # filter 0 + one RGBA pixel
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def content_types():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="{CT_NS}">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Default Extension="png" ContentType="image/png"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>
<Override PartName="/word/footnotes.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml"/>
<Override PartName="/word/endnotes.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml"/>
<Override PartName="/word/comments.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml"/>
<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>
<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>
<Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>
<Override PartName="/word/fontTable.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.fontTable+xml"/>
<Override PartName="/customXml/item1.xml" ContentType="application/xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>"""


def root_rels():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="{PKG_REL_NS}">
<Relationship Id="rId1" Type="{R_NS}/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="{R_NS}/metadata/core-properties" Target="docProps/core.xml"/>
<Relationship Id="rId3" Type="{R_NS}/extended-properties" Target="docProps/app.xml"/>
<Relationship Id="rId4" Type="{R_NS}/customXml" Target="customXml/item1.xml"/>
</Relationships>"""


def document_rels(escape=False):
    extra = ""
    if escape:
        # Escapes the package root: must be rejected with a warning, not read.
        extra = ('<Relationship Id="rId12" Type="%s/settings" Target="../../outside.xml"/>'
                 % R_NS)
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="{PKG_REL_NS}">
<Relationship Id="rId1" Type="{R_NS}/styles" Target="styles.xml"/>
<Relationship Id="rId2" Type="{R_NS}/numbering" Target="numbering.xml"/>
<Relationship Id="rId3" Type="{R_NS}/footnotes" Target="footnotes.xml"/>
<Relationship Id="rId4" Type="{R_NS}/endnotes" Target="endnotes.xml"/>
<Relationship Id="rId5" Type="{R_NS}/comments" Target="comments.xml"/>
<Relationship Id="rId6" Type="{R_NS}/header" Target="header1.xml"/>
<Relationship Id="rId7" Type="{R_NS}/footer" Target="footer1.xml"/>
<Relationship Id="rId8" Type="{R_NS}/image" Target="media/image1.png"/>
<Relationship Id="rId9" Type="{R_NS}/hyperlink" Target="https://example.com/doc" TargetMode="External"/>
<Relationship Id="rId10" Type="{R_NS}/settings" Target="settings.xml"/>
<Relationship Id="rId11" Type="{R_NS}/fontTable" Target="fontTable.xml"/>
{extra}</Relationships>"""


def document_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="{W_NS}" xmlns:r="{R_NS}" xmlns:a="{A_NS}" xmlns:wp="{WP_NS}" xmlns:pic="{PIC_NS}">
<w:body>
<w:p><w:pPr><w:pStyle w:val="Title"/><w:jc w:val="center"/></w:pPr>
<w:r><w:rPr><w:rFonts w:ascii="Calibri" w:eastAsia="SimHei"/><w:b/><w:color w:val="C00000"/><w:sz w:val="44"/><w:u w:val="single"/></w:rPr><w:t>DocCore read test</w:t></w:r>
<w:bookmarkStart w:id="1" w:name="anchor"/><w:bookmarkEnd w:id="1"/>
</w:p>
<w:p><w:pPr><w:pStyle w:val="Normal"/><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr><w:ind w:left="720" w:hanging="360"/></w:pPr>
<w:r><w:t>first item</w:t></w:r><w:tab/><w:r><w:t>tail</w:t></w:r>
<w:r><w:rPr><w:i/></w:rPr><w:delText>deleted text</w:delText></w:r>
<w:ins w:id="9" w:author="Alice" w:date="2026-01-01T00:00:00Z"><w:r><w:t>inserted text</w:t></w:r></w:ins>
<w:hyperlink r:id="rId9"><w:r><w:rPr><w:color w:val="0563C1"/></w:rPr><w:t>example link</w:t></w:r></w:hyperlink>
<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText> HYPERLINK "https://x" </w:instrText></w:r><w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>field result</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r>
<w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteReference w:id="2"/></w:r>
</w:p>
<w:p><w:r><w:drawing><wp:inline><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed="rId8"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing><w:t>after image</w:t></w:r></w:p>
<w:p><w:pPr><w:sectPr>
<w:headerReference w:type="default" r:id="rId6"/>
<w:footerReference w:type="default" r:id="rId7"/>
<w:pgSz w:w="11906" w:h="16838"/>
<w:pgMar w:top="1440" w:bottom="1440" w:left="1800" w:right="1800" w:header="720" w:footer="720"/>
<w:cols w:num="1" w:space="720"/>
<w:pgNumType w:start="3"/>
</w:sectPr></w:pPr><w:r><w:t>section break paragraph</w:t></w:r></w:p>
<w:tbl>
<w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/><w:jc w:val="center"/></w:tblPr>
<w:tblGrid><w:gridCol w:w="4675"/><w:gridCol w:w="4675"/></w:tblGrid>
<w:tr><w:trPr><w:tblHeader/><w:trHeight w:val="400" w:hRule="atLeast"/></w:trPr>
<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/><w:vAlign w:val="center"/></w:tcPr><w:p><w:r><w:t>column one</w:t></w:r></w:p></w:tc>
<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/></w:tcPr><w:p><w:r><w:t>column two</w:t></w:r></w:p></w:tc>
</w:tr>
<w:tr><w:trPr><w:cantSplit/></w:trPr>
<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/><w:vMerge/></w:tcPr><w:p><w:r><w:t>merged</w:t></w:r></w:p></w:tc>
<w:tc><w:tcPr><w:tcW w:w="4675" w:type="dxa"/></w:tcPr><w:p><w:r><w:rPr><w:b/><w:sz w:val="28"/></w:rPr><w:t>bold cell</w:t></w:r></w:p></w:tc>
</w:tr>
</w:tbl>
<w:p><w:commentRangeStart w:id="0"/><w:r><w:t>commented text</w:t></w:r><w:commentRangeEnd w:id="0"/><w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr><w:commentReference w:id="0"/></w:r></w:p>
<w:p><w:pPr><w:keepNext/><w:spacing w:before="240" w:after="120" w:line="480" w:lineRule="auto"/><w:jc w:val="both"/><w:outlineLvl w:val="0"/><w:pBdr><w:bottom w:val="single" w:sz="8" w:color="4472C4"/></w:pBdr></w:pPr><w:r><w:t>heading paragraph</w:t></w:r></w:p>
<w:p><w:r><w:br w:type="page"/><w:t>after page break</w:t></w:r><w:r><w:sym w:font="Wingdings" w:char="F0A7"/></w:r><w:r><w:t>end</w:t></w:r></w:p>
<w:p><w:r><w:t>endnote anchor</w:t></w:r><w:r><w:endnoteReference w:id="2"/></w:r></w:p>
<w:sectPr>
<w:pgSz w:w="16838" w:h="11906" w:orient="landscape"/>
<w:pgMar w:top="1134" w:bottom="1134" w:left="1418" w:right="1418"/>
<w:cols w:num="2" w:space="425"/>
</w:sectPr>
</w:body>
</w:document>"""


def styles_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="{W_NS}">
<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="Calibri" w:hAnsi="Calibri" w:eastAsia="SimSun"/><w:sz w:val="21"/></w:rPr></w:rPrDefault>
<w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>
<w:style w:type="paragraph" w:styleId="Title"><w:name w:val="Title"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:uiPriority w:val="10"/><w:pPr><w:spacing w:after="240"/><w:jc w:val="center"/></w:pPr><w:rPr><w:b/><w:sz w:val="56"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/><w:next w:val="Normal"/><w:pPr><w:keepNext/><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/><w:sz w:val="32"/></w:rPr></w:style>
<w:style w:type="character" w:styleId="DefaultParagraphFont"><w:name w:val="Default Paragraph Font"/><w:uiPriority w:val="1"/></w:style>
<w:style w:type="character" w:styleId="CommentReference"><w:name w:val="annotation reference"/><w:basedOn w:val="DefaultParagraphFont"/><w:rPr><w:sz w:val="16"/></w:rPr></w:style>
<w:style w:type="table" w:styleId="TableGrid"><w:name w:val="Table Grid"/><w:tblPr><w:tblBorders/></w:tblPr></w:style>
<w:style w:type="paragraph" w:styleId="CycleA"><w:name w:val="Cycle A"/><w:basedOn w:val="CycleB"/></w:style>
<w:style w:type="paragraph" w:styleId="CycleB"><w:name w:val="Cycle B"/><w:basedOn w:val="CycleA"/></w:style>
</w:styles>"""


def numbering_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:numbering xmlns:w="{W_NS}">
<w:abstractNum w:abstractNumId="0"><w:multiLevelType w:val="hybridMultilevel"/>
<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/><w:lvlJc w:val="left"/><w:suff w:val="tab"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr><w:rPr><w:rFonts w:ascii="Calibri" w:hint="default"/></w:rPr></w:lvl>
<w:lvl w:ilvl="1"><w:start w:val="1"/><w:numFmt w:val="lowerLetter"/><w:lvlText w:val="%2)"/><w:lvlJc w:val="left"/><w:pPr><w:ind w:left="1440" w:hanging="360"/></w:pPr></w:lvl>
</w:abstractNum>
<w:abstractNum w:abstractNumId="1">
<w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="bullet"/><w:lvlText w:val="&#xF0B7;"/><w:lvlJc w:val="left"/><w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl>
</w:abstractNum>
<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num>
<w:num w:numId="2"><w:abstractNumId w:val="1"/><w:lvlOverride w:ilvl="0"><w:startOverride w:val="5"/></w:lvlOverride></w:num>
</w:numbering>"""


def footnotes_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:footnotes xmlns:w="{W_NS}">
<w:footnote w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>
<w:footnote w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>
<w:footnote w:id="1"><w:p><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteRef/></w:r><w:r><w:t>first footnote</w:t></w:r></w:p></w:footnote>
<w:footnote w:id="2"><w:p><w:pPr><w:pStyle w:val="FootnoteText"/></w:pPr><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteRef/></w:r><w:r><w:t>footnote body text</w:t></w:r></w:p></w:footnote>
</w:footnotes>"""


def endnotes_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:endnotes xmlns:w="{W_NS}">
<w:footnote w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>
<w:footnote w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>
<w:footnote w:id="1"><w:p><w:r><w:t>first endnote</w:t></w:r></w:p></w:footnote>
<w:footnote w:id="2"><w:p><w:r><w:t>endnote body text</w:t></w:r></w:p></w:footnote>
</w:endnotes>"""


def comments_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:comments xmlns:w="{W_NS}">
<w:comment w:id="0" w:author="Bob" w:initials="B" w:date="2026-02-02T09:00:00Z"><w:p><w:r><w:annotationRef/></w:r><w:r><w:t>comment body text</w:t></w:r></w:p></w:comment>
</w:comments>"""


def header_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:hdr xmlns:w="{W_NS}"><w:p><w:pPr><w:jc w:val="right"/></w:pPr><w:r><w:t>header text</w:t></w:r></w:p></w:hdr>"""


def footer_xml():
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:ftr xmlns:w="{W_NS}"><w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r></w:p></w:ftr>"""


def misc_parts():
    return {
        "word/settings.xml": '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                             '<w:settings xmlns:w="%s"><w:zoom w:percent="100"/><w:defaultTabStop w:val="720"/></w:settings>' % W_NS,
        "word/fontTable.xml": '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                              '<w:fonts xmlns:w="%s"><w:font w:name="Calibri"><w:charset w:val="00"/></w:font></w:fonts>' % W_NS,
        "customXml/item1.xml": '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                               '<item xmlns="urn:custom"><b:keep xmlns:b="urn:custom2">kept</b:keep></item>',
        "docProps/core.xml": '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                             '<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties">'
                             '<dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">t</dc:title></cp:coreProperties>',
        "docProps/app.xml": '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                            '<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"/>',
    }


def build(path, escape=False):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", content_types())
        z.writestr("_rels/.rels", root_rels())
        z.writestr("word/document.xml", document_xml())
        z.writestr("word/_rels/document.xml.rels", document_rels(escape=escape))
        z.writestr("word/styles.xml", styles_xml())
        z.writestr("word/numbering.xml", numbering_xml())
        z.writestr("word/footnotes.xml", footnotes_xml())
        z.writestr("word/endnotes.xml", endnotes_xml())
        z.writestr("word/comments.xml", comments_xml())
        z.writestr("word/header1.xml", header_xml())
        z.writestr("word/footer1.xml", footer_xml())
        z.writestr("word/media/image1.png", one_px_png())
        for name, body in misc_parts().items():
            z.writestr(name, body)
    return path


if __name__ == "__main__":
    build(os.path.join(DATA, "sample.docx"))
    build(os.path.join(DATA, "evil_rels.docx"), escape=True)
    print("docx fixtures written to", DATA)

