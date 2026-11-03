"""Regenerates tests/data/sample.msg (fixture for P9 email / F-501 MSG path).

Builds a minimal OLE2/CFB compound file by hand, with the small MSG property
streams stored in the mini stream (via MiniFAT) as real MSG files do:

  __substg1.0_0037001E  PidTagSubject, PT_STRING8  (code-page subject)
  __substg1.0_3FFD0003  PidTagMessageCodepage = 1252 (Windows-1252)
  __substg1.0_0C1A001F  PidTagSenderName, PT_UNICODE
  __substg1.0_0065001F  PidTagSentRepresentingEmailAddress, PT_UNICODE
  __substg1.0_1000001F  PidTagBody, PT_UNICODE

The subject uses a non-Unicode code page (Windows-1252) with bytes 0x93/0x94
(smart quotes) that differ from Latin-1, so the reader must decode per code page.
Validated with olefile. Run:  python tests/make_sample_msg.py
"""
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sample.msg")

SECTOR = 512
MINI = 64
ENDOFCHAIN = 0xFFFFFFFE
FATSECT = 0xFFFFFFFD
FREESECT = 0xFFFFFFFF
NOSTREAM = 0xFFFFFFFF

STREAMS = {
    "__substg1.0_0037001E": "Café “Deal”".encode("windows-1252"),
    "__substg1.0_3FFD0003": struct.pack("<I", 1252),
    "__substg1.0_0C1A001F": "Dana 测试".encode("utf-16-le"),
    "__substg1.0_0065001F": "dana@example.com".encode("utf-16-le"),
    "__substg1.0_1000001F": "Body in unicode.".encode("utf-16-le"),
}


def pad(b, n):
    if len(b) % n:
        b = b + b"\x00" * (n - len(b) % n)
    return b


def main():
    names = list(STREAMS.keys())

    # ---- mini stream：把小流按 64 字节拼接，记录每个流的起始 mini sector ----
    mini_blob = bytearray()
    minifat = []           # mini sector -> next mini sector (或 ENDOFCHAIN)
    start_mini = {}
    for name in names:
        data = STREAMS[name]
        nsec = (len(data) + MINI - 1) // MINI
        start_mini[name] = len(minifat)
        for i in range(nsec):
            minifat.append(ENDOFCHAIN if i == nsec - 1 else len(minifat) + 1)
        mini_blob += pad(data, MINI)
    n_mini_sectors = len(minifat)
    mini_stream = bytes(mini_blob)

    # ---- sector 布局：0=FAT, 之后目录, 之后 MiniFAT, 之后 mini stream 常规扇区 ----
    dir_entry_count = 1 + len(names)
    dir_sectors = (dir_entry_count * 128 + SECTOR - 1) // SECTOR
    n_minifat_sectors = (n_mini_sectors * 4 + SECTOR - 1) // SECTOR if n_mini_sectors else 0
    ms_padded = pad(mini_stream, SECTOR) if mini_stream else b""
    n_ms_sectors = len(ms_padded) // SECTOR

    fat_sector = 0
    dir_start = 1
    minifat_start = dir_start + dir_sectors
    ms_start = minifat_start + n_minifat_sectors
    total_sectors = ms_start + n_ms_sectors

    # ---- FAT ----
    fat = [FREESECT] * (total_sectors * (SECTOR // 4))
    fat[fat_sector] = FATSECT
    for i in range(dir_sectors):
        fat[dir_start + i] = ENDOFCHAIN if i == dir_sectors - 1 else dir_start + i + 1
    for i in range(n_minifat_sectors):
        fat[minifat_start + i] = ENDOFCHAIN if i == n_minifat_sectors - 1 else minifat_start + i + 1
    for i in range(n_ms_sectors):
        fat[ms_start + i] = ENDOFCHAIN if i == n_ms_sectors - 1 else ms_start + i + 1

    # ---- MiniFAT 扇区 ----
    minifat_bytes = b"".join(struct.pack("<I", v) for v in minifat)
    minifat_bytes = pad(minifat_bytes, SECTOR * n_minifat_sectors) if n_minifat_sectors else b""

    # ---- 目录 ----
    def dir_entry(name, otype, start, size, left=NOSTREAM, right=NOSTREAM, child=NOSTREAM):
        nb = name.encode("utf-16-le") + b"\x00\x00"
        e = nb.ljust(64, b"\x00")
        e += struct.pack("<H", len(nb))
        e += struct.pack("<B", otype)
        e += struct.pack("<B", 1)              # color = black
        e += struct.pack("<I", left)
        e += struct.pack("<I", right)
        e += struct.pack("<I", child)
        e += b"\x00" * 16                      # CLSID
        e += struct.pack("<I", 0)              # state
        e += b"\x00" * 16                      # times
        e += struct.pack("<I", start)          # starting sector (mini or regular)
        e += struct.pack("<Q", size)           # stream size
        assert len(e) == 128
        return e

    d = dir_entry("Root Entry", 5, ms_start, len(mini_stream), child=1)
    for i, name in enumerate(names):
        idx = i + 1
        right = idx + 1 if idx < len(names) else NOSTREAM
        d += dir_entry(name, 2, start_mini[name], len(STREAMS[name]), right=right)
    d = pad(d, SECTOR * dir_sectors)

    # ---- 头 ----
    hdr = bytearray(SECTOR)
    hdr[0:8] = bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1])
    struct.pack_into("<H", hdr, 0x18, 0x003E)          # minor version
    struct.pack_into("<H", hdr, 0x1A, 0x0003)          # major version 3
    struct.pack_into("<H", hdr, 0x1C, 0xFFFE)          # little-endian
    struct.pack_into("<H", hdr, 0x1E, 9)               # sector shift -> 512
    struct.pack_into("<H", hdr, 0x20, 6)               # mini sector shift -> 64
    struct.pack_into("<I", hdr, 0x2C, 1)               # number of FAT sectors
    struct.pack_into("<I", hdr, 0x30, dir_start)       # first directory sector
    struct.pack_into("<I", hdr, 0x38, 4096)            # mini stream cutoff
    struct.pack_into("<I", hdr, 0x3C, minifat_start)   # first MiniFAT sector
    struct.pack_into("<I", hdr, 0x40, n_minifat_sectors)
    struct.pack_into("<I", hdr, 0x44, ENDOFCHAIN)      # first DIFAT sector (none)
    struct.pack_into("<I", hdr, 0x48, 0)               # number of DIFAT sectors
    struct.pack_into("<I", hdr, 0x4C, fat_sector)      # DIFAT[0]

    # ---- 按 sector 写 ----
    sectors = [b"\x00" * SECTOR for _ in range(total_sectors)]
    fat_bytes = pad(b"".join(struct.pack("<I", v) for v in fat[: total_sectors * (SECTOR // 4)]), SECTOR)
    for i in range(len(fat_bytes) // SECTOR):
        sectors[fat_sector + i] = fat_bytes[i * SECTOR:(i + 1) * SECTOR]
    for i in range(dir_sectors):
        sectors[dir_start + i] = d[i * SECTOR:(i + 1) * SECTOR]
    for i in range(n_minifat_sectors):
        sectors[minifat_start + i] = minifat_bytes[i * SECTOR:(i + 1) * SECTOR]
    for i in range(n_ms_sectors):
        sectors[ms_start + i] = ms_padded[i * SECTOR:(i + 1) * SECTOR]

    with open(OUT, "wb") as f:
        f.write(bytes(hdr) + b"".join(sectors))
    print("wrote", OUT, "sectors", total_sectors, "mini_sectors", n_mini_sectors)


if __name__ == "__main__":
    main()
