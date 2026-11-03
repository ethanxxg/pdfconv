"""Detect and emit real page headers / footers.

Instead of discarding running-heads as 'furniture', we lift the most common
header-band text into the DOCX section header and add an automatic PAGE field
to the footer -- matching what Aspose does for paginated documents.
"""
import collections
import re
from io import BytesIO

from docx.shared import Pt
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

from .config import XML_DROP


def _pt(value):
    """Snap to Word's half-point grid (see writer._pt)."""
    return Pt(round(float(value) * 2.0) / 2.0)


def _norm(t):
    """Collapse whitespace and drop XML-illegal control bytes.

    Band text is read straight from the PDF text layer, which can carry raw
    C0 controls: 047's running head arrives as "\\x07\\x08 HE et al. : ...".
    Emitting that into the Word header makes python-docx raise
    ``ValueError: All strings must be XML compatible`` and the whole
    conversion fails, so the filter has to happen here rather than only on
    body tokens.
    """
    return XML_DROP.sub("", " ".join(t.split()))


_FOLIO = re.compile(r"\s+[A-Za-z]?\d{1,5}\s*$")     # trailing page number
_LEAD = re.compile(r"^\s*[A-Za-z]?\d{1,5}\s+")       # leading page number


def _strip_folio(txt):
    """Drop a folio that the text layer glued onto a running head.

    A recurring header like "Meteorological data from KLAWS-2G" reaches the
    detector as "Meteorological data from KLAWS-2G 9", "... 10", ... -- each
    occurrence is unique so the >=2 recurrence test fails and the head is never
    lifted.  Stripping the trailing standalone number (and, for journals that
    put the folio first, a leading one) makes the head recur and lets it be
    lifted out of the body."""
    t = txt.strip()
    if len(t) > 6 and _FOLIO.search(t):
        t = _FOLIO.sub("", t).strip()
    elif len(t) > 6 and _LEAD.match(t):
        t = _LEAD.sub("", t).strip()
    return t


def detect_running_heads(pdf, page_h, max_pages=24, page_lines=None):
    """Return (header_text, footer_text) -- the recurring text in the top /
    bottom bands across the sampled pages, or None.

    A *running* head/foot is text that recurs on page after page, so the result
    is the set of band lines seen on at least two sampled pages, joined into one
    string.  Joining (rather than keeping only the single most common line)
    matters for OCR'd pages where the head is several separate lines
    ("安全牛", "AQNIU.NET") that must all be lifted out of the body.

    `page_lines` lets the caller supply text that did not come from the PDF's
    own text layer -- on a scanned or undecodable-CID document the extracted
    text is garbage or absent, so the running head has to be read from OCR.  It
    maps a 0-based page number to [(x0, y0, x1, y1, text), ...].  When it is
    supplied we only trust those pages: pages not pre-scanned are *skipped*
    rather than read from the (garbage) PDF text, so the counters are never
    polluted by undecodable CID noise.
    """
    headers, footers = collections.Counter(), collections.Counter()
    n = len(pdf)
    for i in range(min(n, max_pages)):
        if page_lines is not None:
            lines = page_lines.get(i)
            if lines is None:
                continue            # OCR doc, page not pre-scanned: skip
        else:
            page = pdf[i]
            lines = []
            for b in page.get_text("dict")["blocks"]:
                if b.get("type", 0) != 0:
                    continue
                x0, y0, x1, y1 = b["bbox"]
                txt = _norm(" ".join(s["text"] for l in b["lines"]
                                     for s in l["spans"]))
                if txt:
                    lines.append((x0, y0, x1, y1, txt))
        for x0, y0, x1, y1, txt in lines:
            txt = _norm(txt)
            if not txt:
                continue
            key = _strip_folio(txt)
            if y1 < page_h * 0.10:
                headers[key] += 1
            elif y0 > page_h * 0.90:
                footers[key] += 1

    def recurring(c):
        # a single stray glyph in the margin band ("i", "U") recurs trivially
        # and is not a running head -- it would be emitted into the Word header
        # and then poison the body filter, which matches by substring
        common = sorted(v for v, cnt in c.items()
                        if cnt >= 2 and len(v.strip()) >= 2)
        return " ".join(common) if common else None

    return recurring(headers), recurring(footers)


