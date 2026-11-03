"""Rate limiting without user accounts.

Two independent token buckets guard every expensive call:

* one keyed by client IP  -- survives a forged fingerprint, but is shared by
  everyone behind one campus or office NAT, so it cannot be the only gate;
* one keyed by device fingerprint -- finer grained, but entirely client
  supplied and therefore trivially reset by anyone who cares.

A request must pass both.  That is deliberately conservative: an abuser has to
rotate IP *and* fingerprint, while an honest user behind a big NAT still gets
the per-device allowance.  The IP bucket is sized so a 500-person office can
work normally (see IP_JOBS in config) rather than treating one NAT as one user.
"""
import time


class _Bucket:
    __slots__ = ("tokens", "ts")

    def __init__(self, tokens, ts):
        self.tokens = tokens
        self.ts = ts


class Limiter:
    def __init__(self):
        self._buckets = {}
        self._last_sweep = time.monotonic()

    def take(self, key, rate, burst, cost=1.0, now=None):
        """Consume *cost* tokens.  Returns (allowed, retry_after_seconds)."""
        now = now if now is not None else time.monotonic()
        b = self._buckets.get(key)
        if b is None:
            b = self._buckets[key] = _Bucket(float(burst), now)
        else:
            b.tokens = min(float(burst), b.tokens + (now - b.ts) * rate)
            b.ts = now
        if b.tokens >= cost:
            b.tokens -= cost
            return True, 0.0
        return False, (cost - b.tokens) / rate

    def sweep(self, idle=3600, now=None):
        """Drop buckets nobody has touched -- the dict is otherwise a leak,
        and an attacker rotating keys turns that leak into a memory DoS."""
        now = now if now is not None else time.monotonic()
        if now - self._last_sweep < 300:
            return
        self._last_sweep = now
        dead = [k for k, b in self._buckets.items() if now - b.ts > idle]
        for k in dead:
            del self._buckets[k]

    def __len__(self):
        return len(self._buckets)
