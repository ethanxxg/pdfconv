"""Photo -> clean scan: the image side of a document scanner.

Pure numpy/cv2 functions, no service concerns and no I/O beyond the two
``*_file`` helpers.  Every stage is defensive: when it cannot find what it
needs it returns the image unchanged.  A wrong crop or a wild deskew is much
worse than leaving that part of the photo alone.

Stages run in this order:

    crop (perspective) -> deskew -> flatten illumination -> denoise
                       -> colour mode -> sharpen

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
    True) and mode ("color" | "gray" | "bw").
    """
    _need()
    o = options or {}
    on = lambda key: str(o.get(key, "true")).lower() not in (
        "false", "0", "no", "off")
    mode = str(o.get("mode") or "color").lower()
    if mode not in MODES:
        mode = "color"
    out = img
    for key, fn in (("crop", _auto_crop), ("deskew", _deskew),
                    ("flatten", _flatten), ("denoise", _denoise)):
        if on(key):
            out = fn(out)
    out = _mode(out, mode)
    if on("sharpen"):
        out = _sharpen(out)
    return out


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
