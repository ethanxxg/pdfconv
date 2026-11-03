"""Translation layer: pluggable MT + the existing layout engine.

The converter already produces a *structure* (paragraphs, tables, cells) and
already owns a line-breaking engine that measures real glyph widths.  Those two
things are exactly what a document translator needs, so translation is not a new
pipeline -- it is a stage inserted between "read" and "write":

    PDF ──convert()──> DOCX model ──translate──> DOCX ──docx2pdf()──> PDF

Three ways to land the translated text, in order of how much layout survives:

``reflow`` (default)
    replace paragraph text, let the layout engine re-flow with target-language
    metrics.  Nothing ever overflows; page count may change.

``inplace``
    redact the original block and re-draw the translation inside the same box,
    shrinking type down to 75% before giving up.  Page count never changes,
    which is what contracts and manuals need.  Implemented in
    :mod:`pdfconv.translate_inplace`.

``bilingual``
    original paragraph followed by the translation (grey, slightly smaller).
    Reviewers read both; nothing is lost.

Translation units are sentences, not paragraphs: MT quality drops sharply on
long inputs, and sentence boundaries are what the placeholder protection keys
off.  Everything that must not be translated -- equations, citations, DOIs,
URLs, e-mail addresses, units, identifiers -- is lifted out into ``⟦n⟧``
placeholders before the call and restored afterwards; a unit whose placeholders
do not all come back is rejected and keeps its source text.

Engines are pluggable: :class:`HttpTranslator` speaks the OpenAI-compatible
chat API (any vendor, local or hosted), :class:`GlossaryTranslator` is a
deterministic term replacer used by the tests, :class:`EchoTranslator` is the
identity engine used to prove the plumbing adds no damage.
"""
import json
import os
import re
import urllib.request
from dataclasses import dataclass, field

from docx import Document
from docx.oxml.ns import qn

from . import measure

CJK_TARGETS = {"zh", "zh-cn", "zh-tw", "ja", "ko"}
PH_OPEN, PH_CLOSE = "\u27e6", "\u27e7"          # ⟦ ⟧ -- rare in real text

# ---------------------------------------------------------------------------
# protection: spans that must survive translation byte for byte
# ---------------------------------------------------------------------------
_PROTECT = (
    ("math", re.compile(r"\$\$[^$]{1,200}\$\$|\$[^$\n]{1,120}\$")),
    ("url", re.compile(r"https?://[^\s<>\"')]+|www\.[^\s<>\"')]+")),
    ("email", re.compile(r"[\w.+-]+@[\w-]+\.[\w.]{2,}")),
    ("doi", re.compile(r"\b10\.\d{4,9}/[-._;()/:A-Za-z0-9]+\b")),
    ("cite", re.compile(r"\[[\d,\s\u2013\u2014-]{1,20}\]")),
    ("id", re.compile(r"\b[A-Z]{2,}[A-Z0-9]*[-_]?\d[\w-]*\b")),
    ("unit", re.compile(r"\b\d+(?:\.\d+)?\s?(?:%|\u00b0C|mm|cm|km|kg|ms|Hz|"
                        r"kHz|MHz|GHz|GB|MB|KB|TB|dpi|fps)\b")),
)

# abbreviations that end with a dot but do not end a sentence
_ABBR = {"e.g", "i.e", "et al", "fig", "figs", "eq", "eqs", "ref", "refs",
         "vs", "cf", "no", "vol", "pp", "sec", "al", "approx", "resp"}

_SENT = re.compile(r"(?<=[.!?\u3002\uff01\uff1f])\s+|(?<=[.!?])(?=[A-Z(\[])")
_CJK = re.compile(r"[\u3400-\u9fff\u3040-\u30ff\uac00-\ud7af]")


def is_cjk_text(text):
    return bool(_CJK.search(text or ""))


# ---------------------------------------------------------------------------
# translator protocol + engines
# ---------------------------------------------------------------------------
class Translator:
    """Translate a batch of units.

    ``units`` are already placeholder-protected strings.  ``context`` carries
    the previous/next unit so the engine can keep pronouns and terminology
    consistent, and ``budget`` is the target length in characters (the caller
    derives it from the box the text has to fit into).
    """

    name = "base"

    def translate(self, units, src="en", tgt="zh", glossary=None,
                  context=None, budget=None):
        raise NotImplementedError


class EchoTranslator(Translator):
    """Identity engine: proves the plumbing itself damages nothing."""

    name = "echo"

    def translate(self, units, src="en", tgt="zh", glossary=None,
                  context=None, budget=None):
        return list(units)


