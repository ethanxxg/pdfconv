"""PDF merge service -- P0 single-node reference implementation.

The API contract is deliberately the production one, so moving from local disk
to OSS/COS changes only ``storage.py``:

    POST /api/v1/uploads            -> signed upload targets
    PUT  /api/v1/uploads/{file_id}  -> raw bytes, streamed, never buffered
    POST /api/v1/jobs               -> enqueue a merge
    GET  /api/v1/jobs/{job_id}      -> poll status
    GET  /api/v1/jobs/{job_id}/download
    DELETE /api/v1/jobs/{job_id}    -> drop files early

Run:  python -m uvicorn service.main:app --host 0.0.0.0 --port 8090
"""
import asyncio
import json
import os
import re
import time
import uuid
import zipfile

from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import (FileResponse, HTMLResponse, JSONResponse,
                               RedirectResponse)
from fastapi.staticfiles import StaticFiles

import pymupdf as fitz

from .edit import (ANGLES, ANNOT_KINDS, OPS as EDIT_OPS, POSITIONS,
                   form_fields)
from .jobs import Runner, JobStore
from .limits import Limiter
from .plans import PLANS, Quota, describe, plan_of, resolve_plan
from .storage import LocalStorage, StorageError, new_secret
from . import tools as tool_registry

# -- configuration ---------------------------------------------------------
# Per-plan limits live in plans.py; these are the deployment-wide knobs that do
# not change with billing.
FREE = plan_of("free")
MAX_FILES = FREE["max_files"]
MAX_FILE_BYTES = FREE["max_file_mb"] * 1048576
MAX_TASK_BYTES = FREE["max_task_mb"] * 1048576
TTL = int(os.environ.get("PDFM_TTL", "3600"))
WORKERS = int(os.environ.get("PDFM_WORKERS", "0")) or max(2, (os.cpu_count() or 4) - 1)
QUEUE_MAX = int(os.environ.get("PDFM_QUEUE", "64"))
DATA = os.environ.get("PDFM_DATA", os.path.join(os.path.dirname(__file__), "data"))

# Token buckets.  Rates are per second, bursts are the instantaneous allowance.
# The IP bucket is sized for a shared NAT (a 500-person office), the device
# bucket for one person; both must pass.  PDFM_RATE_MULT scales both -- useful
# when load-testing a single node, where 60 jobs/hour would mask throughput.
_RATE_MULT = float(os.environ.get("PDFM_RATE_MULT", "1"))
IP_UPLOAD = (240 / 3600.0 * _RATE_MULT, 60 * _RATE_MULT)
IP_JOB = (60 / 3600.0 * _RATE_MULT, 20 * _RATE_MULT)
DEV_JOB = (120 / 3600.0 * _RATE_MULT, 30 * _RATE_MULT)

storage = LocalStorage(os.path.join(DATA, "objects"),
                       new_secret(os.path.join(DATA, "secret.key")))
store = JobStore()
limiter = Limiter()
quota = Quota()
runner = None
_started = time.time()
_stats = {"uploads": 0, "jobs": 0, "rejected": 0}


