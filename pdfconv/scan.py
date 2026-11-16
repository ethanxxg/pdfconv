"""Photo -> clean scan: the image side of a document scanner.

Pure numpy/cv2 functions, no service concerns and no I/O beyond the two
``*_file`` helpers.  Every stage is defensive: when it cannot find what it
needs it returns the image unchanged.  A wrong crop or a wild deskew is much
worse than leaving that part of the photo alone.

Stages run in this order:

    definger -> crop (perspective) -> deskew -> flatten illumination
             -> demoire -> debleed -> denoise -> colour mode -> sharpen

The first three AI capture fixes are opt-in: a wrong "finger" or moire removal
is far more visible than the artefact itself, so callers must ask for them.

OpenCV is imported lazily so that a machine without it can still import the
rest of ``pdfconv``; ``service.scan`` turns the RuntimeError below into a
stable job error code.
"""
try:
    import cv2
    import numpy as np
except ImportError:                       # pragma: no cover - deploy guard
    cv2 = None
    np = None

DETECT_SIDE = 2000        # downscale to this for contour/deskew detection
MODES = ("color", "gray", "bw")


def available():
    return cv2 is not None and np is not None


def _need():
    if not available():
        raise RuntimeError("opencv_unavailable")


def _detect_copy(img):
    h, w = img.shape[:2]
    scale = min(1.0, float(DETECT_SIDE) / max(h, w))
    if scale >= 1.0:
        return img, 1.0
    small = cv2.resize(img, None, fx=scale, fy=scale,
                       interpolation=cv2.INTER_AREA)
    return small, scale


