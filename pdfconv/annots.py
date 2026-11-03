"""PDF annotations -> Word comments.

Word comments must anchor to a run, while a PDF annotation is just a rectangle
on a page.  The mapping is done by geometry: each annotation is attached to the
paragraph on the same page whose box it overlaps most, falling back to the
nearest paragraph above it when there is no overlap (a sticky-note icon often
sits in the margin, outside the text column).

This is deliberately our own logic -- MuPDF only enumerates the annotations.
"""
import fitz


def collect(pdf):
    """Return {page_no (1-based): [(rect, kind, text, author), ...]}.

    Only annotations that carry text are kept: a bare highlight with no note
    has nothing to say in a Word comment, and inventing a placeholder would be
    worse than dropping it.

    The page's annot *generator* is consumed directly.  Materialising it into
    a list first invalidates the annotation handles in PyMuPDF 1.27 ("annotation
    not bound to any page"), which an earlier except-swallowing loop turned
    into silently losing every annotation.
    """
    out = {}
    for pno in range(len(pdf)):
        try:
            gen = pdf[pno].annots() or ()
        except Exception:
            continue
        for a in gen:
            try:
                info = a.info or {}
                text = (info.get("content") or "").strip()
                author = (info.get("title") or "").strip()
                kind = a.type[1]
                rect = fitz.Rect(a.rect)
            except Exception:
                continue
            if not text:
                continue
            out.setdefault(pno + 1, []).append((rect, kind, text, author))
    return out


def _overlap(a, b):
    r = a & b
    return 0.0 if r.is_empty else r.get_area()


def attach(doc, annots, emitted):
    """Attach collected annotations as Word comments.

    `emitted` is [(page_no, bbox, paragraph), ...] in document order.
    """
    if not annots or not emitted:
        return 0
    made = 0
    for pno, items in annots.items():
        page_paras = [e for e in emitted if e[0] == pno and e[2].runs]
        if not page_paras:
            continue
        for rect, kind, text, author in items:
            best, best_ov = None, 0.0
            for _p, box, para in page_paras:
                ov = _overlap(box, rect)
                if ov > best_ov:
                    best_ov, best = ov, para
            if best is None:
                # no overlap: take the last paragraph that starts above the note
                above = [e for e in page_paras if e[1].y0 <= rect.y0]
                best = (above[-1] if above else page_paras[0])[2]
            label = text
            if (kind and kind != "Text"
                    and not text.lower().startswith(kind.lower())):
                label = "%s: %s" % (kind, text)
            try:
                doc.add_comment(best.runs, text=label,
                                author=author or "PDF annotation",
                                initials=(author[:2] if author else "PDF"))
                made += 1
            except Exception:
                continue
    return made
