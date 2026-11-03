"""Shared constants and compiled patterns for the converter.

Kept in one place so the rest of the package carries no magic values.
"""
import re

# Font-role heuristics (name based, general -- not paper specific).
ITALIC_RE = re.compile(r"(italic|oblique)", re.I)
BOLD_RE = re.compile(r"(bold|black|semibold|heavy)", re.I)
SYMBOL_RE = re.compile(
    r"(CMSY|CMEX|CMMI|CMSL|MSBM|MTSY|MTMI|MTEX|MT Extra|RBLMI|RMTMIB|RMTMI|"
    r"BLEX|Symbol|MathematicalPi|STIXMath|LatinModernMath|XITSMath)", re.I)

# XML 1.0 forbids these even though they are not < 0x20.
XML_DROP = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")

# Output geometry / rendering defaults.
DOC_MARGIN_PT = 54.0        # page margin applied to the generated DOCX
FIGURE_DPI = 200            # rasterisation DPI for cropped figures

# A line counts as "ending a paragraph" when it stops this far short of its
# neighbours' right edge.  Expressed both in ems and as a fraction of the
# column width; the larger wins.  Too small shreds justified body text into one
# paragraph per line, too large glues reference entries together, so both
# bounds matter.
SHORT_LINE_EM = 3.0
SHORT_LINE_RATIO = 0.10
