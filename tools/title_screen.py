#!/usr/bin/env python3
"""
Build the animated Game Boy Color title screen from the Mega Drive layers rendered by `title_layers.py`.

What the Mega Drive title does (GM_Title, Deform_GHZ, objects 0E/0F, PalCycle_Title), and how it is mapped here:

  * The GHZ background (plane B) scrolls left behind the emblem: the camera moves 2 px/frame and Deform_GHZ gives
    every band its own speed (three auto-scrolling cloud bands, distant mountains, hills, and the water whose
    speed grows line by line). The GBC has one BG plane, so:
      - tile rows without any emblem pixel are scrolled in hardware: a chain of LYC interrupts writes SCX
        per band (`title_lcd_isr`);
      - tile rows that contain the emblem keep SCX = 0, and their background cells are scrolled in software:
        for every scroll position p (period 128 px = one GHZ chunk) the cell graphics (background under the
        emblem, already composited) are precomputed here and streamed into a fixed set of VRAM tiles with
        GDMA during VBlank.
  * The emblem (plane A) stays put; there are no high-priority emblem tiles, Sonic is drawn over all of it.
  * Big Sonic (object 0E) waits 30 frames, rises 8 px/frame from y=$5E to y=$16, then plays frames 1..5 and
    loops the finger-wag frames 6, 7 (8 frames each). On the Mega Drive a row of "line limiter" sprites
    (Map_PSB frame 2) hides him below y=104; here the sprite graphics are clipped at the same line.
    He is built from 8x16 hardware sprites in up to two layers (3 colours + transparent each).
  * "PRESS START BUTTON" (object 0F) flashes every 32 frames (hardware sprites); the "TM" is part of the BG.
  * PalCycle_Title rotates the four water colours every 6 frames: one BG palette holds only water colours
    and is rotated the same way.

Vertical mapping: GBC y = 15 + Mega Drive y / 2, which puts every Deform_GHZ band boundary on a tile row.
Outputs `generated/title_*.c`, `generated/title_data.h` and preview images in `verification/`.
"""
from __future__ import annotations

import os
import sys

import numpy as np
from PIL import Image

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)
from title_layers import render_layers, load_pal  # noqa: E402
from gfx_converter import rgb8_to_rgb555, rgb555_to_rgb8  # noqa: E402

GEN_DIR = os.path.join(ROOT, "generated")
VER_DIR = os.path.join(ROOT, "verification")

Y_OFF = 15                  # GBC y = Y_OFF + md_y / 2
BG_PLANE_Y = 14             # Deform_GHZ BG Y scroll on the title (GHZ1 start camera): plane row = md_y + 14
SW_PERIOD = 128             # software-scrolled rows: one GHZ chunk (chunk 59 tiles with itself in ghzbg.bin)
SW_CHUNKS = [59]
HW_CHUNKS = [59, 61]        # hardware rows: 256 px = the full 32-tile BG map (59 -> 61 -> 59 occurs in ghzbg.bin)
MASK_Y_MD = 104             # Map_PSB frame 2 "line limiter" hides big Sonic on lines 104..199
SONIC_X_MD = 0x70
SONIC_Y_FINAL = 0x16
SONIC_Y_START = 0x5E
PSB_X_MD, PSB_Y_MD = 0x50, 0xB0
TM_X_MD, TM_Y_MD = 0xF0, 0x78

# Deform_GHZ (REV01) band speeds in Mega Drive px/frame with the title camera moving 2 px/frame.
# Clouds auto-scroll by 1, 0.75, 0.5 px/frame plus the mountains' 0.375 * camera; mountains 0.375 * camera;
# hills 0.5 * camera; water line i (from the top of the water band) moves at hills + (camera - hills) * i / $68.
CAM = 2.0


def band_speed_md(md_y: float) -> float:
    if md_y < 18:
        return 1.0 + 0.375 * CAM
    if md_y < 34:
        return 0.75 + 0.375 * CAM
    if md_y < 50:
        return 0.5 + 0.375 * CAM
    if md_y < 98:
        return 0.375 * CAM
    if md_y < 138:
        return 0.5 * CAM
    i = md_y - 138
    return 0.5 * CAM + (CAM - 0.5 * CAM) * i / 0x68


# ------------------------------------------------------------------------------------------------------------
# Downscaling helpers
# ------------------------------------------------------------------------------------------------------------
def lum(c):
    return 0.299 * c[..., 0] + 0.587 * c[..., 1] + 0.114 * c[..., 2]


def downscale_mode(rgb: np.ndarray) -> np.ndarray:
    """2x2 -> 1 keeping exact source colours: most frequent colour of each block, ties -> brightest."""
    h, w = rgb.shape[0] // 2, rgb.shape[1] // 2
    out = np.zeros((h, w, 3), np.uint8)
    for y in range(h):
        for x in range(w):
            blk = rgb[2 * y:2 * y + 2, 2 * x:2 * x + 2].reshape(4, 3)
            best, bc, bl = None, -1, -1.0
            for i in range(4):
                c = tuple(int(v) for v in blk[i])
                n = sum(1 for j in range(4) if tuple(int(v) for v in blk[j]) == c)
                lu = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
                if n > bc or (n == bc and lu > bl):
                    best, bc, bl = c, n, lu
            out[y, x] = best
    return out


