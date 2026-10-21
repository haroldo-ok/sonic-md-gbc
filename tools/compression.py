#!/usr/bin/env python3
"""
Exact bit-accurate decompressors for Sega Mega Drive Sonic 1 formats:
  - Nemesis (.nem)  — 4bpp planar/packed tile art compression
  - Kosinski (.kos) — LZSS general binary/chunk compression
  - Enigma (.eni)   — 16-bit VDP word packet compression (map16, sslayout, tilemaps)
"""

from __future__ import annotations


def decompress_nemesis(data: bytes, offset: int = 0) -> bytes:
    """Decompress a Nemesis-compressed stream into 4bpp Mega Drive 8x8 tile bytes (32 bytes/tile)."""
    pos = offset
    if pos + 2 > len(data):
        raise ValueError("Nemesis stream too short")

    header = (data[pos] << 8) | data[pos + 1]
    pos += 2
    xor_mode = bool(header & 0x8000)
    num_tiles = header & 0x7FFF

    # Build 256-entry lookup table indexed by 8-bit peek: (code_len, repeat_count, palette_idx)
    lut = [(0, 0, 0)] * 256
    pal_idx = 0
    while True:
        b = data[pos]
        pos += 1
        if b == 0xFF:
            break
        if b & 0x80:
            pal_idx = b & 0x0F
            b = data[pos]
            pos += 1
        repeat = ((b >> 4) & 0x07) + 1
        code_len = b & 0x0F
        code = data[pos]
        pos += 1
        if code_len == 0 or code_len > 8:
            raise ValueError(f"Invalid Nemesis code length {code_len}")
        shift = 8 - code_len
        base = (code << shift) & 0xFF
        span = 1 << shift
        entry = (code_len, repeat, pal_idx)
        for i in range(span):
            lut[base + i] = entry

    # Bit reader
    bit_buf = 0
    bits_left = 0

    def ensure_bits(n: int) -> None:
        nonlocal pos, bit_buf, bits_left
        while bits_left < n:
            val = data[pos] if pos < len(data) else 0
            pos += 1
            bit_buf = (bit_buf << 8) | val
            bits_left += 8

    def peek8() -> int:
        ensure_bits(8)
        return (bit_buf >> (bits_left - 8)) & 0xFF

    def consume(n: int) -> int:
        nonlocal bit_buf, bits_left
        ensure_bits(n)
        bits_left -= n
        val = (bit_buf >> bits_left) & ((1 << n) - 1)
        bit_buf &= (1 << bits_left) - 1
        return val

    total_rows = num_tiles * 8
    out = bytearray(num_tiles * 32)
    out_pos = 0
    prev_row = 0
    cur_row = 0
    nybbles_in_row = 0
    rows_written = 0

    while rows_written < total_rows:
        idx = peek8()
        if (idx >> 2) == 0b111111:
            consume(6)
            repeat = consume(3) + 1
            pal = consume(4)
        else:
            code_len, repeat, pal = lut[idx]
            if code_len == 0:
                raise ValueError(f"Unmapped Nemesis prefix 0x{idx:02X} at row {rows_written}/{total_rows}")
            consume(code_len)

        for _ in range(repeat):
            cur_row = ((cur_row << 4) | pal) & 0xFFFFFFFF
            nybbles_in_row += 1
            if nybbles_in_row == 8:
                if xor_mode:
                    prev_row ^= cur_row
                    row_val = prev_row
                else:
                    row_val = cur_row
                out[out_pos] = (row_val >> 24) & 0xFF
                out[out_pos + 1] = (row_val >> 16) & 0xFF
                out[out_pos + 2] = (row_val >> 8) & 0xFF
                out[out_pos + 3] = row_val & 0xFF
                out_pos += 4
                cur_row = 0
                nybbles_in_row = 0
                rows_written += 1
                if rows_written >= total_rows:
                    break

    return bytes(out)


