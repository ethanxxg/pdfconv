"""Minimal HTTP service exposing the converter (stdlib only, no deps).

Endpoints
---------
    GET  /health          -> {"status": "ok"}
    POST /convert         -> body = raw PDF bytes, response = DOCX bytes

Run
---
    python -m pdfconv.serve --host 0.0.0.0 --port 8000
"""
import argparse
import json
import os
import shutil
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .convert import convert

DOCX_MIME = ("application/vnd.openxmlformats-officedocument"
             ".wordprocessingml.document")


class Handler(BaseHTTPRequestHandler):
    server_version = "pdfconv/1.0"

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"status": "ok"})
        else:
            self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/convert":
            self._json(404, {"error": "not found"})
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        data = self.rfile.read(n) if n > 0 else b""
        if not data:
            self._json(400, {"error": "empty body; POST raw PDF bytes"})
            return
        tmpd = tempfile.mkdtemp(prefix="pdfconv_http_")
        inp = os.path.join(tmpd, "in.pdf")
        outp = os.path.join(tmpd, "out.docx")
        try:
            with open(inp, "wb") as f:
                f.write(data)
            convert(inp, outp)
            with open(outp, "rb") as f:
                out = f.read()
        except Exception as e:
            self._json(500, {"error": repr(e)})
            return
        finally:
            shutil.rmtree(tmpd, ignore_errors=True)
        self.send_response(200)
        self.send_header("Content-Type", DOCX_MIME)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, fmt, *args):
        pass  # quiet


def main(argv=None):
    p = argparse.ArgumentParser(prog="pdfconv.serve",
                                description="HTTP service for pdfconv")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8000)
    a = p.parse_args(argv)
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"pdfconv service listening on http://{a.host}:{a.port}")
    print("  GET  /health   |   POST /convert (raw PDF body -> DOCX)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
