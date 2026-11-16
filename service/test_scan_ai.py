"""Focused engine tests for the opt-in capture-AI scan stages.

These are deliberately small, synthetic images: a real photo is too variable
to assert that a finger or a moire pattern actually went away.  Each fixture
has a measurable property before the stage and a different one after it, plus
a "normal page" case that must be left byte-for-byte alone.  That second half
is the important one -- an aggressive cleaner that eats text is worse than no
cleaner at all.

No HTTP server is started here; the API wiring is asserted in test_scan.py and
the capability contract in test_cli.py.
"""
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pdfconv import scan as S  # noqa: E402


def clean_page(h=360, w=480):
    """Warm-white sheet with ordinary dark text, no capture artefacts."""
    img = np.full((h, w, 3), (245, 243, 235), dtype=np.uint8)  # BGR
    for i in range(8):
        cv2.rectangle(img, (50, 40 + i * 34), (350 - (i % 3) * 60, 52 + i * 34),
                      (35, 35, 40), -1)
    return img


def strongest_offcenter_peak(img):
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    f = cv2.dft(np.float32(gray), flags=cv2.DFT_COMPLEX_OUTPUT)
    sh = np.fft.fftshift(f)
    mag = cv2.magnitude(sh[:, :, 0], sh[:, :, 1])
    mag[h // 2 - 12:h // 2 + 13, w // 2 - 12:w // 2 + 13] = 0
    return float(mag.max())


def bleed_depth(img):
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    k = max(31, (min(gray.shape) // 10) | 1)
    paper = cv2.GaussianBlur(gray, (0, 0), k / 3.0)
    depth = paper.astype("float32") - gray.astype("float32")
    band = (depth > 8) & (depth < 40)
    return int(band.sum()), float(depth[band].mean()) if band.any() else 0.0


print("1. definger removes an edge-coloured skin blob, not the text")
img = clean_page()
cv2.ellipse(img, (240, 365), (75, 60), 0, 0, 360, (115, 155, 195), -1)
out = S._definger(img)
assert out.shape == img.shape
# The inpaint should restore paper near the finger centre.
assert int(out[350, 240].mean()) > int(img[350, 240].mean()) + 30, \
    (img[350, 240], out[350, 240])
# Text is nowhere near the skin mask and must survive unchanged.
assert np.array_equal(out[40:56, 50:290], img[40:56, 50:290])
print("   finger centre %s -> %s" % (img[350, 240].tolist(), out[350, 240].tolist()))

print("2. demoire suppresses a screen grating and keeps colour")
h, w = 360, 480
yy, xx = np.ogrid[:h, :w]
wave = 22 * np.sin(2 * np.pi * (xx * 0.13 + yy * 0.13))
g = np.clip(235 + wave, 0, 255).astype("uint8")
img = cv2.cvtColor(g, cv2.COLOR_GRAY2BGR)
img[:, :, 1] = np.clip(img[:, :, 1].astype(int) - 8, 0, 255).astype("uint8")
img[:, :, 0] = np.clip(img[:, :, 0].astype(int) - 16, 0, 255).astype("uint8")
out = S._demoire(img)
before, after = strongest_offcenter_peak(img), strongest_offcenter_peak(out)
assert after < before * 0.5, (before, after)
assert abs(float(out.mean()) - float(img.mean())) < 4.0, (img.mean(), out.mean())
# A colour photo must not come back grey.
spread = float(np.ptp(out.reshape(-1, 3).mean(axis=0)))
assert spread > 10.0, spread
print("   peak %.0f -> %.0f, channel spread %.1f" % (before, after, spread))

print("3. debleed lifts shallow backside strokes, keeps dark ink and hue")
img = np.full((h, w, 3), (248, 242, 230), dtype=np.uint8)
for i in range(8):
    cv2.rectangle(img, (50, 50 + i * 38), (350 - (i % 3) * 70, 55 + i * 38),
                  (35, 35, 40), -1)
for i in range(30):
    y = 70 + i * 10
    cv2.rectangle(img, (55, y), (360 - (i % 4) * 40, y + 4), (205, 200, 190), -1)
out = S._debleed(img)
n0, d0 = bleed_depth(img)
n1, d1 = bleed_depth(out)
assert n1 < n0 * 0.8 and d1 < d0, (n0, d0, n1, d1)
assert np.array_equal(out[50:56, 50:280], img[50:56, 50:280])
b0, g0, r0 = img[80, 70].astype(int)
b1, g1, r1 = out[80, 70].astype(int)
# In BGR the warm paper has B > R; that gap must survive the clean-up.
assert (b1 - r1) > 8 and (b0 - r0) > 8, ((b0, g0, r0), (b1, g1, r1))
print("   bleed px %d -> %d, depth %.1f -> %.1f" % (n0, n1, d0, d1))

print("4. the three AI stages are opt-in and leave a normal page alone")
img = clean_page()
out = S.enhance(img, {"crop": False, "deskew": False, "flatten": False,
                      "denoise": False, "sharpen": False})
assert np.array_equal(out, img), np.abs(out.astype(int) - img.astype(int)).max()
# String booleans from a CLI/schema client are normalised the same way.
out2 = S.enhance(img, {"crop": False, "deskew": False, "flatten": False,
                       "denoise": False, "sharpen": False,
                       "definger": "false", "demoire": "0", "debleed": "off"})
assert np.array_equal(out2, img)
print("   defaults and false-ish strings both no-op")

print("\nALL OK")
