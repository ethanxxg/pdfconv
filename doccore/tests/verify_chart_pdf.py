"""Independent verifier for DocCore's chart rendering (XLSX -> PDF).

The acceptance criterion in docs/03 (P9) is:

    含图表的工作簿导出 PDF，柱高与数据成比例，容差 1 个像素（在 150 DPI 光栅化下）

This script independently checks two chart families straight off the rasterized
PDF pixels (PyMuPDF), never touching DocCore code paths:

  * column / stacked-column charts -- each series' bar (or stacked segment)
    height must be proportional to its backing value (residual <= 1 px after a
    least-squares fit).  We measure the longest continuous ink run per column,
    which is immune to antialiasing edges and to stray gridline/text pixels.
  * pie charts -- each slice's angular sweep (2*pi - largest angular gap) must
    be proportional to its value share (relative deviation <= 8 %).  A slice is
    a contiguous arc of one palette colour; the legend swatches are excluded by
    distance from the pie centre.

Fixtures are written by openpyxl; the expected values come straight from that
workbook, so nothing here depends on DocCore internals.

Usage:
    python tests/verify_chart_pdf.py [--exe PATH] [--dpi 150] [--keep] [--seed N]
                                     [--kind col|stacked|pie|all]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

import fitz  # PyMuPDF
import numpy as np
import openpyxl
from openpyxl.chart import BarChart, PieChart, Reference
from openpyxl.utils import get_column_letter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# DocCore 的默认系列调色板（src/render/sheet_layout.cpp kSeriesPalette）。
SERIES_RGB = [
    (0x4F, 0x81, 0xBD),
    (0xC0, 0x50, 0x4D),
    (0x9B, 0xBB, 0x59),
    (0x80, 0x6A, 0xA6),
    (0x4B, 0xAC, 0xC6),
    (0xF7, 0x96, 0x46),
]

failures = []


def check(cond, message):
    if not cond:
        failures.append(message)
        print("FAIL: %s" % message)


def build_workbook(path, seed, kind="col"):
    """写一份带图表的工作簿，返回 (s1, s2, kind)。

    col / stacked 用两系列柱图；pie 用单系列饼图（s2 返回 None）。
    """
    rnd = random.Random(seed)
    cats = ["North", "South", "East", "West", "Central"]
    # 堆叠图的 Y 轴跨度是每个分类的两系列之和（可达 ~120），单段比例相应变矮；
    # 把下限抬到 15，确保最小的堆叠段也有足够像素高度可测。
    s1 = [rnd.randint(15, 60) for _ in cats]
    s2 = [rnd.randint(15, 60) for _ in cats] if kind != "pie" else None
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Report"
    if kind == "pie":
        ws.append(["Region", "Share"])
        for i, name in enumerate(cats):
            ws.append([name, s1[i]])
        chart = PieChart()
        chart.title = "Share by region"
        data = Reference(ws, min_col=2, max_col=2, min_row=1, max_row=1 + len(cats))
        cats_ref = Reference(ws, min_col=1, min_row=2, max_row=1 + len(cats))
        chart.add_data(data, titles_from_data=True)
        chart.set_categories(cats_ref)
    else:
        ws.append(["Region", "Q1", "Q2"])
        for i, name in enumerate(cats):
            ws.append([name, s1[i], s2[i]])
        chart = BarChart()
        chart.type = "col"
        chart.grouping = "stacked" if kind == "stacked" else "clustered"
        chart.title = "Revenue by region"
        data = Reference(ws, min_col=2, max_col=3, min_row=1, max_row=1 + len(cats))
        cats_ref = Reference(ws, min_col=1, min_row=2, max_row=1 + len(cats))
        chart.add_data(data, titles_from_data=True)
        chart.set_categories(cats_ref)
    ws.add_chart(chart, "E2")
    wb.save(path)
    return s1, s2


def export_pdf(exe, xlsx, pdf, font):
    cmd = [exe, "xlsx", xlsx, "--mode", "pdf", "--out", pdf]
    if os.path.exists(font):
        cmd += ["--font", font]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(pdf):
        print("导出 PDF 失败: %s %s" % (proc.stdout, proc.stderr))
        return False
    print("导出 %s" % proc.stdout.strip())
    return True


def coverage_map(pixmap, rgb, max_resid=40.0):
    """每像素对"背景白 -> 柱色"这条线段的覆盖率。

    直接数同色像素会被抗锯齿边缘骗到（差 1~2 像素）。把像素投影到 背景→柱色
    方向上得到 alpha：纯背景 0、纯柱色 1、两者混合按比例落在中间。只有落在这条
    线段附近（残差小）且没超出柱色（黑色文字会投影到 >1）的像素才算，
    否则浅灰网格线和深色文字都会混进来。
    """
    arr = np.frombuffer(pixmap.samples, dtype=np.uint8).reshape(
        pixmap.height, pixmap.width, pixmap.n
    )[:, :, :3].astype(np.float64)
    bg = np.array([255.0, 255.0, 255.0])
    bar = np.array(rgb, dtype=np.float64)
    delta = bar - bg
    norm = float((delta * delta).sum())
    if norm <= 0:
        return np.zeros((pixmap.height, pixmap.width))
    t = ((arr - bg) @ delta) / norm
    proj = bg + t[:, :, None] * delta
    resid = np.abs(arr - proj).max(axis=2)
    valid = (t >= -0.02) & (t <= 1.02) & (resid <= max_resid)
    return np.where(valid, np.clip(t, 0.0, 1.0), 0.0)


def longest_run_sum(alpha, thresh=0.05):
    """每列"最长一段连续 ink"的覆盖率之和。

    柱子是一整段连续色块；网格线、文字是零星几像素，会被这段连续判定自然分开。
    """
    height, width = alpha.shape
    best = np.zeros(width)
    cur = np.zeros(width)
    for y in range(height):
        row = alpha[y]
        cur = np.where(row > thresh, cur + row, 0.0)
        np.maximum(best, cur, out=best)
    return best


def exact_color_mask(pixmap, rgb, tol=24):
    """按颜色距离取"几乎就是该颜色"的像素（饼图扇区隔离用）。

    饼图各扇区用的是一组相近的饱和色，用投影覆盖率无法可靠区分——例如 teal
    0x4BACC6 投影到 blue 0x4F81BD 上 alpha≈0.9、残差仅 30，会被误判成蓝色。
    但按欧氏距离看：目标色内部距离为 0，而调色板里离它最近的另一个颜色也有
    44（blue↔teal），网格灰更远。所以用 tol=24 就能干净地只选中真正的该色像素，
    把每个扇区单独隔离出来。
    """
    arr = np.frombuffer(pixmap.samples, dtype=np.uint8).reshape(
        pixmap.height, pixmap.width, pixmap.n
    )[:, :, :3].astype(np.float64)
    d = np.sqrt(((arr - np.array(rgb, dtype=np.float64)) ** 2).sum(axis=2))
    return (d <= tol)


def find_rect_bars(pixmap, rgb, min_height_px=15, min_width_px=20):
    """返回 [(x0, x1, height_px)]：等宽的竖直色块，即柱子或堆叠段。

    柱子每一列的同色连续段一样高（亚像素精度），饼图扇区的列高连续变化，
    图例色块太矮（7pt），网格线只是零星像素——都能被分开。min_height 取 15px
    是为了让堆叠图里数值较小的段也能量到，同时仍高于网格线与图例色块。
    """
    alpha = coverage_map(pixmap, rgb)
    coverage = longest_run_sum(alpha)  # 每列连续色段的高度（亚像素）
    bars = []
    x = 0
    width = pixmap.width
    while x < width:
        if coverage[x] >= min_height_px:
            x0 = x
            cols = []
            while x < width and coverage[x] >= min_height_px:
                cols.append(float(coverage[x]))
                x += 1
            # 首末两列是抗锯齿的半覆盖边缘，只取内部列比高度。
            inner = cols[1:-1] if len(cols) > 2 else []
            if len(inner) >= min_width_px:
                height = float(np.median(inner))
                # 绝大多数列等高才算柱子；个别列贴着表格竖网格线， Ink 会多出一截。
                close = sum(1 for c in inner if abs(c - height) <= 2.0)
                if height >= min_height_px and close >= 0.8 * len(inner):
                    bars.append((x0, x, height))
        else:
            x += 1
    return bars


def verify_bars(pdf, values, rgb, label, dpi):
    doc = fitz.open(pdf)
    for page in doc:
        pixmap = page.get_pixmap(dpi=dpi)
        bars = find_rect_bars(pixmap, rgb)
        if len(bars) == len(values):
            check_series(pixmap, values, rgb, label, dpi)
            return True
    check(False, "%s: 任何一页都没量到与数据等数的柱子" % label)
    return False


def check_series(pixmap, values, rgb, label, dpi):
    bars = find_rect_bars(pixmap, rgb)
    check(
        len(bars) == len(values),
        "%s: 找到 %d 根柱子，期望 %d 根" % (label, len(bars), len(values)),
    )
    if len(bars) != len(values):
        return
    heights = [h for _, _, h in bars]
    # 与数值成比例：用最小二乘拟合 h = scale * v，每根柱子残差须 <= 1 像素。
    # （光栅化本身会取整，所以不能拿均值当基准，得先拟合再比残差。）
    vv = [float(v) for v in values]
    scale = sum(h * v for h, v in zip(heights, vv)) / sum(v * v for v in vv)
    for i, (h, v) in enumerate(zip(heights, values)):
        expect = scale * v
        check(
            abs(h - expect) <= 1.0,
            "%s bar %d: 高度 %d px，按比例应为 %.1f px (v=%d, scale=%.3f px/单位)"
            % (label, i, h, expect, v, scale),
        )
    print("  %s: 柱高 %s px，比例 %.3f px/单位" % (label, heights, scale))


def verify_pie(pdf, values, dpi):
    """量饼图每个扇区的张角，验证与数值占比成比例。"""
    doc = fitz.open(pdf)
    n = len(values)
    for page in doc:
        pixmap = page.get_pixmap(dpi=dpi)
        masks = [exact_color_mask(pixmap, SERIES_RGB[i]) for i in range(n)]
        union = masks[0].copy()
        for m in masks[1:]:
            union = union | m
        ys, xs = np.nonzero(union)
        if len(xs) < 200:
            continue
        # 迭代求饼心：用半径阈值把远处的图例色块剔除，反复收敛到饼盘中心。
        cx = float(xs.mean())
        cy = float(ys.mean())
        for _ in range(5):
            dx = xs - cx
            dy = ys - cy
            d = np.sqrt(dx * dx + dy * dy)
            R = float(np.median(d))
            keep = d <= R * 1.25
            if keep.sum() == 0:
                break
            cx = float(xs[keep].mean())
            cy = float(ys[keep].mean())
        dx = xs - cx
        dy = ys - cy
        d = np.sqrt(dx * dx + dy * dy)
        R = float(np.median(d))
        if R < 30:
            continue
        sweeps = []
        ok_all = True
        for i in range(n):
            yc, xc = np.nonzero(masks[i])
            ddc = np.sqrt((xc - cx) ** 2 + (yc - cy) ** 2)
            # 只取饼盘内部的一个圆环：外侧半径 < R 彻底排除图例色块（它们在盘外），
            # 内侧半径 >0 排除扇区尖端汇聚与中心 "%标签" 的杂乱像素。圆环里每个扇区
            # 是一段干净的连续角度带，"整圈减最大缺口" 才稳定。
            mask = (ddc >= 0.25 * R) & (ddc <= 0.92 * R)
            xc = xc[mask]
            yc = yc[mask]
            if len(xc) < 20:
                check(False, "pie slice %d: 量到的像素太少" % i)
                ok_all = False
                sweeps.append(0.0)
                continue
            theta = np.arctan2(yc - cy, xc - cx)
            theta = np.where(theta < 0, theta + 2 * np.pi, theta)
            t = np.sort(theta)
            gap_wrap = 2 * np.pi + t[0] - t[-1]
            gaps = np.diff(t)
            max_gap = max(gap_wrap, gaps.max() if gaps.size else 0.0)
            sweep = 2 * np.pi - max_gap  # 连续弧 = 整圈减去最大缺口
            sweeps.append(sweep)
        if not ok_all:
            continue
        ratios = [sweep / float(v) for sweep, v in zip(sweeps, values)]
        r_mean = float(np.mean(ratios))
        for i, (sweep, v) in enumerate(zip(sweeps, values)):
            rel = abs(ratios[i] - r_mean) / r_mean if r_mean else 1.0
            check(
                rel <= 0.08,
                "pie slice %d: 张角 %.3f rad，按占比应为 %.3f rad (相对偏差 %.1f%%)"
                % (i, sweep, r_mean * v, rel * 100),
            )
        print("  pie: 张角 %s rad，比例 %.4f rad/单位"
              % (["%.3f" % s for s in sweeps], r_mean))
        return True
    check(False, "PDF 上没有量到饼图")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "Release", "doccore.exe"))
    ap.add_argument("--dpi", type=int, default=150)
    ap.add_argument("--font", default=os.path.join(HERE, "data", "test.ttf"))
    ap.add_argument("--keep", action="store_true", help="保留中间文件")
    ap.add_argument("--seed", type=int, default=20261021, help="随机数据种子，可换几组数据复测")
    ap.add_argument("--kind", default="all", choices=("col", "stacked", "pie", "all"))
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到 %s" % exe)
        return 2
    tmp = tempfile.mkdtemp(prefix="doccore_chart_")
    xlsx = os.path.join(tmp, "chart.xlsx")
    pdf = os.path.join(tmp, "chart.pdf")

    kinds = ("col", "stacked", "pie") if args.kind == "all" else (args.kind,)
    for kind in kinds:
        s1, s2 = build_workbook(xlsx, args.seed, kind)
        if not export_pdf(exe, xlsx, pdf, args.font):
            return 2
        print("== %s ==" % kind)
        if kind == "pie":
            verify_pie(pdf, s1, args.dpi)
        else:
            # 两个系列都要量；柱高（簇状）或堆叠段高都与各自数值成比例。
            verify_bars(pdf, s1, SERIES_RGB[0], "series 1", args.dpi)
            verify_bars(pdf, s2, SERIES_RGB[1], "series 2", args.dpi)

    if not args.keep:
        for path in (xlsx, pdf):
            try:
                os.remove(path)
            except OSError:
                pass
        try:
            os.rmdir(tmp)
        except OSError:
            pass
    else:
        print("中间文件保留在 %s" % tmp)

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("verify_chart_pdf: ok (%d dpi)" % args.dpi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
