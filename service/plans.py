"""Plans and daily quotas -- the seam where billing will plug in.

Nothing here knows how to take money.  Its only job is that every limit the
product enforces is expressed once, as data, so switching a user to a paid plan
is a change of one string rather than an audit of every route.

``resolve_plan`` is the single place that answers "who is this".  Today it
returns "free" for everyone; when accounts land it verifies a session or a
signed token there and nothing else changes.  Critically it must never trust a
client-supplied plan header -- that is the one bug that makes a paid tier
meaningless.

The counter is in-process, so with ``uvicorn --workers 4`` the real allowance is
4x the advertised number.  That is acceptable for P0 and unacceptable in
production: swap ``Quota`` for a Redis INCR with a midnight expiry, which is a
drop-in because the interface is already (key, limit) -> (ok, used).
"""
import os
import threading
import time

PLANS = {
    "free": {
        "id": "free",
        "name": "免费版",
        "max_files": 20,         # per merge job
        "max_file_mb": 20,
        "max_task_mb": 200,
        "max_pages": 5,          # pages in the finished document
        "jobs_per_day": 1,
        "batch_compress": False,  # one file per compress job
        # OCR ships enabled on the free tier because there are no accounts or
        # billing yet -- leaving it Pro-only would mean nobody can use it.
        # PDFM_FREE_OCR=0 restores it as the paid hook when billing lands.
        "ocr": True,
        "levels": ["recommended", "less", "extreme"],
    },
    "pro": {
        "id": "pro",
        "name": "Pro",
        "max_files": 50,
        "max_file_mb": 2048,
        "max_task_mb": 4096,
        "max_pages": 5000,
        "jobs_per_day": 0,       # 0 means unlimited
        "batch_compress": True,
        "ocr": True,
        "levels": ["recommended", "less", "extreme"],
    },
}
DEFAULT_PLAN = "free"

# Deployments tune the free tier without editing code.  Applied at import so a
# route never has to remember which of two sources is authoritative.
for _env, _key, _cast in (
        ("PDFM_MAX_FILES", "max_files", int),
        ("PDFM_MAX_FILE_MB", "max_file_mb", int),
        ("PDFM_MAX_TASK_MB", "max_task_mb", int),
        ("PDFM_MAX_PAGES", "max_pages", int),
        ("PDFM_FREE_JOBS_PER_DAY", "jobs_per_day", int)):
    _v = os.environ.get(_env)
    if _v:
        try:
            PLANS["free"][_key] = _cast(_v)
        except ValueError:
            pass

# OCR is a boolean, so it needs its own parse (the loop above only casts ints).
_v = os.environ.get("PDFM_FREE_OCR")
if _v is not None:
    PLANS["free"]["ocr"] = _v.strip().lower() in ("1", "true", "yes", "on")


def resolve_plan(request):
    """Which plan this caller is on.

    Everything is free for now.  When accounts exist, this is the one function
    to change: look up the session/token here and return a key of PLANS.

    Deliberately does *not* read any client header such as X-Plan: a header is
    attacker-controlled, and honouring it would let anyone grant themselves Pro.
    """
    return DEFAULT_PLAN


def plan_of(name):
    return PLANS.get(name) or PLANS[DEFAULT_PLAN]


def describe(name, used=0):
    p = plan_of(name)
    left = None if not p["jobs_per_day"] else max(0, p["jobs_per_day"] - used)
    return {
        "id": p["id"],
        "name": p["name"],
        "max_files": p["max_files"],
        "max_file_mb": p["max_file_mb"],
        "max_task_mb": p["max_task_mb"],
        "max_pages": p["max_pages"],
        "jobs_per_day": p["jobs_per_day"],
        "jobs_left": left,
        "batch_compress": p["batch_compress"],
        "ocr": p["ocr"],
        "levels": p["levels"],
    }


class Quota:
    """Daily job counter per identity.

    Keyed by device rather than IP: a shared office NAT would otherwise burn one
    person's allowance for the whole building.  The IP bucket in limits.py still
    runs alongside this and catches the case where the device id is forged.
    """

    def __init__(self):
        self._used = {}
        self._lock = threading.Lock()

    @staticmethod
    def _day():
        return time.strftime("%Y%m%d")

    def used(self, who):
        with self._lock:
            return self._used.get((who, self._day()), 0)

    def check(self, who, plan_name):
        """Return (ok, used, left).  Does not consume."""
        limit = plan_of(plan_name)["jobs_per_day"]
        used = self.used(who)
        if not limit:
            return True, used, None
        return used < limit, used, max(0, limit - used)

    def take(self, who):
        """Consume one job.  Call only after check() returned ok."""
        with self._lock:
            k = (who, self._day())
            self._used[k] = self._used.get(k, 0) + 1
            return self._used[k]

    def sweep(self):
        today = self._day()
        with self._lock:
            for k in [k for k in self._used if k[1] != today]:
                del self._used[k]

    def __len__(self):
        return len(self._used)
