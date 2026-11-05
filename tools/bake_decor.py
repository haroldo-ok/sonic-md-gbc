#!/usr/bin/env python3
"""
Bake the purely decorative, static Sonic 1 sprites into the level background (run after `convert_all.py`).

Objects baked (they never move or animate, so drawing them as OAM sprites every frame only costs CPU / OAM slots):
    GHZ 0x1C  bridge stump            (`1C GHZ, SYZ Scenery.asm`, Map_Bri frame 1)
    GHZ 0x44  edge wall art           (`44 GHZ Edge Walls.asm`, Map_Edge frames 0..2; solid walls keep their object
                                       for collision, flagged with subtype bit 5 = "art baked")
    SLZ 0x1C  lava-thrower body       (Map_Scen frame 0)

For every act of the zone the decoration is rendered from the original art, placed at its object position,
composited over the half-scale terrain cell it covers (honouring high-priority terrain), quantised to the best
zone BG palette and de-duplicated.  New tiles go into the zone's unused BG tile slots; each covered map cell gets an
overlay entry `{block_x, block_y, tile, attr}` that `enqueue_world_span()` (src/main.c) applies while streaming.

Outputs:
    generated/decor.c / decor.h      per-act overlay tables (bank DECOR_BANK)
    generated/bank_NNN.c             `vset_N_bgtiles0/1` patched with the new tiles, `act_N_obj` with baked objects
                                     removed (stumps / non-solid walls) or flagged (solid walls)
"""
from __future__ import annotations

import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghz_props as G  # noqa: E402
import zart  # noqa: E402
import zone_props as ZP  # noqa: E402
from compression import decompress_kosinski  # noqa: E402
from gfx_converter import BG_VRAM_SLOTS, encode_2bpp_tile, rgb555_to_rgb8  # noqa: E402

GEN = G.GEN_DIR
S1 = zart.S1
DECOR_BANK = 125
CYCLE_SLOT = {0: 1, 3: 1}          # BG palette whose colour 3 is cycled at runtime (`hw_step_palette_cycle`)

# vset -> (zone ctor args, kos file, [(act id, fg layout, objpos file)])
ZONES = {
    0: (("GHZ", [0], "Green Hill Zone.bin", ["PLC_GHZ", "PLC_GHZ2"]), "GHZ.kos",
        [(0, "ghz1.bin", "ghz1.bin"), (1, "ghz2.bin", "ghz2.bin"), (2, "ghz3.bin", "ghz3 (REV01).bin")]),
    3: (("SLZ", [3], "Star Light Zone.bin", ["PLC_SLZ", "PLC_SLZ2"]), "SLZ.kos",
        [(12, "slz1.bin", "slz1.bin"), (13, "slz2.bin", "slz2.bin"), (14, "slz3.bin", "slz3.bin")]),
}


# ---- generated C helpers -----------------------------------------------------------------------------------------
def _find_array(name):
    for fn in sorted(os.listdir(GEN)):
        if not (fn.startswith("bank_") and fn.endswith(".c")):
            continue
        path = os.path.join(GEN, fn)
        src = open(path).read()
        m = re.search(r"(const uint8_t %s\[(\d+)\] = \{)(.*?)(\n\};)" % re.escape(name), src, re.S)
        if m:
            vals = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(3)))
            assert len(vals) == int(m.group(2)), name
            return path, vals
    raise SystemExit(f"array {name} not found")


def _write_array(name, data):
    path, old = _find_array(name)
    assert len(old) == len(data)
    src = open(path).read()
    m = re.search(r"(const uint8_t %s\[(\d+)\] = \{)(.*?)(\n\};)" % re.escape(name), src, re.S)
    lines = ["    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 24]) + "," for i in range(0, len(data), 24)]
    open(path, "w").write(src[:m.start(3)] + "\n" + "\n".join(lines) + src[m.end(3):])


def _vset_pals(vset):
    src = open(os.path.join(GEN, "bank_008_tables.c")).read()
    m = re.search(r"\{\s*\d+,\s*vset_%d_bgtiles0,.*?\{([^}]*)\}" % vset, src)
    words = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1))]
    return [[rgb555_to_rgb8(words[p * 4 + i]) for i in range(4)] for p in range(8)]


