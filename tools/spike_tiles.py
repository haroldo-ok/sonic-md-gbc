#!/usr/bin/env python3
"""
Spike art for the common sprite slots 64..67 (`SPR_TILE_SPIKES`), built from `artnem/Spikes.nem`.

`36 Spikes.asm` draws every spike layout from one 8x32 spike piece: upright (`Map_Spike` frames 0, 2, 3, 4, art tiles
4..7) or sideways (frames 1, 5, art tiles 0..3), flipped for ceiling / left-facing spikes. At half scale one spike is
4x16, i.e. exactly two 8x8 cells, so four tiles cover every layout:

    64  upright spike, tip half      (cell drawn above 65; Y-flip both for ceiling spikes)
    65  upright spike, base half
    66  sideways spike, tip half     (cell drawn left of 67: unflipped sideways spikes point left, as in the art;
    67  sideways spike, base half     X-flip both and swap them for right-facing spikes)

`objects.c` (`spikes_draw`) places one column / row of cells per spike at the `Map_Spike` piece positions.

Usage: python3 tools/spike_tiles.py     (patches `generated/bank_004_common.c` in place)
"""
from __future__ import annotations

import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compression import decompress_nemesis  # noqa: E402
from gfx_converter import decode_4bpp_tile, encode_2bpp_tile, load_md_palette_bytes, rgb555_to_rgb8  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S1 = os.path.join(ROOT, "vendor", "s1disasm")
FIRST_TILE = 64          # SPR_TILE_SPIKES
COMMON_BASE = 32         # common_sprite_tiles[] starts at OBJ tile 32


def spike_subtype(osub: int, flags_hv: int) -> int:
    """`36 Spikes.asm` subtype + flips, packed losslessly for the engine: bits 0-1 movement (low nybble 0..2),
    bits 2-4 layout (`Spikes_Config` row = high nybble 0..5), bit 6 X flip, bit 7 Y flip. The generic
    `osub | flags << 6` packing used for other objects clobbers the layout nybble (an X-flipped `$10` read back as `$50`)."""
    return (osub & 3) | (((osub >> 4) & 7) << 2) | (flags_hv << 6)


def _shade(pal, idx: np.ndarray) -> int:
    """2bpp shade of a 2x2 block of MD colour indices for the steel OBJ palette (1 dark, 2 mid, 3 white)."""
    lum = [0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] for c in (rgb555_to_rgb8(pal[int(v)]) for v in idx)]
    if max(lum) >= 185:
        return 3
    if min(lum) < 85:
        return 1
    return 2


def _half(art: np.ndarray, pal) -> np.ndarray:
    h, w = art.shape
    out = np.zeros((h // 2, w // 2), np.uint8)
    for y in range(h // 2):
        for x in range(w // 2):
            cell = art[y * 2:y * 2 + 2, x * 2:x * 2 + 2]
            nz = cell[cell > 0]
            if len(nz):                            # any coverage: a 4 px wide spike stays a solid cone at half scale
                out[y, x] = _shade(pal, nz)
    return out


def build(raw: bytes, pal) -> list[bytes]:
    tile = lambda i: decode_4bpp_tile(raw[i * 32:(i + 1) * 32])
    upright = np.vstack([tile(4 + r) for r in range(4)])          # 32 x 8, tip at the top
    sideways = np.hstack([tile(c) for c in range(4)])             # 8 x 32
    up = _half(upright, pal)                                      # 16 x 4
    sd = _half(sideways, pal)                                     # 4 x 16
    col = np.zeros((16, 8), np.uint8)
    col[:, 2:6] = up
    row = np.zeros((8, 16), np.uint8)
    row[2:6, :] = sd
    return [encode_2bpp_tile(col[0:8]), encode_2bpp_tile(col[8:16]),
            encode_2bpp_tile(row[:, 0:8]), encode_2bpp_tile(row[:, 8:16])]


def tiles() -> list[bytes]:
    raw = decompress_nemesis(open(os.path.join(S1, "artnem", "Spikes.nem"), "rb").read())
    pal = load_md_palette_bytes(open(os.path.join(S1, "palette", "Sonic.bin"), "rb").read())
    return build(raw, pal)


def patch_generated():
    path = os.path.join(ROOT, "generated", "bank_004_common.c")
    src = open(path).read()
    m = re.search(r"(const uint8_t common_sprite_tiles\[(\d+)\] = \{)([^}]*)(\})", src)
    data = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(3))]
    assert len(data) == int(m.group(2))
    off = (FIRST_TILE - COMMON_BASE) * 16
    new = b"".join(tiles())
    data[off:off + len(new)] = list(new)
    body = "\n    " + ", ".join(f"0x{b:02X}" for b in data) + "\n"
    open(path, "w").write(src[:m.start(3)] + body + src[m.end(3):])
    print(f"patched {len(new) // 16} spike tiles at OBJ {FIRST_TILE} in {os.path.relpath(path, ROOT)}")


if __name__ == "__main__":
    patch_generated()
