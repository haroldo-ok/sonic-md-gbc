#!/usr/bin/env python3
"""
High-precision Sega Mega Drive VDP -> Game Boy Color (CGB) graphics & palette compiler.

Features:
  - Exact CRAM 9-bit (0BBB0GGG0RRR) -> CGB 15-bit RGB555 (0BBBBBGGGGGRRRRR) conversion
  - Virtual 2048-tile Mega Drive VRAM builder from Pattern Load Cues + uncompressed animated art
  - Frequency-weighted K-Medoids / Greedy Variance-Minimization 7-palette CGB BG solver (BGP0..BGP6)
  - 8-palette CGB Sprite solver (OBP0..OBP7) giving Sonic simultaneous Blue body + Peach skin + Red/White shoes
  - Flip-canonical (H/V hardware flip aware) 448-slot-per-zone terrain packer plus a reserved 16-slot BG ring overlay; zero placeholders
"""

from __future__ import annotations
import numpy as np


def md_word_to_rgb555(w: int) -> int:
    """Convert Mega Drive big-endian CRAM word (0000BBB0GGG0RRR0) to CGB RGB555."""
    r3 = (w >> 1) & 0x07
    g3 = (w >> 5) & 0x07
    b3 = (w >> 9) & 0x07
    r5 = (r3 * 31 + 3) // 7
    g5 = (g3 * 31 + 3) // 7
    b5 = (b3 * 31 + 3) // 7
    return r5 | (g5 << 5) | (b5 << 10)


def rgb555_to_rgb8(c: int) -> tuple[int, int, int]:
    r = ((c & 0x1F) * 255 + 15) // 31
    g = (((c >> 5) & 0x1F) * 255 + 15) // 31
    b = (((c >> 10) & 0x1F) * 255 + 15) // 31
    return (r, g, b)


