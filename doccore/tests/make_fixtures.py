"""Generates test fixtures for the DocCore unit tests (wave P1/P2)."""
import os
import shutil
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
os.makedirs(DATA, exist_ok=True)


def make_deflate():
    raw = bytes((i * 7 + (i >> 3)) & 0xFF for i in range(120000))
    with open(os.path.join(DATA, "deflate.raw"), "wb") as f:
        f.write(raw)
    with open(os.path.join(DATA, "deflate.z"), "wb") as f:
        f.write(zlib.compress(raw, 9))


def make_sample_zip():
    path = os.path.join(DATA, "sample.zip")
    nested = bytes(i & 0xFF for i in range(70000))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("hello.txt", "hello doccore\n")
        z.writestr("dir/nested.bin", nested)
        z.writestr("empty.txt", "")
    return path


def make_evil_zip():
    with zipfile.ZipFile(os.path.join(DATA, "evil.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("../evil.txt", "should never be extracted")


def make_bomb_zip():
    with zipfile.ZipFile(os.path.join(DATA, "bomb.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("bomb.bin", b"\x00" * (10 * 1024 * 1024))


def copy_font():
    candidates = [
        r"C:\Windows\Fonts\simhei.ttf",
        r"C:\Windows\Fonts\msyh.ttf",
        r"C:\Windows\Fonts\simsun.ttf",
        r"C:\Windows\Fonts\arial.ttf",
    ]
    for c in candidates:
        if os.path.exists(c):
            shutil.copyfile(c, os.path.join(DATA, "test.ttf"))
            return c
    raise SystemExit("no TTF font found")


if __name__ == "__main__":
    make_deflate()
    make_sample_zip()
    make_evil_zip()
    make_bomb_zip()
    src = copy_font()
    print("fixtures written to", DATA)
    print("font:", src, os.path.getsize(os.path.join(DATA, "test.ttf")), "bytes")
