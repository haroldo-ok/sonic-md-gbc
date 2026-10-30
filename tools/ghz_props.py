#!/usr/bin/env python3
"""
Green Hill Zone interactive-prop art + data generator (half-scale, CGB 8x8 sprites).

Renders the authentic Mega Drive art of the GHZ bridge log / stump, platforms (small + large column),
collapsing ledge (static + 25 fragment pieces), purple rock and smashable wall (3 variants x 8 fragments)
straight from vendor/s1disasm (`Map_Bri`, `Map_Plat_GHZ`, `Map_Ledge`, `Map_PRock`, `Map_Smash`),
downsamples 2x, quantises to the global CGB object palettes (5 = steel/rock, 6 = wood/dirt, 7 = grass),
de-duplicates 8x8 tiles (including H/V flips) into the 28 zone-prop sprite slots (tiles 40..67 of the GHZ
zone sprite bank) and emits `generated/ghz_props.h` (cell tables + bridge bend data + GHZ OBJ palettes).

Usage:   python3 tools/ghz_props.py            # patches generated/bank_047.c in place + writes ghz_props.h
         (convert_all.py calls build() directly so a full regeneration keeps the props.)
"""

from __future__ import annotations
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compression import decompress_nemesis  # noqa: E402
from md_symbols import S1DisasmROM  # noqa: E402
from gfx_converter import decode_4bpp_tile, encode_2bpp_tile, load_md_palette_bytes, rgb555_to_rgb8, rgb8_to_rgb555  # noqa: E402

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
S1_DIR = os.path.join(ROOT_DIR, "vendor", "s1disasm")
GEN_DIR = os.path.join(ROOT_DIR, "generated")

PROP_BASE = 40          # first zone-prop tile in the zone sprite bank
PROP_SLOTS = 28         # tiles 40..67
PAL_STEEL, PAL_DIRT, PAL_GRASS = 5, 6, 7
STEEL_RGB = [(24, 28, 36), (148, 168, 192), (252, 252, 252)]   # g_default_obj_pals[5] (kept for spikes)


def _nem(name: str) -> bytes:
    return decompress_nemesis(open(os.path.join(S1_DIR, "artnem", name), "rb").read())


