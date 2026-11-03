"""Object storage abstraction.

P0 writes to a local directory; the production target is OSS/COS.  The point of
the interface is that the API contract never changes when you swap: the browser
always PUTs bytes to a URL the server hands it, and the server never proxies a
single byte of file content.  That property is what keeps the API tier cheap at
50k concurrent tasks -- see README.
"""
import hashlib
import hmac
import os
import secrets
import time

CHUNK = 1 << 20


class StorageError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class LocalStorage:
    """Filesystem backend.  Keys are server-generated ids, never user paths."""

    def __init__(self, root, secret):
        self.root = os.path.abspath(root)
        os.makedirs(self.root, exist_ok=True)
        # hmac wants bytes; callers pass a hex string from the environment
        self._secret = (secret.encode("utf-8") if isinstance(secret, str)
                        else secret)

    def _path(self, key):
        # Hash the key so a crafted id can never escape the root directory.
        safe = hashlib.sha256(key.encode("utf-8")).hexdigest()[:40]
        return os.path.join(self.root, safe + ".bin")

    def put_stream(self, key, stream, limit):
        """Write an incoming stream, aborting the moment it exceeds *limit*."""
        path = self._path(key)
        n = 0
        try:
            with open(path, "wb") as f:
                while True:
                    chunk = stream.read(CHUNK)
                    if not chunk:
                        break
                    n += len(chunk)
                    if n > limit:
                        raise StorageError(
                            "too_large",
                            "文件超过 %d MB" % (limit // 1048576))
                    f.write(chunk)
        except Exception:
            # never leave a half-written object behind: a later job could pick
            # it up and merge a truncated PDF
            try:
                os.remove(path)
            except OSError:
                pass
            raise
        return n

    def put_bytes(self, key, data):
        with open(self._path(key), "wb") as f:
            f.write(data)
        return len(data)

    def path(self, key):
        return self._path(key)

    def exists(self, key):
        return os.path.exists(self._path(key))

    def size(self, key):
        try:
            return os.path.getsize(self._path(key))
        except OSError:
            return -1

    def delete(self, key):
        try:
            os.remove(self._path(key))
        except OSError:
            pass

    # -- signed links -----------------------------------------------------
    def sign(self, key, ttl=900):
        exp = int(time.time()) + ttl
        msg = "%s|%d" % (key, exp)
        sig = hmac.new(self._secret, msg.encode("utf-8"),
                       hashlib.sha256).hexdigest()[:40]
        return "/api/v1/download/%s?exp=%d&sig=%s" % (key, exp, sig)

    def verify(self, key, exp, sig):
        try:
            exp = int(exp)
        except (TypeError, ValueError):
            return False
        if exp < time.time():
            return False
        msg = "%s|%d" % (key, exp)
        want = hmac.new(self._secret, msg.encode("utf-8"),
                        hashlib.sha256).hexdigest()[:40]
        return hmac.compare_digest(want, sig or "")


def new_secret(path=None):
    """Stable per-deployment secret for signing download links.

    It must survive a restart and be shared by every process: with
    ``uvicorn --workers 4`` an in-memory random secret would mean a link signed
    by worker A is rejected by worker B, and every deploy would break every
    link already handed out.  ``PDFM_SECRET`` wins when set (multi-node
    deployments put it in the secret manager); otherwise the first process to
    start writes one next to the data and the rest read it back.
    """
    env = os.environ.get("PDFM_SECRET")
    if env:
        return env
    if not path:
        return secrets.token_hex(32)
    try:
        with open(path, "r", encoding="utf-8") as f:
            s = f.read().strip()
        if len(s) >= 32:
            return s
    except OSError:
        pass
    s = secrets.token_hex(32)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    # write-then-rename so two workers starting at once cannot read a half
    # written file and silently fall back to a different secret
    tmp = path + ".tmp%d" % os.getpid()
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(s)
    os.replace(tmp, path)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return s