class GlossaryTranslator(Translator):
    """Deterministic longest-match term replacement.

    Not a machine translator -- it exists so the pipeline can be tested without
    a network call, and so terminology can be enforced on top of any engine.
    """

    name = "glossary"

    def __init__(self, table=None):
        self.table = dict(table or {})

    def translate(self, units, src="en", tgt="zh", glossary=None,
                  context=None, budget=None):
        table = dict(self.table)
        table.update(glossary or {})
        keys = sorted(table, key=len, reverse=True)
        out = []
        for u in units:
            s = u
            for k in keys:
                s = re.sub(r"\b%s\b" % re.escape(k), table[k], s, flags=re.I)
            out.append(s)
        return out


class HttpTranslator(Translator):
    """OpenAI-compatible chat endpoint (vLLM, DeepSeek, OpenAI, local Ollama).

    Configure with environment variables so no key ever lands in the repo::

        PDFCONV_MT_BASE=https://api.deepseek.com/v1
        PDFCONV_MT_KEY=sk-...
        PDFCONV_MT_MODEL=deepseek-chat
    """

    name = "http"

    def __init__(self, base=None, key=None, model=None, timeout=90,
                 batch=16, temperature=0.2):
        self.base = (base or os.environ.get("PDFCONV_MT_BASE") or "").rstrip("/")
        self.key = key or os.environ.get("PDFCONV_MT_KEY") or ""
        self.model = model or os.environ.get("PDFCONV_MT_MODEL") or "gpt-4o-mini"
        self.timeout = timeout
        self.batch = batch
        self.temperature = temperature

    def _prompt(self, units, src, tgt, glossary, budget):
        terms = ""
        if glossary:
            pairs = list(glossary.items())[:80]
            terms = ("\nTerminology (must be used exactly):\n" +
                     "\n".join("- %s => %s" % (k, v) for k, v in pairs))
        limit = ""
        if budget:
            limit = ("\nKeep every line short: the target box fits about %d "
                     "characters, so prefer concise wording." % budget)
        sys_p = ("You are a professional technical translator. Translate from "
                 "%s to %s. Preserve every ⟦n⟧ placeholder exactly, keep "
                 "numbers, citations and formulas unchanged, and do not add "
                 "commentary. Return one line per input line, in order."
                 % (src, tgt)) + terms + limit
        body = "\n".join("%d. %s" % (i + 1, u) for i, u in enumerate(units))
        return sys_p, body

    def translate(self, units, src="en", tgt="zh", glossary=None,
                  context=None, budget=None):
        out = []
        for i in range(0, len(units), self.batch):
            chunk = units[i:i + self.batch]
            out.extend(self._call(chunk, src, tgt, glossary, budget))
        return out

    def _call(self, units, src, tgt, glossary, budget):
        sys_p, body = self._prompt(units, src, tgt, glossary, budget)
        payload = {
            "model": self.model,
            "temperature": self.temperature,
            "messages": [{"role": "system", "content": sys_p},
                         {"role": "user", "content": body}],
        }
        req = urllib.request.Request(
            self.base + "/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + self.key})
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            data = json.loads(r.read().decode("utf-8"))
        text = data["choices"][0]["message"]["content"]
        got = _parse_numbered(text, len(units))
        # anything the engine dropped keeps its source text: a partially
        # translated document is acceptable, a shuffled one is not
        return [g if g else u for g, u in zip(got, units)]


def _parse_numbered(text, n):
    """Pull '1. ...' lines out of a model reply, in order."""
    out = [""] * n
    for line in text.splitlines():
        m = re.match(r"^\s*(\d{1,3})[.)]\s*(.+)$", line)
        if not m:
            continue
        i = int(m.group(1)) - 1
        if 0 <= i < n and not out[i]:
            out[i] = m.group(2).strip()
    return out


# ---------------------------------------------------------------------------
# protection / restoration
# ---------------------------------------------------------------------------
def protect(text):
    """Lift non-translatable spans out.  Returns (masked, [(ph, original)])."""
    table = []

    def take(m, kind):
        for ph, orig in table:
            if orig == m.group(0):
                return ph
        ph = "%s%d%s" % (PH_OPEN, len(table), PH_CLOSE)
        table.append((ph, m.group(0)))
        return ph

    masked = text
    for _kind, rx in _PROTECT:
        masked = rx.sub(lambda m: take(m, _kind), masked)
    return masked, table


def restore(text, table):
    """Put the protected spans back.  Returns None when one went missing."""
    out = text
    for ph, orig in table:
        if ph not in out:
            return None
        out = out.replace(ph, orig)
    return out


def split_sentences(text):
    """Sentence split that survives abbreviations and CJK punctuation."""
    return [seg for _a, _b, seg in split_sentences_span(text)]


def split_sentences_span(text):
    """Same split, but returns ``(start, end, sentence)`` character ranges.

    The offsets are what lets :func:`_rewrite_spans` put each translated
    sentence back where its source sentence was, so inline bold / italic and
    footnote markers keep their positions inside the paragraph.
    """
    out = []
    start = 0
    for m in _SENT.finditer(text):
        head = text[start:m.start()].rstrip()
        if head:
            last = re.split(r"[\s(]", head)[-1].rstrip(".").lower()
            if last in _ABBR or len(last) == 1:
                continue                      # "Fig." / "J." do not end here
        if text[start:m.end()].strip():
            out.append((start, m.end(), text[start:m.end()].strip()))
        start = m.end()
    if text[start:].strip():
        out.append((start, len(text), text[start:].strip()))
    return out


# ---------------------------------------------------------------------------
# document translation
# ---------------------------------------------------------------------------
@dataclass
class Stats:
    units: int = 0
    chars_in: int = 0
    chars_out: int = 0
    skipped: int = 0
    fallback: int = 0
    paragraphs: int = 0
    tables: int = 0
    glossary: dict = field(default_factory=dict)
    # inplace mode only: text boxes seen, boxes that had to shrink, and boxes
    # whose translation could not be made to fit at all (kept in the source)
    boxes: int = 0
    shrunk: int = 0
    overflow: int = 0

    def as_dict(self):
        return dict(units=self.units, chars_in=self.chars_in,
                    chars_out=self.chars_out, skipped=self.skipped,
                    fallback=self.fallback, paragraphs=self.paragraphs,
                    tables=self.tables, boxes=self.boxes, shrunk=self.shrunk,
                    overflow=self.overflow)


def _join(sents, tgt):
    """Join translated sentences the way the target language does.

    A CJK target glues sentences together, but a sentence the engine left in
    Latin script still needs a space before it -- a partially translated
    paragraph otherwise reads ``[13].In this paper``.
    """
    if tgt.lower() in CJK_TARGETS:
        out = ""
        for s in sents:
            if not s:
                continue
            if out and not (measure.is_cjk(out[-1]) or measure.is_cjk(s[0])):
                out += " "
            out += s
        return out
    return " ".join(sents)


def translate_sentences(text, translator, src="en", tgt="zh", glossary=None,
                        budget=None, stats=None, memo=None):
    """Translate one paragraph and keep the source offsets of every sentence.

    Returns ``[(start, end, translated), ...]``.  ``memo`` is an optional
    ``{masked sentence: translated}`` dict so a repeated sentence is paid for
    once -- corpus papers repeat captions and headings constantly.
    """
    raw = text or ""
    if not raw.strip():
        return [(0, len(raw), raw)]
    if stats is not None:
        stats.chars_in += len(raw)
    spans = split_sentences_span(raw)
    if not spans:
        if stats is not None:
            stats.chars_out += len(raw)
        return [(0, len(raw), raw)]
    if stats is not None:
        stats.units += len(spans)

    work = []
    for a, b, seg in spans:
        masked, table = protect(seg)
        if not masked.strip() or (len(table) == 1 and
                                  masked.strip() == table[0][0]):
            work.append((a, b, None, seg, table))   # nothing to translate
        else:
            work.append((a, b, masked, seg, table))

    store = memo if memo is not None else {}
    todo = []
    for w in work:
        m = w[2]
        if m is not None and m not in store and m not in todo:
            todo.append(m)
    if todo:
        try:
            got = translator.translate(todo, src=src, tgt=tgt,
                                       glossary=glossary, budget=budget)
        except Exception:
            got = []
        if len(got) != len(todo):
            got = todo                     # engine misbehaved: keep the source
            if stats is not None:
                stats.fallback += 1
        for k, v in zip(todo, got):
            store[k] = v

    out = []
    for a, b, masked, seg, table in work:
        if masked is None:
            if stats is not None:
                stats.skipped += 1
            out.append((a, b, seg))
            continue
        cand = store.get(masked, masked)
        res = restore(cand, table)
        if res is None:
            if stats is not None:
                stats.fallback += 1
            res = seg
        out.append((a, b, res))
    if stats is not None:
        stats.chars_out += len(_join([t for _a, _b, t in out], tgt))
    return out


def translate_text(text, translator, src="en", tgt="zh", glossary=None,
                   budget=None, stats=None, memo=None):
    """Translate one paragraph, protecting what must not change."""
    pieces = translate_sentences(text, translator, src, tgt, glossary,
                                 budget, stats, memo)
    return _join([t for _a, _b, t in pieces], tgt)


def _set_cjk_font(run, font="SimSun"):
    """Point the east-asian font at something that has han glyphs."""
    try:
        rPr = run._element.get_or_add_rPr()
        rFonts = rPr.find(qn("w:rFonts"))
        if rFonts is None:
            from docx.oxml import OxmlElement
            rFonts = OxmlElement("w:rFonts")
            rPr.append(rFonts)
        rFonts.set(qn("w:eastAsia"), font)
    except Exception:
        pass


def _alloc(n, weights):
    """Split *n* characters over *weights* (largest-remainder, exact total)."""
    total = sum(weights) or 1
    raw = [n * w / float(total) for w in weights]
    base = [int(x) for x in raw]
    left = n - sum(base)
    if left > 0:
        order = sorted(range(len(raw)), key=lambda i: raw[i] - base[i],
                       reverse=True)
        for i in order[:left]:
            base[i] += 1
    return base


def _snap(text, pos):
    """Move *pos* out of the middle of a Latin word."""
    n = len(text)
    if pos <= 0 or pos >= n:
        return max(0, min(pos, n))
    if not (text[pos - 1].isalnum() and text[pos].isalnum()):
        return pos
    lo = pos
    while lo > 0 and text[lo - 1].isalnum():
        lo -= 1
    hi = pos
    while hi < n and text[hi].isalnum():
        hi += 1
    return lo if pos - lo <= hi - pos else hi


def _split_translation(target, parts, run_tr):
    """Cut *target* so every piece lines up with the run that produced it.

    Proportional cutting alone slices words in half: ``nonlinearity`` comes
    back as ``非线`` bold and ``性度`` roman.  So each source run is
    translated on its own and the result is *located* inside the sentence
    translation -- a term translated consistently is found verbatim, and its
    real span becomes the boundary.  Only when the engine is
    context-dependent and the run's translation does not appear does the
    proportional estimate stand, snapped out of the middle of a word.

    ``parts`` is ``[(run_index, source_text), ...]`` in document order.
    """
    total = sum(len(s) for _i, s in parts) or 1
    out, cur, cum = [], 0, 0
    last = len(parts) - 1
    for k, (ri, stext) in enumerate(parts):
        cum += len(stext)
        if k == last:
            out.append((ri, target[cur:]))
            break
        end = None
        term = (run_tr(stext) or "").strip()
        if len(term) >= 2:
            j = target.find(term, cur)
            if j >= 0 and j - cur <= max(8, len(term)):
                end = j + len(term)
        if end is None:
            end = _snap(target, int(round(len(target) * cum / float(total))))
        end = min(max(end, cur), len(target))
        out.append((ri, target[cur:end]))
        cur = end
    return out


def _rewrite_spans(par, pieces, tgt, run_tr=None):
    """Rewrite *par* run by run, keeping inline formatting and markers.

    Each translated sentence knows the source character range it came from;
    that range overlaps some runs.  The sentence translation is then cut at
    the boundaries those runs imply, so a bold term stays bold, an italic
    clause stays italic, and a footnote reference -- a run with no ``w:t`` --
    is never touched and never moves.
    """
    runs = list(par.runs)
    if not runs:
        if pieces:
            r = par.add_run(_join([t for _a, _b, t in pieces], tgt))
            if tgt.lower() in CJK_TARGETS:
                _set_cjk_font(r)
        return
    texts = [r.text or "" for r in runs]
    bounds, pos = [], 0
    for t in texts:
        bounds.append((pos, pos + len(t)))
        pos += len(t)
    per = [[] for _ in runs]
    cjk = tgt.lower() in CJK_TARGETS
    last = len(pieces) - 1
    for n, (a, b, tr) in enumerate(pieces):
        parts = []
        for i, (s, e) in enumerate(bounds):
            lo, hi = max(a, s), min(b, e)
            if hi > lo:
                parts.append((i, texts[i][lo - s:hi - s]))
        if not parts:
            parts = [(len(runs) - 1, "")]
        for i, chunk in _split_translation(tr, parts, run_tr or (lambda s: "")):
            per[i].append(chunk)
        if n != last and not cjk:
            per[parts[-1][0]].append(" ")
    for i, r in enumerate(runs):
        txt = "".join(per[i])
        ts = r._element.findall(qn("w:t"))
        if not ts:
            continue                       # footnote ref / drawing / field
        ts[0].text = txt
        ts[0].set(qn("xml:space"), "preserve")
        for t in ts[1:]:
            r._element.remove(t)
        if cjk and txt:
            _set_cjk_font(r)


def _rewrite(par, text, tgt, bilingual=False, grey=(0.42, 0.42, 0.42)):
    """Collapse *par* to one run holding *text*.

    Used for the bilingual clone, where the copy is re-styled wholesale
    anyway.  The normal path is :func:`_rewrite_spans`, which keeps the run
    structure intact.
    """
    runs = par.runs
    if not runs:
        r = par.add_run(text)
    else:
        first = runs[0]
        first.text = text
        # empty the other runs but keep their shells: a run may carry a
        # footnote reference, a drawing or a field code that must survive
        for r in runs[1:]:
            for t in r._element.findall(qn("w:t")):
                r._element.remove(t)
        r = first
    if tgt.lower() in CJK_TARGETS:
        _set_cjk_font(r)


def translate_docx(src_path, dst_path, translator, src="en", tgt="zh",
                   glossary=None, bilingual=False):
    """Translate every paragraph of a DOCX (body, tables, headers, footers)."""
    doc = Document(src_path)
    stats = Stats(glossary=dict(glossary or {}))
    memo = {}                       # masked sentence -> translation
    run_memo = {}                   # single run text -> translation

    def run_tr(text):
        """Translate one run in isolation, for the span-boundary pinning.

        Costs nothing extra for the engines we ship: the result is cached,
        and the glossary engine is deterministic per term.
        """
        key = text.strip()
        if not key:
            return ""
        if key in run_memo:
            return run_memo[key]
        out = translate_text(key, translator, src, tgt, glossary, memo=memo)
        run_memo[key] = out
        return out

    def do_par(par):
        if not par.text.strip():
            return
        pieces = translate_sentences(par.text, translator, src, tgt, glossary,
                                     stats=stats, memo=memo)
        new = _join([t for _a, _b, t in pieces], tgt)
        stats.paragraphs += 1
        if new == par.text:
            return
        if bilingual:
            from copy import deepcopy
            from docx.text.paragraph import Paragraph
            clone = deepcopy(par._p)
            par._p.addnext(clone)
            tp = Paragraph(clone, par._parent)
            _rewrite(tp, new, tgt)
            for r in tp.runs:
                r.font.color.rgb = None
                r.font.size = None
                r.italic = True
                try:
                    from docx.shared import RGBColor
                    r.font.color.rgb = RGBColor(*[int(c * 255) for c in grey])
                except Exception:
                    pass
            return
        _rewrite_spans(par, pieces, tgt, run_tr=run_tr)

    for par in doc.paragraphs:
        do_par(par)
    for tbl in doc.tables:
        stats.tables += 1
        for row in tbl.rows:
            seen = set()
            for cell in row.cells:
                if id(cell._tc) in seen:
                    continue
                seen.add(id(cell._tc))
                for par in cell.paragraphs:
                    do_par(par)
    for sec in doc.sections:
        for part in (sec.header, sec.footer):
            try:
                for par in part.paragraphs:
                    do_par(par)
            except Exception:
                pass
    os.makedirs(os.path.dirname(os.path.abspath(dst_path)) or ".", exist_ok=True)
    doc.save(dst_path)
    return stats


def translate_pdf(pdf_path, out_pdf, translator, src="en", tgt="zh",
                  glossary=None, bilingual=False, mode="reflow", docx_path=None):
    """PDF in, translated PDF out.

    ``mode="reflow"`` (default) converts to DOCX, translates, and renders the
    DOCX with our own engine, so the target language is measured with its own
    glyph widths and nothing overflows -- page count may change.

    ``mode="inplace"`` keeps the original page canvas and refills each text
    box, so page count, figure positions and page numbers are identical to
    the source.  ``mode="bilingual"`` is ``reflow`` with the source paragraph
    kept above each translation.

    Returns ``(stats, pages)``.
    """
    if mode == "inplace":
        from .translate_inplace import translate_pdf_inplace
        return translate_pdf_inplace(pdf_path, out_pdf, translator, src=src,
                                     tgt=tgt, glossary=glossary)
    from .convert import convert
    from .docx2pdf import convert_docx_pdf

    tmp_docx = docx_path or (os.path.splitext(out_pdf)[0] + ".zh.docx")
    convert(pdf_path, tmp_docx)
    stats = translate_docx(tmp_docx, tmp_docx, translator, src=src, tgt=tgt,
                           glossary=glossary,
                           bilingual=bilingual or mode == "bilingual")
    pages, _p, _t = convert_docx_pdf(tmp_docx, out_pdf)
    return stats, pages
