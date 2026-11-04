"""Throwaway server for the functional suites.

``test_e2e`` and ``test_compress`` need real 30-page papers to exercise rotation
and image resampling, but the shipped free tier allows 5 pages.  Those two
concerns must not be mixed: if the functional suites ran against production
limits they would either fail (as they did) or, worse, be quietly rewritten to
use 5-page toy files and stop testing anything.

So this module boots a private server with the allowance raised, on a free port,
with its own data directory.  ``test_limits.py`` deliberately does *not* use it
-- that file is the one that owns the shipped defaults and attacks them.

Usage::

    from service._harness import boot
    srv = boot("e2e", pages=5000)
    srv.base   # -> "http://127.0.0.1:51234"
    ...
    srv.stop() # also registered via atexit
"""
import atexit
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def free_port():
    """Ask the OS for an unused port.  Racy in theory, fine in practice."""
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


class Server:
    def __init__(self, proc, base, data):
        self.proc = proc
        self.base = base
        self.data = data

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def boot(name, pages=5000, file_mb=2048, jobs=0, workers=2, timeout=45,
         extra_env=None):
    """Start a server with relaxed limits and wait until /healthz answers.

    ``jobs=0`` means unlimited, matching the meaning of ``jobs_per_day`` in
    plans.py.  ``pages`` and ``file_mb`` are raised well past the corpus so the
    functional suites never trip an allowance.

    ``extra_env`` overrides any of the above (and adds feature switches such as
    ``PDFM_FREE_OCR``); it is applied last so it always wins.
    """
    data = os.path.join(ROOT, "service", "data_%s" % name)
    shutil.rmtree(data, ignore_errors=True)

    env = dict(os.environ)
    env.update({
        "PDFM_DATA": data,
        "PDFM_WORKERS": str(workers),
        "PDFM_RATE_MULT": "1000",     # rate limiting is test_limits' business
        "PDFM_MAX_PAGES": str(pages),
        "PDFM_MAX_FILE_MB": str(file_mb),
        "PDFM_MAX_TASK_MB": "4096",
        "PDFM_FREE_JOBS_PER_DAY": str(jobs),
    })
    if extra_env:
        env.update({k: str(v) for k, v in extra_env.items()})

    port = free_port()
    base = "http://127.0.0.1:%d" % port
    proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "service.main:app",
         "--host", "127.0.0.1", "--port", str(port), "--log-level", "warning"],
        cwd=ROOT, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    srv = Server(proc, base, data)
    atexit.register(srv.stop)

    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                "server for %r died on startup (exit %s); run it by hand with "
                "PDFM_DATA=%s to see the traceback" % (name, proc.returncode, data))
        try:
            with urllib.request.urlopen(base + "/healthz", timeout=1) as r:
                if r.status == 200:
                    return srv
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(0.2)

    srv.stop()
    raise RuntimeError("server for %r did not become healthy in %ds" % (name, timeout))
