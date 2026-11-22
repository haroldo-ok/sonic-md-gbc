#!/usr/bin/env python3
"""
Render the Sonic 1 title screen as separate layers, at Mega Drive resolution, straight from s1disasm.

GM_Title (sonic.asm) builds the screen from:
  * plane B: the GHZ background (`levels/ghzbg.bin`, `map256/GHZ.kos`, `map16/GHZ.eni`, `8x8 - GHZ1/2.nem`),
    scrolled per line by Deform_GHZ (clouds / mountains / hills / water bands);
  * plane A: the emblem (`tilemaps/Title Screen.eni`, `Title Screen Foreground.nem` at $200), fixed in place;
  * sprites: big Sonic (object 0E, `_maps/Title Screen Sonic.asm`, `Title Screen Sonic.nem` at $300),
    "PRESS START BUTTON" and "TM" (object 0F, `_maps/Press Start and TM.asm`).

Every layer is returned as an RGBA image (alpha 0 = transparent pixel), so the GBC builder
(`tools/title_screen.py`) can scroll the background under the emblem and animate Sonic in front of / behind it.
"""
from __future__ import annotations

import os
import re
import sys

import numpy as np
from PIL import Image

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
from compression import decompress_nemesis, decompress_kosinski, decompress_enigma  # noqa: E402
from gfx_converter import decode_4bpp_tile, md_word_to_rgb555, rgb555_to_rgb8  # noqa: E402

S1_DIR = os.path.join(os.path.dirname(TOOLS_DIR), "vendor", "s1disasm")

# Deform_GHZ band heights (REV01) for the title screen. The BG Y scroll is `$20 - (camera_y >> 5)`; GM_Title
# loads GHZ1's sizes, so the camera Y is the act's start camera and the top cloud band loses that many lines.
BAND_NAMES = ("cloud1", "cloud2", "cloud3", "mountains", "hills", "water")


def _num(tok: str) -> int:
    tok = tok.strip()
    neg = tok.startswith("-")
    if neg:
        tok = tok[1:]
    v = int(tok[1:], 16) if tok.startswith("$") else int(tok, 0)
    return -v if neg else v


def parse_mappings(path: str) -> list[list[dict]]:
    """Parse a `_maps/*.asm` file (mappingsTable / spritePiece macros) into frames of pieces."""
    text = open(path, encoding="latin-1").read()
    entries = re.findall(r"mappingsTableEntry\.w\s+([.\w+]+)", text)
    labels: dict[str, list[dict]] = {}
    cur = None
    for line in text.splitlines():
        line = line.split(";")[0].rstrip()
        m = re.match(r"^(\.\w+):", line)
        if m:
            cur = m.group(1)
            labels[cur] = []
            continue
        m = re.search(r"spritePiece\s+(.*)$", line)
        if m and cur is not None:
            a = [_num(t) for t in m.group(1).split(",")]
            labels[cur].append(dict(x=a[0], y=a[1], w=a[2], h=a[3], tile=a[4], hf=a[5], vf=a[6], pal=a[7], pri=a[8]))
    frames = []
    for e in entries:
        # `.psb+1` is the "invisible frame" hack (points at a 0 byte): treat it as an empty frame.
        frames.append([] if "+" in e else labels[e])
    return frames


def load_pal(name: str) -> list[tuple[int, int, int]]:
    raw = open(os.path.join(S1_DIR, "palette", name), "rb").read()
    return [rgb555_to_rgb8(md_word_to_rgb555((raw[i] << 8) | raw[i + 1])) for i in range(0, len(raw), 2)]


