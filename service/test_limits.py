"""Prove the free-tier limits with the *production* defaults.

The functional suites (test_e2e, test_compress) run against a server whose
limits are raised by environment variables, because they need 30-page papers to
exercise rotation and compression.  That means nothing in those suites would
notice if the real allowance silently became 500 pages -- so this file runs its
own server with the shipped defaults and attacks each limit directly.

Order matters: every rejection is tested before the one job the free tier
allows, so a bug that consumes the quota too early shows up as a cascade rather
than being masked.
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pymupdf as fitz

PORT = 8082
BASE = "http://127.0.0.1:%d" % PORT
DATA = "service/data_limits"
DEV = "limits-test-device"

FREE_PAGES = 5
FREE_MB = 20
FREE_JOBS = 1

shutil.rmtree(DATA, ignore_errors=True)
env = dict(os.environ, PDFM_RATE_MULT="1000", PDFM_WORKERS="1", PDFM_DATA=DATA)
for k in ("PDFM_MAX_PAGES", "PDFM_MAX_FILE_MB", "PDFM_FREE_JOBS_PER_DAY"):
    env.pop(k, None)          # make sure the shipped defaults are what runs

srv = subprocess.Popen(
    [sys.executable, "-m", "uvicorn", "service.main:app",
     "--host", "127.0.0.1", "--port", str(PORT), "--log-level", "warning"],
    env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def req(method, path, body=None, raw=None, expect=200):
    h = {"X-Device-Id": DEV}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    elif raw is not None:
        data = raw
        h["Content-Type"] = "application/pdf"
    r = urllib.request.Request(BASE + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r) as resp:
            payload, code = resp.read(), resp.status
    except urllib.error.HTTPError as e:
        payload, code = e.read(), e.code
    if code != expect:
        raise AssertionError("%s %s -> %d (want %d): %s"
                             % (method, path, code, expect, payload[:240]))
    try:
        return json.loads(payload)
    except Exception:
        return payload


def make_pdf(path, pages, text="page"):
    doc = fitz.open()
    for i in range(pages):
        page = doc.new_page()
        page.insert_text((72, 96), "%s %d" % (text, i + 1))
    doc.save(path, garbage=4, deflate=True)
    doc.close()
    return path


def err(body):
    return (body or {}).get("detail") or {}


try:
    for _ in range(80):
        try:
            req("GET", "/healthz")
            break
        except Exception:
            time.sleep(0.25)
    else:
        raise SystemExit("server did not start")

    os.makedirs("work/_bench", exist_ok=True)
    p2a = make_pdf("work/_bench/lim_2a.pdf", 2)
    p2b = make_pdf("work/_bench/lim_2b.pdf", 2)
    p6 = make_pdf("work/_bench/lim_6.pdf", 6)
    p1 = make_pdf("work/_bench/lim_1.pdf", 1)

    print("1. config reports the shipped free tier")
    cfg = req("GET", "/api/v1/config")
    p = cfg["plan"]
    print("   pages=%s file_mb=%s jobs/day=%s batch=%s"
          % (p["max_pages"], p["max_file_mb"], p["jobs_per_day"],
             p["batch_compress"]))
    assert p["max_pages"] == FREE_PAGES, p
    assert p["max_file_mb"] == FREE_MB, p
    assert p["jobs_per_day"] == FREE_JOBS, p
    assert p["batch_compress"] is False, p

    print("2. a file bigger than %d MB is refused at target creation" % FREE_MB)
    e = err(req("POST", "/api/v1/uploads",
                body={"files": [{"name": "big.pdf",
                                 "size": (FREE_MB + 1) * 1048576}]}, expect=400))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "file_too_large"

    print("2b. exactly %d MB is still allowed (the check must be > not >=)" % FREE_MB)
    ok = req("POST", "/api/v1/uploads",
             body={"files": [{"name": "edge.pdf", "size": FREE_MB * 1048576}]})
    print("   target created, max_file_mb=%s" % ok["max_file_mb"])
    assert ok["max_file_mb"] == FREE_MB

    print("3. and refused again mid-stream, not just on the declared size")
    up = req("POST", "/api/v1/uploads",
             body={"files": [{"name": "big.pdf", "size": 1024}]})["uploads"][0]
    blob = open(p1, "rb").read() + b"0" * ((FREE_MB + 1) * 1048576)
    e = err(req("PUT", up["url"], raw=blob, expect=413))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "file_too_large"

    def upload(path):
        size = os.path.getsize(path)
        t = req("POST", "/api/v1/uploads",
                body={"files": [{"name": os.path.basename(path), "size": size}]})["uploads"][0]
        with open(path, "rb") as f:
            req("PUT", t["url"], raw=f.read())
        return t["file_id"]

    id2a, id2b, id6 = upload(p2a), upload(p2b), upload(p6)

    print("4. a 6-page file is refused, and says how many pages it saw")
    e = err(req("POST", "/api/v1/jobs",
                body={"task": "compress", "files": [{"file_id": id6}]}, expect=400))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "too_many_pages"

    print("5. a merge that ends up over %d pages is refused" % FREE_PAGES)
    # 2 + 2 = 4 is fine; 2 + 6 = 8 is not
    e = err(req("POST", "/api/v1/jobs",
                body={"task": "merge",
                      "files": [{"file_id": id2a}, {"file_id": id6}]}, expect=400))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "too_many_pages"

    print("6. batch compress is refused on the free plan")
    e = err(req("POST", "/api/v1/jobs",
                body={"task": "compress",
                      "files": [{"file_id": id2a}, {"file_id": id2b}]}, expect=403))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "plan_limit"

    print("7. none of those refusals consumed the daily allowance")
    left = req("GET", "/api/v1/config")["plan"]["jobs_left"]
    print("   jobs_left=%s" % left)
    assert left == FREE_JOBS, left

    print("8. a job within the limits succeeds")
    j = req("POST", "/api/v1/jobs",
            body={"task": "merge",
                  "files": [{"file_id": id2a}, {"file_id": id2b}]})
    for _ in range(120):
        st = req("GET", "/api/v1/jobs/" + j["job_id"])
        if st["status"] in ("done", "failed"):
            break
        time.sleep(0.25)
    print("   %s, %d pages, jobs_left=%s" % (st["status"], st["pages"], j["jobs_left"]))
    assert st["status"] == "done", st
    assert st["pages"] == 4, st["pages"]
    assert j["jobs_left"] == 0, j

    print("9. the next job of the day is refused")
    e = err(req("POST", "/api/v1/jobs",
                body={"task": "merge",
                      "files": [{"file_id": id2a}, {"file_id": id2b}]}, expect=429))
    print("   ", e.get("code"), "-", e.get("message"))
    assert e.get("code") == "quota_exceeded"
finally:
    srv.terminate()
    try:
        srv.wait(timeout=10)
    except Exception:
        srv.kill()
    shutil.rmtree(DATA, ignore_errors=True)

print("\nALL OK")
