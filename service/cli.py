"""pdfkit CLI -- a thin client of the running service's capability catalog.

    python -m service.cli list
    python -m service.cli info scan
    python -m service.cli schema scan
    python -m service.cli run scan --input a.jpg --input b.jpg \
            --opt mode=gray -o scan.pdf

The CLI deliberately does no document work itself.  It discovers capabilities
from ``GET /api/v1/capabilities``, coerces ``--opt`` values using the published
JSON Schema, then drives the same upload/job/download API the browser uses.  A
capability invoked here therefore obeys exactly the same quota and free/Pro
rules as one invoked from a page -- there is no second implementation to drift.

Config: PDFKIT_URL (default http://127.0.0.1:8090).  Uses only the stdlib so it
can be dropped onto any machine that can reach the service.
"""
import argparse
import hashlib
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

URL = os.environ.get("PDFKIT_URL", "http://127.0.0.1:8090").rstrip("/")


def device_id():
    try:
        who = os.getlogin()
    except Exception:
        who = os.environ.get("USER") or os.environ.get("USERNAME") or "cli"
    raw = "%s|%s" % (socket.gethostname(), who)
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


DEV = device_id()


def api(method, path, body=None, raw=None, ctype=None, expect=200, timeout=120):
    url = URL + path
    data = None
    headers = {"X-Device-Id": DEV}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif raw is not None:
        data = raw
        if ctype:
            headers["Content-Type"] = ctype
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload, code = r.read(), r.status
    except urllib.error.HTTPError as e:
        # A non-2xx is a normal outcome here; the status is interpreted below.
        payload, code = e.read(), e.code
    except urllib.error.URLError as e:
        raise SystemExit("无法连接 %s：%s（先启动服务，或设置 PDFKIT_URL）"
                         % (URL, e.reason))
    if expect is not None and code != expect:
        detail = {}
        try:
            obj = json.loads(payload)
            detail = obj.get("detail", obj)
        except Exception:
            detail = {}
        if isinstance(detail, dict):
            msg = detail.get("message") or detail.get("code") or str(detail)
        else:
            msg = str(detail)
        raise SystemExit("请求失败 (%d)：%s" % (code, msg))
    try:
        obj = json.loads(payload)
    except Exception:
        return payload
    if isinstance(obj, dict) and set(obj) == {"detail"} and isinstance(obj["detail"], dict):
        return obj["detail"]
    return obj


def catalog():
    return api("GET", "/api/v1/capabilities")["capabilities"]


def find(name, caps):
    for c in caps:
        if c["id"] == name:
            return c
    raise SystemExit("未知能力 %r；用 `pdfkit list` 查看可用能力" % name)


def coerce(value, schema):
    t = schema.get("type")
    if t == "boolean":
        return str(value).lower() in ("1", "true", "yes", "on")
    if t == "integer":
        try:
            return int(value)
        except ValueError:
            raise SystemExit("需要整数：%s" % value)
    if t == "number":
        try:
            return float(value)
        except ValueError:
            raise SystemExit("需要数字：%s" % value)
    if t == "object":
        try:
            return json.loads(value)
        except ValueError:
            raise SystemExit("需要 JSON 对象：%s" % value)
    if t == "array":
        try:
            return json.loads(value)
        except ValueError:
            raise SystemExit("需要 JSON 数组：%s" % value)
    return value


def validate(opts, schema):
    props = schema.get("properties", {})
    unknown = [k for k in opts if k not in props]
    if unknown:
        raise SystemExit("未知选项：%s（可用：%s）"
                         % (", ".join(unknown), ", ".join(sorted(props))))
    for k in schema.get("required", []):
        if k not in opts:
            raise SystemExit("缺少必填选项：%s" % k)


def cmd_list(args):
    caps = catalog()
    if args.json:
        print(json.dumps(caps, ensure_ascii=False, indent=2))
        return
    print("%-16s %-9s %-6s %s" % ("ID", "TASK", "PRO", "说明"))
    for c in caps:
        print("%-16s %-9s %-6s %s"
              % (c["id"], c["task"] + ("/" + c["op"] if c["op"] else ""),
                 "PRO" if c["pro"] else "-", c["description"][:44]))


def cmd_info(args):
    caps = catalog()
    c = find(args.name, caps)
    print(json.dumps(c, ensure_ascii=False, indent=2))


