#!/usr/bin/env python3
"""
Compile Sonic 1 sound effects ($A0..$D0, including Jump $A0) from vendor/s1disasm
into 8,192 Hz 4-bit nibble-packed PCM streams for real-time 3-voice layered mixing
on Game Boy Color Channel 3 (Wave RAM $FF30..$FF3F).

Each sample is noise-shaped and quantized to 0..7 centered at 4 (silence = 0x44).
Two samples are packed per byte (high nibble plays first). Keeping each voice in 0..7
allows nibble-parallel addition in 8-bit SM83 arithmetic with zero nibble carry:
  - 1 active voice (a):        a + a                   -> 0..14 (center 8, 100% vol)
  - 2 active voices (a, b):    a + b                   -> 0..14 (center 8, 50%/50% sum)
  - 3 active voices (a, b, c): (((a + b) >> 1) & 0x77) + c -> 0..14 (center 8, 25%/25%/50%)
"""
from __future__ import annotations

import math
import os
import sys

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from sfx_smps import load_sfx, simulate          # noqa: E402
from md_synth import render_tracks, SR as MD_SR   # noqa: E402

TARGET_SR = 8192.0
CUTOFF_HZ = 3600.0
TAIL_FRAMES = 24
MAX_DURATION_S = 3.0
MAX_BANK_BYTES = 16000
START_BANK = 128
TABLE_BANK = 8
SFX_FIRST = 0xA0
SFX_LAST = 0xD0


def lowpass_sinc(sig: np.ndarray, sr: float, cutoff_hz: float, num_taps: int = 65) -> np.ndarray:
    """Zero-phase Blackman-windowed sinc FIR low-pass filter."""
    if len(sig) == 0:
        return sig
    m = np.arange(num_taps, dtype=np.float64) - (num_taps - 1) / 2.0
    h = np.sinc(2.0 * (cutoff_hz / sr) * m) * np.blackman(num_taps)
    h /= np.sum(h)
    return np.convolve(sig, h, mode="same")


def trim_trailing_silence(sig: np.ndarray, sr: float) -> np.ndarray:
    """Trim trailing silence where 20 ms sliding RMS stays below 0.5% of peak, leaving ~30 ms tail."""
    if len(sig) == 0:
        return sig
    peak = float(np.max(np.abs(sig)))
    if peak < 1e-6:
        return sig[:min(len(sig), int(0.05 * sr))]
    win = max(1, int(0.020 * sr))
    sq = sig * sig
    kernel = np.ones(win, dtype=np.float64) / win
    rms = np.sqrt(np.maximum(0.0, np.convolve(sq, kernel, mode="same")))
    thresh = 0.005 * peak
    active = np.flatnonzero(rms > thresh)
    if len(active) == 0:
        return sig[:min(len(sig), int(0.05 * sr))]
    end = min(len(sig), int(active[-1]) + int(0.030 * sr))
    max_samples = int(MAX_DURATION_S * sr)
    return sig[:min(end, max_samples)]


def shift_fm_octave(frames: list[list[dict]], oct_shift: int) -> list[list[dict]]:
    """Shift YM2612 3-bit octave block on FM channels (clamped 0..7) so ultrasonics stay inside 8,192 Hz PCM passband."""
    shifted: list[list[dict]] = []
    for snap in frames:
        ns: list[dict] = []
        for ch in snap:
            c = dict(ch)
            if c.get("fm") and c.get("freq"):
                w = int(c["freq"])
                blk = max(0, min(7, ((w >> 11) & 7) + oct_shift))
                c["freq"] = (blk << 11) | (w & 0x7FF)
            ns.append(c)
        shifted.append(ns)
    return shifted


