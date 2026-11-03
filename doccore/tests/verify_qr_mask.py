#!/usr/bin/env python3
"""QR 掩膜选择独立校验（F-601 质量项）。

从 DocCore 生成的 PNG 反推出模块矩阵 → 读格式信息里的掩膜号 → 反掩膜得到
"裸"矩阵 → 按 ISO/IEC 18004 表 11 的四条惩罚规则独立计算 8 个候选掩膜的分
数 → 验证 DocCore 选中的掩膜确实是分数最小者。

用法：python tests/verify_qr_mask.py [png ...]
不带参数时校验清单里 version >= 2 的全部 QR。
"""
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out")

# 对齐图案中心（与 ISO/IEC 18004 表 E.1 一致；版本 1 无对齐图案）。
ALIGN = {
    2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30], 6: [6, 34],
    7: [6, 22, 38], 8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50],
    11: [6, 30, 54], 12: [6, 32, 58], 13: [6, 34, 62], 14: [6, 26, 46, 66],
    15: [6, 26, 48, 70], 16: [6, 26, 50, 74], 17: [6, 30, 54, 78],
    18: [6, 30, 56, 82], 19: [6, 30, 58, 86], 20: [6, 34, 62, 90],
    21: [6, 28, 50, 72, 94], 22: [6, 26, 50, 74, 98], 23: [6, 30, 54, 78, 102],
    24: [6, 28, 54, 80, 106], 25: [6, 32, 58, 84, 110], 26: [6, 30, 58, 86, 114],
    27: [6, 34, 62, 90, 118], 28: [6, 26, 50, 74, 98, 122],
    29: [6, 30, 54, 78, 102, 126], 30: [6, 26, 52, 78, 104, 130],
    31: [6, 30, 56, 82, 108, 134], 32: [6, 34, 60, 86, 112, 138],
    33: [6, 30, 58, 86, 114, 142], 34: [6, 34, 62, 90, 118, 146],
    35: [6, 30, 54, 78, 102, 126, 150], 36: [6, 24, 50, 76, 102, 128, 154],
    37: [6, 28, 54, 80, 106, 132, 158], 38: [6, 32, 58, 84, 110, 136, 162],
    39: [6, 26, 54, 82, 110, 138, 166], 40: [6, 30, 58, 86, 114, 142, 170],
}

MASK_FUNCS = [
    lambda r, c: (r + c) % 2 == 0,
    lambda r, c: r % 2 == 0,
    lambda r, c: c % 3 == 0,
    lambda r, c: (r + c) % 3 == 0,
    lambda r, c: (r // 2 + c // 3) % 2 == 0,
    lambda r, c: (r * c) % 2 + (r * c) % 3 == 0,
    lambda r, c: ((r * c) % 2 + (r * c) % 3) % 2 == 0,
    lambda r, c: ((r + c) % 2 + (r * c) % 3) % 2 == 0,
]


def function_map(version, n):
    """标记功能模块（True = 功能模块，不参与数据填充与掩膜）。"""
    f = [[False] * n for _ in range(n)]

    def box(r0, c0, h, w):
        for r in range(r0, r0 + h):
            for c in range(c0, c0 + w):
                if 0 <= r < n and 0 <= c < n:
                    f[r][c] = True

    # 定位图案 + 分隔符
    box(0, 0, 8, 8)
    box(0, n - 8, 8, 8)
    box(n - 8, 0, 8, 8)
    # 定时图案
    for i in range(n):
        f[6][i] = True
        f[i][6] = True
    # 对齐图案
    centers = ALIGN.get(version, [])
    for a in centers:
        for b in centers:
            if (a, b) in ((centers[0], centers[0]),
                          (centers[0], centers[-1]),
                          (centers[-1], centers[0])):
                continue
            box(a - 2, b - 2, 5, 5)
    # 格式信息（两份）
    for i in range(9):
        f[8][i] = True
        f[i][8] = True
    for i in range(8):
        f[8][n - 1 - i] = True
        f[n - 1 - i][8] = True
    f[n - 8][8] = True
    # 版本信息
    if version >= 7:
        for i in range(18):
            a = n - 11 + i % 3
            b = i // 3
            f[a][b] = True
            f[b][a] = True
    return f


def penalty(m, n):
    """ISO/IEC 18004 表 11 的四条惩罚规则。"""
    score = 0
    # 规则 1：行/列连续同色 ≥5
    for row in (m, [list(col) for col in zip(*m)]):
        for line in row:
            run = 1
            for j in range(1, n):
                if line[j] == line[j - 1]:
                    run += 1
                else:
                    if run >= 5:
                        score += 3 + (run - 5)
                    run = 1
            if run >= 5:
                score += 3 + (run - 5)
    # 规则 2：2x2 同色块
    for r in range(n - 1):
        for c in range(n - 1):
            v = m[r][c]
            if m[r][c + 1] == v and m[r + 1][c] == v and m[r + 1][c + 1] == v:
                score += 3
    # 规则 3：类定位图案
    pa = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0]
    pb = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1]
    for row in (m, [list(col) for col in zip(*m)]):
        for line in row:
            for j in range(n - 10):
                w = line[j:j + 11]
                if w == pa or w == pb:
                    score += 40
    # 规则 4：深浅比例
    dark = sum(sum(1 for v in line if v) for line in m)
    total = n * n
    k = (abs(dark * 20 - total * 10) + total - 1) // total - 1
    score += max(k, 0) * 10
    return score