def downscale_rgba(rgba: np.ndarray, keep_black: bool = False) -> np.ndarray:
    """2x2 -> 1 with alpha: opaque if >= 2 of 4 source pixels are opaque; colour = mean of the opaque ones.
    keep_black: a block holding pure black (Sonic's pupils / nose) stays pure black."""
    h, w = rgba.shape[0] // 2, rgba.shape[1] // 2
    blk = rgba[:2 * h, :2 * w].reshape(h, 2, w, 2, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4, 4).astype(np.float32)
    a = blk[..., 3] > 0
    n = a.sum(axis=2)
    col = (blk[..., :3] * a[..., None]).sum(axis=2) / np.maximum(n, 1)[..., None]
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = np.round(col).astype(np.uint8)
    out[..., 3] = np.where(n >= 2, 255, 0)
    if keep_black:
        black = (a & (blk[..., :3].sum(axis=3) == 0)).any(axis=2)
        out[black] = (0, 0, 0, 255)
    return out


def snap555(rgb: np.ndarray) -> np.ndarray:
    """Round RGB8 to the nearest CGB-representable RGB555 colour (vectorised)."""
    q = np.round(rgb.astype(np.float32) * 31.0 / 255.0)
    return np.round(q * 255.0 / 31.0).astype(np.uint8)