def condition_and_quantize(
    mono: np.ndarray,
    drive: float = 1.8,
    cutoff_hz: float = CUTOFF_HZ,
    noise_shape: float = 0.5,
    gate_thresh: float = 1e-4,
    max_dur_s: float = MAX_DURATION_S,
    fade_out_s: float = 0.0,
) -> bytes:
    """Filter, resample to 8192 Hz, soft-compress, fade tail, pad to 32-sample block, and pack 0..7 nibbles."""
    trimmed = trim_trailing_silence(mono.astype(np.float64), MD_SR)
    if len(trimmed) == 0 or float(np.max(np.abs(trimmed))) < 1e-6:
        return b"\x44" * 16

    max_md = max(1, int(max_dur_s * MD_SR))
    if len(trimmed) > max_md:
        trimmed = trimmed[:max_md].copy()
    if fade_out_s > 0.0:
        n_fade = min(len(trimmed), max(1, int(fade_out_s * MD_SR)))
        trimmed = trimmed.copy()
        trimmed[-n_fade:] *= np.linspace(1.0, 0.0, n_fade, dtype=np.float64) ** 1.5

    filtered = lowpass_sinc(trimmed, MD_SR, cutoff_hz, num_taps=65)
    n_out = max(32, int(math.ceil(len(filtered) * (TARGET_SR / MD_SR))))
    max_out = int(max_dur_s * TARGET_SR)
    n_out = min(n_out, max_out)
    t_out = np.arange(n_out, dtype=np.float64) * (MD_SR / TARGET_SR)
    t_in = np.arange(len(filtered), dtype=np.float64)
    resampled = np.interp(t_out, t_in, filtered)

    peak = float(np.max(np.abs(resampled)))
    if peak > 1e-9:
        y = resampled / peak
        # Gentle soft-knee compression keeps quiet chime tails audible in 3-bit depth.
        y = np.tanh(drive * y) / math.tanh(drive)
    else:
        y = np.zeros_like(resampled)

    fade_n = min(16, len(y))
    if fade_n > 0:
        y[-fade_n:] *= np.linspace(1.0, 0.0, fade_n, dtype=np.float64)

    rem = len(y) % 32
    if rem:
        y = np.pad(y, (0, 32 - rem), mode="constant", constant_values=0.0)

    # 3-bit (0..7) quantization centered at 4 with optional 1st-order noise shaping and silence gate.
    q = np.empty(len(y), dtype=np.uint8)
    err = 0.0
    for i, x in enumerate(y):
        fx = float(x)
        if abs(fx) < gate_thresh:
            q[i] = 4
            err = 0.0
        else:
            v = 4.0 + 3.5 * fx + noise_shape * err
            qi = int(min(7, max(0, round(v))))
            q[i] = qi
            err = max(-0.5, min(0.5, v - qi))

    packed = ((q[0::2] << 4) | q[1::2]).astype(np.uint8)
    # Sanity invariant: every nibble must strictly lie in 0..7.
    hi = packed >> 4
    lo = packed & 0x0F
    assert np.all(hi <= 7) and np.all(lo <= 7), "Nibble out of 0..7 range!"
    assert len(packed) % 16 == 0, "Packed stream must be a multiple of 16 bytes (32 samples)"
    return packed.tobytes()


def compile_all_pcm(asm_dir: str):
    srcs, tables = load_sfx(asm_dir)
    pri = tables["priorities"]
    compiled = {}
    for sid in range(SFX_FIRST, SFX_LAST + 1):
        if sid not in srcs:
            continue
        src = srcs[sid]
        fr = simulate(src, tables)
        if sid == 0xBE:
            # SFX_ROLL / Spin Dash: -1 octave fits 2.2-3.1 kHz cleanly; disable dithering + 0.80s gated tail removes high hiss.
            fr = shift_fm_octave(fr, -1)
            nfr = len(fr) + TAIL_FRAMES
            sigs, _ = render_tracks(fr, nfr)
            mono = np.sum(sigs, axis=0) if sigs else np.zeros(32, dtype=np.float64)
            pcm = condition_and_quantize(
                mono,
                drive=2.0,
                cutoff_hz=3200.0,
                noise_shape=0.0,
                gate_thresh=0.14,
                max_dur_s=0.80,
                fade_out_s=0.22,
            )
        else:
            nfr = len(fr) + TAIL_FRAMES
            sigs, _ = render_tracks(fr, nfr)
            mono = np.sum(sigs, axis=0) if sigs else np.zeros(32, dtype=np.float64)
            pcm = condition_and_quantize(mono)
        prio = pri[sid - 0x81] if (sid - 0x81) < len(pri) else 0x70
        compiled[sid] = {
            "sid": sid,
            "name": src.name,
            "prio": prio,
            "pcm": pcm,
            "blocks": len(pcm) // 16,
        }
    return compiled


def pack_into_banks(compiled: dict[int, dict]) -> list[tuple[int, list[dict]]]:
    """Greedy first-fit decreasing bin-packing into 16 KB ROM banks starting at START_BANK."""
    items = sorted(compiled.values(), key=lambda x: len(x["pcm"]), reverse=True)
    bins: list[dict] = []
    for item in items:
        sz = len(item["pcm"])
        assert sz <= MAX_BANK_BYTES, f"SFX 0x{item['sid']:02X} ({sz} bytes) exceeds bank limit"
        placed = False
        for b in bins:
            if b["size"] + sz <= MAX_BANK_BYTES:
                b["items"].append(item)
                b["size"] += sz
                placed = True
                break
        if not placed:
            bins.append({"bank": START_BANK + len(bins), "size": sz, "items": [item]})

    for b in bins:
        b["items"].sort(key=lambda x: x["sid"])
        for it in b["items"]:
            it["bank"] = b["bank"]
    return [(b["bank"], b["items"]) for b in bins]