def rgb8_to_rgb555(r: int, g: int, b: int) -> int:
    return ((r * 31 + 127) // 255) | (((g * 31 + 127) // 255) << 5) | (((b * 31 + 127) // 255) << 10)


def load_md_palette_bytes(raw: bytes) -> list[int]:
    """Convert big-endian CRAM words into a list of RGB555 ints."""
    out = []
    for i in range(0, len(raw), 2):
        w = (raw[i] << 8) | raw[i + 1]
        out.append(md_word_to_rgb555(w))
    return out


def decode_4bpp_tile(tile32: bytes) -> np.ndarray:
    """Decode a 32-byte Mega Drive 4bpp 8x8 tile into an (8, 8) uint8 array of color indices 0..15."""
    arr = np.zeros((8, 8), dtype=np.uint8)
    for y in range(8):
        row = tile32[y * 4:(y + 1) * 4]
        for x in range(4):
            b = row[x]
            arr[y, x * 2] = (b >> 4) & 0x0F
            arr[y, x * 2 + 1] = b & 0x0F
    return arr


def encode_2bpp_tile(idx8x8: np.ndarray) -> bytes:
    """Encode an (8, 8) uint8 array (values 0..3) into 16-byte Game Boy 2bpp tile data."""
    out = bytearray(16)
    for y in range(8):
        lo = 0
        hi = 0
        for x in range(8):
            v = int(idx8x8[y, x]) & 3
            bit = 7 - x
            lo |= (v & 1) << bit
            hi |= ((v >> 1) & 1) << bit
        out[y * 2] = lo
        out[y * 2 + 1] = hi
    return bytes(out)


def solve_zone_bg_palettes(
    tile_rgb_samples: list[tuple[np.ndarray, int]],
    backdrop_rgb555: int,
    num_pals: int = 7,
) -> list[list[int]]:
    """
    Solve `num_pals` x 4-color CGB BG palettes (BGP0..BGP6) minimizing weighted RGB squared error
    across all rendered 8x8 tiles of a Zone/Screen.

    `tile_rgb_samples`: list of (rgb8_64x3 float32 array, frequency_weight).
    Returns `num_pals` lists of 4 RGB555 values.
    """
    backdrop_rgb = np.array(rgb555_to_rgb8(backdrop_rgb555), dtype=np.float32)

    if not tile_rgb_samples:
        return [[backdrop_rgb555, 0, rgb8_to_rgb555(128, 128, 128), 0x7FFF] for _ in range(num_pals)]

    # Represent each tile by its 4-color local palette (via fast median/quantiles or unique colors)
    tile_pals = []
    weights = []
    for px, w in tile_rgb_samples:
        uniq, counts = np.unique(px.reshape(-1, 3), axis=0, return_counts=True)
        order = np.argsort(-counts)
        uniq = uniq[order]
        if len(uniq) <= 4:
            p = np.zeros((4, 3), dtype=np.float32)
            p[:len(uniq)] = uniq
            for k in range(len(uniq), 4):
                p[k] = uniq[-1]
        else:
            # Keep top 3 frequent colors + weighted centroid of remainder
            p = np.zeros((4, 3), dtype=np.float32)
            p[:3] = uniq[:3]
            rem_w = counts[order[3:]].astype(np.float32)
            p[3] = np.average(uniq[3:], axis=0, weights=rem_w)
        # Sort by luminance so slot order 0..3 matches dark->light cleanly!
        lum = 0.299 * p[:, 0] + 0.587 * p[:, 1] + 0.114 * p[:, 2]
        p = p[np.argsort(lum)]
        tile_pals.append(p)
        weights.append(float(max(w, 1)))

    X = np.stack(tile_pals, axis=0)  # (N, 4, 3)
    W = np.array(weights, dtype=np.float32)

    N = len(X)
    if N <= num_pals:
        centers = [X[i % N].copy() for i in range(num_pals)]
    else:
        # Weighted K-means++ initialization on flattened 12-D sorted 4-color descriptors!
        flat = X.reshape(N, 12)
        rng = np.random.default_rng(19910623)  # Sonic 1 release date seed! Deterministic!
        chosen = [int(np.argmax(W))]
        for _ in range(1, num_pals):
            c_mat = flat[chosen]  # (K, 12)
            dists = np.min(np.sum((flat[:, None, :] - c_mat[None, :, :]) ** 2, axis=2), axis=1)
            probs = dists * W
            total = float(probs.sum())
            if total <= 1e-6:
                chosen.append(len(chosen) % N)
            else:
                chosen.append(int(rng.choice(N, p=probs / total)))
        centers = [X[i].copy() for i in chosen]

        # Refine with pixel-level EM iterations: assign each actual 8x8 tile to best 4-color palette,
        # then update each palette's 4 colors by 4-means over all assigned pixels!
        px_stack = np.stack([px.reshape(64, 3) for px, _ in tile_rgb_samples], axis=0)  # (N, 64, 3)
        for _it in range(10):
            pal_mat = np.stack(centers, axis=0)  # (P, 4, 3)
            # Compute exact per-tile pixel error for each candidate palette p in 0..P-1
            errs = np.zeros((N, num_pals), dtype=np.float32)
            for p_idx in range(num_pals):
                # diff: (N, 64, 4, 3)
                diff = px_stack[:, :, None, :] - pal_mat[p_idx][None, None, :, :]
                min_px_err = np.min(np.sum(diff * diff, axis=3), axis=2)  # (N, 64)
                errs[:, p_idx] = np.sum(min_px_err, axis=1)
            assign = np.argmin(errs, axis=1)

            for p_idx in range(num_pals):
                members = np.where(assign == p_idx)[0]
                if len(members) == 0:
                    worst = int(np.argmax(errs[np.arange(N), assign] * W))
                    centers[p_idx] = X[worst].copy()
                    continue
                # Gather weighted pixels of all tiles assigned to palette `p_idx`
                sub_px = px_stack[members]  # (M, 64, 3)
                sub_w = np.repeat(W[members], 64)  # (M*64,)
                pts = sub_px.reshape(-1, 3)
                cur_c = centers[p_idx].copy()
                for _sub in range(5):
                    d4 = np.sum((pts[:, None, :] - cur_c[None, :, :]) ** 2, axis=2)  # (M*64, 4)
                    ca = np.argmin(d4, axis=1)
                    for c_i in range(4):
                        mask = ca == c_i
                        if np.any(mask):
                            cur_c[c_i] = np.average(pts[mask], axis=0, weights=sub_w[mask])
                lum = 0.299 * cur_c[:, 0] + 0.587 * cur_c[:, 1] + 0.114 * cur_c[:, 2]
                centers[p_idx] = cur_c[np.argsort(lum)]

    # Ensure palette 0 contains the exact backdrop color so empty sky tiles match seamlessly!
    c0_dists = [np.min(np.sum((c - backdrop_rgb[None, :]) ** 2, axis=1)) for c in centers]
    best_bg_pal = int(np.argmin(c0_dists))
    centers[0], centers[best_bg_pal] = centers[best_bg_pal], centers[0]
    nearest_slot = int(np.argmin(np.sum((centers[0] - backdrop_rgb[None, :]) ** 2, axis=1)))
    centers[0][nearest_slot] = backdrop_rgb

    out_rgb555: list[list[int]] = []
    for c in centers:
        pal555 = [
            rgb8_to_rgb555(int(round(rgb[0])), int(round(rgb[1])), int(round(rgb[2])))
            for rgb in c
        ]
        out_rgb555.append(pal555)
    return out_rgb555


# Canonical CGB VRAM BG Tile Slots (464 total):
#   Slots 0..255   -> VBK=0, BG byte 0..255
#   Slots 256..335 -> VBK=1, BG byte 0..79
#   Slots 336..463 -> VBK=1, BG byte 128..255
# convert_all.py packs terrain into slots 0..447; slots 448..463 (VBK=1, tile IDs 240..255) are reserved for stage-ring composites.
BG_VRAM_SLOTS: list[tuple[int, int]] = (
    [(0, t) for t in range(256)]
    + [(1, t) for t in range(80)]
    + [(1, t) for t in range(128, 256)]
)
assert len(BG_VRAM_SLOTS) == 464


def pack_zone_bg_tiles(
    md_vram: list[bytes],
    cram64: list[int],
    used_words_freq: dict[tuple[int, int], int],
    bg_pals_rgb555: list[list[int]],
    max_slots: int = 464,
) -> tuple[dict[tuple[int, int], tuple[int, int, int, int, int]], list[bytes]]:
    """
    Convert EVERY `(pal_line, tile11)` 8x8 tile into:
      - optimal CGB BG palette `pal_id` (0..6)
      - 2bpp index matrix (8,8) in 0..3
      - canonical flip-deduplicated VRAM slot `0..max_slots-1` + `(add_hflip, add_vflip)`

    Returns:
      mapping[(pal_line, tile11)] = (vram_bank, bg_tile_byte, pal_id, add_hflip, add_vflip)
      slot_tiles_2bpp: list of `max_slots` 16-byte 2bpp tile data arrays.
    """
    pal_rgb8 = np.array(
        [[rgb555_to_rgb8(c) for c in pal] for pal in bg_pals_rgb555],
        dtype=np.float32,
    )  # (P, 4, 3)

    # First quantize every (pal_line, tile11) to its minimum-RGB-error CGB palette + 2bpp array
    quantized: dict[tuple[int, int], tuple[int, np.ndarray, int]] = {}
    for (pline, tidx), freq in used_words_freq.items():
        t4 = decode_4bpp_tile(md_vram[tidx if tidx < len(md_vram) else 0])
        px_rgb = np.zeros((8, 8, 3), dtype=np.float32)
        for y in range(8):
            for x in range(8):
                ci = int(t4[y, x])
                c555 = cram64[0] if ci == 0 else cram64[pline * 16 + ci]
                px_rgb[y, x] = rgb555_to_rgb8(c555)

        best_p = 0
        best_err = 1e30
        best_q = None
        flat_px = px_rgb.reshape(64, 3)
        for p_i in range(len(bg_pals_rgb555)):
            diff = flat_px[:, None, :] - pal_rgb8[p_i][None, :, :]  # (64, 4, 3)
            d2 = np.sum(diff * diff, axis=2)  # (64, 4)
            q = np.argmin(d2, axis=1)
            err = float(np.sum(d2[np.arange(64), q]))
            if err < best_err:
                best_err = err
                best_p = p_i
                best_q = q.reshape(8, 8).astype(np.uint8)
        quantized[(pline, tidx)] = (best_p, best_q, freq)

    # Flip canonicalization helper: returns (canon_bytes, hflip_to_get_orig, vflip_to_get_orig)
    # Note: if stored canon tile is C, and hardware applies hflip/vflip to C to render original O:
    #   render(C, hf, vf) == O
    def canonicalize(arr: np.ndarray) -> tuple[bytes, int, int, list[tuple[bytes, int, int]]]:
        variants = [
            (arr, 0, 0),
            (arr[:, ::-1], 1, 0),
            (arr[::-1, :], 0, 1),
            (arr[::-1, ::-1], 1, 1),
        ]
        encoded = [(arr_v.tobytes(), hf, vf) for arr_v, hf, vf in variants]
        best_b, best_hf, best_vf = min(encoded, key=lambda item: item[0])
        return best_b, best_hf, best_vf, encoded

    canon_groups: dict[bytes, int] = {}  # canon_bytes -> total frequency weight
    tile_canon_info: dict[tuple[int, int], tuple[int, bytes, int, int]] = {}

    # Always reserve slot 0 as pure backdrop color index in palette 0!
    bg0_rgb = rgb555_to_rgb8(cram64[0])
    bg0_idx = int(np.argmin(np.sum((pal_rgb8[0] - np.array(bg0_rgb, dtype=np.float32)) ** 2, axis=1)))
    empty_arr = np.full((8, 8), bg0_idx, dtype=np.uint8)
    empty_canon, _, _, _ = canonicalize(empty_arr)
    canon_groups[empty_canon] = 10_000_000

    for key, (p_i, q_arr, freq) in quantized.items():
        cb, hf, vf, _ = canonicalize(q_arr)
        canon_groups[cb] = canon_groups.get(cb, 0) + max(freq, 1)
        tile_canon_info[key] = (p_i, cb, hf, vf)

    # Sort unique canonical tiles by descending usage weight
    sorted_canons = sorted(canon_groups.items(), key=lambda kv: -kv[1])
    kept_canons = [cb for cb, _ in sorted_canons[:max_slots]]
    canon_to_slot: dict[bytes, int] = {cb: idx for idx, cb in enumerate(kept_canons)}

    # Prebuild all 4 orientation matrices of all kept canonical tiles for instant nearest-neighbor matching
    # when `len(sorted_canons) > max_slots` (zero blank/placeholder tiles ever!)
    kept_mats_orient = []
    for slot_idx, cb in enumerate(kept_canons):
        mat = np.frombuffer(cb, dtype=np.uint8).reshape(8, 8)
        for vf in (0, 1):
            for hf in (0, 1):
                m = mat
                if hf:
                    m = m[:, ::-1]
                if vf:
                    m = m[::-1, :]
                kept_mats_orient.append((slot_idx, hf, vf, m.reshape(64)))

    orient_matrix = np.stack([item[3] for item in kept_mats_orient], axis=0)  # (4*M, 64)

    overflow_cache: dict[bytes, tuple[int, int, int]] = {}
    mapping: dict[tuple[int, int], tuple[int, int, int, int, int]] = {}

    for key, (p_i, cb, hf, vf) in tile_canon_info.items():
        if cb in canon_to_slot:
            slot = canon_to_slot[cb]
            vbank, tbyte = BG_VRAM_SLOTS[slot]
            mapping[key] = (vbank, tbyte, p_i, hf, vf)
        else:
            q_raw = quantized[key][1].tobytes()
            if q_raw not in overflow_cache:
                target = np.frombuffer(q_raw, dtype=np.uint8)
                # Weighted visual error (exact match distance + luminance difference)
                diffs = np.sum((orient_matrix.astype(np.int16) - target[None, :].astype(np.int16)) ** 2, axis=1)
                best_idx = int(np.argmin(diffs))
                slot, m_hf, m_vf, _ = kept_mats_orient[best_idx]
                overflow_cache[q_raw] = (slot, m_hf, m_vf)
            slot, m_hf, m_vf = overflow_cache[q_raw]
            vbank, tbyte = BG_VRAM_SLOTS[slot]
            mapping[key] = (vbank, tbyte, p_i, m_hf, m_vf)

    slot_tiles_2bpp = [encode_2bpp_tile(np.frombuffer(cb, dtype=np.uint8).reshape(8, 8)) for cb in kept_canons]
    while len(slot_tiles_2bpp) < max_slots:
        slot_tiles_2bpp.append(bytes(16))

    return mapping, slot_tiles_2bpp


def downsample_m16_block_to_8x8(
    bwords: tuple[int, int, int, int],
    md_vram: list[bytes],
    cram64: list[int],
) -> tuple[np.ndarray, bool, bool]:
    """
    Composite a full Mega Drive 16x16 `map16` block (2x2 8x8 4bpp tiles with exact CRAM palette lines
    and subtile H/V flips) and crisp alpha-aware downsample `2x` (`16x16 -> 8x8` half size!).

    Returns `(rgb8x8 float32 (8,8,3), has_high_prio bool, has_sky_px bool)`.
    """
    rgb16 = np.zeros((16, 16, 3), dtype=np.float32)
    fg16 = np.zeros((16, 16), dtype=bool)
    bg0 = np.array(rgb555_to_rgb8(cram64[0]), dtype=np.float32)
    prio_cnt = 0

    for q in range(4):
        w = bwords[q]
        tidx = w & 0x07FF
        hf = (w >> 11) & 1
        vf = (w >> 12) & 1
        pline = (w >> 13) & 3
        if (w >> 15) & 1:
            prio_cnt += 1

        t4 = decode_4bpp_tile(md_vram[tidx if tidx < len(md_vram) else 0])
        if hf:
            t4 = t4[:, ::-1]
        if vf:
            t4 = t4[::-1, :]

        oy = (q >> 1) * 8
        ox = (q & 1) * 8
        for y in range(8):
            for x in range(8):
                ci = int(t4[y, x])
                if ci == 0:
                    rgb16[oy + y, ox + x] = bg0
                else:
                    fg16[oy + y, ox + x] = True
                    rgb16[oy + y, ox + x] = rgb555_to_rgb8(cram64[pline * 16 + ci])

    total_fg = int(np.sum(fg16))
    min_fg_in_cell = 1 if (0 < total_fg <= 32) else 2

    out8 = np.zeros((8, 8, 3), dtype=np.float32)
    sky_cells = 0
    for y in range(8):
        for x in range(8):
            cell_fg = fg16[y * 2:y * 2 + 2, x * 2:x * 2 + 2]
            cnt = int(np.sum(cell_fg))
            if cnt >= min_fg_in_cell:
                cell_px = rgb16[y * 2:y * 2 + 2, x * 2:x * 2 + 2][cell_fg]
                out8[y, x] = np.mean(cell_px, axis=0)
            else:
                out8[y, x] = bg0
                sky_cells += 1

    return out8, (prio_cnt >= 3), (sky_cells > 0)


def pack_half_scale_m16_blocks(
    block_rgb_list: list[np.ndarray],
    block_prio_list: list[bool],
    block_sky_list: list[bool],
    block_weights: dict[int, int],
    cram64: list[int],
    bg_pals_rgb555: list[list[int]],
    max_slots: int = 464,
) -> tuple[bytearray, list[bytes], int]:
    """
    Pack EVERY downsampled `8x8` `map16` block (`bi` in `0..num_m16-1`) into:
      - `m16_gbc`: `bytearray(num_m16 * 2)` where `m16_gbc[bi*2] = tile_byte`, `m16_gbc[bi*2+1] = cgb_attr`
      - `slot_tiles_2bpp`: list of `max_slots` 16-byte 2bpp tile data arrays
      - `unique_canon_count`: count of unique flip-canonical 8x8 tiles; if unique patterns exceed `max_slots`, low-frequency patterns are approximated by the nearest retained tile

    Guarantees ZERO sky seams AND ZERO sky bleed into opaque checkerboard/brick shadows:
      - `bg_pals_rgb555[0..3]` (4 sky-backed palettes) include exact `cram64[0]` backdrop color in slot 0
        so every sky-touching tile (`palm trees`, `flowers`, `clouds`, `pillars`, `lamps`) blends seamlessly!
      - `bg_pals_rgb555[4..6]` (3 solid interior palettes) keep pure rich dark shadows (`chocolate brown`,
        `charcoal steel`, `obsidian lava shadow`) with NO sky color overriding their dark slot!
    """
    # Force slot 0 of sky-capable palettes 0..3 to exact Zone backdrop color `cram64[0]`!
    for p_sky in range(min(4, len(bg_pals_rgb555))):
        p_rgb = np.array([rgb555_to_rgb8(c) for c in bg_pals_rgb555[p_sky]], dtype=np.float32)
        bg_rgb = np.array(rgb555_to_rgb8(cram64[0]), dtype=np.float32)
        nearest = int(np.argmin(np.sum((p_rgb - bg_rgb[None, :]) ** 2, axis=1)))
        bg_pals_rgb555[p_sky][nearest] = bg_pals_rgb555[p_sky][0]
        bg_pals_rgb555[p_sky][0] = cram64[0]

    pal_rgb8 = np.array(
        [[rgb555_to_rgb8(c) for c in pal] for pal in bg_pals_rgb555],
        dtype=np.float32,
    )
    num_m16 = len(block_rgb_list)

    def canonicalize(arr: np.ndarray) -> tuple[bytes, int, int]:
        variants = [
            (arr.tobytes(), 0, 0),
            (arr[:, ::-1].tobytes(), 1, 0),
            (arr[::-1, :].tobytes(), 0, 1),
            (arr[::-1, ::-1].tobytes(), 1, 1),
        ]
        return min(variants, key=lambda item: item[0])

    bg0_rgb = np.array(rgb555_to_rgb8(cram64[0]), dtype=np.float32)
    bg0_idx = int(np.argmin(np.sum((pal_rgb8[0] - bg0_rgb[None, :]) ** 2, axis=1)))
    empty_arr = np.full((8, 8), bg0_idx, dtype=np.uint8)
    empty_canon, _, _ = canonicalize(empty_arr)

    canon_groups: dict[bytes, int] = {empty_canon: 10_000_000}
    quantized: list[tuple[int, np.ndarray, bytes, int, int]] = []

    for bi in range(num_m16):
        if bi == 0:
            quantized.append((0, empty_arr, empty_canon, 0, 0))
            continue
        flat_px = block_rgb_list[bi].reshape(64, 3)
        best_p = 0
        best_err = 1e30
        best_q = empty_arr
        cand_pals = range(min(4, len(bg_pals_rgb555))) if block_sky_list[bi] else range(len(bg_pals_rgb555))
        for p_i in cand_pals:
            diff = flat_px[:, None, :] - pal_rgb8[p_i][None, :, :]
            d2 = np.sum(diff * diff, axis=2)
            q = np.argmin(d2, axis=1)
            err = float(np.sum(d2[np.arange(64), q]))
            if err < best_err:
                best_err = err
                best_p = p_i
                best_q = q.reshape(8, 8).astype(np.uint8)
        cb, hf, vf = canonicalize(best_q)
        canon_groups[cb] = canon_groups.get(cb, 0) + max(block_weights.get(bi, 1), 1)
        quantized.append((best_p, best_q, cb, hf, vf))

    sorted_canons = sorted(canon_groups.items(), key=lambda kv: -kv[1])
    kept_canons = [cb for cb, _ in sorted_canons[:max_slots]]
    canon_to_slot: dict[bytes, int] = {cb: idx for idx, cb in enumerate(kept_canons)}

    kept_mats_orient = []
    for slot_idx, cb in enumerate(kept_canons):
        mat = np.frombuffer(cb, dtype=np.uint8).reshape(8, 8)
        for vf in (0, 1):
            for hf in (0, 1):
                m = mat[:, ::-1] if hf else mat
                m = m[::-1, :] if vf else m
                kept_mats_orient.append((slot_idx, hf, vf, m.reshape(64)))
    orient_matrix = np.stack([item[3] for item in kept_mats_orient], axis=0)

    m16_gbc = bytearray(num_m16 * 2)
    for bi, (p_i, q_arr, cb, hf, vf) in enumerate(quantized):
        if bi == 0:
            m16_gbc[0] = 0
            m16_gbc[1] = 0
            continue
        if cb in canon_to_slot:
            slot = canon_to_slot[cb]
        else:
            diffs = np.sum((orient_matrix.astype(np.int16) - q_arr.reshape(64)[None, :].astype(np.int16)) ** 2, axis=1)
            best_idx = int(np.argmin(diffs))
            slot, hf, vf, _ = kept_mats_orient[best_idx]
        vbank, tbyte = BG_VRAM_SLOTS[slot]
        prio = 0x80 if block_prio_list[bi] else 0x00
        attr = (
            (p_i & 0x07)
            | (0x08 if vbank else 0x00)
            | (0x20 if hf else 0x00)
            | (0x40 if vf else 0x00)
            | prio
        )
        m16_gbc[bi * 2] = tbyte
        m16_gbc[bi * 2 + 1] = attr

    slot_tiles_2bpp = [encode_2bpp_tile(np.frombuffer(cb, dtype=np.uint8).reshape(8, 8)) for cb in kept_canons]
    while len(slot_tiles_2bpp) < max_slots:
        slot_tiles_2bpp.append(bytes(16))

    return m16_gbc, slot_tiles_2bpp, len(sorted_canons)

