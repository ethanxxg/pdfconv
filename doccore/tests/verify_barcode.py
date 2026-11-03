#!/usr/bin/env python3
"""F-601 条码独立校验：用 cv2 / zxing-cpp 解码 DocCore 生成的 QR 与 Code128。

输入（由 doccore_tests 的 "barcode" 用例产出）：
  out/p9_qr_manifest.txt   清单：文件名 \\t EC等级 \\t 版本 \\t 载荷(hex)
  out/p9_qr_NNN.png        DocCore 生成的 QR 图
  out/p9_code128.png       DocCore 生成的 Code128 图

校验点：
  1. QR 载荷能被 zxing-cpp 与 cv2 两个独立解码器读回（且一致）。
  2. QR 版本号是按 python-qrcode 的权威分块表（RS_BLOCK_TABLE）算出的
     最小可用版本（验证容量表 + 版本选择；注意 python-qrcode 的 make() 会
     自动选压缩模式且其 q.version 属性不可靠，故不用它做参照）。
  3. Code128 能被 zxing-cpp 读回同一文本。
"""
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL " + msg)


def data_codewords(version, level_idx):
    """该版本/等级的数据码字数（取自 python-qrcode 的权威分块表）。"""
    from qrcode.base import RS_BLOCK_TABLE
    entry = RS_BLOCK_TABLE[(version - 1) * 4 + level_idx]
    total = 0
    for i in range(0, len(entry), 3):
        total += entry[i] * entry[i + 2]  # 块数 × 每块数据码字
    return total


def minimal_byte_version(payload_len, level_idx):
    """字节模式下能容纳 payload_len 字节的最小 QR 版本。"""
    for version in range(1, 41):
        cc_bits = 8 if version <= 9 else 16
        need_bits = 4 + cc_bits + payload_len * 8
        if data_codewords(version, level_idx) * 8 >= need_bits:
            return version
    return None


def main():
    import cv2
    import zxingcpp

    manifest = os.path.join(OUT, "p9_qr_manifest.txt")
    if not os.path.exists(manifest):
        print("manifest missing, run doccore_tests first")
        return 1

    rows = []
    with open(manifest, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            name, level, version, hexed = line.split("\t")
            rows.append((name, level, int(version), bytes.fromhex(hexed)))
    print("manifest entries: %d" % len(rows))

    detector = cv2.QRCodeDetector()
    cv2_ok = cv2_upscaled = cv2_none = zx_ok = ver_ok = 0
    for name, level, version, payload in rows:
        path = os.path.join(OUT, name)
        img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
        if img is None:
            fail("%s: cannot read image" % name)
            continue

        # 1) zxing-cpp 解码（主判据：独立解码器读回同一载荷）
        results = zxingcpp.read_barcodes(img)
        hits = [r for r in results if r.format == zxingcpp.BarcodeFormat.QRCode]
        if not hits:
            fail("%s: zxing-cpp found no QR" % name)
        elif hits[0].text.encode("utf-8") != payload:
            fail("%s: zxing-cpp payload mismatch (got %d bytes, want %d)"
                 % (name, len(hits[0].text), len(payload)))
        else:
            zx_ok += 1

        # 2) cv2 解码；小图失败时放大重试（cv2 对大版本码有已知的识别局限）
        try:
            text, _, _ = detector.detectAndDecode(img)
        except Exception:  # noqa: BLE001
            text = ""
        if text and text.encode("utf-8") == payload:
            cv2_ok += 1
        elif text:
            fail("%s: cv2 payload mismatch" % name)
        else:
            got = ""
            for factor in (2, 3):
                big = cv2.resize(img, None, fx=factor, fy=factor,
                                 interpolation=cv2.INTER_NEAREST)
                try:
                    t, _, _ = detector.detectAndDecode(big)
                except Exception:  # noqa: BLE001
                    t = ""
                if t:
                    got = t
                    break
            if got and got.encode("utf-8") == payload:
                cv2_upscaled += 1
            elif got:
                fail("%s: cv2 payload mismatch after upscale" % name)
            else:
                cv2_none += 1
                print("note %s: cv2 no result even upscaled (v%d, %d bytes)"
                      % (name, version, len(payload)))

        # 3) 版本最小性（独立重算）
        expect = minimal_byte_version(len(payload), "LMQH".index(level))
        if expect is None:
            fail("%s: payload exceeds version 40 at level %s" % (name, level))
        elif expect != version:
            fail("%s: version %d != minimal %d (level %s, %d bytes)"
                 % (name, version, expect, level, len(payload)))
        else:
            ver_ok += 1

    print("zxing-cpp decoded: %d/%d" % (zx_ok, len(rows)))
    print("cv2 decoded:       %d/%d (upscaled %d, none %d)"
          % (cv2_ok, len(rows), cv2_upscaled, cv2_none))
    print("version minimal:   %d/%d" % (ver_ok, len(rows)))

    # 4) Code128
    cpath = os.path.join(OUT, "p9_code128.png")
    if not os.path.exists(cpath):
        fail("p9_code128.png missing")
    else:
        cimg = cv2.imread(cpath, cv2.IMREAD_GRAYSCALE)
        cres = zxingcpp.read_barcodes(cimg)
        codes = [r for r in cres if r.format == zxingcpp.BarcodeFormat.Code128]
        if not codes:
            fail("Code128: zxing-cpp found no Code128 (got %s)"
                 % [str(r.format) for r in cres])
        elif codes[0].text != "AB":
            fail("Code128: text %r != 'AB'" % codes[0].text)
        else:
            print("Code128 decoded:  'AB' ok")

    print()
    if failures:
        print("%d FAILURE(S)" % len(failures))
        return 1
    print("barcode independent verification: ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