def decompress_kosinski(data: bytes, offset: int = 0) -> bytes:
    """Decompress a Kosinski-compressed stream matching 68000 KosDec bit refill timing."""
    pos = offset
    desc = data[pos] | (data[pos + 1] << 8)
    pos += 2
    desc_bits = 16
    out = bytearray()

    def get_bit() -> int:
        nonlocal pos, desc, desc_bits
        bit = desc & 1
        desc >>= 1
        desc_bits -= 1
        if desc_bits == 0:
            desc = (data[pos] | (data[pos + 1] << 8)) if pos + 1 < len(data) else 0
            pos += 2
            desc_bits = 16
        return bit

    def get_byte() -> int:
        nonlocal pos
        b = data[pos]
        pos += 1
        return b

    while True:
        if get_bit() == 1:
            out.append(get_byte())
        else:
            if get_bit() == 0:
                b1 = get_bit()
                b0 = get_bit()
                count = ((b1 << 1) | b0) + 2
                disp = get_byte() - 256  # negative distance (-256..-1)
            else:
                low = get_byte()
                high = get_byte()
                count_bits = high & 0x07
                disp = ((high & 0xF8) << 5) | low
                disp -= 0x2000  # negative 13-bit distance (-8192..-1)
                if count_bits != 0:
                    count = count_bits + 2
                else:
                    ext = get_byte()
                    if ext == 0:
                        break
                    if ext == 1:
                        continue
                    count = ext + 1

            src_idx = len(out) + disp
            for _ in range(count):
                out.append(out[src_idx])
                src_idx += 1

    return bytes(out)


def decompress_enigma(data: bytes, start_art_tile: int = 0, offset: int = 0) -> bytes:
    """Decompress an Enigma-compressed 16-bit big-endian VDP word stream."""
    pos = offset
    inline_bits = data[pos]
    render_flags = data[pos + 1]
    inc_word = ((data[pos + 2] << 8) | data[pos + 3]) & 0xFFFF
    static_word = ((data[pos + 4] << 8) | data[pos + 5]) & 0xFFFF
    pos += 6

    bit_buf = 0
    bits_left = 0

    def get_bits(n: int) -> int:
        nonlocal pos, bit_buf, bits_left
        if n == 0:
            return 0
        while bits_left < n:
            val = data[pos] if pos < len(data) else 0
            pos += 1
            bit_buf = (bit_buf << 8) | val
            bits_left += 8
        bits_left -= n
        val = (bit_buf >> bits_left) & ((1 << n) - 1)
        bit_buf &= (1 << bits_left) - 1
        return val

    flag_positions = [15, 14, 13, 12, 11]  # priority, pal1, pal0, vflip, hflip

    def read_inline_word() -> int:
        flags = 0
        for i, bit_pos in enumerate(flag_positions):
            if render_flags & (1 << (4 - i)):
                if get_bits(1):
                    flags |= 1 << bit_pos
        tile = get_bits(inline_bits)
        return (flags | tile) & 0xFFFF

    out = bytearray()

    def emit_word(w: int) -> None:
        w = (w + start_art_tile) & 0xFFFF
        out.append((w >> 8) & 0xFF)
        out.append(w & 0xFF)

    while True:
        first = get_bits(1)
        if first == 0:
            mode = get_bits(1)
            count = get_bits(4) + 1
            if mode == 0:
                for _ in range(count):
                    emit_word(inc_word)
                    inc_word = (inc_word + 1) & 0xFFFF
            else:
                for _ in range(count):
                    emit_word(static_word)
        else:
            mode = get_bits(2)
            count_raw = get_bits(4)
            if mode == 3 and count_raw == 0x0F:
                break
            count = count_raw + 1
            if mode == 0:
                w = read_inline_word()
                for _ in range(count):
                    emit_word(w)
            elif mode == 1:
                w = read_inline_word()
                for _ in range(count):
                    emit_word(w)
                    w = (w + 1) & 0xFFFF
            elif mode == 2:
                w = read_inline_word()
                for _ in range(count):
                    emit_word(w)
                    w = (w - 1) & 0xFFFF
            elif mode == 3:
                for _ in range(count):
                    w = read_inline_word()
                    emit_word(w)

    return bytes(out)
