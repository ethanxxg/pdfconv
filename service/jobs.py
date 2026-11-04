"""Job store, bounded queue and worker pool.

The queue is the shock absorber that makes a 50k-task spike survivable: when
it fills, callers get 503 instead of the process dying, and the work already
accepted still completes at full speed.  Sizing rules are in README.
"""
import os
import queue
import threading
import time
import traceback
import uuid

from concurrent.futures import ProcessPoolExecutor

from .compress import compress_task, CompressError
from .convert import convert_task, ConvertError
from .merge import merge_task, MergeError
from .media import (pdf_to_images_task, images_to_pdf_task, MediaError)
from .pages import pages_task, PagesError


class Job:
    __slots__ = ("id", "kind", "opts", "result", "status", "created", "started",
                 "finished", "error", "pages", "out_key", "file_keys",
                 "rotations", "device", "ip")

    def __init__(self, jid, file_keys, device, ip, rotations=None,
                 kind="merge", opts=None):
        self.id = jid
        self.kind = kind
        self.opts = opts or {}
        self.result = None
        self.status = "queued"
        self.created = time.time()
        self.started = None
        self.finished = None
        self.error = None
        self.pages = 0
        # The extension belongs in the key: the download route decides the
        # Content-Type from it, and a DOCX served as application/pdf is a file
        # Word refuses to open.  Convert jobs can produce docx, xlsx or pptx, so
        # the target kind has to travel in opts.
        ext = ".pdf"
        if kind == "convert":
            ext = "." + str((opts or {}).get("dst") or "docx")
        elif kind == "compress":
            # Compress now takes PDF *or* JPG/PNG; the output extension must
            # match the source so the download route picks the right MIME.
            ext = (opts or {}).get("out_ext") or ".pdf"
        elif kind == "pages":
            # Split packs its many outputs into one zip; the other three page
            # ops produce a single PDF.
            ext = ".zip" if (opts or {}).get("op") == "split" else ".pdf"
        elif kind == "media":
            # PDF -> image returns a zip of PNG/JPG; image -> PDF returns a PDF.
            ext = ".zip" if (opts or {}).get("op") == "pdf2img" else ".pdf"
        self.out_key = "out/%s%s" % (jid, ext)
        self.file_keys = file_keys
        self.rotations = rotations or [0] * len(file_keys)
        self.device = device
        self.ip = ip

    def as_dict(self, ttl=None):
        d = {
            "job_id": self.id,
            "task": self.kind,
            "status": self.status,
            "pages": self.pages,
            "result": self.result,
            "error": self.error,
            "created": self.created,
            "elapsed": round((self.finished or time.time()) - self.created, 3),
            # queue wait vs actual work: the two failure modes of a saturated
            # pool look identical from the outside, and only this split tells
            # you whether to add workers or shorten the queue
            "queue_wait": (round(self.started - self.created, 3)
                           if self.started else None),
            "run_time": (round(self.finished - self.started, 3)
                         if self.finished and self.started else None),
        }
        if ttl:
            # what the result page counts down; computed here rather than in
            # the browser so a skewed client clock cannot mislead the user
            d["expires_in"] = max(0, int(self.created + ttl - time.time()))
        return d