def _decode_2bpp(b):
    out = np.zeros((8, 8), np.uint8)
    for y in range(8):
        lo, hi = b[y * 2], b[y * 2 + 1]
        for x in range(8):
            out[y, x] = ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1)
    return out


SLOT_OF = {v: i for i, v in enumerate(BG_VRAM_SLOTS)}


# ---- decoration art ------------------------------------------------------------------------------------------------
def _decor_canvas(Z, vset, oid, sub):
    """(rgba MD-res, x0, y0, xflip) for a decorative object, or None when it is not baked."""
    xf = bool(sub & 0x40)
    if vset == 0 and oid == 0x1C and (sub & 0x0F) == 3:
        c = Z.frame("Map_Bri", 1, 3, zart.const("ArtTile_GHZ_Bridge"), 2)
    elif vset == 0 and oid == 0x44:
        fr = sub & 0x0F
        c = Z.frame("Map_Edge", min(fr, 2), 3, zart.const("ArtTile_GHZ_Edge_Wall"), 2)
    elif vset == 3 and oid == 0x1C and (sub & 0x0F) <= 2:
        c = Z.frame("Map_Scen", 0, 1, zart.const("ArtTile_SLZ_Fireball_Launcher"), 2)
    else:
        return None
    rgba, x0, y0 = c
    if xf:
        rgba = rgba[:, ::-1]
        x0 = -(x0 + rgba.shape[1])
    return rgba, x0, y0


def _objs(objfile):
    raw = open(os.path.join(S1, "objpos", objfile), "rb").read()
    out = []
    for i in range(0, len(raw) - 5, 6):
        ox = (raw[i] << 8) | raw[i + 1]
        if ox == 0xFFFF:
            break
        oy_raw = (raw[i + 2] << 8) | raw[i + 3]
        out.append((ox, oy_raw & 0x0FFF, raw[i + 4] & 0x7F, raw[i + 5] | (((oy_raw >> 14) & 3) << 6)))
    return out


