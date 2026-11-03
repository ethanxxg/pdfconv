# Build from the repository root:
#   docker build -f service/Dockerfile -t pdfkit .
#
# Deliberately one uvicorn worker: the job store and the daily quota are
# in-process, so a second worker would 404 on jobs the first one created.
# CPU parallelism comes from PDFM_WORKERS child processes instead.
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PDFM_DATA=/data

WORKDIR /app

COPY service/requirements.txt ./service/requirements.txt
RUN pip install --no-cache-dir -r service/requirements.txt

COPY service ./service
# The PDF->Word engine lives at the repository root, not inside service/. Without
# it the convert task fails at import time, and only that one tool breaks -- the
# kind of partial outage that is easy to miss in a smoke test of the homepage.
COPY pdfconv ./pdfconv

RUN useradd --system --uid 10001 pdfkit \
 && mkdir -p /data \
 && chown -R pdfkit:pdfkit /data /app
USER pdfkit

EXPOSE 8090

# ProxyHandler({}) so an inherited HTTP_PROXY cannot turn this into a false alarm.
HEALTHCHECK --interval=30s --timeout=3s --start-period=15s \
  CMD python -c "import urllib.request,sys;o=urllib.request.build_opener(urllib.request.ProxyHandler({}));sys.exit(0 if o.open('http://127.0.0.1:8090/healthz',timeout=2).status==200 else 1)"

CMD ["python", "-m", "uvicorn", "service.main:app", \
     "--host", "0.0.0.0", "--port", "8090", "--workers", "1"]