# -- crop -----------------------------------------------------------------
def _paper_quad(small):
    """Largest bright quadrilateral in *small*, or None.

    The common case is a bright sheet on a darker desk.  Otsu on a blurred
    grayscale separates the two; a morphological close bridges the text so
    the sheet is one contour instead of a cloud of glyphs.  If the scene does
    not have that contrast (a white desk, a dark document) we return None and
    the caller skips cropping rather than cutting into the subject.
    """
    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (5, 5), 0)
    thr = cv2.threshold(gray, 0, 255,
                        cv2.THRESH_BINARY + cv2.THRESH_OTSU)[1]
    k = max(7, min(small.shape[:2]) // 40)
    kernel = cv2.getStructuringElement(
        cv2.MORPH_ELLIPSE, (k, k))
    thr = cv2.morphologyEx(thr, cv2.MORPH_CLOSE, kernel, iterations=2)
    contours, _ = cv2.findContours(thr, cv2.RETR_EXTERNAL,
                                   cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None
    best, best_area = None, 0.0
    for c in contours:
        area = abs(cv2.contourArea(c))
        if area <= best_area:
            continue
        peri = cv2.arcLength(c, True)
        approx = cv2.approxPolyDP(c, 0.02 * peri, True)
        if len(approx) == 4:
            best, best_area = approx.reshape(4, 2).astype("float32"), area
    if best is None:
        return None
    if best_area < 0.30 * small.shape[0] * small.shape[1]:
        return None
    return best


def _order(pts):
    s = pts.sum(axis=1)
    tl, br = pts[np.argmin(s)], pts[np.argmax(s)]
    d = np.diff(pts, axis=1).reshape(-1)
    tr, bl = pts[np.argmin(d)], pts[np.argmax(d)]
    return np.array([tl, tr, br, bl], dtype="float32")


def _auto_crop(img):
    small, scale = _detect_copy(img)
    quad = _paper_quad(small)
    if quad is None:
        return img
    pts = _order(quad) / scale
    tl, tr, br, bl = pts
    w = max(float(np.linalg.norm(tr - tl)), float(np.linalg.norm(br - bl)))
    h = max(float(np.linalg.norm(bl - tl)), float(np.linalg.norm(br - tr)))
    ih, iw = img.shape[:2]
    if w < 40 or h < 40 or w * h < 0.25 * iw * ih or w * h > 6.0 * iw * ih:
        return img
    dst = np.array([[0, 0], [w - 1, 0], [w - 1, h - 1], [0, h - 1]],
                   dtype="float32")
    m = cv2.getPerspectiveTransform(pts, dst)
    return cv2.warpPerspective(img, m, (int(w), int(h)))


# -- deskew ---------------------------------------------------------------
def _deskew(img):
    small, _ = _detect_copy(img)
    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(gray, 60, 160)
    lines = cv2.HoughLinesP(edges, 1, np.pi / 180, threshold=50,
                            minLineLength=max(20, small.shape[1] // 12),
                            maxLineGap=8)
    if lines is None:
        return img
    angles = []
    for x1, y1, x2, y2 in lines.reshape(-1, 4):
        a = np.degrees(np.arctan2(y2 - y1, x2 - x1))
        a = (a + 90) % 180 - 90          # fold into [-90, 90)
        if abs(a) < 12:                  # ignore genuinely vertical strokes
            angles.append(a)
    if len(angles) < 4:
        return img
    angle = float(np.median(angles))
    if abs(angle) < 0.5:
        return img
    h, w = img.shape[:2]
    center = (w / 2.0, h / 2.0)
    m = cv2.getRotationMatrix2D(center, angle, 1.0)
    cos, sin = abs(m[0, 0]), abs(m[0, 1])
    nw, nh = int(h * sin + w * cos), int(h * cos + w * sin)
    m[0, 2] += nw / 2.0 - center[0]
    m[1, 2] += nh / 2.0 - center[1]
    return cv2.warpAffine(img, m, (nw, nh), flags=cv2.INTER_LINEAR,
                          borderMode=cv2.BORDER_REPLICATE)


# -- illumination ---------------------------------------------------------
def _flatten(img):
    """Divide out slow illumination changes (shadows, uneven desk light).

    The blurred background is mapped to a fixed white point (~245), not to
    the image mean: after a crop the page is mostly bright, so using the mean
    would pull the whole sheet down to grey.  Dark text stays dark because it
    is far below the background estimate; only the slow gradient is scaled.
    """
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    k = max(21, (min(img.shape[:2]) // 8) | 1)   # odd, >= 21
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    bg = cv2.morphologyEx(gray, cv2.MORPH_CLOSE, kernel)
    bg = cv2.GaussianBlur(bg, (0, 0), k / 4.0)
    white = float(np.percentile(bg, 95)) or 245.0
    ratio = np.clip(white / (bg.astype("float32") + 1.0), 0.8, 1.9)
    out = img.astype("float32") * ratio[:, :, None]
    return np.clip(out, 0, 255).astype("uint8")


# -- capture-time artefacts ------------------------------------------------
def _definger(img):
    """Inpaint a finger/skin-coloured occlusion intruding from an image edge.

    Skin detection in YCrCb is only a *candidate* mask; the stage is defensive
    in three ways: candidates must touch the outer frame, form a blob of
    plausible size, and cover well under a tenth of the image.  A false
    positive that eats real text costs far more than a missed finger.
    """
    h, w = img.shape[:2]
    ycc = cv2.cvtColor(img, cv2.COLOR_BGR2YCrCb)
    cr, cb = ycc[:, :, 1], ycc[:, :, 2]
    cand = (((cr > 133) & (cr < 180) & (cb > 77) & (cb < 128))
            .astype("uint8") * 255)
    band = max(8, int(min(h, w) * 0.18))
    frame = np.zeros((h, w), dtype="uint8")
    frame[:band, :] = frame[-band:, :] = 255
    frame[:, :band] = frame[:, -band:] = 255
    cand = cv2.bitwise_and(cand, frame)
    k = max(9, min(h, w) // 40) | 1
    cand = cv2.morphologyEx(cand, cv2.MORPH_CLOSE,
                            cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k)))
    cand = cv2.morphologyEx(cand, cv2.MORPH_OPEN,
                            cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)))
    n, labels, stats, _ = cv2.connectedComponentsWithStats(cand)
    mask = np.zeros((h, w), dtype="uint8")
    total = h * w
    for i in range(1, n):
        area = int(stats[i, cv2.CC_STAT_AREA])
        if 0.004 * total < area < 0.12 * total:
            mask[labels == i] = 255
    nfill = int(cv2.countNonZero(mask))
    if nfill < 0.003 * total:
        return img
    mask = cv2.dilate(mask,
                      cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7)))
    return cv2.inpaint(img, mask, 7, cv2.INPAINT_TELEA)


