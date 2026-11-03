"""Page-level result cache.

Re-running the corpus costs minutes almost entirely inside OCR, and OCR is a
pure function of (file bytes, page number, settings).  Results are therefore
cached on disk keyed by content hash + page + a version tag:

* an unchanged PDF re-converts without touching the engine at all,
* editing the PDF invalidates only that file's pages (the hash changes),
* bumping :data:`VERSION` invalidates everything (do this whenever the OCR
  pipeline changes, otherwise stale text would be replayed).

The cache is a performance device, never a source of truth: any read or write
error is swallowed and the caller recomputes.
"""
import hashlib
import json
import os

VERSION = "4"
_DIR = os.path.join(os.path.expanduser("~"), ".cache", "pdfconv")
_hash_memo = {}


def _file_hash(path):
    try:
        st = os.stat(path)
    except OSError:
        return ""
    key = (os.path.abspath(path), st.st_size, int(st.st_mtime))
    hit = _hash_memo.get(key)
    if hit:
        return hit
    d = hashlib.sha1()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                d.update(chunk)
    except OSError:
        return ""
    h = d.hexdigest()[:20]
    _hash_memo[key] = h
    return h


def _slot(path, tag, pno):
    h = _file_hash(path)
    if not h:
        return None
    return os.path.join(_DIR, "%s-%s-%s-%d.json" % (h, VERSION, tag, pno))


def get(path, tag, pno):
    """Cached object for (file, tag, page), or None."""
    p = _slot(path, tag, pno)
    if not p:
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def put(path, tag, pno, obj):
    p = _slot(path, tag, pno)
    if not p:
        return
    try:
        os.makedirs(_DIR, exist_ok=True)
        tmp = "%s.%d.tmp" % (p, os.getpid())
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        os.replace(tmp, p)
    except Exception:
        pass


def stats():
    """(entries, bytes) currently held on disk."""
    n = size = 0
    try:
        for name in os.listdir(_DIR):
            try:
                size += os.path.getsize(os.path.join(_DIR, name))
                n += 1
            except OSError:
                pass
    except OSError:
        pass
    return n, size


def clear():
    removed = 0
    try:
        for name in os.listdir(_DIR):
            if name.endswith(".json"):
                try:
                    os.remove(os.path.join(_DIR, name))
                    removed += 1
                except OSError:
                    pass
    except OSError:
        pass
    return removed
