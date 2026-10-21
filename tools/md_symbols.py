#!/usr/bin/env python3
"""
Builds vendor/s1disasm bit-perfectly and parses the complete ASL symbol table + binary readers.
"""

from __future__ import annotations
import os
import re
import struct
import subprocess
from dataclasses import dataclass

S1_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "vendor", "s1disasm"))


@dataclass
class SpritePiece:
    y: int          # signed pixel offset from center
    width: int      # width in 8x8 tiles (1..4)
    height: int     # height in 8x8 tiles (1..4)
    prio: bool
    pal: int        # 0..3
    vflip: bool
    hflip: bool
    tile: int       # 11-bit tile offset relative to object base art tile
    x: int          # signed pixel offset from center


class S1DisasmROM:
    def __init__(self, s1_root: str = S1_ROOT) -> None:
        self.root = s1_root
        self.rom_path = os.path.join(self.root, "s1built.bin")
        self.lst_path = os.path.join(self.root, "sonic.lst")
        self._ensure_built()
        with open(self.rom_path, "rb") as f:
            self.rom = f.read()
        self.symbols: dict[str, int] = {}
        self.symbols_ci: dict[str, int] = {}
        self._parse_symbols()

    def _ensure_built(self) -> None:
        if not os.path.exists(self.rom_path):
            subprocess.run(["lua", "build.lua"], cwd=self.root, check=True)
            subprocess.run(["lua", "build_tools/chkbitperfect.lua"], cwd=self.root, check=True)
        if not os.path.exists(self.lst_path):
            subprocess.run(
                [
                    "./build_tools/Linux-x86_64/asl",
                    "-xx", "-n", "-q", "-A", "-L", "-s",
                    "-U", "-E", "-i", ".", "sonic.asm",
                ],
                cwd=self.root,
                check=True,
            )

    def _parse_symbols(self) -> None:
        pat = re.compile(r"\*?([A-Za-z0-9_.$]+)\s*:\s*([0-9A-Fa-f]+)\s+[CD-]")
        with open(self.lst_path, "r", errors="replace") as f:
            text = f.read()
        # Symbol table starts near end after "Symbol Table" or last quarter
        for m in pat.finditer(text):
            name = m.group(1)
            val = int(m.group(2), 16)
            self.symbols[name] = val
            self.symbols_ci[name.lower()] = val

    def sym(self, name: str) -> int:
        if name in self.symbols:
            return self.symbols[name]
        low = name.lower()
        if low in self.symbols_ci:
            return self.symbols_ci[low]
        raise KeyError(f"Symbol not found: {name}")

    def u8(self, addr: int) -> int:
        return self.rom[addr]

    def s8(self, addr: int) -> int:
        v = self.rom[addr]
        return v - 256 if v >= 128 else v

    def u16(self, addr: int) -> int:
        return struct.unpack_from(">H", self.rom, addr)[0]

    def s16(self, addr: int) -> int:
        return struct.unpack_from(">h", self.rom, addr)[0]

    def u32(self, addr: int) -> int:
        return struct.unpack_from(">I", self.rom, addr)[0]

    def slice(self, start: int, end: int) -> bytes:
        return self.rom[start:end]

    def parse_plc(self, plc_id: int) -> list[tuple[int, int]]:
        """Return list of (rom_nem_addr, vram_tile_index) for PLC `plc_id`."""
        alc = self.sym("ArtLoadCues")
        rel = self.s16(alc + plc_id * 2)
        pos = alc + rel
        count = self.s16(pos) + 1
        pos += 2
        entries = []
        for _ in range(count):
            gfx_addr = self.u32(pos) & 0x00FFFFFF
            vram_bytes = self.u16(pos + 4)
            entries.append((gfx_addr, vram_bytes // 32))
            pos += 6
        return entries

    def parse_sprite_mapping_table(self, base_addr: int, num_frames: int) -> list[list[SpritePiece]]:
        """Parse Sonic 1 sprite mapping table of `num_frames` word offsets."""
        frames: list[list[SpritePiece]] = []
        for i in range(num_frames):
            off = self.s16(base_addr + i * 2)
            if off == 0 and i > 0:
                frames.append([])
                continue
            pos = base_addr + off
            piece_count = self.u8(pos)
            pos += 1
            pieces: list[SpritePiece] = []
            for _ in range(piece_count):
                y = self.s8(pos)
                size = self.u8(pos + 1)
                attr = self.u16(pos + 2)
                x = self.s8(pos + 4)
                pos += 5
                w = ((size >> 2) & 3) + 1
                h = (size & 3) + 1
                prio = bool(attr & 0x8000)
                pal = (attr >> 13) & 3
                vflip = bool(attr & 0x1000)
                hflip = bool(attr & 0x0800)
                tile = attr & 0x07FF
                pieces.append(SpritePiece(y, w, h, prio, pal, vflip, hflip, tile, x))
            frames.append(pieces)
        return frames

    def parse_sonic_dplc(self, num_frames: int) -> list[list[tuple[int, int]]]:
        """Parse SonicDynPLC: for each frame returns list of (num_tiles, start_tile_in_sonic_art)."""
        base = self.sym("SonicDynPLC")
        frames = []
        for i in range(num_frames):
            off = self.s16(base + i * 2)
            pos = base + off
            cnt = self.u8(pos)
            pos += 1
            cues = []
            for _ in range(cnt):
                entry = self.u16(pos)
                pos += 2
                num_t = ((entry >> 12) & 0x0F) + 1
                start_t = entry & 0x0FFF
                cues.append((num_t, start_t))
            frames.append(cues)
        return frames