class Runner:
    """Bounded queue in front of a process pool.

    Processes, not threads: merging is CPU-bound inside MuPDF, and the GIL is
    only released for some operations.  Each worker process also carries a
    ~120 MB interpreter + PyMuPDF baseline, so worker count is a memory
    decision as much as a CPU one.
    """

    def __init__(self, store, storage, workers=4, max_queue=64, ttl=3600):
        self.store = store
        self.storage = storage
        self.ttl = ttl
        self.q = queue.Queue(maxsize=max_queue)
        self.pool = ProcessPoolExecutor(max_workers=workers)
        self._pending = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.dispatcher = threading.Thread(target=self._dispatch, daemon=True)
        self.dispatcher.start()
        self.sweeper = threading.Thread(target=self._sweep, daemon=True)
        self.sweeper.start()

    # -- admission --------------------------------------------------------
    def submit(self, job, paths, out_path):
        try:
            self.q.put_nowait((job.id, paths, out_path))
        except queue.Full:
            raise RuntimeError("queue full")
        with self._lock:
            self._pending[job.id] = out_path
        job.status = "queued"
        return job

    def depth(self):
        return self.q.qsize()

    # -- worker loop ------------------------------------------------------
    def _dispatch(self):
        while not self._stop.is_set():
            try:
                jid, paths, out_path = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            job = self.store.get(jid)
            if job is None or job.status in ("cancelled", "expired"):
                continue
            job.status = "running"
            job.started = time.time()
            try:
                opts = job.opts or {}
                if job.kind == "compress":
                    fut = self.pool.submit(compress_task, paths[0], out_path,
                                           opts.get("level"),
                                           opts.get("max_pages"))
                elif job.kind == "convert":
                    fut = self.pool.submit(convert_task, paths[0], out_path,
                                           opts.get("tool"),
                                           opts.get("max_pages"),
                                           bool(opts.get("ocr")),
                                           opts.get("progress_path"))
                elif job.kind == "pages":
                    fut = self.pool.submit(
                        pages_task, paths[0], out_path, opts.get("op"),
                        opts.get("max_pages"), opts.get("pages"),
                        opts.get("angle"), opts.get("ranges"),
                        opts.get("every"))
                elif job.kind == "media":
                    if opts.get("op") == "pdf2img":
                        fut = self.pool.submit(
                            pdf_to_images_task, paths[0], out_path,
                            opts.get("fmt"), opts.get("dpi"),
                            opts.get("max_pages"))
                    else:
                        fut = self.pool.submit(
                            images_to_pdf_task, paths, out_path,
                            opts.get("max_pages"))
                else:
                    fut = self.pool.submit(merge_task, paths, out_path,
                                           job.rotations,
                                           opts.get("max_pages"))
            except Exception as e:                     # pool shut down
                job.status = "failed"
                job.error = {"code": "internal", "message": str(e)}
                continue
            fut.add_done_callback(lambda f, j=jid: self._done(j, f))

    def _done(self, jid, fut):
        job = self.store.get(jid)
        if job is None:
            return
        with self._lock:
            out_path = self._pending.pop(jid, None)
        try:
            res = fut.result()
            # merge returns a page count, compress returns a size report; both
            # are legitimate results, so normalise instead of branching on kind
            if isinstance(res, dict):
                job.pages = int(res.get("pages") or 0)
                job.result = res
            else:
                job.pages = int(res or 0)
            job.status = "done"
        except (MergeError, CompressError, ConvertError, PagesError,
                MediaError) as e:
            job.status = "failed"
            job.error = {"code": e.code, "message": e.message}
        except SystemExit as e:
            # The engine signals "no extractable text" with SystemExit, which
            # is a BaseException and so would escape the handler below -- and a
            # job whose done-callback raises never gets a final status, leaving
            # the UI polling forever.  Turn it into a normal failure.
            job.status = "failed"
            job.error = {"code": "no_text",
                         "message": str(e) or "文件没有可提取的文字"}
        except Exception:
            job.status = "failed"
            job.error = {"code": "internal", "message": "处理失败，请重试"}
            traceback.print_exc()
        finally:
            job.finished = time.time()
            if job.status != "done" and out_path:
                try:
                    os.remove(out_path)
                except OSError:
                    pass

    # -- retention --------------------------------------------------------
    def _sweep(self):
        while not self._stop.is_set():
            time.sleep(60)
            now = time.time()
            for job in self.store.expired(now, self.ttl):
                self._reap(job)

    def _reap(self, job):
        for k in job.file_keys:
            self.storage.delete(k)
        if job.out_key:
            self.storage.delete(job.out_key)
        job.status = "expired"
        self.store.drop(job.id)

    def cancel(self, job):
        for k in job.file_keys:
            self.storage.delete(k)
        if job.out_key:
            self.storage.delete(job.out_key)
        job.status = "cancelled"
        self.store.drop(job.id)

    def shutdown(self):
        self._stop.set()
        self.pool.shutdown(wait=False, cancel_futures=True)


class JobStore:
    def __init__(self):
        self._jobs = {}
        self._lock = threading.Lock()

    def create(self, file_keys, device, ip, rotations=None, kind="merge",
               opts=None):
        jid = uuid.uuid4().hex[:16]
        job = Job(jid, file_keys, device, ip, rotations, kind, opts)
        with self._lock:
            self._jobs[jid] = job
        return job

    def get(self, jid):
        return self._jobs.get(jid)

    def drop(self, jid):
        with self._lock:
            self._jobs.pop(jid, None)

    def expired(self, now, ttl):
        with self._lock:
            return [j for j in self._jobs.values()
                    if now - j.created > ttl
                    and j.status in ("done", "failed", "queued")]

    def __len__(self):
        return len(self._jobs)