# ---- main ----------------------------------------------------------------------------------------------------------
def bake():
    tables = {}
    report = []
    for vset, (zargs, kos, acts) in ZONES.items():
        Z = ZP.Zone(*zargs)
        _, t0 = _find_array(f"vset_{vset}_bgtiles0")
        _, t1 = _find_array(f"vset_{vset}_bgtiles1")
        slots = [t0[i * 16:(i + 1) * 16] for i in range(256)] + [t1[i * 16:(i + 1) * 16] for i in range(208)]
        _, m16 = _find_array(f"vset_{vset}_map16")
        pals = np.array(_vset_pals(vset), np.float32)
        free = [i for i in range(1, 448) if slots[i] == bytes(16)]
        chunks = decompress_kosinski(open(os.path.join(S1, "map256", kos), "rb").read())
        n_m16 = len(m16) // 2
        new_tiles = {}           # (2bpp bytes) -> slot
        cyc = CYCLE_SLOT.get(vset)

        def cell_pixels(word):
            """Half-scale RGB 8x8 of the terrain cell (block word), plus its 'high priority' flag."""
            bi = word & 0x3FF
            if bi == 0 or bi >= n_m16:
                return np.tile(pals[0][0], (8, 8, 1)), False, np.ones((8, 8), bool)
            tb, at = m16[bi * 2], m16[bi * 2 + 1]
            slot = SLOT_OF[(1 if at & 0x08 else 0, tb)]
            idx = _decode_2bpp(slots[slot])
            hf = bool(at & 0x20) ^ bool(word & 0x0800)
            vf = bool(at & 0x40) ^ bool(word & 0x1000)
            if hf:
                idx = idx[:, ::-1]
            if vf:
                idx = idx[::-1, :]
            return pals[at & 7][idx], bool(at & 0x80), idx == 0

        for aid, fg_file, obj_file in acts:
            fg = open(os.path.join(S1, "levels", fg_file), "rb").read()
            fw, fh = fg[0] + 1, fg[1] + 1
            grid = fg[2:2 + fw * fh]

            def block_word(bx, by):
                cx, cy = bx >> 4, by >> 4
                if bx < 0 or by < 0 or cx >= fw or cy >= fh:
                    return 0
                cid = grid[cy * fw + cx] & 0x7F
                if cid == 0 or (cid - 1) * 512 + 512 > len(chunks):
                    return 0
                off = (cid - 1) * 512 + (((by & 15) << 4) | (bx & 15)) * 2
                return (chunks[off] << 8) | chunks[off + 1]

            cells = {}                 # (bx, by) -> rgba accumulation (8x8x3 composite)
            baked, skipped = set(), 0
            for k, (ox, oy, oid, sub) in enumerate(_objs(obj_file)):
                c = _decor_canvas(Z, vset, oid, sub)
                if c is None:
                    continue
                rgba, x0, y0 = c
                wx0, wy0 = ox + x0, oy + y0
                bx0, by0 = wx0 >> 4, wy0 >> 4
                bx1, by1 = (wx0 + rgba.shape[1] + 15) >> 4, (wy0 + rgba.shape[0] + 15) >> 4
                canvas = np.zeros(((by1 - by0) * 16, (bx1 - bx0) * 16, 4), np.float32)
                py, px = wy0 - by0 * 16, wx0 - bx0 * 16
                canvas[py:py + rgba.shape[0], px:px + rgba.shape[1]] = rgba
                half = G._half(canvas)
                pending = {}
                for cy in range(by1 - by0):
                    for cx in range(bx1 - bx0):
                        d = half[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
                        m = d[:, :, 3] > 0
                        if not m.any():
                            continue
                        key = (bx0 + cx, by0 + cy)
                        if key in cells:
                            base, prio, transp = cells[key]
                        else:
                            base, prio, transp = cell_pixels(block_word(*key))
                            base = base.copy()
                        draw = m & (transp if prio else True)
                        base[draw] = d[:, :, :3][draw]
                        pending[key] = (base, prio, transp)
                # quantise this object's cells; abort (keep it a sprite) if the free slots run out
                trial = dict(new_tiles)
                out = {}
                ok = True
                for key, (base, prio, transp) in pending.items():
                    best = None
                    for p in range(7):
                        pp = pals[p]
                        d2 = ((base[:, :, None, :] - pp[None, None, :, :]) ** 2).sum(axis=3)
                        if p == cyc:
                            d2[:, :, 3] += 1e9
                        q = d2.argmin(axis=2)
                        err = float(d2.min(axis=2).sum())
                        if best is None or err < best[0]:
                            best = (err, p, q.astype(np.uint8))
                    _, p, q = best
                    tb = encode_2bpp_tile(q)
                    if tb not in trial:
                        if len(trial) >= len(free):
                            ok = False
                            break
                        trial[tb] = free[len(trial)]
                    slot = trial[tb]
                    vb, tbyte = BG_VRAM_SLOTS[slot]
                    out[key] = (tbyte, p | (0x08 if vb else 0))
                if not ok:
                    skipped += 1
                    continue
                new_tiles = trial
                for key, v in out.items():
                    cells[key] = pending[key]
                    tables.setdefault(aid, {})[key] = v
                baked.add(k)
            # patch the act's object list: drop pure decorations, flag baked solid walls
            name = f"act_{aid}_obj"
            _, ob = _find_array(name)
            ob = bytearray(ob)
            src_objs = _objs(obj_file)
            baked_pos = {(src_objs[k][0], src_objs[k][1], src_objs[k][2]) for k in baked}
            ents = []
            for i in range(0, len(ob) - 5, 6):
                x, y, oid, sub = struct.unpack_from("<HHBB", ob, i)
                if x == 0xFFFF:
                    break
                if (x, y, oid) in baked_pos:
                    if oid == 0x44 and not (sub & 0x10):
                        ents.append((x, y, oid, sub | 0x20))
                    continue
                ents.append((x, y, oid, sub))
            nb = bytearray()
            for e in ents:
                nb += struct.pack("<HHBB", *e)
            nb += b"\xFF" * (len(ob) - len(nb))
            _write_array(name, bytes(nb))
            report.append(f"act {aid:2d}: baked {len(baked)} objects into {len(tables.get(aid, {}))} cells"
                          + (f", {skipped} kept as sprites (no free BG tiles)" if skipped else ""))
        # write the new tiles into the free slots
        for tb, slot in new_tiles.items():
            slots[slot] = tb
        _write_array(f"vset_{vset}_bgtiles0", b"".join(slots[:256]))
        _write_array(f"vset_{vset}_bgtiles1", b"".join(slots[256:464]))
        report.append(f"vset {vset}: {len(new_tiles)} new BG tiles ({len(free)} free slots)")

    # ---- overlay tables ----
    h = ["/* GENERATED by tools/bake_decor.py - baked decoration overlay cells. Do not edit. */",
         "#ifndef DECOR_H", "#define DECOR_H", "#include <stdint.h>",
         f"#define DECOR_BANK {DECOR_BANK}",
         "typedef struct { uint16_t bx; uint16_t by; uint8_t tile; uint8_t attr; } DecorCell;",
         "extern const DecorCell * const g_decor_tab[20];     /* sorted by (bx, by): column spans */",
         "extern const DecorCell * const g_decor_rtab[20];    /* same cells sorted by (by, bx): row spans */",
         "extern const uint16_t g_decor_n[20];",
         "extern const uint8_t * const g_decor_colbits[20];  /* bit bx set: some cell in column bx */",
         "extern const uint8_t * const g_decor_rowbits[20];  /* bit by set: some cell in row by */",
         "extern const uint16_t g_decor_colbytes[20];",
         "extern const uint8_t g_decor_rowbytes[20];", "#endif", ""]
    c = ["/* GENERATED by tools/bake_decor.py - baked decoration overlay cells. Do not edit. */",
         f"#pragma bank {DECOR_BANK}", '#include "decor.h"', ""]
    for aid in sorted(tables):
        ents = sorted(tables[aid].items())
        for tag, order in (("", ents), ("r", sorted(ents, key=lambda e: (e[0][1], e[0][0])))):
            c.append(f"static const DecorCell s_decor{tag}_{aid}[{len(ents)}] = {{")
            for (bx, by), (t, a) in order:
                c.append(f"    {{ {bx}, {by}, {t}, 0x{a:02X} }},")
            c.append("};")
    colbytes, rowbytes = {}, {}
    for aid in sorted(tables):
        keys = tables[aid].keys()
        nc = (max(k[0] for k in keys) >> 3) + 1
        nr = (max(k[1] for k in keys) >> 3) + 1
        cb, rb = [0] * nc, [0] * nr
        for bx, by in keys:
            cb[bx >> 3] |= 1 << (bx & 7)
            rb[by >> 3] |= 1 << (by & 7)
        colbytes[aid], rowbytes[aid] = nc, nr
        c.append(f"static const uint8_t s_decorcb_{aid}[{nc}] = {{ " + ", ".join(f"0x{v:02X}" for v in cb) + " };")
        c.append(f"static const uint8_t s_decorrb_{aid}[{nr}] = {{ " + ", ".join(f"0x{v:02X}" for v in rb) + " };")
    c.append("const uint8_t * const g_decor_colbits[20] = { " +
             ", ".join(f"s_decorcb_{a}" if a in tables else "0" for a in range(20)) + " };")
    c.append("const uint8_t * const g_decor_rowbits[20] = { " +
             ", ".join(f"s_decorrb_{a}" if a in tables else "0" for a in range(20)) + " };")
    c.append("const uint16_t g_decor_colbytes[20] = { " + ", ".join(str(colbytes.get(a, 0)) for a in range(20)) + " };")
    c.append("const uint8_t g_decor_rowbytes[20] = { " + ", ".join(str(rowbytes.get(a, 0)) for a in range(20)) + " };")
    c.append("const DecorCell * const g_decor_tab[20] = { " +
             ", ".join(f"s_decor_{a}" if a in tables else "0" for a in range(20)) + " };")
    c.append("const DecorCell * const g_decor_rtab[20] = { " +
             ", ".join(f"s_decorr_{a}" if a in tables else "0" for a in range(20)) + " };")
    c.append("const uint16_t g_decor_n[20] = { " +
             ", ".join(str(len(tables.get(a, {}))) for a in range(20)) + " };")
    open(os.path.join(GEN, "decor.h"), "w").write("\n".join(h))
    open(os.path.join(GEN, "decor.c"), "w").write("\n".join(c) + "\n")
    print("\n".join(report))


if __name__ == "__main__":
    bake()