def _add_page_field(paragraph):
    """Insert a { PAGE } field so Word numbers pages automatically."""
    run = paragraph.add_run()
    fld1 = OxmlElement("w:fldChar")
    fld1.set(qn("w:fldCharType"), "begin")
    instr = OxmlElement("w:instrText")
    instr.set(qn("xml:space"), "preserve")
    instr.text = "PAGE"
    fld2 = OxmlElement("w:fldChar")
    fld2.set(qn("w:fldCharType"), "end")
    run._r.append(fld1)
    run._r.append(instr)
    run._r.append(fld2)


def detect_running_logo(pdf, page_h, max_pages=12, band=None):
    """The recurring image printed in the top band, or None.

    Corporate reports carry a small bitmap logo in the corner of every page --
    too small for the figure extractor (69x30pt on 安全牛) and, once the
    top-band text is recognised as furniture, the only trace of it left is the
    OCR'd text, which is then dropped as well: the logo simply disappears.

    A logo is exactly what a Word *header* is for, so the image is lifted into
    it the same way the running head text is.  Detection is by xref: the same
    embedded image, wholly inside the top band, on at least two pages.
    """
    band = float(band or page_h * 0.10)
    seen = collections.Counter()
    box = {}
    n = len(pdf)
    for i in range(min(n, max_pages)):
        try:
            infos = pdf[i].get_image_info(xrefs=True)
        except Exception:
            continue
        for info in infos:
            x0, y0, x1, y1 = info["bbox"]
            if y1 > band or (x1 - x0) < 20 or (y1 - y0) < 10:
                continue
            xref = info.get("xref") or 0
            if not xref:
                continue
            seen[xref] += 1
            box.setdefault(xref, (x0, y0, x1, y1))
    if not seen:
        return None
    xref, count = seen.most_common(1)[0]
    if count < 2:
        return None
    try:
        got = pdf.extract_image(xref)
    except Exception:
        return None
    if not got or not got.get("image"):
        return None
    blob = got["image"]
    # A logo's transparency lives in a separate /SMask, not in the base image:
    # extract_image() hands back the base bitmap alone, whose "transparent"
    # pixels are stored as solid black -- the logo then arrives in Word inside
    # a black rectangle.  Compositing the mask gives a real RGBA PNG.
    smask = got.get("smask") or 0
    if smask:
        try:
            import fitz as _fz
            base = _fz.Pixmap(pdf, xref)
            mask = _fz.Pixmap(pdf, smask)
            if not mask.n == 1:
                mask = _fz.Pixmap(mask, 1)
            blob = _fz.Pixmap(base, mask).tobytes("png")
        except Exception:
            pass
    x0, y0, x1, y1 = box[xref]
    return dict(image=blob, ext="png",
                width=(x1 - x0), height=(y1 - y0), x0=x0, page_w=pdf[0].rect.width)


def apply_header_footer(section, header_text, footer_text, body_size,
                        logo=None):
    """Write the running head into the section header and a PAGE field into the
    footer.  Header/footer font is scaled down to body*0.8.

    When the document's running head is a *picture*, the picture goes in the
    header instead of its OCR'd text -- the text is only a reading of the logo
    and would duplicate it.
    """
    hdr = section.header
    hp = hdr.paragraphs[0]
    hp.text = ""
    if logo:
        # keep the logo where the source prints it (right margin in the
        # reports this handles) rather than centring it
        hp.alignment = (WD_ALIGN_PARAGRAPH.RIGHT
                        if logo["x0"] > logo["page_w"] * 0.5
                        else WD_ALIGN_PARAGRAPH.LEFT)
        try:
            hp.add_run().add_picture(BytesIO(logo["image"]),
                                     width=Pt(logo["width"]))
        except Exception:
            pass
    else:
        hp.alignment = WD_ALIGN_PARAGRAPH.CENTER
        if header_text:
            r = hp.add_run(header_text)
            r.font.size = _pt(body_size * 0.8)

    ftr = section.footer
    fp = ftr.paragraphs[0]
    fp.text = ""
    fp.alignment = WD_ALIGN_PARAGRAPH.CENTER
    if footer_text:
        r = fp.add_run(footer_text + "  ")
        r.font.size = _pt(body_size * 0.8)
    _add_page_field(fp)