def _pid_alive(pid):
    """Is *pid* running?  Deliberately not os.kill(pid, 0).

    On Windows ``os.kill(pid, 0)`` is not a probe -- it calls TerminateProcess
    with exit code 0 and would kill the process we are trying to check.
    """
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        QUERY = 0x1000
        STILL_ACTIVE = 259
        h = ctypes.windll.kernel32.OpenProcess(QUERY, False, pid)
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return bool(ok) and code.value == STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _instance_guard():
    """Warn when two processes share one data dir.

    Job state and the daily quota are in-process, so a second instance on the
    same port range would answer 404 for jobs the first one created.  That looks
    like a random failure in the browser and is invisible in the logs, so say it
    out loud at startup instead.
    """
    path = os.path.join(DATA, "instance.pid")
    try:
        with open(path, "r", encoding="utf-8") as f:
            old = int((f.read() or "0").strip())
    except (OSError, ValueError):
        old = 0
    if old and old != os.getpid() and _pid_alive(old):
        print("WARNING: another instance (pid %d) already uses %s. Job state is "
              "per-process -- run exactly one web process per data directory."
              % (old, DATA), flush=True)
    try:
        os.makedirs(DATA, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(str(os.getpid()))
    except OSError:
        pass


@asynccontextmanager
async def lifespan(app):
    global runner
    _instance_guard()
    runner = Runner(store, storage, workers=WORKERS, max_queue=QUEUE_MAX, ttl=TTL)

    async def sweeper():
        while True:
            await asyncio.sleep(300)
            limiter.sweep()
            quota.sweep()

    task = asyncio.create_task(sweeper())
    yield
    task.cancel()
    runner.shutdown()


app = FastAPI(title="pdfkit", version="0.2.0", lifespan=lifespan)


# -- helpers ---------------------------------------------------------------
def client_ip(request):
    """Real client address behind a CDN.

    Only the left-most XFF entry is meaningful; the rest can be forged by the
    client, so trusting them would let anyone rotate their own rate-limit key.
    """
    xff = request.headers.get("x-forwarded-for")
    if xff:
        return xff.split(",")[0].strip()
    return request.client.host if request.client else "0.0.0.0"


def device_id(request):
    d = (request.headers.get("x-device-id") or "").strip()[:64]
    if d:
        return d
    # degraded fallback: no JS, or a client that strips the header
    return "ua:" + (request.headers.get("user-agent") or "")[:48]


def guard(request, bucket, cost=1.0):
    """Apply both buckets; raise 429 with Retry-After on either."""
    ip = client_ip(request)
    dev = device_id(request)
    ip_rate, ip_burst = bucket
    checks = ((("ip", ip), ip_rate, ip_burst),
              (("dev", dev), DEV_JOB[0], DEV_JOB[1]))
    for key, rate, burst in checks:
        ok, wait = limiter.take(key, rate, burst, cost)
        if not ok:
            _stats["rejected"] += 1
            raise HTTPException(
                status_code=429,
                detail={"code": "rate_limited",
                        "message": "请求过于频繁，请 %d 秒后再试" % max(1, int(wait + 0.5))},
                headers={"Retry-After": str(max(1, int(wait + 0.5)))},
            )
    return ip, dev


@app.exception_handler(StorageError)
async def _storage_error(request, exc):
    return JSONResponse(status_code=413,
                        content={"code": exc.code, "message": exc.message})


def _detect_kind(path):
    """pdf / docx / xlsx / pptx, or None.

    The zip magic is shared by all three OOXML kinds, so the container has to be
    opened and its top-level folders inspected.  This runs before the job is
    queued, which is the point: posting a .pptx to the Word tool must fail as a
    400 here, not as a stack trace inside a worker after the quota is spent.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(5)
        if head.startswith(b"%PDF-"):
            return "pdf"
        if not head.startswith(b"PK\x03\x04"):
            return None
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
    except (OSError, zipfile.BadZipFile):
        return None
    if any(n.startswith("word/") for n in names):
        return "docx"
    if any(n.startswith("xl/") for n in names):
        return "xlsx"
    if any(n.startswith("ppt/") for n in names):
        return "pptx"
    return None


def _detect_compress_kind(path):
    """pdf / jpg / png, or None.

    The compress task accepts both PDF and raster images, unlike the convert
    tools (which only take PDF or one OOXML kind).  Office files are refused
    with a clean ``wrong_type`` rather than a confusing "not a PDF" later.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    if head.startswith(b"%PDF-"):
        return "pdf"
    if head[:3] == b"\xff\xd8\xff":
        return "jpg"
    if head.startswith(b"\x89PNG"):
        return "png"
    return None


def _safe_stem(name):
    """Turn a user-supplied filename into a safe download stem.

    It only ever reaches the Content-Disposition header, but a name carrying a
    path separator or a newline is still worth refusing: the first is how a
    "rename" turns into a traversal, the second is header injection.
    """
    base = os.path.basename(str(name or "").replace("\\", "/"))
    base = os.path.splitext(base)[0]
    base = "".join(ch for ch in base
                   if ch.isprintable() and ch not in '"/\\:*?<>|').strip(" .")
    # An absent name must stay absent: the download route picks a per-task
    # default (edited / archive / pages / compressed / converted) via
    # ``stem or default``.  Returning "converted" here used to shadow every
    # one of those defaults whenever the caller omitted ``options.name``.
    return base[:80]


# -- edit option validation ------------------------------------------------
# Everything below is re-validated in service/edit.py, but doing it here too is
# what lets a malformed request fail with 400 *before* the daily allowance is
# consumed.  The shape is deliberately a closed whitelist: no unknown keys from
# the browser ever reach the worker.
_EDIT_HEX = re.compile(r"^#?[0-9a-fA-F]{6}$")


def _bad(code, message):
    raise HTTPException(400, detail={"code": code, "message": message})


def _estr(value, code, label, limit, default=""):
    s = str(value if value is not None else default).strip()
    if len(s) > limit:
        _bad(code, "%s最多 %d 个字符" % (label, limit))
    return s


def _enum(value, lo, hi, default, code, label):
    try:
        v = float(value if value not in (None, "") else default)
    except (TypeError, ValueError):
        _bad(code, "%s必须是数字" % label)
    if not lo <= v <= hi:
        _bad(code, "%s只能在 %s–%s 之间" % (label, lo, hi))
    return v


def _ecolor(value, default="#ff0000"):
    s = str(value or default).strip()
    if not _EDIT_HEX.match(s):
        _bad("bad_color", "颜色格式不正确，应为 #RRGGBB")
    return s if s.startswith("#") else "#" + s


def _epos(value, default="center"):
    pos = str(value or default).strip().lower()
    if pos not in POSITIONS:
        _bad("bad_position", "位置无效")
    return pos


def _ekeywords(o):
    raw = o.get("keywords") or []
    if isinstance(raw, str):
        raw = raw.replace("，", "\n").splitlines()
    if not isinstance(raw, list):
        _bad("bad_keywords", "关键词格式不正确")
    out = [str(x).strip()[:200] for x in raw if str(x).strip()]
    if len(out) > 50:
        _bad("bad_keywords", "一次最多 50 个关键词")
    return out


def _erects(o):
    raw = o.get("rects") or []
    if not isinstance(raw, list) or len(raw) > 200:
        _bad("bad_rect", "框选区域最多 200 个")
    out = []
    for b in raw:
        if not isinstance(b, dict):
            _bad("bad_rect", "框选区域格式不正确")
        try:
            d = {k: float(b.get(k)) for k in ("x", "y", "w", "h")}
            d["page"] = int(b.get("page") or 0)
        except (TypeError, ValueError):
            _bad("bad_rect", "框选区域格式不正确")
        if not all(0 <= d[k] <= 1 for k in ("x", "y", "w", "h")) \
                or d["w"] <= 0 or d["h"] <= 0:
            _bad("bad_rect", "框选区域超出页面范围")
        out.append(d)
    return out


def _edit_options(op, o):
    """Validate and normalise the per-op option dict for an edit job."""
    e = {"op": op,
         "pages": _estr(o.get("pages"), "bad_pages", "页码", 200)}
    if op == "watermark":
        e["text"] = _estr(o.get("text"), "bad_text", "水印文字", 120)
        if not e["text"]:
            _bad("bad_text", "请填写水印文字")
        e["position"] = _epos(o.get("position"))
        e["size"] = _enum(o.get("size"), 6, 96, 36, "bad_size", "字号")
        e["opacity"] = _enum(o.get("opacity"), 0.05, 1, 0.3,
                             "bad_opacity", "透明度")
        try:
            angle = int(o.get("angle") or 0)
        except (TypeError, ValueError):
            angle = -1
        if angle not in ANGLES:
            _bad("bad_angle", "水印角度只能是 0 / 30 / 45 / 60 / 90")
        e["angle"] = angle
        e["color"] = _ecolor(o.get("color"))
        e["tile"] = bool(o.get("tile"))
    elif op == "redact":
        e["keywords"] = _ekeywords(o)
        e["rects"] = _erects(o)
        if not e["keywords"] and not e["rects"]:
            _bad("bad_keywords", "请填写要抹掉的文字或框选区域")
        e["color"] = _ecolor(o.get("color"), "#000000")
    elif op == "annotate":
        kind = str(o.get("kind") or "highlight").strip().lower()
        if kind not in ANNOT_KINDS:
            _bad("bad_kind", "未知的批注类型")
        e["kind"] = kind
        e["keywords"] = _ekeywords(o)
        e["rects"] = _erects(o)
        e["note"] = _estr(o.get("note"), "bad_note", "批注内容", 500)
        e["author"] = _estr(o.get("author"), "bad_note", "批注人", 60) or "pdfkit"
        e["color"] = _ecolor(o.get("color"), "#ffd900")
        if kind in ("note", "freetext") and not e["note"]:
            _bad("bad_note", "请填写批注内容")
        if kind not in ("note", "freetext") and not e["keywords"] and not e["rects"]:
            _bad("bad_keywords", "请填写要标注的文字或框选区域")
    elif op == "stamp":
        e["position"] = _epos(o.get("position"), "br")
        e["scale"] = _enum(o.get("scale"), 0.05, 0.6, 0.18,
                           "bad_scale", "印章大小")
        e["opacity"] = _enum(o.get("opacity"), 0.1, 1, 1,
                             "bad_opacity", "透明度")
        try:
            rotation = int(o.get("rotation") or 0)
        except (TypeError, ValueError):
            rotation = -1
        if rotation not in (0, 90, 180, 270):
            _bad("bad_angle", "印章角度只能是 0 / 90 / 180 / 270")
        e["rotation"] = rotation
    elif op == "form":
        fields = o.get("fields") or {}
        if not isinstance(fields, dict) or not fields or len(fields) > 200:
            _bad("bad_fields", "请填写 1–200 个表单字段")
        e["fields"] = {str(k)[:120]:
                       (v if isinstance(v, bool) else str(v)[:2000])
                       for k, v in fields.items()}
        e["flatten"] = bool(o.get("flatten", True))
    else:  # rewrite
        raw = o.get("pairs") or []
        if not isinstance(raw, list) or len(raw) > 20:
            _bad("bad_pairs", "一次最多 20 条替换规则")
        pairs = []
        for it in raw:
            if not isinstance(it, dict):
                continue
            find = _estr(it.get("find"), "bad_pairs", "查找文字", 200)
            repl = _estr(it.get("replace"), "bad_pairs", "替换文字", 200)
            if find:
                pairs.append({"find": find, "replace": repl})
        if not pairs:
            _bad("bad_pairs", "请填写要查找和替换的文字")
        e["pairs"] = pairs
    return e


def _read_progress(job):
    """(done, total) for a running convert job, or None."""
    path = (job.opts or {}).get("progress_path")
    if not path:
        return None
    try:
        with open(path) as f:
            done, _, total = f.read().strip().partition("/")
        return int(done), int(total)
    except (OSError, ValueError):
        return None


def _count_pages(keys):
    """Total pages across the uploaded files.

    This runs in the API process, before the job is queued, because the daily
    quota is consumed at submit time: a job that fails on page count inside the
    worker would burn the user's only free attempt and hand back an error.  Only
    the xref is parsed, so a 15 MB file costs a few milliseconds.
    """
    total = 0
    for k in keys:
        try:
            with fitz.open(storage.path(k)) as d:
                if d.needs_pass:
                    raise HTTPException(400, detail={
                        "code": "encrypted",
                        "message": "文件已加密，请先解除密码"})
                total += d.page_count
        except HTTPException:
            raise
        except Exception:
            raise HTTPException(400, detail={
                "code": "bad_pdf", "message": "文件无法读取，请重新上传"})
    return total


# -- routes ----------------------------------------------------------------
@app.get("/healthz")
async def healthz():
    return {
        "ok": True,
        "uptime": round(time.time() - _started, 1),
        "workers": WORKERS,
        "queue_depth": runner.depth() if runner else 0,
        "jobs": len(store),
        "buckets": len(limiter),
        "quota_keys": len(quota),
        "stats": _stats,
    }


@app.get("/api/v1/config")
async def config(request: Request):
    """Limits, so a page never hardcodes a number the server disagrees with.

    Without this the browser's "max 20 files" drifts from PDFM_MAX_FILES the
    first time anyone changes the environment, and the user finds out only
    after uploading.
    """
    plan = resolve_plan(request)
    used = quota.used(device_id(request))
    return {
        "max_files": MAX_FILES,
        "max_file_mb": MAX_FILE_BYTES // 1048576,
        "max_task_mb": MAX_TASK_BYTES // 1048576,
        "max_pages": FREE["max_pages"],
        "min_files": 2,
        "ttl": TTL,
        "rotations": [0, 90, 180, 270],
        "levels": ["extreme", "recommended", "less"],
        "ocr": plan_of(plan)["ocr"],
        "tools": tool_registry.describe_all(),
        "plan": describe(plan, used),
    }


@app.get("/api/v1/tools")
async def tools_index():
    """The tool grid and the shared nav both build themselves from this."""
    return {"tools": tool_registry.describe_all()}


@app.post("/api/v1/edit/fields")
async def edit_form_fields(request: Request):
    """List AcroForm fields so the form-filling page can build its inputs.

    Read-only and xref-only, so it costs about as much as a page count.  Gated
    by the same plan switch as the edit jobs themselves: otherwise a free user
    could enumerate a form's field names even though filling them is blocked.
    """
    guard(request, (IP_JOB[0], IP_JOB[1]))
    if not plan_of(resolve_plan(request))["edit"]:
        raise HTTPException(403, detail={
            "code": "plan_limit",
            "message": "PDF 编辑是 Pro 功能，升级后可填写表单"})
    body = await request.json()
    fid = str(body.get("file_id") or "").strip()
    if not fid or len(fid) > 128:
        raise HTTPException(400, detail={"code": "bad_file_id",
                                         "message": "文件无效"})
    key = "in/" + fid
    path = storage.path(key)
    if storage.size(key) <= 0:
        raise HTTPException(400, detail={"code": "missing_file",
                                         "message": "文件已过期，请重新上传"})
    kind = await asyncio.to_thread(_detect_kind, path)
    if kind != "pdf":
        raise HTTPException(400, detail={"code": "wrong_type",
                                         "message": "编辑工具只支持 PDF 文件"})
    try:
        fields = await asyncio.to_thread(form_fields, path)
    except Exception:
        raise HTTPException(400, detail={"code": "bad_pdf",
                                         "message": "无法读取表单，请检查文件"})
    return {"fields": fields, "count": len(fields)}


@app.get("/api/v1/plans")
async def plans_index(request: Request):
    """Every tier, so the pricing page and the upsell card read from one source.

    Free is first and current; the rest are shown but not selectable -- choosing
    one has to go through billing, not through this endpoint.
    """
    here = resolve_plan(request)
    return {
        "current": here,
        "plans": [dict(describe(k), current=(k == here)) for k in PLANS],
    }


@app.post("/api/v1/uploads")
async def create_uploads(request: Request):
    guard(request, (IP_UPLOAD[0], IP_UPLOAD[1]))
    plan = plan_of(resolve_plan(request))
    max_files = plan["max_files"]
    max_bytes = plan["max_file_mb"] * 1048576
    max_task = plan["max_task_mb"] * 1048576
    body = await request.json()
    files = body.get("files") or []
    if not files:
        raise HTTPException(400, detail={"code": "no_files",
                                         "message": "没有选择文件"})
    if len(files) > max_files:
        raise HTTPException(400, detail={"code": "too_many_files",
                                         "message": "最多 %d 个文件" % max_files})
    total = 0
    targets = []
    for f in files:
        size = int(f.get("size") or 0)
        if size <= 0:
            raise HTTPException(400, detail={"code": "bad_size",
                                             "message": "文件为空"})
        if size > max_bytes:
            raise HTTPException(400, detail={
                "code": "file_too_large",
                "message": "单个文件不能超过 %d MB" % plan["max_file_mb"]})
        total += size
        fid = uuid.uuid4().hex
        targets.append({"file_id": fid,
                        "url": "/api/v1/uploads/" + fid,
                        "method": "PUT",
                        "name": (f.get("name") or "file.pdf")[:120]})
    if total > max_task:
        raise HTTPException(400, detail={
            "code": "task_too_large",
            "message": "合计不能超过 %d MB" % plan["max_task_mb"]})
    _stats["uploads"] += len(targets)
    return {"uploads": targets, "max_files": max_files,
            "max_file_mb": plan["max_file_mb"]}


@app.put("/api/v1/uploads/{file_id}")
async def upload_body(file_id: str, request: Request):
    key = "in/" + file_id
    path = storage.path(key)
    limit = plan_of(resolve_plan(request))["max_file_mb"] * 1048576
    n = 0
    head = b""
    try:
        with open(path, "wb") as f:
            async for chunk in request.stream():
                if not chunk:
                    continue
                n += len(chunk)
                if n > limit:
                    raise HTTPException(413, detail={
                        "code": "file_too_large",
                        "message": "文件超过 %d MB" % (limit // 1048576)})
                if len(head) < 5:
                    head += chunk[:5 - len(head)]
                if len(head) >= 5 and not (
                        head.startswith(b"%PDF-")
                        or head.startswith(b"PK\x03\x04")
                        or head.startswith(b"\xff\xd8\xff")
                        or head.startswith(b"\x89PNG")):
                        raise HTTPException(415, detail={
                            "code": "not_pdf",
                            "message": "不是有效的 PDF、Office 或图片文件"})
                # keep the event loop free while the page hits the disk
                await asyncio.to_thread(f.write, chunk)
    except Exception:
        try:
            os.remove(path)
        except OSError:
            pass
        raise
    if n == 0:
        try:
            os.remove(path)
        except OSError:
            pass
        raise HTTPException(400, detail={"code": "empty", "message": "空文件"})
    return {"file_id": file_id, "size": n}


@app.post("/api/v1/jobs")
async def create_job(request: Request):
    ip, dev = guard(request, (IP_JOB[0], IP_JOB[1]))
    plan_name = resolve_plan(request)
    plan = plan_of(plan_name)

    # Daily allowance is checked before anything else: it is the one limit that
    # exists for business reasons, so failing it must not consume an upload.
    ok, _used, _left = quota.check(dev, plan_name)
    if not ok:
        raise HTTPException(429, detail={
            "code": "quota_exceeded",
            "message": "今天的 %d 次免费额度已用完，明天再来，或升级 Pro"
                       % plan["jobs_per_day"]},
            headers={"Retry-After": "3600"})

    body = await request.json()
    task = str(body.get("task") or "merge").strip().lower()
    if task not in ("merge", "compress", "convert", "pages", "media", "pdfa",
                    "edit"):
        raise HTTPException(400, detail={"code": "bad_task",
                                         "message": "不支持的任务类型"})

    # Editing is the paid hook.  Check before any file work: returning 403
    # here must not consume an upload or the caller's daily allowance.
    if task == "edit" and not plan["edit"]:
        raise HTTPException(403, detail={
            "code": "plan_limit",
            "message": "PDF 编辑是 Pro 功能：升级后可用加水印、涂黑脱敏、批注、"
                       "盖章、表单填写与文字原地改写"})

    # Two accepted shapes: the current one carries a per-file rotation, the
    # flat one is what the first cut of the page sent.  Keeping both means an
    # already-cached page does not start failing after this deploy.
    raw = body.get("files")
    if raw is None:
        raw = [{"file_id": x} for x in (body.get("file_ids") or [])]
    if not isinstance(raw, list) or not raw:
        raise HTTPException(400, detail={"code": "bad_request",
                                         "message": "请求格式不正确"})
    if len(raw) > plan["max_files"]:
        raise HTTPException(400, detail={"code": "too_many_files",
                                         "message": "最多 %d 个文件"
                                                    % plan["max_files"]})

    if task == "merge":
        if len(raw) < 2:
            raise HTTPException(400, detail={"code": "need_two",
                                             "message": "至少需要 2 个 PDF 才能合并"})
    elif task == "convert":
        # One PDF in, one DOCX out: merging several documents into one Word file
        # is a different product decision, not something to guess at.
        if len(raw) != 1:
            raise HTTPException(400, detail={
                "code": "one_file",
                "message": "一次只能转换 1 个文件"})
    elif task == "pages":
        # Every page operation works on one PDF: there is no meaningful
        # "split two documents at once".
        if len(raw) != 1:
            raise HTTPException(400, detail={
                "code": "one_file",
                "message": "一次只能处理 1 个 PDF"})
    elif task == "pdfa":
        # Archiving is per document: the XMP and output intent describe one
        # file, so merging several into one PDF/A would be a different tool.
        if len(raw) != 1:
            raise HTTPException(400, detail={
                "code": "one_file",
                "message": "一次只能归档 1 个 PDF"})
    elif task == "edit":
        eop = str((body.get("options") or {}).get("op") or "").strip().lower()
        if eop not in EDIT_OPS:
            raise HTTPException(400, detail={"code": "bad_op",
                                             "message": "未知的编辑操作"})
        # Stamp is the one edit that takes a second upload (the seal image).
        want = 2 if eop == "stamp" else 1
        if len(raw) != want:
            raise HTTPException(400, detail={
                "code": "wrong_file_count",
                "message": ("盖章需要 1 个 PDF 和 1 张 JPG/PNG 图片"
                            if want == 2 else "编辑一次只能处理 1 个 PDF")})
    elif task == "media":
        mop = str((body.get("options") or {}).get("op") or "").strip().lower()
        if mop == "pdf2img":
            if len(raw) != 1:
                raise HTTPException(400, detail={
                    "code": "one_file",
                    "message": "PDF 转图片一次只能处理 1 个 PDF"})
        elif mop == "img2pdf":
            if not raw:
                raise HTTPException(400, detail={
                    "code": "no_files", "message": "没有选择图片"})
        else:
            raise HTTPException(400, detail={"code": "bad_op",
                                             "message": "未知的图片操作"})
    elif len(raw) != 1:
        # Batch compression is the paid hook: the free tier does one file, and
        # saying so is better than silently compressing only the first one.
        if not plan["batch_compress"]:
            raise HTTPException(403, detail={
                "code": "plan_limit",
                "message": "免费版一次只能压缩 1 个文件，升级 Pro 可批量压缩"})
        raise HTTPException(400, detail={"code": "not_implemented",
                                         "message": "批量压缩尚未开放"})

    fids, rots = [], []
    for i, item in enumerate(raw):
        if isinstance(item, str):
            item = {"file_id": item}
        item = item or {}
        fid = str(item.get("file_id") or "").strip()
        if not fid:
            raise HTTPException(400, detail={"code": "bad_file_id",
                                             "message": "第 %d 个文件无效" % (i + 1)})
        try:
            deg = int(item.get("rotate") or 0) % 360
        except (TypeError, ValueError):
            deg = -1
        if deg % 90:
            raise HTTPException(400, detail={
                "code": "bad_rotation",
                "message": "旋转角度只能是 90 的整数倍"})
        fids.append(fid)
        rots.append(deg)

    level = None
    opts = {"max_pages": plan["max_pages"]}
    tool = None
    if task == "compress":
        level = str((body.get("options") or {}).get("level") or "recommended").lower()
        if level not in plan["levels"]:
            raise HTTPException(400, detail={"code": "bad_level",
                                             "message": "压缩档位无效"})
        opts["level"] = level
    elif task == "convert":
        tool_id = str((body.get("options") or {}).get("tool") or "pdf-to-word")
        tool = tool_registry.get(tool_id)
        if tool is None or tool_id in ("merge", "compress"):
            raise HTTPException(400, detail={"code": "bad_tool",
                                             "message": "未知的转换类型"})
        want_ocr = bool((body.get("options") or {}).get("ocr"))
        if want_ocr and not plan["ocr"]:
            raise HTTPException(403, detail={
                "code": "plan_limit",
                "message": "OCR 识别是 Pro 功能，免费版只能转换带文字层的 PDF"})
        opts["ocr"] = want_ocr
        opts["tool"] = tool_id
        opts["src"] = tool["src"]
        opts["dst"] = tool["dst"]
        # For Office -> PDF the output page count cannot be known before
        # rendering, and the allowance is already spent by then.  Failing a
        # job for being "too many pages" after charging for it is the exact
        # anti-pattern the PDF path avoids by pre-counting, so this direction
        # gets the hard ceiling instead of the plan's small number.  The 20 MB
        # input cap is what bounds abuse here.
        if tool["src"] != "pdf":
            opts["max_pages"] = 5000
        # Carried only to name the downloaded file; never used to build a path.
        opts["name"] = _safe_stem((body.get("options") or {}).get("name"))
    elif task == "pages":
        o = body.get("options") or {}
        op = str(o.get("op") or "").strip().lower()
        if op not in ("split", "extract", "delete", "rotate"):
            raise HTTPException(400, detail={"code": "bad_op",
                                             "message": "未知的页面操作"})
        opts["op"] = op
        # Only names the download; never used to build a path.
        opts["name"] = _safe_stem(o.get("name"))
        if op == "split":
            ranges = str(o.get("ranges") or "").strip()
            if ranges:
                opts["ranges"] = ranges
            every = o.get("every")
            if every not in (None, ""):
                try:
                    every = int(every)
                except (TypeError, ValueError):
                    raise HTTPException(400, detail={
                        "code": "bad_every", "message": "「每 N 页」必须是整数"})
                if every < 1:
                    raise HTTPException(400, detail={
                        "code": "bad_every", "message": "「每 N 页」必须大于 0"})
                opts["every"] = every
            if not opts.get("ranges") and not opts.get("every"):
                raise HTTPException(400, detail={
                    "code": "bad_pages",
                    "message": "请填写拆分区间，或设置每 N 页拆分"})
        elif op in ("extract", "delete"):
            spec = str(o.get("pages") or "").strip()
            if not spec:
                raise HTTPException(400, detail={
                    "code": "bad_pages",
                    "message": "请填写页码，例如 1,3,5-7"})
            opts["pages"] = spec
        elif op == "rotate":
            try:
                angle = int(o.get("angle") or 0)
            except (TypeError, ValueError):
                angle = 0
            if angle not in (90, 180, 270):
                raise HTTPException(400, detail={
                    "code": "bad_angle", "message": "旋转角度只能是 90 / 180 / 270"})
            opts["angle"] = angle
            spec = str(o.get("pages") or "").strip()
            if spec:
                opts["pages"] = spec
    elif task == "media":
        o = body.get("options") or {}
        op = str(o.get("op") or "").strip().lower()
        if op not in ("pdf2img", "img2pdf"):
            raise HTTPException(400, detail={"code": "bad_op",
                                             "message": "未知的图片操作"})
        opts["op"] = op
        opts["name"] = _safe_stem(o.get("name"))
        if op == "pdf2img":
            fmt = str(o.get("fmt") or "png").lower()
            if fmt in ("jpg", "jpeg"):
                fmt = "jpg"
            elif fmt != "png":
                raise HTTPException(400, detail={
                    "code": "bad_fmt", "message": "图片格式只能是 PNG 或 JPG"})
            opts["fmt"] = fmt
            try:
                dpi = int(o.get("dpi") or 150)
            except (TypeError, ValueError):
                dpi = 0
            if dpi not in (72, 150, 300):
                raise HTTPException(400, detail={
                    "code": "bad_dpi",
                    "message": "分辨率只能是 72 / 150 / 300 DPI"})
            opts["dpi"] = dpi
    elif task == "pdfa":
        o = body.get("options") or {}
        # Level B only: level A additionally requires correct tagging and a
        # logical structure tree, which cannot be invented for an arbitrary
        # input PDF.
        part = str(o.get("part") or "2").strip().lower().rstrip("b")
        if part not in ("2", "3"):
            raise HTTPException(400, detail={
                "code": "bad_part",
                "message": "PDF/A 版本只能是 2b 或 3b"})
        opts["part"] = part
        # Only names the download; never used to build a path.
        opts["name"] = _safe_stem(o.get("name"))
    elif task == "edit":
        o = body.get("options") or {}
        op = str(o.get("op") or "").strip().lower()
        if op not in EDIT_OPS:
            raise HTTPException(400, detail={"code": "bad_op",
                                             "message": "未知的编辑操作"})
        opts["op"] = op
        opts["options"] = _edit_options(op, o)
        opts["name"] = _safe_stem(o.get("name"))

    keys = ["in/" + f for f in fids]
    total = 0
    for k in keys:
        s = storage.size(k)
        if s <= 0:
            raise HTTPException(400, detail={
                "code": "missing_file",
                "message": "文件已过期或未上传完成，请重新上传"})
        total += s
    if total > plan["max_task_mb"] * 1048576:
        raise HTTPException(400, detail={
            "code": "task_too_large",
            "message": "合计不能超过 %d MB" % plan["max_task_mb"]})

    if tool is not None:
        # The declared extension is a claim; the container is the fact.
        kind = await asyncio.to_thread(_detect_kind, storage.path(keys[0]))
        if kind != tool["src"]:
            raise HTTPException(400, detail={
                "code": "wrong_type",
                "message": "这个工具需要 %s 文件，请换一个工具或换一个文件"
                           % tool["src"].upper()})

    if task == "compress":
        # PDF, JPEG and PNG are all valid inputs; everything else is refused
        # here with a clean message instead of a stack trace in the worker.
        ckind = await asyncio.to_thread(_detect_compress_kind,
                                        storage.path(keys[0]))
        if ckind is None:
            raise HTTPException(400, detail={
                "code": "wrong_type",
                "message": "压缩仅支持 PDF、JPG 和 PNG 文件"})
        opts["src_kind"] = ckind
        opts["out_ext"] = {"pdf": ".pdf", "jpg": ".jpg", "png": ".png"}[ckind]
        opts["name"] = _safe_stem((body.get("options") or {}).get("name"))

    if task in ("pages", "pdfa", "edit"):
        # All three are PDF-only.  Refuse a .docx here with a clean message
        # instead of letting it reach the worker and fail on fitz.open.
        pkind = await asyncio.to_thread(_detect_kind, storage.path(keys[0]))
        if pkind != "pdf":
            label = {"pages": "页面工具", "pdfa": "PDF/A 归档",
                     "edit": "编辑工具"}[task]
            raise HTTPException(400, detail={
                "code": "wrong_type",
                "message": "%s只支持 PDF 文件" % label})
        if task == "edit" and opts["op"] == "stamp":
            ikind = await asyncio.to_thread(_detect_compress_kind,
                                            storage.path(keys[1]))
            if ikind not in ("jpg", "png"):
                raise HTTPException(400, detail={
                    "code": "wrong_type",
                    "message": "印章图片只支持 JPG 或 PNG"})

    if task == "media":
        if opts["op"] == "pdf2img":
            mkind = await asyncio.to_thread(_detect_kind, storage.path(keys[0]))
            if mkind != "pdf":
                raise HTTPException(400, detail={
                    "code": "wrong_type",
                    "message": "PDF 转图片需要 PDF 文件"})
        else:
            # Every input must be a real JPEG/PNG, judged by magic bytes.
            for i, k in enumerate(keys):
                ik = await asyncio.to_thread(_detect_compress_kind,
                                             storage.path(k))
                if ik not in ("jpg", "png"):
                    raise HTTPException(400, detail={
                        "code": "wrong_type",
                        "message": "第 %d 个文件不是 JPG 或 PNG 图片"
                                   % (i + 1)})

    # Page counting only applies to PDF inputs.  Merge and any PDF-source task
    # are PDF; Office -> PDF is checked in the worker (its page count is unknown
    # before rendering); image inputs have no pages at all, so the free-tier
    # page cap must not fire on them.
    src_is_pdf = (task in ("merge", "pages", "pdfa", "edit")
                  or (tool is not None and tool["src"] == "pdf")
                  or (task == "compress" and opts.get("src_kind") == "pdf"))
    if src_is_pdf:
        # A stamp job's second upload is an image, not a PDF: it has no pages
        # and must not be handed to _count_pages.
        page_keys = keys[:1] if task == "edit" else keys
        pages = await asyncio.to_thread(_count_pages, page_keys)
        if pages > plan["max_pages"]:
            raise HTTPException(400, detail={
                "code": "too_many_pages",
                "message": "共 %d 页，%s最多 %d 页" % (pages, plan["name"],
                                                    plan["max_pages"])})

    if task == "media":
        # pdf2img is bounded by the PDF's page count; img2pdf by the number of
        # images (one page each).  Neither goes through src_is_pdf.
        if opts["op"] == "pdf2img":
            pages = await asyncio.to_thread(_count_pages, keys)
            if pages > plan["max_pages"]:
                raise HTTPException(400, detail={
                    "code": "too_many_pages",
                    "message": "共 %d 页，%s最多 %d 页"
                               % (pages, plan["name"], plan["max_pages"])})
        elif len(keys) > plan["max_pages"]:
            raise HTTPException(400, detail={
                "code": "too_many_pages",
                "message": "共 %d 张图片，%s最多 %d 页"
                           % (len(keys), plan["name"], plan["max_pages"])})

    job = store.create(keys, dev, ip, rots, kind=task, opts=opts)
    out_path = storage.path(job.out_key)
    if task == "convert":
        # The worker is a separate process; this file is how its per-page
        # counter gets back to the polling loop.
        job.opts["progress_path"] = out_path + ".progress"
    try:
        runner.submit(job, [storage.path(k) for k in keys], out_path)
    except RuntimeError:
        store.drop(job.id)
        raise HTTPException(503, detail={
            "code": "busy",
            "message": "服务繁忙，请稍后重试"},
            headers={"Retry-After": "5"})
    # consume only after the job is actually queued, so a rejected request or a
    # full queue never burns someone's daily allowance
    quota.take(dev)
    _stats["jobs"] += 1

    return {"job_id": job.id, "task": task, "status": job.status,
            "queue_depth": runner.depth(),
            "jobs_left": quota.check(dev, plan_name)[2]}


@app.get("/api/v1/jobs/{job_id}")
async def job_status(job_id: str):
    job = store.get(job_id)
    if job is None:
        raise HTTPException(404, detail={"code": "not_found",
                                         "message": "任务不存在或已过期"})
    d = job.as_dict(ttl=TTL)
    if job.status == "done":
        d["download"] = storage.sign(job.out_key, ttl=900)
    elif job.status == "running" and job.kind == "convert":
        # Conversion is ~4 s/page, so "running" with no counter looks hung.
        p = _read_progress(job)
        if p:
            d["progress"] = {"done": p[0], "total": p[1]}
    return d


@app.get("/api/v1/jobs/{job_id}/download")
async def job_download(job_id: str):
    job = store.get(job_id)
    if job is None or job.status != "done":
        raise HTTPException(404, detail={"code": "not_ready",
                                         "message": "文件尚未生成"})
    return {"url": storage.sign(job.out_key, ttl=900)}


@app.delete("/api/v1/jobs/{job_id}")
async def job_delete(job_id: str):
    job = store.get(job_id)
    if job is None:
        return {"ok": True}
    runner.cancel(job)
    return {"ok": True}


@app.get("/api/v1/download/{key:path}")
async def signed_download(key: str, exp: str = "", sig: str = ""):
    if not storage.verify(key, exp, sig):
        raise HTTPException(403, detail={"code": "bad_signature",
                                         "message": "链接无效或已过期"})
    path = storage.path(key)
    if not os.path.exists(path):
        raise HTTPException(404, detail={"code": "gone", "message": "文件已删除"})

    # The key carries the extension precisely so this route does not have to
    # guess: a DOCX sent as application/pdf is a file Word refuses to open.
    ext = os.path.splitext(key)[1].lower()
    kind = {".docx": "docx", ".xlsx": "xlsx", ".pptx": "pptx",
            ".jpg": "jpg", ".jpeg": "jpg", ".png": "png",
            ".zip": "zip"}.get(ext, "pdf")
    media = tool_registry.MIME.get(kind, "application/pdf")
    job = store.get(os.path.splitext(os.path.basename(key))[0])
    stem = ((job.opts or {}).get("name") if job else None)
    if job is not None and job.kind in ("convert", "compress", "pages",
                                        "media", "pdfa", "edit"):
        op = (job.opts or {}).get("op")
        zipped = ((job.kind == "pages" and op == "split")
                  or (job.kind == "media" and op == "pdf2img"))
        if zipped:
            name = (stem or "output") + ".zip"
        else:
            default = {"convert": "converted", "compress": "compressed",
                       "pages": "pages", "media": "images",
                       "pdfa": "archive", "edit": "edited"}[job.kind]
            name = (stem or default) + ext
    elif key.startswith("out/"):
        name = "merged.pdf"
    else:
        name = "input.pdf"
    return FileResponse(path, media_type=media, filename=name,
                        headers={"Cache-Control": "no-store"})


@app.get("/")
async def index():
    page = os.path.join(os.path.dirname(__file__), "static", "index.html")
    return FileResponse(page, media_type="text/html")


@app.get("/compress")
async def compress_page():
    page = os.path.join(os.path.dirname(__file__), "static", "compress.html")
    return FileResponse(page, media_type="text/html")


@app.get("/word")
async def word_page():
    # Kept so links issued before the tool grid existed keep working.
    return RedirectResponse("/convert/pdf-to-word", status_code=307)


@app.get("/tools")
async def tools_page():
    page = os.path.join(os.path.dirname(__file__), "static", "tools.html")
    return FileResponse(page, media_type="text/html")


@app.get("/convert/{tool_id}")
async def convert_page(tool_id: str):
    """One template, eight tools.

    The page is rendered per request with its tool baked in, rather than
    fetching the config afterwards: a tool page that briefly shows the wrong
    title and file types is worse than one that arrives correct.
    """
    tool = tool_registry.get(tool_id)
    if tool is None or tool_id in ("merge", "compress"):
        raise HTTPException(404, detail={"code": "no_such_tool",
                                         "message": "没有这个工具"})
    page = os.path.join(os.path.dirname(__file__), "static", "convert.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    accept = tool.get("accept") or [".pdf"]
    payload = {
        "id": tool["id"], "title": tool["title"], "sub": tool["sub"],
        "src": tool["src"], "dst": tool["dst"],
        "action": tool.get("action"), "result": tool.get("result"),
        "accept": accept,
        "mime": tool.get("mime") or "application/pdf",
        "warn": tool.get("warn"),
        "ocr": tool["src"] == "pdf",
    }
    subs = {
        "__TOOL_JSON__": json.dumps(payload, ensure_ascii=False),
        "__TITLE__": tool["title"],
        "__SUB__": tool["sub"],
        "__ACTION__": tool.get("action") or "开始转换",
        "__RESULT__": tool.get("result") or "下载文件",
        "__ACCEPT__": ",".join(accept),
        "__SRC_UP__": tool["src"].upper(),
    }
    for k, v in subs.items():
        html = html.replace(k, v)
    return HTMLResponse(html)


@app.get("/pages/{tool_id}")
async def pages_page(tool_id: str):
    """One template, four page tools (split / extract / delete / rotate).

    Like the convert page, the tool is baked in per request so the page never
    flashes the wrong controls.  The op decides which inputs the template
    reveals; the server still re-validates everything in create_job.
    """
    tool = tool_registry.get(tool_id)
    if tool is None or tool.get("op") not in ("split", "extract",
                                              "delete", "rotate"):
        raise HTTPException(404, detail={"code": "no_such_tool",
                                         "message": "没有这个工具"})
    op = tool["op"]
    labels = {
        "extract": "要保留的页码（必填）",
        "delete": "要删除的页码（必填）",
        "split": "拆分区间（可选，如 1-3,5-7）",
        "rotate": "要旋转的页码（留空 = 全部）",
    }
    payload = {
        "id": tool["id"], "title": tool["title"], "sub": tool["sub"],
        "src": tool["src"], "dst": tool["dst"],
        "accept": tool.get("accept") or [".pdf"],
        "mime": tool.get("mime") or "application/pdf",
        "op": op,
        "action": tool.get("action") or "开始处理",
        "result": tool.get("result") or "下载文件",
        # Every page op shows a page-spec field; split and rotate treat it as
        # optional, extract/delete require it.
        "showRange": True,
        "showEvery": op == "split",
        "showAngle": op == "rotate",
        "rangeRequired": op in ("extract", "delete"),
        "rangeLabel": labels.get(op, "页码"),
        "rangePlaceholder": "1,3,5-7",
        "every": 1,
    }
    page = os.path.join(os.path.dirname(__file__), "static", "pages.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    accept = payload["accept"]
    subs = {
        "__TOOL_JSON__": json.dumps(payload, ensure_ascii=False),
        "__TITLE__": tool["title"],
        "__SUB__": tool["sub"],
        "__ACTION__": payload["action"],
        "__RESULT__": payload["result"],
        "__ACCEPT__": ",".join(accept),
        "__SRC_UP__": tool["src"].upper(),
    }
    for k, v in subs.items():
        html = html.replace(k, v)
    return HTMLResponse(html)


@app.get("/media/{tool_id}")
async def media_page(tool_id: str):
    """PDF -> image and image -> PDF.

    Two templates because the interactions genuinely differ: one PDF in vs many
    images in.  The tool is baked in per request, like the other tool pages.
    """
    tool = tool_registry.get(tool_id)
    if tool is None or tool.get("op") not in ("pdf2img", "img2pdf"):
        raise HTTPException(404, detail={"code": "no_such_tool",
                                         "message": "没有这个工具"})
    op = tool["op"]
    tpl = "pdf2image.html" if op == "pdf2img" else "image2pdf.html"
    payload = {
        "id": tool["id"], "title": tool["title"], "sub": tool["sub"],
        "op": op, "dst": tool["dst"],
        "accept": tool.get("accept") or [".pdf"],
        "mime": tool.get("mime") or "application/pdf",
        "action": tool.get("action") or "开始处理",
        "result": tool.get("result") or "下载文件",
    }
    page = os.path.join(os.path.dirname(__file__), "static", tpl)
    with open(page, encoding="utf-8") as f:
        html = f.read()
    subs = {
        "__TOOL_JSON__": json.dumps(payload, ensure_ascii=False),
        "__TITLE__": tool["title"],
        "__SUB__": tool["sub"],
        "__ACTION__": payload["action"],
        "__RESULT__": payload["result"],
        "__ACCEPT__": ",".join(payload["accept"]),
    }
    for k, v in subs.items():
        html = html.replace(k, v)
    return HTMLResponse(html)


@app.get("/pdfa")
async def pdfa_page():
    """PDF -> PDF/A archival conversion.

    A bespoke page rather than the generic convert template: the output is a
    PDF again, and the interesting part of the result is the conformance report
    (which part, whether every font got embedded), not a page count.
    """
    tool = tool_registry.get("pdfa")
    if tool is None:
        raise HTTPException(404, detail={"code": "no_such_tool",
                                         "message": "没有这个工具"})
    payload = {
        "id": tool["id"], "title": tool["title"], "sub": tool["sub"],
        "accept": tool.get("accept") or [".pdf"],
        "action": tool.get("action") or "转换为 PDF/A",
        "result": tool.get("result") or "下载 PDF/A",
    }
    page = os.path.join(os.path.dirname(__file__), "static", "pdfa.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    subs = {
        "__TOOL_JSON__": json.dumps(payload, ensure_ascii=False),
        "__TITLE__": tool["title"],
        "__SUB__": tool["sub"],
        "__ACTION__": payload["action"],
        "__RESULT__": payload["result"],
        "__ACCEPT__": ",".join(payload["accept"]),
    }
    for k, v in subs.items():
        html = html.replace(k, v)
    return HTMLResponse(html)


@app.get("/edit/{tool_id}")
async def edit_page(tool_id: str):
    """One template, six structured-edit operations.

    The op decides which controls the page reveals; the server re-validates
    everything in create_job.  Positions, angles and annotation kinds are
    injected from the same constants the worker uses, so the browser can never
    offer a value the backend would refuse.
    """
    tool = tool_registry.get(tool_id)
    if tool is None or tool_id not in tool_registry.EDIT_TOOLS:
        raise HTTPException(404, detail={"code": "no_such_tool",
                                         "message": "没有这个工具"})
    payload = {
        "id": tool["id"], "title": tool["title"], "sub": tool["sub"],
        "op": tool["op"], "src": tool["src"], "dst": tool["dst"],
        "accept": tool.get("accept") or [".pdf"],
        "mime": tool.get("mime") or "application/pdf",
        "action": tool.get("action") or "开始编辑",
        "result": tool.get("result") or "下载 PDF",
        "pro": bool(tool.get("pro")),
        "positions": list(POSITIONS),
        "angles": list(ANGLES),
        "annotKinds": list(ANNOT_KINDS),
    }
    page = os.path.join(os.path.dirname(__file__), "static", "edit.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    subs = {
        "__TOOL_JSON__": json.dumps(payload, ensure_ascii=False),
        "__TITLE__": tool["title"],
        "__SUB__": tool["sub"],
        "__ACTION__": payload["action"],
        "__RESULT__": payload["result"],
        "__ACCEPT__": ",".join(payload["accept"]),
    }
    for k, v in subs.items():
        html = html.replace(k, v)
    return HTMLResponse(html)


# One stylesheet for every tool page: the palette and the drop zone were being
# copied per page, which is how two tools end up looking like two products.
app.mount("/static", StaticFiles(directory=os.path.join(
    os.path.dirname(__file__), "static")), name="static")