# ------------------------------------------------------------------------------------------------------------
# Scene construction
# ------------------------------------------------------------------------------------------------------------
class Scene:
    def __init__(self) -> None:
        L = render_layers()
        self.L = L
        pal = L["pal"]
        self.backdrop = pal[0x20]           # vreg_bgcolor = line 2, entry 0
        self.sky = pal[0x31]                 # opaque sky blue used by the cloud chunks (line 3, entry 1)
        self.water_cols = [pal[0x28], pal[0x29], pal[0x2A], pal[0x2B]]
        self.cycle = load_pal("Cycle - Title Screen Water.bin")  # 4 frames x colours 8..B of line 2

        def plane(chunk_list):
            w = 256 * len(chunk_list)
            img = np.zeros((256, w, 3), np.uint8)
            img[:, :] = self.backdrop
            for i, cid in enumerate(chunk_list):
                ch = L["chunks"][cid]
                m = ch[..., 3] > 0
                sub = img[:, i * 256:(i + 1) * 256]
                sub[m] = ch[..., :3][m]
            return img

        self.plane_sw = plane(SW_CHUNKS)
        self.plane_hw = plane(HW_CHUNKS)

        # Emblem layer (plane A) + TM, at Mega Drive resolution, then half scale on the GBC canvas.
        emb = L["emb_lo"].copy()
        tm = L["tm"]
        tx, ty = TM_X_MD - 8, TM_Y_MD - 4
        m = tm[..., 3] > 0
        sub = emb[ty:ty + tm.shape[0], tx:tx + tm.shape[1]]
        sub[m] = tm[m]
        self.emb_md = emb
        self.emblem = np.zeros((144, 160, 4), np.uint8)           # GBC-space RGBA
        half = downscale_rgba(emb)                                  # 112 x 160
        # "(C)SEGA 1991" (the emblem's bottom rows, over the water): white letters on a dark shadow; averaging
        # turns them grey, so snap them to white / the water backdrop to keep them sharp and on the water palette.
        txt = half[96:]
        op = txt[..., 3] > 0
        lu = lum(txt[..., :3].astype(np.float32))
        txt[op & (lu >= 100)] = (255, 255, 255, 255)
        txt[op & (lu < 100)] = (*self.backdrop, 255)
        self.emblem[Y_OFF:Y_OFF + 112] = half

    # GBC-scale background strip for a list of GBC rows (y), as an (len, width/2, 3) array.
    def bg_strip(self, plane_img: np.ndarray, y0: int, y1: int) -> np.ndarray:
        rows = []
        for gy in range(y0, y1):
            src = []
            for k in range(2):
                md_y = 2 * (gy - Y_OFF) + k
                py = md_y + BG_PLANE_Y
                if py < 0:
                    src.append(np.tile(np.array(self.sky, np.uint8), (plane_img.shape[1], 1)))
                elif py >= 256:
                    src.append(np.tile(np.array(self.backdrop, np.uint8), (plane_img.shape[1], 1)))
                else:
                    src.append(plane_img[py])
            rows.append(np.stack(src))
        pair = np.stack(rows)                      # (n, 2, W, 3)
        out = np.zeros((y1 - y0, plane_img.shape[1] // 2, 3), np.uint8)
        for i in range(y1 - y0):
            out[i] = downscale_mode(pair[i])[0]
        return out


def row_speed_88(r: int) -> int:
    mid_md = 2 * (r * 8 + 4 - Y_OFF)
    if r <= 2:
        mid_md = 0      # rows above the Mega Drive screen belong to the top cloud band
    gbc = band_speed_md(mid_md) / 2.0
    return int(round(gbc * 256))


# ------------------------------------------------------------------------------------------------------------
# Palette solving (k-means over 8x8 tiles, CGB 4-colour palettes)
# ------------------------------------------------------------------------------------------------------------
def tile_err(tiles: np.ndarray, pal: np.ndarray):
    """tiles (N,64,3) float, pal (4,3) -> (err (N,), idx (N,64))"""
    d = ((tiles[:, :, None, :] - pal[None, None, :, :]) ** 2).sum(axis=3)
    idx = d.argmin(axis=2)
    e = np.take_along_axis(d, idx[:, :, None], axis=2)[:, :, 0].sum(axis=1)
    return e, idx


def solve_palettes(tiles: np.ndarray, weights: np.ndarray, water_ok: np.ndarray, water_pal: np.ndarray,
                   n_free: int = 7, rounds: int = 24, seed: int = 1991, fixed: dict | None = None):
    fixed = fixed or {}
    rng = np.random.default_rng(seed)
    N = len(tiles)
    pals = np.zeros((n_free, 4, 3), np.float32)
    # farthest-point seeding on tile mean colours
    means = tiles.mean(axis=1)
    order = [int(np.argmax(weights))]
    for _ in range(1, n_free):
        d = np.min([((means - means[o]) ** 2).sum(axis=1) for o in order], axis=0)
        order.append(int(np.argmax(d * np.sqrt(weights))))
    for k, o in enumerate(order):
        px = tiles[o]
        pals[k] = px[rng.choice(64, 4, replace=False)]
    for k, v in fixed.items():
        pals[k] = v
    BIG = 1e18
    for _ in range(rounds):
        errs = np.zeros((N, n_free + 1))
        for k in range(n_free):
            errs[:, k], _ = tile_err(tiles, pals[k])
        we, _ = tile_err(tiles, water_pal)
        errs[:, n_free] = np.where(water_ok, we, BIG)
        best = errs.argmin(axis=1)
        for k in range(n_free):
            if k in fixed:
                continue
            sel = np.where(best == k)[0]
            if len(sel) == 0:
                worst = int(np.argmax(errs[np.arange(N), best] * weights))
                pals[k] = tiles[worst][rng.choice(64, 4, replace=False)]
                continue
            px = tiles[sel].reshape(-1, 3)
            w = np.repeat(weights[sel], 64)
            c = pals[k].copy()
            for _it in range(6):
                d = ((px[:, None, :] - c[None, :, :]) ** 2).sum(axis=2)
                a = d.argmin(axis=1)
                for j in range(4):
                    m = a == j
                    if m.any():
                        c[j] = (px[m] * w[m, None]).sum(axis=0) / w[m].sum()
                    else:
                        far = int(np.argmax(d[np.arange(len(px)), a] * w))
                        c[j] = px[far]
            pals[k] = snap555(c).astype(np.float32)
    return pals


# ------------------------------------------------------------------------------------------------------------
def encode_2bpp(idx: np.ndarray) -> bytes:
    out = bytearray()
    for y in range(idx.shape[0]):
        lo = hi = 0
        for x in range(8):
            v = int(idx[y, x])
            lo |= (v & 1) << (7 - x)
            hi |= ((v >> 1) & 1) << (7 - x)
        out += bytes((lo, hi))
    return bytes(out)


def build():
    S = Scene()
    L = S.L

    # ---- classify tile rows ----------------------------------------------------------------------------------
    emb_a = S.emblem[..., 3] > 0
    hw_rows = [r for r in range(18) if not emb_a[r * 8:r * 8 + 8].any()]
    sw_rows = [r for r in range(18) if r not in hw_rows]
    speeds = [row_speed_88(r) for r in range(18)]
    water_top_gbc = Y_OFF + 138 // 2     # first GBC line of the water band
    print("hw rows", hw_rows, "sw rows", sw_rows)

    # ---- software rows: composite every cell at every scroll position -------------------------------------
    strip_sw = S.bg_strip(S.plane_sw, 0, 144)          # (144, 128, 3)
    strip_hw = S.bg_strip(S.plane_hw, 0, 144)          # (144, 256, 3)
    emb_rgb = S.emblem[..., :3]

    sw_cells = {}       # (r, c) -> array (P, 8, 8, 3) or None if static
    sw_static = {}      # (r, c) -> (8, 8, 3)
    for r in sw_rows:
        ys = slice(r * 8, r * 8 + 8)
        for c in range(20):
            xs = np.arange(c * 8, c * 8 + 8)
            a = emb_a[ys, c * 8:c * 8 + 8]
            if a.all():
                sw_static[(r, c)] = emb_rgb[ys, c * 8:c * 8 + 8].copy()
                continue
            frames = np.zeros((SW_PERIOD, 8, 8, 3), np.uint8)
            for p in range(SW_PERIOD):
                bg = strip_sw[ys][:, (xs + p) % SW_PERIOD]
                frames[p] = np.where(a[..., None], emb_rgb[ys, c * 8:c * 8 + 8], bg)
            if (frames == frames[0]).all():
                sw_static[(r, c)] = frames[0]
            else:
                sw_cells[(r, c)] = frames
    print("dynamic cells", len(sw_cells), "static sw cells", len(sw_static))

    # ---- hardware rows: 32 cells of the 256 px strip -----------------------------------------------------------
    hw_cells = {}
    for r in hw_rows:
        ys = slice(r * 8, r * 8 + 8)
        for c in range(32):
            hw_cells[(r, c)] = strip_hw[ys][:, c * 8:c * 8 + 8].copy()

    # ---- water palette ---------------------------------------------------------------------------------------
    # PalCycle_Title rotates line 2 colours 8..B (the water). BG palette 7 = [backdrop, colour 8, colour 9, white]:
    # its two middle entries follow the cycle, the brightest water sparkles (colours A/B) share the static white,
    # so text and emblem edges over the water ("(C)SEGA 1991", banner tails) can use it too without shimmering.
    wcols = S.water_cols
    water_pal = snap555(np.array([S.backdrop, wcols[0], wcols[1], (255, 255, 255)], np.float32)).astype(np.float32)
    wset = {tuple(int(v) for v in snap555(np.array(c, np.float32))) for c in [S.backdrop] + wcols}
    still = snap555(np.array([S.backdrop, (255, 255, 255)], np.float32)).astype(np.float32)

    def water_compatible(t: np.ndarray, mask) -> bool:
        """Background pixels are water colours and emblem pixels are close to the backdrop or white."""
        px = snap555(t.reshape(64, 3).astype(np.float32))
        m = np.zeros(64, bool) if mask is None else mask.reshape(64)
        for i in range(64):
            if m[i]:
                if ((still - px[i]) ** 2).sum(axis=1).min() > 3 * 48 * 48:
                    return False
            elif tuple(int(v) for v in px[i]) not in wset:
                return False
        return True

    def cell_mask(r, c):
        return emb_a[r * 8:r * 8 + 8, c * 8:c * 8 + 8]

    # ---- gather unique tiles for palette solving ---------------------------------------------------------------
    uniq: dict[bytes, list] = {}

    def add(t: np.ndarray, w: float, water: bool):
        k = t.tobytes()
        if k in uniq:
            uniq[k][1] += w
            uniq[k][2] = uniq[k][2] and water
        else:
            uniq[k] = [t, w, water]

    for (r, c), t in sw_static.items():
        add(t, 4.0 * SW_PERIOD, r * 8 >= water_top_gbc and water_compatible(t, cell_mask(r, c)))
    for (r, c), fr in sw_cells.items():
        wrow = r * 8 >= water_top_gbc
        for p in range(SW_PERIOD):
            add(fr[p], 1.0, wrow and water_compatible(fr[p], cell_mask(r, c)))
    for (r, c), t in hw_cells.items():
        add(t, SW_PERIOD / 1.6, r * 8 >= water_top_gbc and water_compatible(t, None))
    keys = list(uniq.keys())
    tiles = np.stack([uniq[k][0].reshape(64, 3) for k in keys]).astype(np.float32)
    weights = np.array([uniq[k][1] for k in keys], np.float32)
    water_ok = np.array([uniq[k][2] for k in keys], bool)
    print("unique tiles for palette solve", len(keys))

    free_pals = solve_palettes(tiles, weights, water_ok, water_pal)
    W_IDX = 7
    all_pals = np.concatenate([free_pals, water_pal[None]], axis=0)    # palette 7 = water

    def quant(t: np.ndarray, water_allowed: bool):
        tf = t.reshape(1, 64, 3).astype(np.float32)
        best = None
        for k in range(8):
            if k == W_IDX and not water_allowed:
                continue
            e, idx = tile_err(tf, all_pals[k])
            if best is None or e[0] < best[0]:
                best = (e[0], k, idx[0].reshape(8, 8))
        return best[1], best[2]


    # Water palette entry -> logical cycle slot (index into the 4 cycled colours) for the rotation table.
    # palette 7 = [backdrop, c8, c9, c11]; PalCycle_Title writes colours 8..B from Pal_TitleCyc_Water.
    cyc = S.cycle
    cycle_frames = []
    for f in range(4):
        q = cyc[f * 4:(f + 1) * 4]
        cycle_frames.append([S.backdrop, q[0], q[1], (255, 255, 255)])

    # ---- VRAM allocation -------------------------------------------------------------------------------------
    # BG tile slots: bank 0: 0..255, bank 1: 0..79 and 128..255 (80..127 hold the HUD font used by other screens).
    ranges = [(0, 0, 128), (0, 128, 256), (1, 0, 80), (1, 128, 256)]
    free = [list(range(a, b)) for (_, a, b) in ranges]

    def alloc_run(n):
        for i, (bank, a, b) in enumerate(ranges):
            f = free[i]
            # contiguous run at the start of the remaining free list
            if len(f) >= n and f[n - 1] - f[0] == n - 1:
                run = f[:n]
                del f[:n]
                return bank, run[0]
        raise RuntimeError("out of BG tile slots for a dynamic row")

    def alloc_one():
        for i, (bank, a, b) in enumerate(ranges):
            if free[i]:
                return bank, free[i].pop()
        raise RuntimeError("out of BG tile slots")

    # dynamic rows first (contiguous runs)
    rows_out = []
    for r in sw_rows:
        cols = [c for c in range(20) if (r, c) in sw_cells]
        if not cols:
            rows_out.append(dict(r=r, cols=[], n=0))
            continue
        bank, first = alloc_run(len(cols))
        rows_out.append(dict(r=r, cols=cols, n=len(cols), bank=bank, first=first))

    static_tiles: dict[bytes, tuple] = {}     # 2bpp+bank -> (bank, slot)
    tile_store = {0: {}, 1: {}}               # bank -> slot -> bytes

    def place_static(idx: np.ndarray) -> tuple:
        data = encode_2bpp(idx)
        if data in static_tiles:
            return static_tiles[data]
        bank, slot = alloc_one()
        static_tiles[data] = (bank, slot)
        tile_store[bank][slot] = data
        return bank, slot

    def attr_byte(pal, bank):
        return (pal & 7) | (0x08 if bank else 0)

    bg_map = np.zeros((18, 32), np.uint8)
    bg_attr = np.zeros((18, 32), np.uint8)

    # preview reconstruction buffers
    def pal_rgb(k):
        return all_pals[k].astype(np.uint8)

    for r in hw_rows:
        for c in range(32):
            t = hw_cells[(r, c)]
            k, idx = quant(t, r * 8 >= water_top_gbc and water_compatible(t, None))
            bank, slot = place_static(idx)
            bg_map[r, c] = slot
            bg_attr[r, c] = attr_byte(k, bank)
    for (r, c), t in sw_static.items():
        k, idx = quant(t, r * 8 >= water_top_gbc and water_compatible(t, cell_mask(r, c)))
        bank, slot = place_static(idx)
        bg_map[r, c] = slot
        bg_attr[r, c] = attr_byte(k, bank)

    # dynamic tables: record per p = n*16 tile bytes + 32 attribute bytes (full map row)
    for ro in rows_out:
        r = ro["r"]
        if ro["n"] == 0:
            continue
        for i, c in enumerate(ro["cols"]):
            bg_map[r, c] = ro["first"] + i
        recs = []
        for p in range(SW_PERIOD):
            tb = bytearray()
            arow = bytearray(bg_attr[r].tobytes())
            for i, c in enumerate(ro["cols"]):
                t = sw_cells[(r, c)][p]
                water = (r * 8 >= water_top_gbc) and water_compatible(t, cell_mask(r, c))
                k, idx = quant(t, water)
                tb += encode_2bpp(idx)
                arow[c] = attr_byte(k, ro["bank"])
            recs.append(bytes(tb) + bytes(arow))
        ro["recs"] = recs
        ro["rec_size"] = len(recs[0])
        for i, c in enumerate(ro["cols"]):
            bg_attr[r, c] = recs[0][ro["n"] * 16 + c]
    print("static BG tiles", len(static_tiles),
          "dynamic slots", sum(ro["n"] for ro in rows_out),
          "dynamic ROM bytes", sum(len(ro.get("recs", [])) * ro.get("rec_size", 0) for ro in rows_out))

    # ---- Sonic sprite states ------------------------------------------------------------------------------------
    sonic_states = []
    rise = list(range(SONIC_Y_START, SONIC_Y_FINAL - 1, -8))     # $5E, $56, ... $16 (10 positions)
    for y in rise:
        sonic_states.append((0, y))
    for f in range(1, 8):
        sonic_states.append((f, SONIC_Y_FINAL))

    def sonic_gbc_image(frame: int, y_md: int) -> np.ndarray:
        canvas = np.zeros((224 + 128, 320, 4), np.uint8)
        img = L["sonic_frames"][frame]
        canvas[y_md:y_md + 128, SONIC_X_MD:SONIC_X_MD + 128] = img[:, :min(128, 320 - SONIC_X_MD)]
        canvas[MASK_Y_MD:200] = 0
        canvas = canvas[:224]
        half = downscale_rgba(canvas, keep_black=True)          # (112, 160, 4)
        out = np.zeros((144, 160, 4), np.uint8)
        out[Y_OFF:Y_OFF + 112] = half
        return out

    sonic_imgs = [sonic_gbc_image(f, y) for (f, y) in sonic_states]
    gx0 = SONIC_X_MD // 2

    # sprite palettes: 7 palettes x 3 colours, cells may stack two layers
    all_px = np.concatenate([im[..., :3][im[..., 3] > 0] for im in sonic_imgs]).astype(np.float32)
    # global colour reduction first (k-means, 10 colours), black kept exact
    rng = np.random.default_rng(7)
    K = 10
    cent = all_px[rng.choice(len(all_px), K, replace=False)]
    for _ in range(20):
        d = ((all_px[:, None] - cent[None]) ** 2).sum(axis=2)
        a = d.argmin(axis=1)
        for j in range(K):
            if (a == j).any():
                cent[j] = all_px[a == j].mean(axis=0)
    cent = snap555(cent).astype(np.float32)
    cent = np.unique(cent, axis=0)
    K = len(cent)

    def to_k(img):
        px = img[..., :3].astype(np.float32)
        d = ((px[..., None, :] - cent[None, None]) ** 2).sum(axis=3)
        k = d.argmin(axis=2)
        k[img[..., 3] == 0] = -1
        return k

    kimgs = [to_k(im) for im in sonic_imgs]

    # cells: (state, col, row) -> histogram of colour ids
    cells = []
    for s, kim in enumerate(kimgs):
        rows_any = np.where((kim >= 0).any(axis=1))[0]
        if len(rows_any) == 0:
            cells.append([])
            continue
        top = int(rows_any.min())
        bot = int(rows_any.max())
        lst = []
        for gy in range(top, bot + 1, 16):
            for col in range(5):
                blk = kim[gy:gy + 16, gx0 + col * 8:gx0 + col * 8 + 8]
                if (blk >= 0).any():
                    lst.append((gy, col, blk))
        cells.append(lst)

    import itertools
    triples = list(itertools.combinations(range(K), 3))

    def cell_cost(hist, pal_set):
        # best single or pair of palettes covering colours; cost = sum over pixels of squared dist to nearest
        best = None
        cand = [(p,) for p in pal_set] + list(itertools.combinations(pal_set, 2))
        for cnd in cand:
            avail = sorted(set(c for p in cnd for c in triples[p]))
            cost = 0.0
            for col, n in hist.items():
                cost += n * min(((cent[col] - cent[a]) ** 2).sum() for a in avail)
            if best is None or cost < best[0] - 1e-6 or (abs(cost - best[0]) < 1e-6 and len(cnd) < len(best[1])):
                best = (cost, cnd)
        return best

    hists = []
    for lst in cells:
        for (_, _, blk) in lst:
            u, n = np.unique(blk[blk >= 0], return_counts=True)
            hists.append(dict(zip(u.tolist(), n.tolist())))
    # greedy palette choice
    pal_set = []
    for _ in range(7):
        best = None
        for t in range(len(triples)):
            if t in pal_set:
                continue
            trial = pal_set + [t]
            tot = sum(cell_cost(h, trial)[0] for h in hists)
            if best is None or tot < best[0]:
                best = (tot, t)
        pal_set.append(best[1])
        print("sprite palette", len(pal_set), "cost", int(best[0]))
    obj_pals = [[cent[c] for c in triples[t]] for t in pal_set]

    # build sprites per state
    states_out = []
    for s, lst in enumerate(cells):
        sprites = []   # (y, x, tile_bytes(32), pal)
        for (gy, col, blk) in lst:
            hist = {}
            u, n = np.unique(blk[blk >= 0], return_counts=True)
            hist = dict(zip(u.tolist(), n.tolist()))
            _, cnd = cell_cost(hist, pal_set)
            # assign each pixel to a layer: nearest colour among the chosen palettes
            layers = [np.zeros((16, 8), np.uint8) for _ in cnd]
            for yy in range(blk.shape[0]):
                for xx in range(8):
                    k = blk[yy, xx]
                    if k < 0:
                        continue
                    best = None
                    for li, p in enumerate(cnd):
                        for ci, cc in enumerate(triples[p]):
                            d = ((cent[k] - cent[cc]) ** 2).sum()
                            if best is None or d < best[0]:
                                best = (d, li, ci + 1)
                    layers[best[1]][yy, xx] = best[2]
            for li, p in enumerate(cnd):
                if not layers[li].any():
                    continue
                data = encode_2bpp(layers[li][:8]) + encode_2bpp(layers[li][8:16])
                sprites.append((gy, gx0 + col * 8, data, pal_set.index(p)))
        states_out.append(sprites)
    print("sonic sprites per state", [len(s) for s in states_out])

    # ---- PRESS START BUTTON sprites: half width, full height for legibility -----------------------------------
    psb = L["psb"]                                    # (16, 320, 4), origin top-left
    ps_cols = np.where((psb[..., 3] > 0).any(axis=0))[0]
    pw = int(ps_cols.max()) + 1
    pw += pw & 1
    psb = psb[:8, :pw]
    # horizontal halving, keep darker outline / brighter fill by mean of opaque
    hp = np.zeros((8, pw // 2, 4), np.uint8)
    for y in range(8):
        for x in range(pw // 2):
            a = psb[y, 2 * x:2 * x + 2]
            op = a[a[:, 3] > 0]
            if len(op):
                hp[y, x, :3] = op[:, :3].mean(axis=0)
                hp[y, x, 3] = 255
    ppx = hp[..., :3][hp[..., 3] > 0].astype(np.float32)
    pc = ppx[rng.choice(len(ppx), 3, replace=False)]
    for _ in range(20):
        d = ((ppx[:, None] - pc[None]) ** 2).sum(axis=2)
        a = d.argmin(axis=1)
        for j in range(3):
            if (a == j).any():
                pc[j] = ppx[a == j].mean(axis=0)
    pc = snap555(pc).astype(np.float32)
    order = np.argsort(lum(pc))[::-1]
    pc = pc[order]
    psb_sprites = []
    psb_gx = PSB_X_MD // 2
    psb_gy = Y_OFF + PSB_Y_MD // 2 - 2     # 8 px tall text centred on the half-scale 4 px line
    for col in range((hp.shape[1] + 7) // 8):
        blk = np.zeros((16, 8), np.uint8)
        part = hp[:, col * 8:col * 8 + 8]
        for y in range(8):
            for x in range(part.shape[1]):
                if part[y, x, 3]:
                    d = ((pc - part[y, x, :3].astype(np.float32)) ** 2).sum(axis=1)
                    blk[y, x] = int(d.argmin()) + 1
        if blk.any():
            psb_sprites.append((psb_gy, psb_gx + col * 8, encode_2bpp(blk[:8]) + encode_2bpp(blk[8:])))
    print("psb sprites", len(psb_sprites))

    return dict(S=S, hw_rows=hw_rows, sw_rows=sw_rows, speeds=speeds, rows_out=rows_out, tile_store=tile_store,
                bg_map=bg_map, bg_attr=bg_attr, all_pals=all_pals, cycle_frames=cycle_frames,
                states=states_out, sonic_states=sonic_states, obj_pals=obj_pals, psb_sprites=psb_sprites,
                psb_pal=pc, sw_cells=sw_cells)


# ------------------------------------------------------------------------------------------------------------
# C emission
# ------------------------------------------------------------------------------------------------------------
def c_bytes(data: bytes, per=24) -> str:
    out = []
    for i in range(0, len(data), per):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + per]) + ",")
    return "\n".join(out)


def rgb555(c) -> int:
    c = [int(v) for v in c]
    return rgb8_to_rgb555(*c)


# Banks with free space (measured from sonic.map; conservative budgets in bytes)
# Banks 11..46, 47..63 (odd) and 71..73 hold a multiple of 16 bytes of older data, so arrays appended there stay
# 16-byte aligned (required for the GDMA'd row tables).
ALIGNED_BANKS = set(range(11, 47)) | set(range(47, 64, 2)) | {71, 72, 73}
DATA_BANKS = ([(120, 15000)] + [(b, 8000) for b in range(11, 47)] +
              [(b, 6900) for b in range(47, 64, 2)] + [(b, 8200) for b in (71, 72, 73)])
CODE_BANK = 7


def emit(B: dict) -> None:
    files: dict[int, list[str]] = {}
    budget = {b: s for b, s in DATA_BANKS}
    order = [b for b, _ in DATA_BANKS]

    def place(name: str, data: bytes, aligned: bool = False) -> int:
        # aligned: GDMA reads these straight from ROM, so they must start on a 16-byte boundary. Banks 11..46
        # hold exactly 8192 bytes of older data and every title array is a multiple of 16 bytes long.
        assert len(data) % 16 == 0 or not aligned
        for b in order:
            if aligned and b not in ALIGNED_BANKS:
                continue
            if budget[b] >= len(data):
                budget[b] -= len(data)
                files.setdefault(b, []).append(f"const uint8_t {name}[{len(data)}] = {{\n{c_bytes(data)}\n}};\n")
                return b
        raise RuntimeError("out of ROM space for " + name)

    hdr = ["/* Generated by tools/title_screen.py from s1disasm - animated title screen data. */",
           "#ifndef TITLE_DATA_H", "#define TITLE_DATA_H", "#include <stdint.h>", "#include <gbdk/platform.h>", ""]
    desc = [f"#pragma bank {CODE_BANK}", "#include \"title_data.h\"", ""]

    # static tiles (per VRAM bank, slot runs)
    tile_chunks = []
    for vb in (0, 1):
        slots = sorted(B["tile_store"][vb].keys())
        # group into contiguous runs
        runs = []
        for s in slots:
            if runs and s == runs[-1][-1] + 1 and len(runs[-1]) < 128:
                runs[-1].append(s)
            else:
                runs.append([s])
        for run in runs:
            data = b"".join(B["tile_store"][vb][s] for s in run)
            nm = f"title_st_{vb}_{run[0]}"
            b = place(nm, data)
            hdr.append(f"extern const uint8_t {nm}[];")
            tile_chunks.append((b, nm, vb, run[0], len(run)))
    hdr.append("typedef struct { uint8_t bank; const uint8_t *data; uint8_t vbank, first, count; } TitleTileChunk;")
    hdr.append(f"#define TITLE_N_TILE_CHUNKS {len(tile_chunks)}")
    hdr.append("extern const TitleTileChunk title_tile_chunks[];")
    desc.append("const TitleTileChunk title_tile_chunks[] = {")
    for (b, nm, vb, first, cnt) in tile_chunks:
        desc.append(f"    {{ {b}, {nm}, {vb}, {first}, {cnt} }},")
    desc.append("};")

    # maps
    hdr.append("extern const uint8_t title_map[18 * 32];")
    hdr.append("extern const uint8_t title_attr[18 * 32];")
    desc.append(f"const uint8_t title_map[18 * 32] = {{\n{c_bytes(B['bg_map'].tobytes(), 32)}\n}};")
    desc.append(f"const uint8_t title_attr[18 * 32] = {{\n{c_bytes(B['bg_attr'].tobytes(), 32)}\n}};")

    # palettes: PaletteFadeIn steps 0..16 precomputed (every channel climbs 2 levels/frame from black)
    base = [rgb555(c) for p in B["all_pals"][:7] for c in p] + [rgb555(c) for c in B["cycle_frames"][0]]
    opals = []
    for p in B["obj_pals"]:
        opals += [0] + [rgb555(c) for c in p]
    opals += [0] + [rgb555(c) for c in B["psb_pal"]]
    full = base + opals
    fade = []
    for f in range(17):
        lim = 2 * f
        for c in full:
            r, g, b_ = c & 31, (c >> 5) & 31, (c >> 10) & 31
            if f < 16:
                r, g, b_ = min(r, lim), min(g, lim), min(b_, lim)
            fade.append(r | (g << 5) | (b_ << 10))
    hdr.append("#define TITLE_FADE_STEPS 16")
    hdr.append("extern const uint16_t title_fade_pals[17 * 64];")
    desc.append("const uint16_t title_fade_pals[17 * 64] = {\n" +
                "\n".join("    " + ", ".join(f"0x{v:04X}" for v in fade[i:i + 16]) + "," for i in range(0, len(fade), 16)) + "\n};")
    cyc = [rgb555(c) for f in B["cycle_frames"] for c in f]
    hdr.append("#define TITLE_WATER_PAL 7")
    hdr.append("extern const uint16_t title_water_cycle[16];")
    desc.append("const uint16_t title_water_cycle[16] = { " + ", ".join(f"0x{v:04X}" for v in cyc) + " };")

    # row scroll speeds (8.8 GBC px/frame), hardware rows and the LYC chain
    speeds = B["speeds"]
    hw = [1 if r in B["hw_rows"] else 0 for r in range(18)]
    hdr.append("extern const uint16_t title_row_speed[18];")
    desc.append("const uint16_t title_row_speed[18] = { " + ", ".join(str(v) for v in speeds) + " };")
    lyc_line, lyc_row = [], []
    for r in range(1, 18):
        if hw[r] != hw[r - 1] or (hw[r] and speeds[r] != speeds[r - 1]):
            lyc_line.append(r * 8 - 1)
            lyc_row.append(r)
    lyc_line.append(143)
    lyc_row.append(0)
    hdr.append(f"#define TITLE_N_LYC {len(lyc_line)}")
    hdr.append("extern const uint8_t title_lyc_line[];")
    hdr.append("extern const uint8_t title_lyc_row[];   /* 0xFF: software row (SCX 0) */")
    desc.append("const uint8_t title_lyc_line[] = { " + ", ".join(map(str, lyc_line)) + ", 0xFF };")
    desc.append("const uint8_t title_lyc_row[] = { " + ", ".join(str(r if hw[r] else 0xFF) for r in lyc_row) + " };")
    hdr.append(f"#define TITLE_SW_PERIOD {SW_PERIOD}")

    # software rows: flat tables (tile row, count, VRAM bank, tile destination) + per-position record bank/pointer
    rows = [ro for ro in B["rows_out"] if ro["n"] > 0]
    N = len(rows)
    hdr.append(f"#define TITLE_N_SW_ROWS {N}")
    for nm in ("y", "n", "vbank"):
        hdr.append(f"extern const uint8_t title_sw_{nm}[];")
    hdr.append("extern const uint16_t title_sw_tdst[];")
    hdr.append("extern const uint8_t title_sw_maxly[];   /* last VBlank line at which the row's DMAs still fit */")
    desc.append("const uint8_t title_sw_maxly[] = { " + ", ".join(str(152 - -(-(ro["n"] + 2) // 12)) for ro in rows) + " };")
    hdr.append("extern const uint8_t title_sw_rec_bank[];     /* [row * 128 + p] */")
    hdr.append("extern const uint8_t * const title_sw_rec_ptr[];")
    desc.append("const uint8_t title_sw_y[] = { " + ", ".join(str(ro["r"]) for ro in rows) + " };")
    desc.append("const uint8_t title_sw_n[] = { " + ", ".join(str(ro["n"]) for ro in rows) + " };")
    desc.append("const uint8_t title_sw_vbank[] = { " + ", ".join(str(ro["bank"]) for ro in rows) + " };")
    desc.append("const uint16_t title_sw_tdst[] = { " + ", ".join(
        hex(0x9000 + ro["first"] * 16 if ro["first"] < 128 else 0x8800 + (ro["first"] - 128) * 16) for ro in rows) + " };")
    rec_bank, rec_ptr = [], []
    for ro in rows:
        rs = ro["rec_size"]
        per = 6800 // rs
        nchunks = (SW_PERIOD + per - 1) // per
        names = []
        for ci in range(nchunks):
            data = b"".join(ro["recs"][ci * per:(ci + 1) * per])
            nm = f"title_r{ro['r']:02d}_{ci}"
            b = place(nm, data, aligned=True)
            hdr.append(f"extern const uint8_t {nm}[];")
            names.append((nm, b))
        for p_ in range(SW_PERIOD):
            nm, b = names[p_ // per]
            rec_bank.append(str(b))
            rec_ptr.append(f"{nm} + {(p_ % per) * rs}")
    desc.append("const uint8_t title_sw_rec_bank[] = {\n    " + ", ".join(rec_bank) + "\n};")
    desc.append("const uint8_t * const title_sw_rec_ptr[] = {\n    " + ",\n    ".join(rec_ptr) + "\n};")

    # Sonic states: tile data (GDMA'd, aligned) + ready-made OAM bytes for both halves of OBJ VRAM bank 1
    hdr.append(f"#define TITLE_N_SONIC_STATES {len(B['states'])}")
    hdr.append("extern const uint8_t title_son_n[];")
    hdr.append("extern const uint8_t title_son_bank[];")
    hdr.append("extern const uint8_t title_son_maxly[];")
    desc.append("const uint8_t title_son_maxly[] = { " + ", ".join(str(152 - -(-(2 * len(sp)) // 12)) for sp in B["states"]) + " };")
    hdr.append("extern const uint8_t * const title_son_tiles[];")
    hdr.append("extern const uint8_t * const title_son_oam[];   /* [state * 2 + half] */")
    ns, bs, ts_, os_ = [], [], [], []
    for i, sp in enumerate(B["states"]):
        data = b"".join(s_[2] for s_ in sp)
        nm = f"title_son{i:02d}"
        if data:
            b = place(nm, data, aligned=True)
            hdr.append(f"extern const uint8_t {nm}[];")
        else:
            b = CODE_BANK
        for half in (0, 1):
            oam = bytearray()
            for j, s_ in enumerate(sp):
                oam += bytes(((s_[0] + 16) & 255, (s_[1] + 8) & 255, half * 64 + 2 * j, s_[3] | 0x08))
            desc.append(f"static const uint8_t title_son{i:02d}_oam{half}[] = {{ " + (", ".join(str(v) for v in oam) or "0") + " };")
        ns.append(str(len(sp)))
        bs.append(str(b))
        ts_.append(nm if data else "0")
        os_ += [f"title_son{i:02d}_oam0", f"title_son{i:02d}_oam1"]
    desc.append("const uint8_t title_son_n[] = { " + ", ".join(ns) + " };")
    desc.append("const uint8_t title_son_bank[] = { " + ", ".join(bs) + " };")
    desc.append("const uint8_t * const title_son_tiles[] = { " + ", ".join(ts_) + " };")
    desc.append("const uint8_t * const title_son_oam[] = { " + ", ".join(os_) + " };")

    # PSB
    ps = B["psb_sprites"]
    hdr.append(f"#define TITLE_N_PSB {len(ps)}")
    hdr.append("extern const uint8_t title_psb_tiles[];")
    hdr.append("extern const uint8_t title_psb_oam[];")
    desc.append(f"const uint8_t title_psb_tiles[{32 * len(ps)}] = {{\n{c_bytes(b''.join(s_[2] for s_ in ps))}\n}};")
    oam = bytearray()
    for j, s_ in enumerate(ps):
        oam += bytes(((s_[0] + 16) & 255, (s_[1] + 8) & 255, 2 * j, 7))
    desc.append("const uint8_t title_psb_oam[] = { " + ", ".join(str(v) for v in oam) + " };")

    hdr.append("")
    hdr.append("#endif")

    # write
    for f in os.listdir(GEN_DIR):
        if f.startswith("title_b") and f.endswith(".c"):
            os.remove(os.path.join(GEN_DIR, f))
    open(os.path.join(GEN_DIR, "title_data.h"), "w").write("\n".join(hdr) + "\n")
    open(os.path.join(GEN_DIR, f"title_b{CODE_BANK:03d}.c"), "w").write("\n".join(desc) + "\n")
    for b, chunks in files.items():
        txt = f"#pragma bank {b}\n#include <stdint.h>\n/* Generated by tools/title_screen.py */\n\n" + "\n".join(chunks)
        open(os.path.join(GEN_DIR, f"title_b{b:03d}.c"), "w").write(txt)
    used = {b: s - budget[b] for b, s in DATA_BANKS if budget[b] != s}
    print("ROM data placed:", sum(used.values()), "bytes in banks", sorted(used))


if __name__ == "__main__":
    B = build()
    emit(B)
