"""Start the service with deployment defaults.

One entry point for every platform, because the flags that make this safe are
not the ones people remember: exactly one uvicorn worker (job state is in
process memory), bound to 0.0.0.0 so a phone on the same network can reach it,
and a data directory that survives a restart.

    python service/run.py              # LAN accessible, opens nothing
    python service/run.py --open       # ... and opens the browser
    python service/run.py --port 9000 --host 127.0.0.1
"""
import argparse
import os
import socket
import sys
import webbrowser

# `python service/run.py` puts service/ itself on sys.path, so `import
# service.main` would fail; the repository root is what has to be importable.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import uvicorn

from service import tools


def lan_ip():
    """Best-effort address other devices can use. No packet is actually sent."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return None
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="run the pdfkit web service")
    ap.add_argument("--host", default=os.environ.get("PDFM_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("PDFM_PORT", "8090")))
    ap.add_argument("--open", action="store_true",
                    help="open the browser once the server is up")
    args = ap.parse_args()

    # The job store and the daily quota live in this process's memory.  A second
    # uvicorn worker would answer GET /api/v1/jobs/{id} with 404 for jobs created
    # by the first, so the count is fixed at 1 on purpose -- parallelism comes
    # from PDFM_WORKERS (child processes that only do the PDF work).
    workers = int(os.environ.get("PDFM_WORKERS", "0")) or max(
        2, (os.cpu_count() or 4) - 1)

    print("pdfkit")
    print("  local : http://127.0.0.1:%d/" % args.port)
    if args.host == "0.0.0.0":
        ip = lan_ip()
        if ip:
            print("  LAN   : http://%s:%d/  (same Wi-Fi / office network)" % (ip, args.port))
        print("  note  : binding 0.0.0.0 exposes this to your network;"
              " there is no login by design")
    print("  merge : http://127.0.0.1:%d/" % args.port)
    print("  compress: http://127.0.0.1:%d/compress" % args.port)
    print("  convert : http://127.0.0.1:%d/tools  (%d tools / %d directions)" % (
        args.port, len(tools.ORDER), len(tools.CONVERT_TOOLS)))
    print("  workers: %d PDF processes, 1 web process" % workers)
    print("  data   : %s" % os.environ.get(
        "PDFM_DATA", os.path.join(os.path.dirname(__file__), "data")))
    sys.stdout.flush()

    if args.open:
        import threading
        threading.Timer(
            1.5, lambda: webbrowser.open("http://127.0.0.1:%d/" % args.port)
        ).start()

    uvicorn.run("service.main:app", host=args.host, port=args.port,
                workers=1, log_level="info")


if __name__ == "__main__":
    main()