def _demoire(img):
    """Notch-out high-frequency peaks caused by photographing a screen.

    A screen shot shows up as a few very sharp peaks in the Fourier spectrum.
    Only peaks that are both far from DC and far above the local background
    are removed; a normal document has a smooth spectrum and is left alone.
    """
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    f = cv2.dft(np.float32(gray), flags=cv2.DFT_COMPLEX_OUTPUT)
    sh = np.fft.fftshift(f)
    mag = cv2.magnitude(sh[:, :, 0], sh[:, :, 1])
    logmag = np.log(mag + 1.0)
    cy, cx = h // 2, w // 2
    r0 = max(8, int(0.06 * min(h, w)))
    yy, xx = np.ogrid[:h, :w]
    r = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    search = (r > r0) & (r < 0.48 * min(h, w))
    if int(search.sum()) < 100:
        return img
    local = cv2.GaussianBlur(logmag.astype("float32"), (0, 0), 9)
    thresh = float(np.percentile(logmag[search], 99.6))
    peaks = (search & (logmag > thresh) & (logmag - local > 1.8)
             & (logmag == cv2.dilate(logmag, np.ones((5, 5), np.uint8))))
    ys, xs = np.nonzero(peaks)
    if len(xs) == 0 or len(xs) > 40:
        return img
    # Keep the strongest handful only.
    order = np.argsort(logmag[ys, xs])[::-1][:6]
    notch = np.zeros((h, w), dtype=np.uint8)
    rad = max(3, min(h, w) // 220)
    for i in order:
        cv2.circle(notch, (int(xs[i]), int(ys[i])), rad, 1, -1)
    # Apply the same notch to every channel.  Doing the inverse DFT on the
    # grayscale alone would turn a colour photo grey; a document scanner must
    # not silently discard the source colour just because a screen was shot.
    holes = notch.astype(bool)
    out = np.empty_like(img)
    for c in range(3):
        ch = cv2.dft(np.float32(img[:, :, c]), flags=cv2.DFT_COMPLEX_OUTPUT)
        sch = np.fft.fftshift(ch)
        sch[holes] = 0
        back = np.fft.ifftshift(sch)
        # DFT_SCALE matters: without it OpenCV returns N times the image and
        # clipping that to uint8 turns a clean page into salt-and-pepper noise.
        rec = cv2.idft(back, flags=cv2.DFT_SCALE)
        out[:, :, c] = cv2.magnitude(rec[:, :, 0], rec[:, :, 1])
    return np.clip(out, 0, 255).astype("uint8")


def _debleed(img):
    """Suppress low-contrast type showing through from the reverse side.

    Bleed-through strokes sit just a little below the paper white, while real
    ink is much darker.  Estimate the local paper level, then pull only the
    shallow band toward white.  Require a clear dark-ink population first, so
    a light-gray photograph is not mistaken for bleed-through.
    """
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    k = max(31, (min(gray.shape) // 10) | 1)
    paper = cv2.GaussianBlur(gray, (0, 0), k / 3.0)
    depth = (paper.astype("float32") - gray.astype("float32"))
    ink_frac = float(np.mean(depth > 55))
    bleed = float(np.mean((depth > 8) & (depth < 40)))
    if ink_frac < 0.005 or bleed < 2.0 * ink_frac:
        return img
    band = (depth > 8) & (depth < 40)
    out = img.astype("float32")
    # Keep 12% of the shallow stroke; the rest goes back to paper white.
    # Work per channel so a blue pen or a coloured stamp keeps its hue -- the
    # grayscale mask only decides *where* the bleed-through is.
    for c in range(3):
        ch = img[:, :, c].astype("float32")
        local = cv2.GaussianBlur(ch, (0, 0), k / 3.0)
        d = local - ch
        out[:, :, c][band] = local[band] - d[band] * 0.12
    return np.clip(out, 0, 255).astype("uint8")


# -- finishing ------------------------------------------------------------
def _denoise(img):
    return cv2.bilateralFilter(img, 5, 35, 35)


def _sharpen(img):
    blur = cv2.GaussianBlur(img, (0, 0), 1.2)
    return cv2.addWeighted(img, 1.35, blur, -0.35, 0)


def _mode(img, mode):
    if mode == "gray":
        g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        return cv2.cvtColor(g, cv2.COLOR_GRAY2BGR)
    if mode == "bw":
        g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        bw = cv2.adaptiveThreshold(g, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                                   cv2.THRESH_BINARY, 31, 15)
        return cv2.cvtColor(bw, cv2.COLOR_GRAY2BGR)
    return img


def enhance(img, options=None):
    """Enhance one BGR image; returns a new BGR image.

    ``options`` keys: crop, deskew, flatten, denoise, sharpen (bools, default
    True), definger/demoire/debleed (bools, default False), and mode
    ("color" | "gray" | "bw").
    """
    _need()
    o = options or {}

    def on(key, default=True):
        v = o.get(key, default)
        return v if isinstance(v, bool) else str(v).strip().lower() not in (
            "false", "0", "no", "off")
    mode = str(o.get("mode") or "color").lower()
    if mode not in MODES:
        mode = "color"
    out = img
    # A finger is a capture artefact on the *source* photo, so remove it
    # before the crop can cut it off.  Moiré and bleed-through are pixel-level
    # artefacts best handled after the geometry and illumination are fixed.
    if on("definger", False):
        out = _definger(out)
    for key, fn, default in (
            ("crop", _auto_crop, True), ("deskew", _deskew, True),
            ("flatten", _flatten, True), ("demoire", _demoire, False),
            ("debleed", _debleed, False), ("denoise", _denoise, True)):
        if on(key, default):
            out = fn(out)
    out = _mode(out, mode)
    if on("sharpen"):
        out = _sharpen(out)
    return out


def document_frame(path):
    """Detect a document in a photo for the camera viewfinder overlay.

    Returns {"quad": [[x, y], ...], "area": 0..1} with points in the source
    image's pixel coordinates, or {"quad": None, "area": 0} when no confident
    sheet was found.  Uses the same detector as the scan path so what the user
    sees framed in the viewfinder is what the scanner will actually crop.
    """
    _need()
    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError("bad_image")
    small, scale = _detect_copy(img)
    quad = _paper_quad(small)
    if quad is None:
        return {"quad": None, "area": 0.0}
    pts = _order(quad) / scale
    area = float(abs(cv2.contourArea(quad))) / float(small.shape[0] * small.shape[1])
    return {"quad": [[round(float(x), 1), round(float(y), 1)] for x, y in pts],
            "area": round(area, 3)}


def _card_quad(img):
    """Largest four-point contour with an ID-card-like aspect ratio, or None.

    Cards are usually brighter than the desk; some are dark.  Try both polarities
    and accept a loose aspect range so a tilted card still survives.
    """
    small, scale = _detect_copy(img)
    gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (5, 5), 0)
    k = max(7, min(small.shape[:2]) // 40)
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    for mode in (cv2.THRESH_BINARY, cv2.THRESH_BINARY_INV):
        thr = cv2.threshold(gray, 0, 255, mode + cv2.THRESH_OTSU)[1]
        closed = cv2.morphologyEx(thr, cv2.MORPH_CLOSE, kernel, iterations=2)
        contours, _ = cv2.findContours(closed, cv2.RETR_EXTERNAL,
                                       cv2.CHAIN_APPROX_SIMPLE)
        for c in sorted(contours, key=cv2.contourArea, reverse=True)[:8]:
            if abs(cv2.contourArea(c)) < 0.05 * small.shape[0] * small.shape[1]:
                continue
            peri = cv2.arcLength(c, True)
            approx = cv2.approxPolyDP(c, 0.02 * peri, True)
            if len(approx) != 4:
                continue
            pts = _order(approx.reshape(4, 2).astype("float32"))
            tl, tr, br, bl = pts
            w = max(float(np.linalg.norm(tr - tl)), float(np.linalg.norm(br - bl)))
            h = max(float(np.linalg.norm(bl - tl)), float(np.linalg.norm(br - tr)))
            if w < 30 or h < 30:
                continue
            if 1.25 <= w / h <= 2.0:
                return pts / scale
    return None


def _warp_card(img, pts, dpi):
    """Warp a card quad to the ISO 7810 size in pixels at *dpi*."""
    cw = int(round(85.6 / 25.4 * dpi))
    ch = int(round(54.0 / 25.4 * dpi))
    dst = np.array([[0, 0], [cw - 1, 0], [cw - 1, ch - 1], [0, ch - 1]],
                   dtype="float32")
    m = cv2.getPerspectiveTransform(pts, dst)
    return cv2.warpPerspective(img, m, (cw, ch))


def idcard_sheet(paths, dst, options=None, dpi=200, fmt="pdf"):
    """Lay one or two card photos onto a single A4 sheet at *dst*.

    With one photo the card is centred; with two the first is the front and
    the second the back, stacked down the page.  Each card is detected,
    perspective-corrected to its real dimensions, then lightly enhanced.
    """
    _need()
    from PIL import Image
    o = options or {}
    mode = str(o.get("mode") or "color").lower()
    if mode not in MODES:
        mode = "color"
    clean = {"crop": False, "deskew": False,
             "flatten": o.get("flatten", True),
             "denoise": o.get("denoise", True),
             "sharpen": o.get("sharpen", True), "mode": mode}

    aw = int(round(210.0 / 25.4 * dpi))
    ah = int(round(297.0 / 25.4 * dpi))
    cw = int(round(85.6 / 25.4 * dpi))
    ch = int(round(54.0 / 25.4 * dpi))
    sheet = np.full((ah, aw, 3), 255, dtype="uint8")

    for i, src in enumerate(paths[:2]):
        img = cv2.imread(src, cv2.IMREAD_COLOR)
        if img is None:
            raise RuntimeError("bad_image")
        quad = _card_quad(img)
        card = _warp_card(img, quad, dpi) if quad is not None else \
            cv2.resize(img, (cw, ch), interpolation=cv2.INTER_AREA)
        card = enhance(card, clean)
        x = (aw - cw) // 2
        if len(paths) == 1:
            y = (ah - ch) // 2
        else:
            y = int(round(25.0 / 25.4 * dpi)) + i * (ch + int(round(15.0 / 25.4 * dpi)))
        y = max(0, min(y, ah - ch))
        sheet[y:y + ch, x:x + cw] = card

    rgb = cv2.cvtColor(sheet, cv2.COLOR_BGR2RGB)
    im = Image.fromarray(rgb)
    fmt = "jpg" if str(fmt).lower() in ("jpg", "jpeg") else "pdf"
    if fmt == "pdf":
        im.save(dst, format="PDF", resolution=float(dpi))
    else:
        im.save(dst, format="JPEG", quality=92, dpi=(int(dpi), int(dpi)))
    return {"width": aw, "height": ah, "cards": min(len(paths), 2)}


def enhance_file(src, dst, options=None, fmt="jpg", dpi=200):
    """Read *src*, enhance it, and write *dst* with DPI metadata.

    DPI matters: ``pdfconv.pages.images_to_pdf`` builds each PDF page from the
    image, and without metadata a 200-DPI scan becomes a 6x8-inch page of tiny
    text instead of a normal A4 sheet.
    """
    _need()
    from PIL import Image
    img = cv2.imread(src, cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError("bad_image")
    out = enhance(img, options)
    rgb = cv2.cvtColor(out, cv2.COLOR_BGR2RGB)
    im = Image.fromarray(rgb)
    fmt = "jpg" if str(fmt).lower() in ("jpg", "jpeg") else "png"
    save = {"dpi": (int(dpi), int(dpi))}
    if fmt == "jpg":
        save.update(quality=92, subsampling=0)
    im.save(dst, format="JPEG" if fmt == "jpg" else "PNG", **save)
    return {"width": out.shape[1], "height": out.shape[0]}