def read_matrix(path, scale, quiet):
    import cv2
    import numpy as np
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    n = img.shape[0] // scale - 2 * quiet
    m = [[0] * n for _ in range(n)]
    for r in range(n):
        for c in range(n):
            y = (r + quiet) * scale + scale // 2
            x = (c + quiet) * scale + scale // 2
            m[r][c] = 1 if img[y, x] < 128 else 0
    return m, n


def bch_format_remainder(data):
    """格式信息的 BCH(15,5) 余数（生成多项式 0x537）。"""
    rem = data
    for _ in range(10):
        rem = (rem << 1) ^ (0x537 if (rem >> 9) & 1 else 0)
    return rem & 0x3FF


def format_bits(m):
    """从第一份格式信息读出 15 位（bit0 = LSB）。"""
    # (row, col)：第 8 列行 0..5、(7,8)、(8,8)、(8,7)、第 8 行列 5..0
    pos = [(i, 8) for i in range(6)] + [(7, 8), (8, 8), (8, 7)] + \
          [(8, 14 - i) for i in range(9, 15)]
    bits = 0
    for i, (r, c) in enumerate(pos):
        if m[r][c]:
            bits |= 1 << i
    return bits ^ 0x5412  # 去掉生成多项式掩码


def write_format_info(m, ec_fmt, mask, n):
    """把某掩膜对应的 15 位格式信息写进矩阵（两份副本 + 固定深色模块）。"""
    data = (ec_fmt << 3) | mask
    bits = ((data << 10) | bch_format_remainder(data)) ^ 0x5412
    first = [(i, 8) for i in range(6)] + [(7, 8), (8, 8), (8, 7)] + \
            [(8, 14 - i) for i in range(9, 15)]
    second = [(8, n - 1 - i) for i in range(8)] + \
             [(n - 15 + i, 8) for i in range(8, 15)]
    for i, (r, c) in enumerate(first):
        m[r][c] = (bits >> i) & 1
    for i, (r, c) in enumerate(second):
        m[r][c] = (bits >> i) & 1
    m[n - 8][8] = 1


def check(path, scale=4, quiet=4):
    m, n = read_matrix(path, scale, quiet)
    version = (n - 17) // 4
    raw = format_bits(m)
    data = raw >> 10
    rem = raw & 0x3FF
    if bch_format_remainder(data) != rem:
        return "format BCH check failed (raw=0x%04x)" % raw
    ec_fmt = (data >> 3) & 3
    mask = data & 7
    level = {1: "L", 0: "M", 3: "Q", 2: "H"}[ec_fmt]

    fmap = function_map(version, n)
    # 反掩膜得到裸矩阵
    bare = [row[:] for row in m]
    for r in range(n):
        for c in range(n):
            if not fmap[r][c] and MASK_FUNCS[mask](r, c):
                bare[r][c] ^= 1

    scores = []
    for cand in range(8):
        cand_m = [row[:] for row in bare]
        for r in range(n):
            for c in range(n):
                if not fmap[r][c] and MASK_FUNCS[cand](r, c):
                    cand_m[r][c] ^= 1
        # 格式信息位随掩膜变化，必须按候选掩膜重写后再计分。
        write_format_info(cand_m, ec_fmt, cand, n)
        scores.append(penalty(cand_m, n))
    best = min(range(8), key=lambda i: scores[i])
    if best != mask:
        return "v%d %s mask=%d but penalty-minimal mask=%d (scores=%s)" % (
            version, level, mask, best, scores)
    return None


def main():
    targets = []
    if len(sys.argv) > 1:
        targets = sys.argv[1:]
    else:
        man = os.path.join(OUT, "p9_qr_manifest.txt")
        with open(man, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.strip():
                    targets.append(os.path.join(OUT, line.split("\t")[0]))
    bad = 0
    for path in targets:
        err = check(path)
        if err:
            bad += 1
            print("FAIL %s: %s" % (os.path.basename(path), err))
    print("checked %d QR images, %d mask-selection mismatch(es)" % (len(targets), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
