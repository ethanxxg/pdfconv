"""Shared GUI safety net for DocForge.

PyQt6 turns an unhandled Python exception inside a slot into ``qFatal()``,
which aborts the whole process.  In the windowed (console-less) exe that shows
up as "the window just vanished" with no message anywhere -- which is exactly
how the ``ConversionWorker(jobs, fmt=...)`` TypeError presented itself to the
user: click 开始转换, app gone, no clue why.

Every slot that can touch user data is wrapped with :func:`slot` so a failure
becomes a message box plus a traceback in
``%LOCALAPPDATA%\\DocForge\\docforge_crash.log`` instead of a silent crash.
"""

import functools
import inspect
import os
import time
import traceback

from PyQt6.QtWidgets import QMessageBox, QWidget

# The install dir is read-only for a per-user install (%LOCALAPPDATA%\Programs),
# so logs go to the per-user app-data folder instead.
CRASH_LOG_DIR = os.path.join(
    os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "DocForge")


def write_crash_log(text):
    """Append a traceback to the crash log; return its path ('' on failure)."""
    try:
        os.makedirs(CRASH_LOG_DIR, exist_ok=True)
        path = os.path.join(CRASH_LOG_DIR, "docforge_crash.log")
        with open(path, "a", encoding="utf-8") as f:
            f.write("\n===== %s =====\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
            f.write(text.rstrip() + "\n")
        return path
    except OSError:
        return ""


def slot(fn):
    """Decorator: keep an exception inside a slot from killing the process.

    IMPORTANT -- argument trimming.  When a signal is connected to a Python
    callable, PyQt passes only as many arguments as the callable can take:
    ``clicked(bool)`` -> ``_add_files_dialog(self)`` receives nothing.  A naive
    wrapper ``def wrapper(self, *args)`` advertises varargs, so PyQt hands over
    *every* signal argument and the wrapped function blows up with
    "takes 1 positional argument but 2 were given".  We therefore truncate to
    the wrapped function's real signature, preserving PyQt's own behaviour.
    """
    params = list(inspect.signature(fn).parameters.values())
    positional = [p for p in params
                  if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
    takes_varargs = any(p.kind == p.VAR_POSITIONAL for p in params)
    # 'self' is bound by the descriptor protocol, so it does not consume a
    # signal argument; keep at most (positional - 1) of them.
    max_extra = None if takes_varargs else max(len(positional) - 1, 0)

    @functools.wraps(fn)
    def wrapper(self, *args, **kwargs):
        if max_extra is not None and len(args) > max_extra:
            args = args[:max_extra]
        try:
            return fn(self, *args, **kwargs)
        except Exception:  # noqa: BLE001 - last line of defence for the GUI
            tb = traceback.format_exc()
            path = write_crash_log(tb)
            last = tb.strip().splitlines()[-1] if tb.strip() else "未知错误"
            try:
                QMessageBox.critical(
                    self if isinstance(self, QWidget) else None,
                    "DocForge 出错了",
                    "这个操作失败了，但程序没有退出。\n\n%s\n\n"
                    "详细日志：%s" % (last, path or "（无法写入日志）"))
            except Exception:  # noqa: BLE001 - never fail inside the handler
                pass
            return None
    return wrapper
