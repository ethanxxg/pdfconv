"""Single-node throughput probe.

Uploads a fixed set once, then fires N merge jobs concurrently and measures
completion rate.  The same file ids are reused across jobs, which is legitimate
here: uploads are only reaped on TTL, not on job completion.

Boots its own server with the allowance raised -- a load test that stops after
one job because of the daily quota would measure nothing.
"""
import json
import os
import sys
import threading
import time
import urllib.request
from collections import Counter

import pymupdf as fitz

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from service._harness import ROOT, boot  # noqa: E402

os.chdir(ROOT)
srv = boot("load", pages=5000, jobs=0, workers=max(2, (os.cpu_count() or 4) - 1))
BASE = srv.base
N = int(sys.argv[1]) if len(sys.argv) > 1 else 40


def req(method, path, body=None, raw=None, headers=None):
    h = {"X-Device-Id": "load-device"}
    if headers:
        h.update(headers)
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
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, {}


files = [p for p in sorted(__import__("glob").glob("docs/*.pdf"))
         if 0.3 <= os.path.getsize(p) / 1048576.0 <= 1.5][:3]
meta = [{"name": os.path.basename(p), "size": os.path.getsize(p)} for p in files]
_, up = req("POST", "/api/v1/uploads", body={"files": meta})
ids = []
for t, p in zip(up["uploads"], files):
    with open(p, "rb") as f:
        req("PUT", t["url"], raw=f.read())
    ids.append(t["file_id"])
total_mb = sum(os.path.getsize(p) for p in files) / 1048576.0
print("%d jobs x %.2f MB (%d files each)" % (N, total_mb, len(ids)))

codes = Counter()
job_ids = []
lock = threading.Lock()


def submit():
    code, body = req("POST", "/api/v1/jobs", body={"file_ids": ids})
    with lock:
        codes[code] += 1
        if code == 200:
            job_ids.append(body["job_id"])


t0 = time.perf_counter()
threads = [threading.Thread(target=submit) for _ in range(N)]
for t in threads:
    t.start()
for t in threads:
    t.join()
t_submit = time.perf_counter() - t0
print("submitted in %.2f s  status codes %s" % (t_submit, dict(codes)))

done = failed = 0
waits, runs = [], []
deadline = time.time() + 180
while job_ids and time.time() < deadline:
    still = []
    for jid in job_ids:
        _, st = req("GET", "/api/v1/jobs/" + jid)
        s = st.get("status")
        if s == "done":
            done += 1
            if st.get("queue_wait") is not None:
                waits.append(st["queue_wait"])
            if st.get("run_time") is not None:
                runs.append(st["run_time"])
        elif s == "failed":
            failed += 1
            print("   failed:", st.get("error"))
        else:
            still.append(jid)
    job_ids = still
    if job_ids:
        time.sleep(0.1)

t_total = time.perf_counter() - t0
print("done=%d failed=%d  wall=%.2f s" % (done, failed, t_total))
if waits:
    waits.sort(); runs.sort()
    print("queue_wait  median %.3f s  max %.3f s" % (waits[len(waits)//2], waits[-1]))
    print("run_time    median %.3f s  max %.3f s" % (runs[len(runs)//2], runs[-1]))
    print("sum(run)/workers = %.2f s of the %.2f s wall"
          % (sum(runs) / 7.0, t_total))
if done:
    print("throughput: %.1f jobs/s  (%.1f MB/s merged)"
          % (done / t_total, done * total_mb / t_total))
_, h = req("GET", "/healthz")
print("healthz:", {k: h.get(k) for k in ("workers", "queue_depth", "jobs", "stats")})