def cmd_schema(args):
    caps = catalog()
    c = find(args.name, caps)
    print(json.dumps(c["input_schema"], ensure_ascii=False, indent=2))


def guess_mime(path):
    ext = os.path.splitext(path)[1].lower()
    return {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
            ".pdf": "application/pdf", ".docx": ("application/vnd.openxmlformats"
             "-officedocument.wordprocessingml.document"),
            ".xlsx": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            ".pptx": "application/vnd.openxmlformats-officedocument.presentationml.presentation"}.get(
                ext, "application/octet-stream")


def cmd_run(args):
    caps = catalog()
    c = find(args.capability, caps)

    inputs = args.input
    if len(inputs) < c["min_files"]:
        raise SystemExit("需要至少 %d 个输入文件，实际 %d"
                         % (c["min_files"], len(inputs)))
    if len(inputs) > c["max_files"]:
        raise SystemExit("最多 %d 个输入文件，实际 %d"
                         % (c["max_files"], len(inputs)))
    for p in inputs:
        if not os.path.isfile(p):
            raise SystemExit("文件不存在：%s" % p)

    schema = c["input_schema"]
    opts = {}
    for item in args.opt or []:
        if "=" not in item:
            raise SystemExit("--opt 需要 key=value 形式：%s" % item)
        k, v = item.split("=", 1)
        opts[k] = coerce(v, schema["properties"].get(k, {"type": "string"}))
    validate(opts, schema)
    if args.name:
        opts["name"] = args.name
    if c["op"]:
        opts["op"] = c["op"]

    print("上传 %d 个文件…" % len(inputs), file=sys.stderr)
    meta = [{"name": os.path.basename(p), "size": os.path.getsize(p)} for p in inputs]
    ups = api("POST", "/api/v1/uploads", body={"files": meta})["uploads"]
    for up, path in zip(ups, inputs):
        with open(path, "rb") as f:
            api("PUT", urllib.parse.urlsplit(up["url"]).path, raw=f.read(),
                ctype=guess_mime(path))

    print("提交任务 %s…" % c["id"], file=sys.stderr)
    job = api("POST", "/api/v1/jobs", body={"task": c["task"],
              "files": [{"file_id": u["file_id"]} for u in ups],
              "options": opts})
    jid = job["job_id"]
    for _ in range(1500):
        time.sleep(0.4)
        st = api("GET", "/api/v1/jobs/" + jid)
        status = st["status"]
        if status == "done":
            break
        if status == "failed":
            e = st.get("error") or {}
            raise SystemExit("任务失败：%s" % (e.get("message") or e.get("code")))
    else:
        raise SystemExit("等待超时")

    out = args.out
    if not out:
        ext = {".txt": ".txt", ".docx": ".docx", ".zip": ".zip"}.get(
            c["dst"], ".pdf")
        out = "output" + ext
    with urllib.request.urlopen(URL + st["download"], timeout=120) as r:
        blob = r.read()
    with open(out, "wb") as f:
        f.write(blob)
    print("已保存 %s（%d 字节）" % (out, len(blob)))


def main(argv=None):
    p = argparse.ArgumentParser(prog="pdfkit", description="在线 PDF 工具命令行客户端")
    sub = p.add_subparsers(dest="cmd")
    pl = sub.add_parser("list", help="列出所有能力")
    pl.add_argument("--json", action="store_true")
    pl.set_defaults(fn=cmd_list)
    pi = sub.add_parser("info", help="查看一个能力")
    pi.add_argument("name"); pi.set_defaults(fn=cmd_info)
    ps = sub.add_parser("schema", help="打印能力的 JSON Schema")
    ps.add_argument("name"); ps.set_defaults(fn=cmd_schema)
    pr = sub.add_parser("run", help="执行一个能力")
    pr.add_argument("capability")
    pr.add_argument("-i", "--input", action="append", default=[], required=True)
    pr.add_argument("--opt", action="append", help="key=value，可重复")
    pr.add_argument("--name", help="下载文件名（不含扩展名）")
    pr.add_argument("-o", "--out", help="输出路径")
    pr.set_defaults(fn=cmd_run)
    args = p.parse_args(argv)
    if not getattr(args, "cmd", None):
        p.print_help()
        return 0
    args.fn(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
