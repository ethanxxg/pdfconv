#!/usr/bin/env sh
# Start the service in the foreground. For a background daemon use the systemd
# unit in service/deploy/pdfkit.service.
set -eu
cd "$(dirname "$0")/.."
export PDFM_DATA="${PDFM_DATA:-$(pwd)/service/data}"
exec "${PYTHON:-python3}" service/run.py "$@"
