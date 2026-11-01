#!/usr/bin/env python3
"""
Generic zone-prop art generator (half-scale CGB 8x8 sprite cells) for MZ / SYZ / LZ / SLZ / SBZ.

For every zone a list of *groups* is rendered from the authentic Mega Drive art (`Map_*` sprite mappings + the zone's
pattern-load-cue art, `tools/zart.py`), downsampled 2x, cut into 8x8 cells, quantised to the zone's two override OBJ
palettes (6, 7) or the fixed steel palette (5), de-duplicated (H/V flips) and merged into the 28 zone-prop sprite
slots (tiles 40..67 of the zone's sprite bank).  Output:

    generated/zp_<zone>.h   cell tables  `zp_<zone>_<group>[]` (PropCell: dx, dy, tile, attr) + `ZP_<ZONE>_<GROUP>_N`
    generated/zp_pals.h     `g_zp_pal67[9][8]`  OBJ palette 6/7 override for each visual set (vset)
    generated/bank_NNN.c    `vset_N_sprtiles` patched at tiles 40..67

Usage:   python3 tools/zone_props.py [MZ SYZ LZ SLZ SBZ]
"""

from __future__ import annotations
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghz_props as G  # noqa: E402
import zart  # noqa: E402
from gfx_converter import encode_2bpp_tile, rgb8_to_rgb555  # noqa: E402
from md_symbols import S1DisasmROM  # noqa: E402

GEN_DIR = G.GEN_DIR
PROP_BASE, PROP_SLOTS = G.PROP_BASE, G.PROP_SLOTS
STEEL, P6, P7 = 5, 6, 7
FIX1 = [(16, 16, 24), (244, 28, 24), (252, 252, 252)]     # OBJ pal 1: crimson + white
FIX2 = [(108, 52, 0), (252, 204, 0), (252, 252, 252)]    # OBJ pal 2: gold + bronze + white


