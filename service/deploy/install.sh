#!/usr/bin/env bash
# One-shot installer: puts pdfkit on a systemd host and starts it.
#
#   sudo ./service/deploy/install.sh                    # LAN accessible, :8090
#   sudo ./service/deploy/install.sh --port 9000
#   sudo ./service/deploy/install.sh --host 127.0.0.1   # nginx in front
#
# It is idempotent: re-running upgrades the code in place and restarts the
# service, leaving /var/lib/pdfkit (uploads, signing key) untouched.
#
# What it actually does -- there is no magic, and you can do each step by hand:
#   1. create the system user `pdfkit` (no shell, no home)
#   2. copy service/ to /opt/pdfkit
#   3. build a venv there and pip install service/requirements.txt
#   4. install the systemd unit, enable it, start it
#   5. wait for /healthz and print the URL
#
# The unit binds 127.0.0.1 by default (safe behind nginx); this script rewrites
# that to --host, which defaults to 0.0.0.0 because a first-time user wants to
# open the page from another machine.
set -euo pipefail

HOST=0.0.0.0
PORT=8090
APP_DIR=/opt/pdfkit
DATA_DIR=/var/lib/pdfkit
SVC_USER=pdfkit
UNIT=/etc/systemd/system/pdfkit.service

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs a value}"; shift 2 ;;
        --port) PORT="${2:?--port needs a value}"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
[ -f "$REPO_DIR/service/main.py" ] || die "cannot find service/main.py near $SCRIPT_DIR"

# ---------------------------------------------------------------- preconditions
say "0/5  checking the host"
[ "$(id -u)" -eq 0 ] || die "run this with sudo"
[ -d /run/systemd/system ] || die "systemd not running; use Docker or 'python service/run.py' instead"
command -v python3 >/dev/null || die "python3 not found: apt install python3 python3-venv"

# python3-venv is a separate package on Debian/Ubuntu and the usual thing to be
# missing.  Probe it rather than guessing from the distro name.
if ! python3 -c 'import venv' 2>/dev/null; then
    say "     installing python3-venv"
    if command -v apt-get >/dev/null; then
        apt-get update -qq && apt-get install -y -qq python3-venv
    elif command -v dnf >/dev/null; then
        dnf install -y -q python3-libs
    else
        die "install the venv module for python3, then re-run"
    fi
fi
echo "     python3 $(python3 -V 2>&1 | cut -d' ' -f2)  ->  ${HOST}:${PORT}"

# ------------------------------------------------------------------- user, dirs
say "1/5  user and directories"
if id "$SVC_USER" >/dev/null 2>&1; then
    echo "     user $SVC_USER already exists"
else
    useradd --system --home-dir "$APP_DIR" --shell /usr/sbin/nologin "$SVC_USER"
    echo "     created user $SVC_USER"
fi
mkdir -p "$APP_DIR" "$DATA_DIR"

# ------------------------------------------------------------------------ code
say "2/5  installing code into $APP_DIR"
# tar rather than rsync: rsync is not installed by default on minimal images.
# service/data is the *development* data directory -- never ship it.
# pdfconv/ is the PDF->Word engine and lives at the repo root; the convert task
# imports it, so shipping only service/ would break that one tool.
tar -C "$REPO_DIR" -cf - \
    --exclude='service/data' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    service pdfconv | tar -C "$APP_DIR" -xf -
echo "     $(find "$APP_DIR/service" -name '*.py' | wc -l) python files copied"

# ------------------------------------------------------------------------ venv
say "3/5  python environment"
if [ ! -x "$APP_DIR/.venv/bin/python" ]; then
    python3 -m venv "$APP_DIR/.venv"
    echo "     venv created"
fi
"$APP_DIR/.venv/bin/pip" install --quiet --upgrade pip
"$APP_DIR/.venv/bin/pip" install --quiet -r "$APP_DIR/service/requirements.txt"
echo "     dependencies installed"

chown -R root:root "$APP_DIR"
chown -R "$SVC_USER:$SVC_USER" "$DATA_DIR"
chmod 750 "$DATA_DIR"

# ------------------------------------------------------------------------ unit
say "4/5  systemd unit"
install -m 644 "$SCRIPT_DIR/pdfkit.service" "$UNIT"
# The unit ships 127.0.0.1; rewrite the two values we were asked for.
sed -i "s|^Environment=PDFM_HOST=.*|Environment=PDFM_HOST=${HOST}|" "$UNIT"
sed -i "s|^Environment=PDFM_PORT=.*|Environment=PDFM_PORT=${PORT}|" "$UNIT"
systemctl daemon-reload
systemctl enable pdfkit >/dev/null 2>&1
systemctl restart pdfkit
echo "     unit installed and started"

# ---------------------------------------------------------------------- verify
say "5/5  waiting for the service"
ok=0
for _ in $(seq 1 60); do
    # ProxyHandler({}) is load-bearing: on a host with HTTP_PROXY set (common in
    # corporate networks) urllib would send this loopback request to the proxy
    # and the probe would report "not healthy" for a perfectly good service.
    if "$APP_DIR/.venv/bin/python" - "$PORT" <<'PY' 2>/dev/null
import sys, urllib.request
port = sys.argv[1]
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
r = opener.open("http://127.0.0.1:%s/healthz" % port, timeout=2)
sys.exit(0 if r.status == 200 else 1)
PY
    then ok=1; break; fi
    sleep 1
done

if [ "$ok" -ne 1 ]; then
    echo
    systemctl status pdfkit --no-pager -l | tail -20 || true
    die "service did not become healthy; see 'journalctl -u pdfkit -n 50'"
fi

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
say "done"
echo "  merge    : http://${IP:-<server-ip>}:${PORT}/"
echo "  compress : http://${IP:-<server-ip>}:${PORT}/compress"
echo "  logs     : journalctl -u pdfkit -f"
echo "  restart  : systemctl restart pdfkit"
echo "  data     : ${DATA_DIR}"
if [ "$HOST" = "0.0.0.0" ]; then
    echo
    echo "  NOTE: bound to 0.0.0.0 -- anyone who can reach this port can upload"
    echo "        files and spend the daily quota. There is no login by design."
    echo "        For the public internet re-run with --host 127.0.0.1 and put"
    echo "        service/deploy/nginx.conf in front of it."
fi