def emit_files(compiled: dict[int, dict], banks: list[tuple[int, list[dict]]], out_dir: str) -> None:
    os.makedirs(out_dir, exist_ok=True)
    # Remove any stale sfx_pcm_b*.c files from previous runs.
    for fn in os.listdir(out_dir):
        if fn.startswith("sfx_pcm_b") and fn.endswith(".c"):
            os.remove(os.path.join(out_dir, fn))

    # 1. Header
    hdr = [
        "/* Generated by tools/sfx_pcm.py. Do not edit. */",
        "#ifndef SFX_PCM_H",
        "#define SFX_PCM_H",
        "#include <stdint.h>",
        "",
        f"#define SFX_PCM_TABLE_BANK {TABLE_BANK}",
        f"#define SFX_PCM_FIRST 0x{SFX_FIRST:02X}U",
        f"#define SFX_PCM_LAST  0x{SFX_LAST:02X}U",
        f"#define SFX_PCM_COUNT {SFX_LAST - SFX_FIRST + 1}U",
        "",
        "typedef struct {",
        "    uint8_t bank;",
        "    uint8_t prio;",
        "    uint16_t blocks;",
        "    const uint8_t *data;",
        "} SfxPcmDesc;",
        "",
        f"extern const SfxPcmDesc sfx_pcm_table[{SFX_LAST - SFX_FIRST + 1}];",
        "#endif",
        "",
    ]
    with open(os.path.join(out_dir, "sfx_pcm.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(hdr))

    # 2. Data bank files (sfx_pcm_b128.c, ...)
    for bank, items in banks:
        lines = [
            f"#pragma bank {bank}",
            "#include <stdint.h>",
            "/* Generated by tools/sfx_pcm.py from s1disasm SFX sources. Do not edit. */",
            "",
        ]
        for it in items:
            sid = it["sid"]
            pcm = it["pcm"]
            lines.append(f"/* 0x{sid:02X} {it['name']} ({len(pcm)} bytes, {it['blocks']} blocks) */")
            lines.append(f"const uint8_t sfx_pcm_{sid:02x}[{len(pcm)}] = {{")
            for i in range(0, len(pcm), 24):
                lines.append("    " + ", ".join(f"0x{b:02X}" for b in pcm[i:i + 24]) + ",")
            lines.append("};")
            lines.append("")
        path = os.path.join(out_dir, f"sfx_pcm_b{bank}.c")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))

    # 3. Descriptor table in Bank 8
    tlines = [
        f"#pragma bank {TABLE_BANK}",
        '#include "sfx_pcm.h"',
        "/* Generated by tools/sfx_pcm.py. Do not edit. */",
        "",
    ]
    for sid in range(SFX_FIRST, SFX_LAST + 1):
        if sid in compiled:
            tlines.append(f"extern const uint8_t sfx_pcm_{sid:02x}[];")
    tlines.append("")
    tlines.append(f"const SfxPcmDesc sfx_pcm_table[{SFX_LAST - SFX_FIRST + 1}] = {{")
    for sid in range(SFX_FIRST, SFX_LAST + 1):
        if sid in compiled:
            it = compiled[sid]
            tlines.append(
                f"    {{ {it['bank']:3d}U, 0x{it['prio']:02X}U, {it['blocks']:4d}U, sfx_pcm_{sid:02x} }}, /* 0x{sid:02X} {it['name']} */"
            )
        else:
            tlines.append(f"    {{   0U, 0x00U,    0U, 0 }}, /* 0x{sid:02X} unused */")
    tlines.append("};")
    tlines.append("")
    with open(os.path.join(out_dir, "sfx_pcm_table.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(tlines))


def main() -> None:
    root = os.path.dirname(TOOLS)
    asm_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "vendor", "s1disasm")
    out_dir = os.path.join(root, "generated")
    compiled = compile_all_pcm(asm_dir)
    banks = pack_into_banks(compiled)
    emit_files(compiled, banks, out_dir)

    total_bytes = sum(len(it["pcm"]) for it in compiled.values())
    for sid in sorted(compiled):
        it = compiled[sid]
        dur_s = (it["blocks"] * 32) / TARGET_SR
        print(
            f"0x{sid:02X} {it['name']:32s} bank={it['bank']:3d} prio=0x{it['prio']:02X} "
            f"blocks={it['blocks']:4d} ({len(it['pcm']):5d} B, {dur_s:.3f}s)"
        )
    print(f"Total: {len(compiled)} SFX, {total_bytes} bytes across {len(banks)} ROM banks ({banks[0][0]}..{banks[-1][0]})")


if __name__ == "__main__":
    main()