class Zone:
    def __init__(self, name, vsets, pal_bin, plcs, extra=()):
        self.name, self.vsets = name, vsets
        self.s1 = S1DisasmROM(zart.S1)
        self.img = zart.vram_image(plcs, extra)
        self.pal = zart.pal_lines(pal_bin)
        self.sonic = zart.sonic_line()
        self.groups: dict[str, list] = {}
        self.budget = PROP_SLOTS
        self.meta: dict[str, dict] = {}

    # ---- canvases --------------------------------------------------------------------------------------------
    def line(self, line):
        """`line`: CRAM line the object uses (0 = Sonic's line, 1..3 = zone palette lines 'Tile_Pal2..4')."""
        return self.sonic if line == 0 else self.pal[line - 1]

    def frame(self, sym, idx, n, base, line, pieces=None):
        fr = self.s1.parse_sprite_mapping_table(self.s1.sym(sym), n)[idx]
        if pieces is not None:
            fr = [fr[i] for i in pieces]
        td = {k - base: v for k, v in self.img.items()}
        rgba, x0, y0 = G._render(fr, td, self.line(line))
        return rgba, x0, y0

    @staticmethod
    def crop(c, X0, Y0, X1, Y1):
        rgba, x0, y0 = c
        h, w = rgba.shape[:2]
        ax0, ay0 = max(X0, x0), max(Y0, y0)
        ax1, ay1 = min(X1, x0 + w), min(Y1, y0 + h)
        out = np.zeros((Y1 - Y0, X1 - X0, 4), np.float32)
        if ax1 > ax0 and ay1 > ay0:
            out[ay0 - Y0:ay1 - Y0, ax0 - X0:ax1 - X0] = rgba[ay0 - y0:ay1 - y0, ax0 - x0:ax1 - x0]
        return out, X0, Y0

    @staticmethod
    def stack(parts):
        """Paint several (rgba, x0, y0) canvases onto one (later parts on top)."""
        X0 = min(p[1] for p in parts); Y0 = min(p[2] for p in parts)
        X1 = max(p[1] + p[0].shape[1] for p in parts); Y1 = max(p[2] + p[0].shape[0] for p in parts)
        out = np.zeros((Y1 - Y0, X1 - X0, 4), np.float32)
        for rgba, x0, y0 in parts:
            m = rgba[:, :, 3] > 0
            sub = out[y0 - Y0:y0 - Y0 + rgba.shape[0], x0 - X0:x0 - X0 + rgba.shape[1]]
            sub[m] = rgba[m]
        return out, X0, Y0

    def add(self, name, canvas, pals=(P6, P7), cap=None, order="rows"):
        rgba, x0, y0 = canvas
        assert x0 % 2 == 0 and y0 % 2 == 0, (name, x0, y0)
        cells = G._cut_cells(G._half(rgba), x0 // 2, y0 // 2)
        cells.sort(key=lambda c: (c[1], c[0]))
        if cap is not None and len(cells) > cap:
            raise SystemExit(f"{self.name}.{name}: {len(cells)} cells > cap {cap}")
        self.groups[name] = [(dx, dy, t, tuple(pals), 0) for dx, dy, t in cells]
        return len(cells)


# ----------------------------------------------------------------------------------------------------------------
def _assign_and_pack(Z: Zone):
    groups = list(Z.groups)
    all_cells = [(g, i) for g in groups for i in range(len(Z.groups[g]))]
    # palette candidates: pal 6 / 7 get colours by k-means; steel (5) is fixed.
    assign = {}
    flex = []
    for g, i in all_cells:
        t, pals = Z.groups[g][i][2], Z.groups[g][i][3]
        assign[(g, i)] = pals[0]
        if len(pals) > 1:
            flex.append((g, i))
    # split the flexible cells into two colour families (2-means on the cell's mean colour, 12 restarts)
    if flex:
        feat = np.array([Z.groups[g][i][2][Z.groups[g][i][2][:, :, 3] > 0][:, :3].mean(axis=0) for g, i in flex], np.float32)
        best = None
        rng = np.random.default_rng(1)
        for _ in range(12):
            cen = feat[rng.choice(len(feat), 2, replace=len(feat) < 2)]
            for _ in range(20):
                lab = ((feat[:, None, :] - cen[None]) ** 2).sum(axis=2).argmin(axis=1)
                for k in range(2):
                    if (lab == k).any():
                        cen[k] = feat[lab == k].mean(axis=0)
            cost = float(((feat - cen[lab]) ** 2).sum())
            if best is None or cost < best[0]:
                best = (cost, lab.copy())
        for (g, i), l in zip(flex, best[1]):
            pals = Z.groups[g][i][3]
            assign[(g, i)] = P6 if l == 0 else P7
    pal_rgb = {STEEL: G.STEEL_RGB, 1: FIX1, 2: FIX2}
    for rounds in range(4):
        for pal in (P6, P7):
            px = []
            for (g, i), p in assign.items():
                if p == pal:
                    t = Z.groups[g][i][2]
                    px.append(t[t[:, :, 3] > 0][:, :3])
            pal_rgb[pal] = G._kmeans3(np.vstack(px) if px else np.zeros((0, 3)))
        for g, i in all_cells:
            t, pals = Z.groups[g][i][2], Z.groups[g][i][3]
            if len(pals) > 1:
                assign[(g, i)] = min(pals, key=lambda p: G._err(t, pal_rgb[p]))
    uniq: list[tuple[int, np.ndarray]] = []
    index: dict = {}
    refs: dict[str, list] = {}
    for g in groups:
        lst = []
        for i, (dx, dy, t, pals, pidx) in enumerate(Z.groups[g]):
            pal = assign[(g, i)]
            q = G._quant(t, pal_rgb[pal])
            hit = None
            for v, fh, fv in G._flips(q):
                k = (pal, np.ascontiguousarray(v).tobytes())
                if k in index:
                    tid, th, tv = index[k]
                    hit = (tid, fh ^ th, fv ^ tv)
                    break
            if hit is None:
                tid = len(uniq)
                uniq.append((pal, q))
                for v, fh, fv in G._flips(q):
                    index.setdefault((pal, np.ascontiguousarray(v).tobytes()), (tid, fh, fv))
                hit = (tid, 0, 0)
            lst.append([dx, dy, hit[0], hit[1], hit[2], pidx, pal])
        refs[g] = lst
    print(f"  {Z.name}: {len(uniq)} unique tiles before merge (budget {Z.budget})", file=sys.stderr)
    alive = set(range(len(uniq)))
    while len(alive) > Z.budget:
        use = {t: 0 for t in alive}
        for lst in refs.values():
            for r in lst:
                use[r[2]] += 1
        best = None
        for a_ in alive:
            for b_ in alive:
                if a_ == b_ or uniq[a_][0] != uniq[b_][0]:
                    continue
                for v, fh, fv in G._flips(uniq[a_][1]):
                    d = int((np.asarray(v) != uniq[b_][1]).sum()) + int(((np.asarray(v) == 0) != (uniq[b_][1] == 0)).sum())
                    cost = d * use[a_]
                    if best is None or cost < best[0]:
                        best = (cost, a_, b_, fh, fv, d)
        _, a_, b_, fh, fv, d = best
        for lst in refs.values():
            for r in lst:
                if r[2] == a_:
                    r[2], r[3], r[4] = b_, r[3] ^ fh, r[4] ^ fv
        alive.discard(a_)
    order = sorted(alive)
    remap = {t: n for n, t in enumerate(order)}
    uniq = [uniq[t] for t in order]
    out = {g: [(r[0], r[1], PROP_BASE + remap[r[2]], 0x08 | r[6] | (0x20 if r[3] else 0) | (0x40 if r[4] else 0))
               for r in lst] for g, lst in refs.items()}
    tiles = b"".join(encode_2bpp_tile(q) for _, q in uniq).ljust(PROP_SLOTS * 16, b"\x00")
    print(f"  {Z.name}: {len(uniq)} tiles used", file=sys.stderr)
    return out, tiles, uniq, pal_rgb


def _c565(rgb):
    return rgb8_to_rgb555(*rgb)


def _patch_vset(vset: int, tiles: bytes):
    path = os.path.join(GEN_DIR, f"bank_{47 + 2 * vset:03d}.c")
    src = open(path).read()
    m = re.search(r"(const uint8_t vset_%d_sprtiles\[2048\] = \{)(.*?)(\n\};)" % vset, src, re.S)
    assert m, (vset, path)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
    assert len(vals) == 2048
    vals[PROP_BASE * 16:(PROP_BASE + PROP_SLOTS) * 16] = list(tiles)
    lines = ["    " + ", ".join(f"0x{v:02X}" for v in vals[i:i + 24]) + "," for i in range(0, 2048, 24)]
    open(path, "w").write(src[:m.start(2)] + "\n" + "\n".join(lines) + src[m.end(2):])


def _preview(Z: Zone, out, uniq, pal_rgb, path):
    from PIL import Image
    cols = {k: pal_rgb[k] for k in (1, 2, STEEL, P6, P7)}
    tiles = {PROP_BASE + n: (pal, q) for n, (pal, q) in enumerate(uniq)}
    rows = []
    for g, lst in out.items():
        xs = [c[0] for c in lst]; ys = [c[1] for c in lst]
        w, h = max(xs) - min(xs) + 8, max(ys) - min(ys) + 8
        im = Image.new("RGB", (w * 3, h * 3), (60, 90, 170))
        for dx, dy, tl, at in lst:
            pal, q = tiles[tl]
            fh, fv = bool(at & 0x20), bool(at & 0x40)
            palc = [None] + cols[at & 7]
            for y in range(8):
                for x in range(8):
                    v = q[7 - y if fv else y, 7 - x if fh else x]
                    if v:
                        for yy in range(3):
                            for xx in range(3):
                                im.putpixel(((dx - min(xs) + x) * 3 + xx, (dy - min(ys) + y) * 3 + yy), palc[v])
        rows.append(im)
    W = max(i.width for i in rows) + 8
    H = sum(i.height + 6 for i in rows)
    sheet = Image.new("RGB", (W, H), (30, 30, 30))
    y = 0
    for im in rows:
        sheet.paste(im, (0, y)); y += im.height + 6
    sheet.save(path)


def emit_zone(Z: Zone, out, uniq, pal_rgb):
    name = Z.name.lower()
    L = [f"/* GENERATED by tools/zone_props.py - {Z.name} prop cells (dx, dy, tile, attr). Do not edit. */",
         f"#ifndef ZP_{Z.name}_H", f"#define ZP_{Z.name}_H", ""]
    for g, lst in out.items():
        L.append(f"#define ZP_{Z.name}_{g.upper()}_N {len(lst)}")
        L.append(f"static const PropCell zp_{name}_{g}[{len(lst)}] = {{")
        for dx, dy, tl, at in lst:
            L.append(f"    {{ {dx}, {dy}, {tl}, 0x{at:02X} }},")
        L.append("};")
    L += ["", "#endif", ""]
    open(os.path.join(GEN_DIR, f"zp_{name}.h"), "w").write("\n".join(L))
    return [0x0000] + [_c565(c) for c in pal_rgb[P6]] + [0x0000] + [_c565(c) for c in pal_rgb[P7]]


def run(zones: list[Zone]):
    pals = {}
    for Z in zones:
        out, tiles, uniq, pal_rgb = _assign_and_pack(Z)
        p = emit_zone(Z, out, uniq, pal_rgb)
        for v in Z.vsets:
            _patch_vset(v, tiles)
            pals[v] = p
        _preview(Z, out, uniq, pal_rgb, os.path.join(os.environ.get("ZP_PREVIEW_DIR", "/tmp"), f"zp_{Z.name.lower()}.png"))
    return pals


def write_pals(pals: dict):
    path = os.path.join(GEN_DIR, "zp_pals.json")
    import json
    old = {}
    if os.path.exists(path):
        old = {int(k): v for k, v in json.load(open(path)).items()}
    old.update(pals)
    json.dump({str(k): v for k, v in sorted(old.items())}, open(path, "w"))
    L = ["/* GENERATED by tools/zone_props.py - OBJ palette 6/7 override per visual set (vset 0 = GHZ uses ghz_pals.h) */",
         "#ifndef ZP_PALS_H", "#define ZP_PALS_H", "",
         "static const uint16_t g_zp_pal67[9][8] = {"]
    ghz = [0x0000, 0, 0, 0, 0x0000, 0, 0, 0]
    for v in range(9):
        row = old.get(v, ghz)
        L.append("    { " + ", ".join(f"0x{int(x):04X}" for x in row) + " },")
    L += ["};", "", "#endif", ""]
    open(os.path.join(GEN_DIR, "zp_pals.h"), "w").write("\n".join(L))


# ================================================================================================================
# Zone specs
# ================================================================================================================
def zone_mz() -> Zone:
    Z = Zone("MZ", [2], "Marble Zone.bin", ["PLC_MZ", "PLC_MZ2"])
    c = zart.const
    BL, STO, GL, FIRE, LAVA, BUT = c("ArtTile_MZ_Block"), c("ArtTile_MZ_Spike_Stomper"), c("ArtTile_MZ_Glass_Pillar"), \
        c("ArtTile_MZ_Fireball"), c("ArtTile_MZ_Lava"), c("ArtTile_Button_Main")
    F = Z.frame
    # 0x2F large grassy platforms (level art, Tile_Pal3): top two cell rows of each frame
    for i, ty in enumerate((-48, -64, -48)):
        Z.add(f"grass{i}", Z.crop(F("Map_LGrass", i, 3, 0, 2), -64, ty, 64, ty + 32), (P6, P7))
    Z.add("brick", F("Map_Brick", 0, 1, 0, 2), (P6, P7))                    # 0x46 purple brick 32x32
    Z.add("smab", F("Map_Smab", 0, 3, BL, 2), (P6, P7))                     # 0x51 smashable green block
    Z.add("push", F("Map_Push", 0, 2, BL, 2), (P6, P7))                     # 0x33 pushable block
    Z.add("mblk", F("Map_MBlock", 0, 4, BL, 2), (P6, P7))                   # 0x52 moving block (16x16 half)
    Z.add("stblk", Z.crop(F("Map_CStom", 0, 4, STO, 0), -56, -20, 56, 12), (STEEL,))      # 0x31 block
    Z.add("stspk", Z.crop(F("Map_CStom", 1, 4, STO, 0), -44, -16, 44, 16), (STEEL,))      # spikes
    Z.add("stchn", F("Map_CStom", 3, 4, STO, 0), (STEEL,))                                # chain link
    Z.add("but0", Z.crop(F("Map_But", 0, 2, BUT, 2), -16, -12, 16, 4), (STEEL,))
    Z.add("but1", Z.crop(F("Map_But", 1, 2, BUT, 2), -16, -12, 16, 4), (STEEL,))
    Z.add("fire0", F("Map_Fire", 0, 8, FIRE, 0), (2,))
    Z.add("fire1", F("Map_Fire", 1, 8, FIRE, 0), (2,))
    Z.add("fireh", F("Map_Fire", 3, 8, FIRE, 0), (2,))
    Z.add("gyhead", Z.crop(F("Map_Geyser", 0, 12, LAVA, 3), -24, -20, 24, 12), (2,))
    Z.add("gycol", Z.crop(F("Map_Geyser", 8, 12, LAVA, 3), -32, -112, 0, -96), (2,))     # 16 px lava column cell
    Z.add("glcap", Z.crop(F("Map_Glass", 0, 3, GL, 2), -32, -72, 32, -40), (P6, P7))
    Z.add("glmid", Z.crop(F("Map_Glass", 0, 3, GL, 2), -32, -40, 32, -24), (P6, P7))
    return Z


def zone_syz() -> Zone:
    Z = Zone("SYZ", [4], "Spring Yard Zone.bin", ["PLC_SYZ", "PLC_SYZ2"])
    c = zart.const
    F = Z.frame
    Z.add("blk0", F("Map_FBlock", 0, 8, 0, 2), (P6, P7))                                  # 32x32 block (also SLZ stair tile)
    Z.add("blk1", F("Map_FBlock", 1, 8, 0, 2), (P6, P7))                                  # 64x64 up/down block
    Z.add("door", Z.crop(F("Map_FBlock", 2, 8, 0, 2), -16, -32, 16, 0), (P6, P7))         # 32x64 door: top half (drawn twice)
    Z.add("plat", Z.crop(F("Map_Plat_SYZ", 0, 3, 0, 2), -32, -10, 32, 22), (P6, P7))                  # 0x18 SYZ platform 64x32
    Z.add("bump0", F("Map_Bump", 0, 3, c("ArtTile_SYZ_Bumper"), 0), (1,))
    Z.add("bump1", F("Map_Bump", 1, 3, c("ArtTile_SYZ_Bumper"), 0), (1,))
    Z.add("bball", F("Map_BBall", 0, 2, c("ArtTile_SYZ_Big_Spikeball"), 0), (STEEL,))
    Z.add("sball", F("Map_SBall", 0, 2, c("ArtTile_SYZ_Spikeball_Chain"), 0), (STEEL,))
    Z.add("but0", Z.crop(F("Map_But", 0, 2, c("ArtTile_Button_Main"), 0), -16, -12, 16, 4), (STEEL,))
    Z.add("but1", Z.crop(F("Map_But", 1, 2, c("ArtTile_Button_Main"), 0), -16, -12, 16, 4), (STEEL,))
    return Z


def zone_slz() -> Zone:
    Z = Zone("SLZ", [3], "Star Light Zone.bin", ["PLC_SLZ", "PLC_SLZ2"])
    c = zart.const
    F = Z.frame
    Z.add("plat", F("Map_Plat_SLZ", 0, 1, 0, 2), (P6, P7))                       # 0x18 platform (64x32)
    Z.add("circ", F("Map_Circ", 0, 1, 0, 2), (P6, P7))                           # 0x5A circling platform (48x16)
    Z.add("elev", F("Map_Elev", 0, 1, 0, 2), (P6, P7))                           # 0x59 elevator (80x32)
    Z.add("stair", F("Map_Stair", 0, 1, 0, 2), (P6, P7))                         # 0x5B stair block / 0x56 block (32x32)
    Z.add("see0", Z.crop(F("Map_Seesaw", 0, 4, c("ArtTile_SLZ_Seesaw"), 0), -46, -44, 50, 12), (STEEL,))   # seesaw tilted (descending to the left)
    Z.add("see1", Z.crop(F("Map_Seesaw", 1, 4, c("ArtTile_SLZ_Seesaw"), 0), -48, -26, 48, -2), (STEEL,))   # seesaw flat
    Z.add("ball", F("Map_SSawBall", 1, 2, c("ArtTile_SLZ_Spikeball"), 0), (STEEL,))
    Z.add("fan0", F("Map_Fan", 0, 4, c("ArtTile_SLZ_Fan"), 2), (STEEL,))
    Z.add("fan1", F("Map_Fan", 1, 4, c("ArtTile_SLZ_Fan"), 2), (STEEL,))
    Z.add("cflo", F("Map_CFlo", 0, 3, c("ArtTile_SLZ_Collapsing_Floor"), 2), (P6, P7))
    Z.add("fire0", F("Map_Fire", 0, 8, c("ArtTile_SLZ_Fireball"), 0), (2,))
    Z.add("fireh", F("Map_Fire", 3, 8, c("ArtTile_SLZ_Fireball"), 0), (2,))
    Z.add("smash", F("Map_Smash", 0, 3, 0, 2), (P6, P7))
    return Z


def zone_lz() -> Zone:
    Z = Zone("LZ", [1], "Labyrinth Zone.bin", ["PLC_LZ", "PLC_LZ2"])
    c = zart.const
    F = Z.frame
    LB, HP, DR, MB, SB = c("ArtTile_LZ_Blocks"), c("ArtTile_LZ_Harpoon"), c("ArtTile_LZ_Door"), c("ArtTile_LZ_Moving_Block"), c("ArtTile_LZ_Spikeball_Chain")
    Z.add("lblk0", F("Map_LBlock", 0, 4, LB, 2), (P6, P7))                  # 0x61 block that sinks
    Z.add("lblk1", F("Map_LBlock", 1, 4, LB, 2), (P6, P7))                  # rising platform 64x24
    Z.add("lblk2", F("Map_LBlock", 2, 4, LB, 2), (P6, P7))                  # cork block
    Z.add("door6", F("Map_FBlock", 6, 8, DR, 2), (P6, P7))                  # 0x56 vertical door 16x64
    Z.add("door7", F("Map_FBlock", 7, 8, DR, 2), (P6, P7))                  # horizontal door 128x32
    Z.add("raft", F("Map_MBlockLZ", 0, 1, MB, 2), (P6, P7))                 # 0x52 raft 32x16
    Z.add("harph", F("Map_Harp", 2, 6, HP, 0), (STEEL,))                    # 0x16 harpoon extended (sideways)
    Z.add("harpv", F("Map_Harp", 5, 6, HP, 0), (STEEL,))                    # extended upright
    Z.add("ball", F("Map_SBall2", 1, 3, SB, 0), (STEEL,))                   # 0x57 chain tip (big ball)
    Z.add("link", F("Map_SBall2", 0, 3, SB, 0), (STEEL,))                   # chain link
    Z.add("but0", Z.crop(F("Map_But", 0, 2, c("ArtTile_Button_Main"), 0), -16, -12, 16, 4), (STEEL,))
    Z.add("but1", Z.crop(F("Map_But", 1, 2, c("ArtTile_Button_Main"), 0), -16, -12, 16, 4), (STEEL,))
    return Z


ZONES = {"MZ": zone_mz, "SYZ": zone_syz, "SLZ": zone_slz, "LZ": zone_lz}

if __name__ == "__main__":
    names = sys.argv[1:] or list(ZONES)
    zs = [ZONES[n]() for n in names]
    pals = run(zs)
    write_pals(pals)
    print("done", names)