class TitleVRAM:
    def __init__(self) -> None:
        self.tiles: list[np.ndarray] = [np.zeros((8, 8), np.uint8)] * 2048

    def load_nem(self, fname: str, base: int) -> None:
        raw = decompress_nemesis(open(os.path.join(S1_DIR, "artnem", fname), "rb").read())
        for k in range(len(raw) // 32):
            self.tiles[base + k] = decode_4bpp_tile(raw[k * 32:(k + 1) * 32])

    def tile(self, word: int) -> np.ndarray:
        t = self.tiles[word & 0x7FF]
        if word & 0x0800:
            t = t[:, ::-1]
        if word & 0x1000:
            t = t[::-1, :]
        return t


def _blit_tile(rgba: np.ndarray, t: np.ndarray, pal: list, line: int, x0: int, y0: int) -> None:
    h, w = rgba.shape[:2]
    for yy in range(8):
        y = y0 + yy
        if not 0 <= y < h:
            continue
        for xx in range(8):
            x = x0 + xx
            ci = int(t[yy, xx])
            if ci and 0 <= x < w:
                r, g, b = pal[line * 16 + ci]
                rgba[y, x] = (r, g, b, 255)


def render_layers() -> dict:
    pal = load_pal("Title Screen.bin")
    vram = TitleVRAM()
    vram.load_nem("8x8 - GHZ1.nem", 0x000)
    vram.load_nem("8x8 - GHZ2.nem", 0x1CD)
    vram.load_nem("Title Screen Foreground.nem", 0x200)
    vram.load_nem("Title Screen Sonic.nem", 0x300)
    vram.load_nem("Title Screen TM.nem", 0x510)

    # ---- GHZ background chunks (plane B) --------------------------------------------------------------------
    m16 = decompress_enigma(open(os.path.join(S1_DIR, "map16", "GHZ.eni"), "rb").read())
    m256 = decompress_kosinski(open(os.path.join(S1_DIR, "map256", "GHZ.kos"), "rb").read())
    bg_layout = open(os.path.join(S1_DIR, "levels", "ghzbg.bin"), "rb").read()

    def render_chunk(cid: int) -> np.ndarray:
        img = np.zeros((256, 256, 4), np.uint8)
        for by in range(16):
            for bx in range(16):
                c_off = (cid - 1) * 512 + (by * 16 + bx) * 2
                bw = (m256[c_off] << 8) | m256[c_off + 1]
                bidx, bhf, bvf = bw & 0x3FF, bool(bw & 0x800), bool(bw & 0x1000)
                for sy in range(2):
                    for sx in range(2):
                        ssx, ssy = (1 - sx) if bhf else sx, (1 - sy) if bvf else sy
                        mo = bidx * 8 + (ssy * 2 + ssx) * 2
                        tw = (m16[mo] << 8) | m16[mo + 1]
                        tw ^= (0x800 if bhf else 0) | (0x1000 if bvf else 0)
                        _blit_tile(img, vram.tile(tw), pal, (tw >> 13) & 3, bx * 16 + sx * 8, by * 16 + sy * 8)
        return img

    chunk_ids = sorted(set(bg_layout[2:2 + bg_layout[0] + 1]))
    chunks = {cid: render_chunk(cid) for cid in chunk_ids}
    layout_row = list(bg_layout[2:2 + bg_layout[0] + 1])

    # ---- Emblem (plane A, copied to vram_fg+$206: row 4, column 3; 34x22 cells) -------------------------------
    t_eni = decompress_enigma(open(os.path.join(S1_DIR, "tilemaps", "Title Screen.eni"), "rb").read(), 0)
    emb_lo = np.zeros((224, 320, 4), np.uint8)
    emb_hi = np.zeros((224, 320, 4), np.uint8)
    for row in range(22):
        for col in range(34):
            i = (row * 34 + col) * 2
            tw = (t_eni[i] << 8) | t_eni[i + 1]
            dst = emb_hi if tw & 0x8000 else emb_lo
            _blit_tile(dst, vram.tile(tw), pal, (tw >> 13) & 3, (col + 3) * 8, (row + 4) * 8)

    # ---- Sprites ---------------------------------------------------------------------------------------------
    def render_frame(frames, idx: int, base: int, pal_line: int, size=(160, 160), org=(80, 80)) -> np.ndarray:
        img = np.zeros((size[1], size[0], 4), np.uint8)
        # The VDP draws the first piece of a mapping on top: paint in reverse so earlier pieces win.
        for p in reversed(frames[idx]):
            for cx in range(p["w"]):
                for cy in range(p["h"]):
                    t = vram.tiles[(base + p["tile"] + cx * p["h"] + cy) & 0x7FF]
                    if p["hf"]:
                        t = t[:, ::-1]
                    if p["vf"]:
                        t = t[::-1, :]
                    dx = org[0] + p["x"] + ((p["w"] - 1 - cx) if p["hf"] else cx) * 8
                    dy = org[1] + p["y"] + ((p["h"] - 1 - cy) if p["vf"] else cy) * 8
                    _blit_tile(img, t, pal, (p["pal"] + pal_line) & 3, dx, dy)
        return img

    tson = parse_mappings(os.path.join(S1_DIR, "_maps", "Title Screen Sonic.asm"))
    psb = parse_mappings(os.path.join(S1_DIR, "_maps", "Press Start and TM.asm"))
    # TitleSonic: obGfx = ArtTile_Title_Sonic | Tile_Pal2 (line 1); the sprite's origin is (obX, obScreenY).
    sonic_frames = [render_frame(tson, f, 0x300, 1, (128, 128), (0, 0)) for f in range(len(tson))]
    psb_img = render_frame(psb, 1, 0x200, 0, (320, 16), (0, 0))   # "PRESS START BUTTON", origin top-left
    tm_img = render_frame(psb, 3, 0x510, 1, (32, 16), (8, 4))      # "TM", centred at (8, 4)

    return dict(pal=pal, chunks=chunks, layout_row=layout_row, emb_lo=emb_lo, emb_hi=emb_hi,
                sonic_frames=sonic_frames, psb=psb_img, tm=tm_img)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/title_layers"
    os.makedirs(out, exist_ok=True)
    L = render_layers()
    for cid, img in L["chunks"].items():
        Image.fromarray(img).save(os.path.join(out, f"chunk_{cid:02d}.png"))
    Image.fromarray(L["emb_lo"]).save(os.path.join(out, "emb_lo.png"))
    Image.fromarray(L["emb_hi"]).save(os.path.join(out, "emb_hi.png"))
    for i, f in enumerate(L["sonic_frames"]):
        Image.fromarray(f).save(os.path.join(out, f"sonic_{i}.png"))
    Image.fromarray(L["psb"]).save(os.path.join(out, "psb.png"))
    Image.fromarray(L["tm"]).save(os.path.join(out, "tm.png"))
    print("layout", L["layout_row"])