def _tiles(raw: bytes) -> dict[int, np.ndarray]:
    return {i: decode_4bpp_tile(raw[i * 32:(i + 1) * 32]) for i in range(len(raw) // 32)}


def _render(pieces, td, pal_rgb):
    """Composite MD sprite pieces -> (H, W, 4) RGBA canvas + (x0, y0) of its top-left (MD px, object-relative)."""
    x0 = min(p.x for p in pieces)
    y0 = min(p.y for p in pieces)
    x1 = max(p.x + p.width * 8 for p in pieces)
    y1 = max(p.y + p.height * 8 for p in pieces)
    idx = np.zeros((y1 - y0, x1 - x0), np.uint8)
    for p in reversed(pieces):
        for cx in range(p.width):
            for cy in range(p.height):
                t = td.get(p.tile + cx * p.height + cy)
                if t is None:
                    continue
                if p.hflip:
                    t = t[:, ::-1]
                if p.vflip:
                    t = t[::-1, :]
                px = p.x - x0 + ((p.width - 1 - cx) if p.hflip else cx) * 8
                py = p.y - y0 + ((p.height - 1 - cy) if p.vflip else cy) * 8
                v = idx[py:py + 8, px:px + 8]
                m = t > 0
                v[m] = t[m]
    rgba = np.zeros(idx.shape + (4,), np.float32)
    for c in range(1, 16):
        m = idx == c
        rgba[m, :3] = pal_rgb[c]
        rgba[m, 3] = 255
    return rgba, x0, y0


def _half(rgba: np.ndarray) -> np.ndarray:
    """2x2 box downsample; a half pixel is opaque when >= 2 of its 4 source pixels are."""
    h, w = rgba.shape[:2]
    if h % 2:
        rgba = np.vstack([rgba, np.zeros((1, w, 4), np.float32)])
    if w % 2:
        rgba = np.hstack([rgba, np.zeros((rgba.shape[0], 1, 4), np.float32)])
    h, w = rgba.shape[:2]
    out = np.zeros((h // 2, w // 2, 4), np.float32)
    a = rgba[:, :, 3] > 0
    for y in range(h // 2):
        for x in range(w // 2):
            blk = rgba[y * 2:y * 2 + 2, x * 2:x * 2 + 2]
            m = a[y * 2:y * 2 + 2, x * 2:x * 2 + 2]
            n = int(m.sum())
            if n >= 2:
                out[y, x, :3] = blk[m][:, :3].mean(axis=0)
                out[y, x, 3] = 255
    return out


def _cut_cells(half: np.ndarray, x0h: int, y0h: int):
    """Cut a half-scale canvas into 8x8 cells -> [(dx, dy, tile8x8)] (fully transparent cells dropped)."""
    h, w = half.shape[:2]
    cells = []
    for cy in range(0, h, 8):
        for cx in range(0, w, 8):
            t = np.zeros((8, 8, 4), np.float32)
            sub = half[cy:cy + 8, cx:cx + 8]
            t[:sub.shape[0], :sub.shape[1]] = sub
            if t[:, :, 3].any():
                cells.append((x0h + cx, y0h + cy, t))
    return cells


class Builder:
    def __init__(self):
        self.s1 = S1DisasmROM(S1_DIR)
        pal_raw = open(os.path.join(S1_DIR, "palette", "Green Hill Zone.bin"), "rb").read()
        cr = load_md_palette_bytes(pal_raw)
        self.pal_lines = [[rgb555_to_rgb8(c) for c in cr[i * 16:(i + 1) * 16]] for i in range(len(cr) // 16)]
        self.cells = {}          # group -> list of (dx, dy, rgba8x8, allowed_pals)
        self.lvl = _tiles(_nem("8x8 - GHZ1.nem") + _nem("8x8 - GHZ2.nem"))

    def frames(self, sym, n):
        return self.s1.parse_sprite_mapping_table(self.s1.sym(sym), n)

    def add_whole(self, group, pieces, td, line, pals, max_rows=None):
        rgba, x0, y0 = _render(pieces, td, self.pal_lines[line])
        assert x0 % 2 == 0 and y0 % 2 == 0
        cells = _cut_cells(_half(rgba), x0 // 2, y0 // 2)
        if max_rows is not None:
            cells = [c for c in cells if c[1] < y0 // 2 + 8 * max_rows]
        cells.sort(key=lambda c: (c[1], c[0]))
        self.cells[group] = [(dx, dy, t, pals, 0) for dx, dy, t in cells]
        return rgba

    def add_pieces(self, group, pieces, td, line, pals, sort=False):
        """Cells per MD sprite piece; remember the piece index (fragment / delay tables are per piece)."""
        out = []
        for pi, p in enumerate(pieces):
            rgba, x0, y0 = _render([p], td, self.pal_lines[line])
            assert x0 % 2 == 0 and y0 % 2 == 0, (group, p)
            for dx, dy, t in _cut_cells(_half(rgba), x0 // 2, y0 // 2):
                out.append((dx, dy, t, pals, pi))
        if sort:
            out.sort(key=lambda c: (c[1], c[0]))
        self.cells[group] = out
        return out


# ------------------------------------------------------------------------------------------------------------------
def _lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def _kmeans3(px: np.ndarray) -> list[tuple[int, int, int]]:
    """3 luminance-ordered colours for a set of RGB pixels."""
    if len(px) == 0:
        return [(0, 0, 0), (128, 128, 128), (255, 255, 255)]
    lum = px @ np.array([0.299, 0.587, 0.114])
    order = np.argsort(lum)
    cen = np.array([px[order[int(len(px) * q)]] for q in (0.12, 0.5, 0.92)], np.float32)
    for _ in range(12):
        d = ((px[:, None, :] - cen[None]) ** 2).sum(axis=2)
        lab = d.argmin(axis=1)
        for k in range(3):
            if (lab == k).any():
                cen[k] = px[lab == k].mean(axis=0)
    cen = sorted(cen.tolist(), key=_lum)
    return [tuple(int(round(v / 8.0) * 8 if v < 248 else 252) for v in c) for c in cen]


def _quant(tile, pal):
    """Quantise an RGBA 8x8 tile to palette indices 0..3 (0 = transparent)."""
    out = np.zeros((8, 8), np.uint8)
    pa = np.array(pal, np.float32)
    for y in range(8):
        for x in range(8):
            if tile[y, x, 3] == 0:
                continue
            d = ((pa - tile[y, x, :3]) ** 2 * np.array([0.6, 1.0, 0.45])).sum(axis=1)
            out[y, x] = int(d.argmin()) + 1
    return out


def _err(tile, pal):
    q = _quant(tile, pal)
    pa = np.array(pal, np.float32)
    e = 0.0
    for y in range(8):
        for x in range(8):
            if q[y, x]:
                e += float((((pa[q[y, x] - 1] - tile[y, x, :3]) ** 2) * np.array([0.6, 1.0, 0.45])).sum())
    return e


def _green_frac(tile):
    m = tile[:, :, 3] > 0
    if not m.any():
        return 0.0
    r, g, b = tile[:, :, 0][m], tile[:, :, 1][m], tile[:, :, 2][m]
    return float(((g > r * 1.05) & (g > b)).mean())


def _flips(a):
    return [(a, 0, 0), (a[:, ::-1], 1, 0), (a[::-1, :], 0, 1), (a[::-1, ::-1], 1, 1)]


def build():
    B = Builder()
    s1 = B.s1
    log_td = _tiles(_nem("GHZ Bridge.nem"))
    rock_td = _tiles(_nem("GHZ Purple Rock.nem"))
    smash_td = _tiles(_nem("GHZ Breakable Wall.nem"))
    swing_td = _tiles(_nem("GHZ Swinging Platform.nem"))

    bri = B.frames("Map_Bri", 3)
    B.add_whole("log", bri[0], log_td, 1, [PAL_DIRT])
    B.add_whole("stump", bri[1], log_td, 1, [PAL_DIRT])

    plat = B.frames("Map_Plat_GHZ", 2)
    B.add_whole("plat", plat[0], B.lvl, 1, [PAL_DIRT, PAL_GRASS])
    B.add_whole("col", plat[1], B.lvl, 1, [PAL_DIRT, PAL_GRASS], max_rows=4)

    led = B.frames("Map_Ledge", 4)
    for i in (0, 1):
        B.add_pieces(f"ledge{i}", led[i + 2], B.lvl, 1, [PAL_DIRT, PAL_GRASS], sort=True)

    rock = B.frames("Map_PRock", 1)
    B.add_whole("rock", rock[0], rock_td, 2, [PAL_STEEL])

    sm = B.frames("Map_Smash", 3)
    for i in range(3):
        B.add_pieces(f"smash{i}", sm[i], smash_td, 1, [PAL_DIRT, PAL_GRASS])

    swing = B.frames("Map_Swing_GHZ", 3)
    B.add_whole("swing", swing[0], swing_td, 1, [PAL_DIRT])

    # ---- palette assignment (terrain cells: grass vs dirt) ---------------------------------------------------
    groups = list(B.cells)
    all_cells = [(g, i) for g in groups for i in range(len(B.cells[g]))]
    assign = {}
    for g, i in all_cells:
        dx, dy, t, pals, _pi = B.cells[g][i]
        if len(pals) == 1:
            assign[(g, i)] = pals[0]
        else:
            assign[(g, i)] = PAL_GRASS if _green_frac(t) > 0.18 else PAL_DIRT
    pal_rgb = {PAL_STEEL: STEEL_RGB}
    for _ in range(2):
        for pal in (PAL_DIRT, PAL_GRASS):
            px = []
            for (g, i), p in assign.items():
                if p == pal:
                    t = B.cells[g][i][2]
                    px.append(t[t[:, :, 3] > 0][:, :3])
            pal_rgb[pal] = _kmeans3(np.vstack(px) if px else np.zeros((0, 3)))
        for g, i in all_cells:
            dx, dy, t, pals, _pi = B.cells[g][i]
            if len(pals) > 1:
                assign[(g, i)] = min(pals, key=lambda p: _err(t, pal_rgb[p]))

    # ---- quantise + dedupe (flip-aware) ----------------------------------------------------------------------
    uniq: list[tuple[int, np.ndarray]] = []       # (pal, idx8x8)
    index: dict[tuple[int, bytes], tuple[int, int, int]] = {}
    refs: dict[str, list[list]] = {}              # group -> [[dx, dy, tid, fh, fv, pidx, pal]]
    for g in groups:
        lst = []
        for i, (dx, dy, t, pals, pidx) in enumerate(B.cells[g]):
            pal = assign[(g, i)]
            q = _quant(t, pal_rgb[pal])
            hit = None
            for v, fh, fv in _flips(q):
                k = (pal, np.ascontiguousarray(v).tobytes())
                if k in index:
                    tid, th, tv = index[k]
                    hit = (tid, fh ^ th, fv ^ tv)   # draw stored tile with the flips that reproduce q
                    break
            if hit is None:
                tid = len(uniq)
                uniq.append((pal, q))
                for v, fh, fv in _flips(q):
                    index.setdefault((pal, np.ascontiguousarray(v).tobytes()), (tid, fh, fv))
                hit = (tid, 0, 0)
            lst.append([dx, dy, hit[0], hit[1], hit[2], pidx, pal])
        refs[g] = lst
    print(f"  GHZ props: {len(uniq)} unique tiles before merge (budget {PROP_SLOTS})", file=sys.stderr)

    # ---- merge the most similar tiles until the prop budget fits --------------------------------------------------
    alive = set(range(len(uniq)))
    while len(alive) > PROP_SLOTS:
        use = {t: 0 for t in alive}
        for lst in refs.values():
            for r in lst:
                use[r[2]] += 1
        best = None
        for a_ in alive:
            for b_ in alive:
                if a_ == b_ or uniq[a_][0] != uniq[b_][0]:
                    continue
                for v, fh, fv in _flips(uniq[a_][1]):
                    d = int((np.asarray(v) != uniq[b_][1]).sum()) + int(((np.asarray(v) == 0) != (uniq[b_][1] == 0)).sum())
                    cost = d * use[a_]
                    if best is None or cost < best[0]:
                        best = (cost, a_, b_, fh, fv, d)
        _, a_, b_, fh, fv, d = best
        print(f"    merge tile {a_} -> {b_} (flip {fh}{fv}, diff {d}px, used {use[a_]}x)", file=sys.stderr)
        for lst in refs.values():
            for r in lst:
                if r[2] == a_:
                    r[2], r[3], r[4] = b_, r[3] ^ fh, r[4] ^ fv
        alive.discard(a_)
    order = sorted(alive)
    remap = {t: n for n, t in enumerate(order)}
    uniq = [uniq[t] for t in order]
    out_cells: dict[str, list[tuple[int, int, int, int, int]]] = {}
    for g, lst in refs.items():
        out_cells[g] = [(r[0], r[1], PROP_BASE + remap[r[2]],
                         0x08 | r[6] | (0x20 if r[3] else 0) | (0x40 if r[4] else 0), r[5]) for r in lst]
    print(f"  GHZ props: {len(uniq)} tiles used (budget {PROP_SLOTS})", file=sys.stderr)
    tiles = b"".join(encode_2bpp_tile(q) for _, q in uniq).ljust(PROP_SLOTS * 16, b"\x00")

    # ---- bridge bend data (`Bri_Data_Align`, triangular: row r has r+1 entries) ---------------------------------
    rom = s1.rom
    base = s1.sym("Bri_Data_Align")
    tri = bytearray()
    for r in range(16):
        tri.extend(rom[base + r * 16: base + r * 16 + r + 1])

    # ---- header --------------------------------------------------------------------------------------------
    def c565(rgb):
        return rgb8_to_rgb555(*rgb)

    L = ["/* GENERATED by tools/ghz_props.py - do not edit. Half-scale GHZ prop sprite cells (dx, dy, tile, attr). */",
         "#ifndef GHZ_PROPS_H", "#define GHZ_PROPS_H", "",
         f"#define GHZP_TILE_BASE {PROP_BASE}", f"#define GHZP_TILES {len(uniq)}", "",
         "typedef struct { int8_t dx; int8_t dy; uint8_t tile; uint8_t attr; } GhzCell;", ""]
    for g in groups:
        c = out_cells[g]
        L.append(f"#define GHZC_{g.upper()}_N {len(c)}")
        L.append(f"static const GhzCell ghzc_{g}[{len(c)}] = {{")
        for dx, dy, tl, at, _pi in c:
            L.append(f"    {{ {dx}, {dy}, {tl}, 0x{at:02X} }},")
        L.append("};")
        if g.startswith("ledge") or g.startswith("smash"):
            L.append(f"static const uint8_t ghzp_{g}[{len(c)}] = {{ " + ", ".join(str(x[4]) for x in c) + " };")
    L.append("")
    L.append(f"static const uint8_t ghz_bri_align[{len(tri)}] = {{")
    L.append("    " + ", ".join(f"0x{b:02X}" for b in tri))
    L.append("};")
    L.append("")
    L.append("/* GHZ override for OBJ palettes 6 (dirt/wood) and 7 (grass), colours 0..3 each */")
    L.append("#define GHZP_PAL67 { " + ", ".join(
        ["0x0000"] + [f"0x{c565(c):04X}" for c in pal_rgb[PAL_DIRT]] +
        ["0x0000"] + [f"0x{c565(c):04X}" for c in pal_rgb[PAL_GRASS]]) + " }")
    L.append("")
    L.append("#endif")
    return tiles, "\n".join(L) + "\n", uniq, pal_rgb


def patch_sprite_blob(blob: bytes, tiles: bytes) -> bytes:
    b = bytearray(blob)
    b[PROP_BASE * 16:(PROP_BASE + PROP_SLOTS) * 16] = tiles
    return bytes(b)


def write_header(text: str):
    os.makedirs(GEN_DIR, exist_ok=True)
    with open(os.path.join(GEN_DIR, "ghz_props.h"), "w") as f:
        f.write(text)
    m = re.search(r"#define GHZP_PAL67 (\{.*?\})", text)
    with open(os.path.join(GEN_DIR, "ghz_pals.h"), "w") as f:
        f.write("/* GENERATED by tools/ghz_props.py - GHZ override for CGB OBJ palettes 6 (dirt/wood) and 7 (grass). */\n"
                "#ifndef GHZ_PALS_H\n#define GHZ_PALS_H\n"
                f"static const uint16_t g_ghz_obj_pal67[8] = {m.group(1)};\n#endif\n")


def patch_generated_c(tiles: bytes):
    path = os.path.join(GEN_DIR, "bank_047.c")
    src = open(path).read()
    m = re.search(r"(const uint8_t vset_0_sprtiles\[2048\] = \{)(.*?)(\n\};)", src, re.S)
    assert m, "vset_0_sprtiles not found"
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
    assert len(vals) == 2048
    vals = list(patch_sprite_blob(bytes(vals), tiles))
    lines = []
    for i in range(0, 2048, 24):
        lines.append("    " + ", ".join(f"0x{v:02X}" for v in vals[i:i + 24]) + ",")
    open(path, "w").write(src[:m.start(2)] + "\n" + "\n".join(lines) + src[m.end(2):])


def main():
    tiles, header, uniq, pal_rgb = build()
    write_header(header)
    patch_generated_c(tiles)
    print(f"wrote generated/ghz_props.h and patched vset_0_sprtiles ({len(uniq)} tiles)")
    # debug preview
    try:
        from PIL import Image
        sheet = Image.new("RGB", (len(uniq) * 18, 18), (60, 90, 170))
        for n, (pal, q) in enumerate(uniq):
            cols = [None] + pal_rgb[pal]
            for y in range(8):
                for x in range(8):
                    if q[y, x]:
                        for yy in range(2):
                            for xx in range(2):
                                sheet.putpixel((n * 18 + 1 + x * 2 + xx, 1 + y * 2 + yy), cols[q[y, x]])
        sheet.save(os.path.join(os.environ.get("GHZ_PREVIEW_DIR", "/tmp"), "ghz_tiles.png"))
    except Exception as e:  # pragma: no cover
        print("preview skipped:", e)


if __name__ == "__main__":
    main()
