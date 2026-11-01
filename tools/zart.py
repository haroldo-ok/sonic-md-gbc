"""Zone art helpers: build the per-zone MD VRAM tile image from the PLCs and render object frames."""
import os, re, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghz_props as G
from compression import decompress_nemesis
from gfx_converter import decode_4bpp_tile, load_md_palette_bytes, rgb555_to_rgb8
S1 = G.S1_DIR
_src = open(os.path.join(S1, "sonic.asm")).read()
_plc = open(os.path.join(S1, "_inc", "Pattern Load Cues.asm")).read()
_const = open(os.path.join(S1, "_Constants.asm")).read()
NEM = dict(re.findall(r"^(Nem_\w+):\s*binclude\s+\"artnem/([^\"]+)\"", _src, re.M))
def const(name):
    m = re.search(r"^%s:?\s+equ\s+(.+?)(?:\s*;.*)?$" % re.escape(name), _const, re.M)
    expr = m.group(1).strip()
    def sub(mm): return str(const(mm.group(0)))
    expr = re.sub(r"ArtTile_\w+", sub, expr).replace("$", "0x")
    return eval(expr)
def plc(name):
    m = re.search(r"^%s:\s+plcheader\n(.*?)^%s_end:" % (name, name), _plc, re.M | re.S)
    out = []
    for l in m.group(1).splitlines():
        mm = re.match(r"\s*plcm\s+(Nem_\w+),\s*([^;\s]+(?:\s*[+]\s*\$?\w+)?)", l)
        if mm:
            out.append((mm.group(1), mm.group(2).replace(" ", "")))
    return out
def _tile_expr(e):
    e = re.sub(r"ArtTile_\w+", lambda m: str(const(m.group(0))), e).replace("$", "0x")
    return eval(e)
def nem_tiles(fname):
    raw = decompress_nemesis(open(os.path.join(S1, "artnem", fname), "rb").read())
    return [decode_4bpp_tile(raw[i*32:(i+1)*32]) for i in range(len(raw)//32)]
def vram_image(plcs, extra=()):
    img = {}
    for nm, base in list(plc(plcs[0])) + [p for q in plcs[1:] for p in plc(q)] + list(extra):
        b = _tile_expr(base) if isinstance(base, str) else base
        for i, t in enumerate(nem_tiles(NEM[nm])):
            img[b + i] = t
    return img
def pal_lines(binname):
    cr = load_md_palette_bytes(open(os.path.join(S1, "palette", binname), "rb").read())
    return [[rgb555_to_rgb8(c) for c in cr[i*16:(i+1)*16]] for i in range(len(cr)//16)]
def sonic_line():
    cr = load_md_palette_bytes(open(os.path.join(S1, "palette", "Sonic.bin"), "rb").read())
    return [rgb555_to_rgb8(c) for c in cr[:16]]
