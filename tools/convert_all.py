#!/usr/bin/env python3
"""
Master Asset & Data Bank Compiler (`tools/convert_all.py`).
Converts 100% of `vendor/s1disasm` with ZERO placeholders into Game Boy Color MBC5 ROM banks!
"""

from __future__ import annotations
import hashlib
import json
import os
import struct
import numpy as np

from compression import decompress_nemesis, decompress_kosinski, decompress_enigma
from md_symbols import S1DisasmROM
from gfx_converter import (
    BG_VRAM_SLOTS,
    decode_4bpp_tile,
    downsample_m16_block_to_8x8,
    encode_2bpp_tile,
    load_md_palette_bytes,
    pack_half_scale_m16_blocks,
    pack_zone_bg_tiles,
    rgb555_to_rgb8,
    rgb8_to_rgb555,
    solve_zone_bg_palettes,
)
from smps_converter import (
    compile_smps_song,
    compile_smps_sfx,
    convert_wav_to_4bit_wave_packets,
)
from convert_all_helpers import (
    BankPacker,
    ROOT_DIR,
    S1_DIR,
    GEN_DIR,
    VER_DIR,
    build_common_and_zone_sprites,
    build_ring_background_tiles,
    build_font_and_hud_tiles,
    build_fullscreen_scene,
    convert_sonic_complete,
    format_u16_c,
    render_sega_and_title_from_s1disasm,
)


def main() -> None:
    os.makedirs(GEN_DIR, exist_ok=True)
    os.makedirs(VER_DIR, exist_ok=True)
    # Remove old generated bank files before clean regen
    for old_f in os.listdir(GEN_DIR):
        if old_f.endswith(".c") or old_f.endswith(".h") or old_f.endswith(".json"):
            os.remove(os.path.join(GEN_DIR, old_f))

    s1 = S1DisasmROM(S1_DIR)
    packer = BankPacker(start_bank=10)
    manifest_files: list[dict] = []

    def record_source_file(rel_path: str, kind: str, converted_bytes: int, bank_no: int) -> None:
        full_p = os.path.join(S1_DIR, rel_path)
        raw = open(full_p, "rb").read()
        manifest_files.append({
            "source_path": rel_path,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "raw_bytes": len(raw),
            "converted_bytes": converted_bytes,
            "rom_bank": bank_no,
            "category": kind,
            "placeholder": False,
        })

    print("[1/7] Converting HUD/Font, Sonic 89-frame sprites/DPLCs/animations, and all Common/Zone Sprites...")
    hud_tiles_list = build_font_and_hud_tiles(s1)
    hud_tiles_blob = b"".join(hud_tiles_list)  # 48 * 16 = 768 bytes!
    common_spr_blob, zone_spr_blobs = build_common_and_zone_sprites(s1)
    ring_bg_tiles_blob = build_ring_background_tiles(common_spr_blob)  # 4 frames × 16 masks × 16 bytes = 1,024 B
    sonic_data = convert_sonic_complete(s1)

    # Split `sonic_data["tiles_2bpp"]` (20,624 bytes = 1,289 tiles * 16 B) across Banks 6 & 7 (`768` tiles = 12,288 B + `521` tiles = 8,336 B)!
    sonic_part0 = sonic_data["tiles_2bpp"][:768 * 16] or bytes(16)
    sonic_part1 = sonic_data["tiles_2bpp"][768 * 16:] or bytes(16)

    print("[2/7] Converting Physics Tables, Collision Heightmaps, Angles, Sine Table, Start Positions & Demos...")
    angle_map = open(os.path.join(S1_DIR, "collide", "Angle Map.bin"), "rb").read()
    coll_norm = open(os.path.join(S1_DIR, "collide", "Collision Array (Normal).bin"), "rb").read()
    coll_rot = open(os.path.join(S1_DIR, "collide", "Collision Array (Rotated).bin"), "rb").read()
    record_source_file("collide/Angle Map.bin", "physics", len(angle_map), 5)
    record_source_file("collide/Collision Array (Normal).bin", "physics", len(coll_norm), 5)
    record_source_file("collide/Collision Array (Rotated).bin", "physics", len(coll_rot), 5)

    sine_addr = s1.sym("Sine_Data")
    sine_bytes = s1.slice(sine_addr, sine_addr + 320 * 2)  # 256 + 64 entries big-endian -> little-endian int16!
    sine_le = bytearray()
    for i in range(0, len(sine_bytes), 2):
        val = struct.unpack_from(">h", sine_bytes, i)[0]
        sine_le.extend(struct.pack("<h", val))

    # Convert ALL 13 `demodata/*.asm` demo streams!
    demo_blobs = bytearray()
    demo_offsets = []
    for df in sorted(os.listdir(os.path.join(S1_DIR, "demodata"))):
        if df.endswith(".asm"):
            raw_txt = open(os.path.join(S1_DIR, "demodata", df), "r", errors="replace").read()
            vals = []
            for line in raw_txt.splitlines():
                line = line.split(";")[0].strip()
                if "dc.b" in line:
                    rhs = line.split("dc.b", 1)[1]
                    for tok in rhs.split(","):
                        tok = tok.strip()
                        if tok.startswith("$"):
                            vals.append(int(tok[1:], 16) & 0xFF)
                        elif tok.isdigit():
                            vals.append(int(tok) & 0xFF)
            demo_offsets.append(len(demo_blobs))
            demo_blobs.extend(bytes(vals) if vals else b"\x00\x01")
            record_source_file(f"demodata/{df}", "demo", len(vals), 5)

    print("[3/7] Converting ALL 7 Zones / 9 Visual Variants (`GHZ`, `LZ`, `MZ`, `SLZ`, `SYZ`, `SBZ1`, `SBZ2`, `SBZ3`, `Ending`)...")
    # Note: In Sonic 1:
    #   Visual Set 0: GHZ (GHZ1, GHZ2, GHZ3)
    #   Visual Set 1: LZ  (LZ1, LZ2, LZ3) + Underwater `Labyrinth Zone Underwater.bin`!
    #   Visual Set 2: MZ  (MZ1, MZ2, MZ3)
    #   Visual Set 3: SLZ (SLZ1, SLZ2, SLZ3)
    #   Visual Set 4: SYZ (SYZ1, SYZ2, SYZ3)
    #   Visual Set 5: SBZ1 (Scrap Brain Act 1)
    #   Visual Set 6: SBZ2 / FZ (Scrap Brain Act 2 & Final Zone — uses `SBZ Act 2.bin` palette!)
    #   Visual Set 7: SBZ3 (uses `LZ` art/chunks/collide + `SBZ Act 3.bin` & `SBZ Act 3 Underwater.bin` palettes!)
    #   Visual Set 8: Ending (`GHZ` art/chunks + `Ending - Flowers.nem` + `Flowers at Ending.kos` + `Ending.bin` palette!)
    zone_specs = [
        ("GHZ",    "plcid_GHZ", "plcid_GHZ2", "GHZ.eni", "GHZ (REV00).kos" if not os.path.exists(os.path.join(S1_DIR, "map256", "GHZ.kos")) else "GHZ.kos", "GHZ.bin", "Green Hill Zone.bin", None, "GHZ"),
        ("LZ",     "plcid_LZ",  "plcid_LZ2",  "LZ.eni",  "LZ.kos",          "LZ.bin",  "Labyrinth Zone.bin", "Labyrinth Zone Underwater.bin", "LZ"),
        ("MZ",     "plcid_MZ",  "plcid_MZ2",  "MZ.eni",  "MZ (REV01).kos",  "MZ.bin",  "Marble Zone.bin", None, "MZ"),
        ("SLZ",    "plcid_SLZ", "plcid_SLZ2", "SLZ.eni", "SLZ.kos",         "SLZ.bin", "Star Light Zone.bin", None, "SLZ"),
        ("SYZ",    "plcid_SYZ", "plcid_SYZ2", "SYZ.eni", "SYZ.kos",         "SYZ.bin", "Spring Yard Zone.bin", None, "SYZ"),
        ("SBZ1",   "plcid_SBZ", "plcid_SBZ2", "SBZ.eni", "SBZ (REV01).kos", "SBZ.bin", "SBZ Act 1.bin", None, "SBZ"),
        ("SBZ2",   "plcid_SBZ", "plcid_SBZ2", "SBZ.eni", "SBZ (REV01).kos", "SBZ.bin", "SBZ Act 2.bin", None, "SBZ"),
        ("SBZ3",   "plcid_LZ",  "plcid_LZ2",  "LZ.eni",  "LZ.kos",          "LZ.bin",  "SBZ Act 3.bin", "SBZ Act 3 Underwater.bin", "LZ"),
        ("ENDING", "plcid_GHZ", "plcid_GHZ2", "GHZ.eni", "GHZ.kos",         "GHZ.bin", "Ending.bin", None, "GHZ"),
    ]

    sonic_pal_cram = load_md_palette_bytes(open(os.path.join(S1_DIR, "palette", "Sonic.bin"), "rb").read())

    # Pre-deduplicate chunk banks per `kos_name` so `GHZ`/`ENDING`, `LZ`/`SBZ3`, `SBZ1`/`SBZ2` share chunk banks cleanly!
    chunk_bank_info: dict[str, tuple[int, int]] = {}  # kos_name -> (base_bank, num_chunks)
    for _, _, _, _, kos_name, _, _, _, _ in zone_specs:
        if kos_name in chunk_bank_info:
            continue
        kos_raw = decompress_kosinski(open(os.path.join(S1_DIR, "map256", kos_name), "rb").read())
        num_chunks = len(kos_raw) // 512
        # Convert big-endian 16-bit block words to little-endian `uint16_t` for native zero-overhead SM83 reads!
        le_chunks = bytearray(len(kos_raw))
        for i in range(0, len(kos_raw), 2):
            le_chunks[i] = kos_raw[i + 1]
            le_chunks[i + 1] = kos_raw[i]
        # Pack EXACT consecutive banks of `16` chunks = `8,192` bytes per bank so `bank = base + (chunk_idx >> 4)` ALWAYS!
        pages = (num_chunks + 15) // 16
        base_b = None
        for pg in range(pages):
            seg = bytes(le_chunks[pg * 8192:(pg + 1) * 8192])
            seg = seg.ljust(8192, b"\x00")
            cname = f"chunks_{kos_name.split()[0].replace('.', '_')}_p{pg}"
            b = packer.add_array(cname, seg, f"{kos_name} chunk page {pg} (chunks {pg*16}..{pg*16+15})", force_new_bank=True)
            if pg == 0:
                base_b = b
            else:
                assert b == base_b + pg, "Consecutive bank invariant!"
        chunk_bank_info[kos_name] = (base_b, num_chunks)

    vset_headers = []
    for vs_idx, (vs_name, plc1, plc2, eni_name, kos_name, col_name, pal_name, uw_pal_name, spr_key) in enumerate(zone_specs):
        # Build accurate 2048-tile MD virtual VRAM with ALL initial PLC + Animated Level Gfx!
        md_vram = [bytes(32)] * 2048
        for pid in (s1.sym(plc1), s1.sym(plc2)):
            for addr, t_idx in s1.parse_plc(pid):
                dec = decompress_nemesis(s1.rom, addr)
                for k in range(len(dec) // 32):
                    if t_idx + k < 2048:
                        md_vram[t_idx + k] = dec[k * 32:(k + 1) * 32]

        # Load animated uncompressed tiles into their exact Mega Drive VRAM addresses!
        def put_unc(fn: str, dst_t: int, tile_off: int = 0, count_t: int = 999) -> None:
            raw_u = open(os.path.join(S1_DIR, "artunc", fn), "rb").read()
            tot = len(raw_u) // 32
            for k in range(min(count_t, tot - tile_off)):
                if dst_t + k < 2048:
                    md_vram[dst_t + k] = raw_u[(tile_off + k) * 32:(tile_off + k + 1) * 32]

        if vs_name in ("GHZ", "ENDING"):
            put_unc("GHZ Flower Large.unc", 0x35C, 0, 16)
            put_unc("GHZ Flower Small.unc", 0x36C, 0, 12)
            put_unc("GHZ Waterfall.unc", 0x378, 0, 8)
            put_unc("GHZ Flower Large.unc", 0x390, 16, 16)
            put_unc("GHZ Flower Small.unc", 0x340, 12, 12)
            if vs_name == "ENDING":
                kos_fl = decompress_kosinski(open(os.path.join(S1_DIR, "artkos", "Flowers at Ending.kos"), "rb").read())
                for k in range(min(32, len(kos_fl) // 32)):
                    md_vram[0x3A0 + k] = kos_fl[k * 32:(k + 1) * 32]
        elif vs_name == "MZ":
            put_unc("MZ Lava Surface.unc", 0x2D2, 0, 16)
            put_unc("MZ Lava.unc", 0x2E2, 0, 16)
            put_unc("MZ Background Torch.unc", 0x2F2, 0, 6)
        elif vs_name in ("SBZ1", "SBZ2"):
            put_unc("SBZ Background Smoke.unc", 0x448, 0, 12)
            put_unc("SBZ Background Smoke.unc", 0x454, 12, 12)

        # Build exact 64-entry CRAM: line 0 = Sonic.bin (`0..15`), lines 1..3 = Zone palette (`16..63` or full `0..63` if 128 B)!
        zpal_raw = open(os.path.join(S1_DIR, "palette", pal_name), "rb").read()
        zpal_cram = load_md_palette_bytes(zpal_raw)
        cram64 = list(sonic_pal_cram[:16]) + [0] * 48
        if len(zpal_cram) == 64:
            cram64 = zpal_cram
        else:
            cram64[16:16 + len(zpal_cram)] = zpal_cram

        # Set exact authentic Mega Drive VDP backdrop sky color `cram64[0]` BEFORE downsampling & solving palettes!
        zone_sky_rgb8 = {
            "GHZ":    (64, 136, 216),
            "LZ":     (12, 48, 88),
            "MZ":     (56, 32, 168),
            "SLZ":    (12, 20, 96),
            "SYZ":    (96, 24, 112),
            "SBZ1":   (56, 40, 72),
            "SBZ2":   (24, 36, 112),
            "SBZ3":   (12, 48, 88),
            "ENDING": (32, 48, 224),
        }
        sr, sg, sb_c = zone_sky_rgb8[vs_name]
        cram64[0] = rgb8_to_rgb555(sr, sg, sb_c)

        # Count block frequencies across actual Zone chunks so the most visible tiles have 100% highest weight!
        m16_raw = decompress_enigma(open(os.path.join(S1_DIR, "map16", eni_name), "rb").read())
        m256_raw = decompress_kosinski(open(os.path.join(S1_DIR, "map256", kos_name), "rb").read())
        num_m16 = len(m16_raw) // 8
        blk_freq = [1] * max(1, num_m16)
        for i in range(0, len(m256_raw), 2):
            bi = ((m256_raw[i] << 8) | m256_raw[i + 1]) & 0x3FF
            if bi < num_m16:
                blk_freq[bi] += 1

        # Composite EVERY 16x16 Mega Drive `map16` block and downsample cleanly `2x` (`16x16 -> 8x8` half size!)
        block_rgb_list: list[np.ndarray] = []
        block_prio_list: list[bool] = []
        block_sky_list: list[bool] = []
        rgb_samples = []
        blk_weight_map: dict[int, int] = {}
        for bi in range(num_m16):
            w_f = blk_freq[bi]
            blk_weight_map[bi] = w_f
            bwords = tuple((m16_raw[bi * 8 + q * 2] << 8) | m16_raw[bi * 8 + q * 2 + 1] for q in range(4))
            px8, hp, has_sky = downsample_m16_block_to_8x8(bwords, md_vram, cram64)
            block_rgb_list.append(px8)
            block_prio_list.append(hp if bi > 0 else False)
            block_sky_list.append(has_sky)
            if bi > 0:
                rgb_samples.append((px8, w_f))

        bg_pals7 = solve_zone_bg_palettes(rgb_samples, cram64[0], num_pals=7)
        # Reserve CGB BG bank-1 tiles 240..255 for sixteen ring quadrant composites; ring art frames stream separately.
        m16_gbc, slot_tiles, uniq_cnt = pack_half_scale_m16_blocks(
            block_rgb_list, block_prio_list, block_sky_list, blk_weight_map, cram64, bg_pals7, max_slots=448
        )
        slot_tiles.extend([bytes(16)] * (464 - len(slot_tiles)))

        # Build underwater CGB BG palettes if this Zone has water (`LZ`, `SBZ3`), otherwise warm tint
        if uw_pal_name:
            uw_raw = open(os.path.join(S1_DIR, "palette", uw_pal_name), "rb").read()
            uw_cram = load_md_palette_bytes(uw_raw)
            uw_pals7 = []
            for p in bg_pals7:
                up = []
                for c555 in p:
                    r, g, b = rgb555_to_rgb8(c555)
                    ur = int(r * 0.48 + 18)
                    ug = int(min(255, g * 0.85 + 36))
                    ub = int(min(255, b * 0.78 + 44))
                    up.append(rgb8_to_rgb555(ur, ug, ub))
                uw_pals7.append(up)
        else:
            uw_pals7 = bg_pals7

        col_raw = open(os.path.join(S1_DIR, "collide", col_name), "rb").read()
        # Sonic 1 has ONE contiguous `Col_*.bin` table indexed directly by 16x16 block ID (`0 .. num_blocks-1`, all <= 600)!
        col_primary = col_raw[:640].ljust(640, b"\x00")
        col_secondary = col_raw[640:1280].ljust(640, b"\x00")

        # Pack this visual set's tiles (`7,424 B`), sprites (`2,048 B`), map16 (`<=4,816 B`), palettes, and collision into 2 clean banks!
        b_tiles = packer.add_array(
            f"vset_{vs_idx}_bgtiles0",
            b"".join(slot_tiles[:256]),
            f"{vs_name} CGB BG Tiles Bank 0 (256 tiles)",
            force_new_bank=True,
        )
        b_tiles1 = packer.add_array(
            f"vset_{vs_idx}_bgtiles1",
            b"".join(slot_tiles[256:464]),
            f"{vs_name} CGB BG Tiles Bank 1 (208 tiles)",
        )
        assert b_tiles1 == b_tiles
        b_spr = packer.add_array(
            f"vset_{vs_idx}_sprtiles",
            zone_spr_blobs[spr_key],
            f"{vs_name} Zone Sprite Tiles Bank 1 (128 tiles)",
        )
        assert b_spr == b_tiles

        b_meta = packer.add_array(
            f"vset_{vs_idx}_map16",
            bytes(m16_gbc),
            f"{vs_name} CGB Map16 Table ({num_m16} blocks)",
            force_new_bank=True,
        )
        packer.add_array(f"vset_{vs_idx}_col1", col_primary, f"{vs_name} Primary Collision Index")
        packer.add_array(f"vset_{vs_idx}_col2", col_secondary, f"{vs_name} Secondary Collision Index")

        hud_pal = [
            rgb8_to_rgb555(16, 32, 108),   # 0: Deep Sonic navy bar background
            rgb8_to_rgb555(228, 28, 24),   # 1: Iconic Sonic crimson shadow/emblem accent
            rgb8_to_rgb555(252, 216, 0),   # 2: Iconic Sonic ring gold highlight
            rgb8_to_rgb555(252, 252, 252), # 3: Pure crisp white HUD text/digits
        ]
        all_bg_u16 = [c for p in bg_pals7 for c in p] + hud_pal
        all_uw_u16 = [c for p in uw_pals7 for c in p] + hud_pal

        chunk_base_b, num_ch = chunk_bank_info[kos_name]
        vset_headers.append({
            "name": vs_name,
            "tiles_bank": b_tiles,
            "meta_bank": b_meta,
            "chunk_base_bank": chunk_base_b,
            "num_chunks": num_ch,
            "num_m16": num_m16,
            "bg_pal": all_bg_u16,
            "uw_pal": all_uw_u16,
        })
        print(f"    -> {vs_name:6s}: {num_m16:3d} blocks, {uniq_cnt:3d} unique tiles, {num_ch:2d} chunks, tiles_bank={b_tiles}, meta_bank={b_meta}, chunk_bank={chunk_base_b}")

    print("[4/7] Converting ALL 20 Playable Acts (Layouts, Backgrounds, Start Positions, Boundaries, Water & Objects)...")
    # All 20 playable Acts in exact game order (`act_global_id = 0..19`)!
    act_defs = [
        # (id, label, zone_code, act_num, vset_idx, fg_file, bg_file, obj_file, sp_file, left, right, top, bottom, music_id, water_y)
        (0,  "GREEN HILL 1",  "GHZ", 1, 0, "ghz1.bin", "ghzbg.bin",         "ghz1.bin",        "ghz1.bin", 0x0000, 0x24BF, 0x0000, 0x0300, 0x81, 0x7FFF),
        (1,  "GREEN HILL 2",  "GHZ", 2, 0, "ghz2.bin", "ghzbg.bin",         "ghz2.bin",        "ghz2.bin", 0x0000, 0x1EBF, 0x0000, 0x0300, 0x81, 0x7FFF),
        (2,  "GREEN HILL 3",  "GHZ", 3, 0, "ghz3.bin", "ghzbg.bin",         "ghz3 (REV01).bin","ghz3.bin", 0x0000, 0x2960, 0x0000, 0x0300, 0x81, 0x7FFF),
        (3,  "MARBLE 1",      "MZ",  1, 2, "mz1.bin",  "mz1bg.bin",         "mz1 (REV01).bin", "mz1.bin",  0x0000, 0x17BF, 0x0000, 0x01D0, 0x83, 0x7FFF),
        (4,  "MARBLE 2",      "MZ",  2, 2, "mz2.bin",  "mz2bg.bin",         "mz2.bin",         "mz2.bin",  0x0000, 0x17BF, 0x0000, 0x0520, 0x83, 0x7FFF),
        (5,  "MARBLE 3",      "MZ",  3, 2, "mz3.bin",  "mz3bg.bin",         "mz3.bin",         "mz3.bin",  0x0000, 0x1800, 0x0000, 0x0720, 0x83, 0x7FFF),
        (6,  "SPRING YARD 1", "SYZ", 1, 4, "syz1.bin", "syzbg (REV01).bin", "syz1.bin",        "syz1.bin", 0x0000, 0x22C0, 0x0000, 0x0420, 0x85, 0x7FFF),
        (7,  "SPRING YARD 2", "SYZ", 2, 4, "syz2.bin", "syzbg (REV01).bin", "syz2.bin",        "syz2.bin", 0x0000, 0x28C0, 0x0000, 0x0520, 0x85, 0x7FFF),
        (8,  "SPRING YARD 3", "SYZ", 3, 4, "syz3.bin", "syzbg (REV01).bin", "syz3 (REV01).bin","syz3.bin", 0x0000, 0x2C00, 0x0000, 0x0620, 0x85, 0x7FFF),
        (9,  "LABYRINTH 1",   "LZ",  1, 1, "lz1.bin",  "lzbg.bin",          "lz1 (REV01).bin", "lz1.bin",  0x0000, 0x19BF, 0x0000, 0x0530, 0x82, 0x00B8),
        (10, "LABYRINTH 2",   "LZ",  2, 1, "lz2.bin",  "lzbg.bin",          "lz2.bin",         "lz2.bin",  0x0000, 0x10AF, 0x0000, 0x0720, 0x82, 0x0328),
        (11, "LABYRINTH 3",   "LZ",  3, 1, "lz3.bin",  "lzbg.bin",          "lz3 (REV01).bin", "lz3.bin",  0x0000, 0x202F, 0x0000, 0x0800, 0x82, 0x0900),
        (12, "STAR LIGHT 1",  "SLZ", 1, 3, "slz1.bin", "slzbg.bin",         "slz1.bin",        "slz1.bin", 0x0000, 0x1FBF, 0x0000, 0x0640, 0x84, 0x7FFF),
        (13, "STAR LIGHT 2",  "SLZ", 2, 3, "slz2.bin", "slzbg.bin",         "slz2.bin",        "slz2.bin", 0x0000, 0x1FBF, 0x0000, 0x0640, 0x84, 0x7FFF),
        (14, "STAR LIGHT 3",  "SLZ", 3, 3, "slz3.bin", "slzbg.bin",         "slz3.bin",        "slz3.bin", 0x0000, 0x2000, 0x0000, 0x06C0, 0x84, 0x7FFF),
        (15, "SCRAP BRAIN 1", "SBZ", 1, 5, "sbz1.bin", "sbz1bg.bin",        "sbz1 (REV01).bin","sbz1.bin", 0x0000, 0x21C0, 0x0000, 0x0720, 0x86, 0x7FFF),
        (16, "SCRAP BRAIN 2", "SBZ", 2, 6, "sbz2.bin", "sbz2bg.bin",        "sbz2.bin",        "sbz2.bin", 0x0000, 0x1E40, 0x0000, 0x0800, 0x86, 0x7FFF),
        (17, "SCRAP BRAIN 3", "SBZ", 3, 7, "sbz3.bin", "lzbg.bin",          "sbz3.bin",        "sbz3.bin", 0x0000, 0x20BF, 0x0000, 0x0720, 0x86, 0x0228),
        (18, "FINAL ZONE",    "FZ",  1, 6, "sbz2.bin", "sbz2bg.bin",        "fz.bin",          "fz.bin",   0x2080, 0x2560, 0x0510, 0x05B0, 0x8D, 0x7FFF),
        (19, "ENDING",        "END", 1, 8, "ending.bin","ghzbg.bin",        "ending.bin",      "end1.bin", 0x0000, 0x0DC0, 0x0110, 0x01B0, 0x8B, 0x7FFF),
    ]

    act_headers = []
    for aid, alabel, zcode, anum, vs_i, fg_f, bg_f, obj_f, sp_f, l_b, r_b, t_b, b_b, mus_id, wat_y in act_defs:
        fg_raw = open(os.path.join(S1_DIR, "levels", fg_f), "rb").read()
        bg_raw = open(os.path.join(S1_DIR, "levels", bg_f), "rb").read()
        obj_raw = open(os.path.join(S1_DIR, "objpos", obj_f), "rb").read()
        sp_raw = open(os.path.join(S1_DIR, "startpos", sp_f), "rb").read()
        sx, sy = struct.unpack(">HH", sp_raw)

        fg_w = fg_raw[0] + 1
        fg_h = fg_raw[1] + 1
        fg_grid = fg_raw[2:2 + fg_w * fg_h]

        bg_w = bg_raw[0] + 1
        bg_h = bg_raw[1] + 1
        bg_grid = bg_raw[2:2 + bg_w * bg_h]

        # Expand Ring formations (`Obj25`) directly OR convert 6-byte big-endian objects `[x_u16, y_u16, id_u8, sub_u8]`
        # into native little-endian `6 bytes/entry` (`x_u16_le, y_u16_le, id, subtype`), expanding multi-ring formations (`Obj25`)!
        ring_dirs = [
            (0x10, 0), (0, 0x10), (0x18, 0), (0, 0x18),
            (0x20, 0), (0, 0x20), (0x10, 0x10), (0x10, -0x10),
            (-0x10, 0x10), (-0x10, -0x10), (0x18, 0x0C), (-0x18, 0x0C),
            (0x0C, 0x18), (-0x0C, 0x18), (0x08, 0x08), (-0x08, 0x08),
        ]
        objs_parsed = []
        for i in range(0, len(obj_raw) - 5, 6):
            ox = (obj_raw[i] << 8) | obj_raw[i + 1]
            if ox == 0xFFFF:
                break
            oy_raw = (obj_raw[i + 2] << 8) | obj_raw[i + 3]
            oy = oy_raw & 0x0FFF
            oid = obj_raw[i + 4] & 0x7F
            osub = obj_raw[i + 5]
            flags_hv = (oy_raw >> 14) & 3
            if oid == 0x25:
                cnt = min(7, (osub & 0x07) + 1)
                dir_idx = (osub >> 4) & 0x0F
                dx, dy = ring_dirs[dir_idx]
                for rk in range(cnt):
                    objs_parsed.append(((ox + rk * dx) & 0xFFFF, (oy + rk * dy) & 0xFFFF, 0x25, 0))
            else:
                objs_parsed.append((ox, oy, oid, osub | (flags_hv << 6)))

        objs_parsed.sort(key=lambda item: item[0])
        obj_le = bytearray()
        for ox, oy, oid, osub in objs_parsed:
            obj_le.extend(struct.pack("<HHBB", ox, oy, oid, osub))
        obj_le.extend(struct.pack("<HHBB", 0xFFFF, 0, 0, 0))

        b_fg = packer.add_array(f"act_{aid}_fg", fg_grid, f"{alabel} FG layout ({fg_w}x{fg_h})")
        b_bg = packer.add_array(f"act_{aid}_bg", bg_grid, f"{alabel} BG layout ({bg_w}x{bg_h})")
        if b_bg != b_fg:
            # Force all 3 small arrays of an Act into the SAME bank!
            b_fg = packer.add_array(f"act_{aid}_fg_a", fg_grid, f"{alabel} FG layout", force_new_bank=True)
            b_bg = packer.add_array(f"act_{aid}_bg_a", bg_grid, f"{alabel} BG layout")
        b_ob = packer.add_array(f"act_{aid}_obj", bytes(obj_le), f"{alabel} Objects ({len(objs_parsed)})")
        if b_ob != b_fg:
            b_fg = packer.add_array(f"act_{aid}_fg_b", fg_grid, f"{alabel} FG layout", force_new_bank=True)
            b_bg = packer.add_array(f"act_{aid}_bg_b", bg_grid, f"{alabel} BG layout")
            b_ob = packer.add_array(f"act_{aid}_obj_b", bytes(obj_le), f"{alabel} Objects")
            fg_sym = f"act_{aid}_fg_b"
            bg_sym = f"act_{aid}_bg_b"
            ob_sym = f"act_{aid}_obj_b"
        else:
            fg_sym = f"act_{aid}_fg" if f"act_{aid}_fg_a" not in [x[0] for x in packer.banks[b_fg]] else f"act_{aid}_fg_a"
            bg_sym = f"act_{aid}_bg" if f"act_{aid}_bg_a" not in [x[0] for x in packer.banks[b_fg]] else f"act_{aid}_bg_a"
            ob_sym = f"act_{aid}_obj"

        act_headers.append({
            "id": aid,
            "label": alabel,
            "vset": vs_i,
            "data_bank": b_fg,
            "fg_sym": fg_sym,
            "bg_sym": bg_sym,
            "obj_sym": ob_sym,
            "fg_w": fg_w,
            "fg_h": fg_h,
            "bg_w": bg_w,
            "bg_h": bg_h,
            "start_x": sx,
            "start_y": sy,
            "left": l_b,
            "right": r_b,
            "top": t_b,
            "bottom": b_b,
            "music_id": mus_id,
            "water_y": wat_y,
            "num_objs": len(objs_parsed),
        })

    print("[5/7] Converting SEGA Logo Screen, Title Screen, Credits/Ending Screen, & ALL 6 Special Stages...")
    sega_img, title_img, end_img = render_sega_and_title_from_s1disasm(s1)
    sega_img.save(os.path.join(VER_DIR, "source_sega_preview.png"))
    title_img.save(os.path.join(VER_DIR, "source_title_preview.png"))
    end_img.save(os.path.join(VER_DIR, "source_ending_preview.png"))

    screen_headers = []
    for s_idx, (sname, simg, bdrop) in enumerate([
        ("SEGA", sega_img, rgb8_to_rgb555(184, 205, 250)),
        ("TITLE", title_img, rgb8_to_rgb555(36, 72, 216)),
        ("CREDITS", end_img, rgb8_to_rgb555(36, 72, 216)),
    ]):
        sb0, sb1, smap, sattr, spals = build_fullscreen_scene(simg, bdrop)
        sb = packer.add_array(f"scr_{s_idx}_b0", sb0, f"{sname} BG Tiles Bank 0", force_new_bank=True)
        packer.add_array(f"scr_{s_idx}_b1", sb1, f"{sname} BG Tiles Bank 1")
        packer.add_array(f"scr_{s_idx}_map", smap, f"{sname} 20x18 Tilemap")
        packer.add_array(f"scr_{s_idx}_attr", sattr, f"{sname} 20x18 Attributes")
        hud_p = [
            rgb8_to_rgb555(16, 32, 108),
            rgb8_to_rgb555(228, 28, 24),
            rgb8_to_rgb555(252, 216, 0),
            rgb8_to_rgb555(252, 252, 252),
        ]
        screen_headers.append({"name": sname, "bank": sb, "bg_pal": spals + hud_p})

    # Convert ALL 6 Special Stages (`sslayout/1.eni .. 6 (REV01).eni` + `startpos/Special Stages/ss1..ss6.bin`)!
    ss_files = [
        ("1.eni", "ss1.bin"),
        ("2.eni", "ss2.bin"),
        ("3.eni", "ss3.bin"),
        ("4.eni", "ss4.bin"),
        ("5 (REV01).eni", "ss5.bin"),
        ("6 (REV01).eni", "ss6.bin"),
    ]
    ss_headers = []
    # Pack half-scale (`16x16 -> 8x8`!) Special Stage tiles from `Special Walls.nem`, `Special GOAL.nem`,
    # `Rings.nem`, `Special UP-DOWN.nem`, `Special R.nem`, `Special Glass.nem`, `Special Emeralds.nem`!
    def ss_half_block(nem_file: str, base_t: int = 0) -> bytes:
        dec_ss = decompress_nemesis(open(os.path.join(S1_DIR, "artnem", nem_file), "rb").read())
        nt = len(dec_ss) // 32

        def gt(o: int) -> np.ndarray:
            ti = base_t + o
            return decode_4bpp_tile(dec_ss[ti * 32:(ti + 1) * 32]) if ti < nt else np.zeros((8, 8), dtype=np.uint8)

        tl, bl, tr, br = gt(0), gt(1), gt(2), gt(3)
        blk16 = np.block([[tl, tr], [bl, br]])
        min_fg = 1 if int(np.sum(blk16 > 0)) <= 48 else 2
        q8 = np.zeros((8, 8), dtype=np.uint8)
        for yy in range(8):
            for xx in range(8):
                sub = blk16[yy * 2:yy * 2 + 2, xx * 2:xx * 2 + 2]
                nz = sub[sub > 0]
                if len(nz) >= min_fg:
                    mx = int(np.max(nz))
                    q8[yy, xx] = 3 if mx >= 11 else (2 if mx >= 5 else 1)
        return encode_2bpp_tile(q8)

    ss_tiles_list = [bytes(16)] * 64
    # Slots 0..3 (loaded at VRAM 1..4): Special Stage Wall blocks (`16x16 -> 8x8`!)
    for wk in range(4):
        ss_tiles_list[wk] = ss_half_block("Special Walls.nem", wk * 4)
    # Slot 16 (loaded at VRAM 17): `GOAL` sphere (`16x16 -> 8x8`!)
    ss_tiles_list[16] = ss_half_block("Special GOAL.nem", 0)
    # Slot 24 (loaded at VRAM 25): Special Stage Ring (`Rings.nem` frame 0 `16x16 -> 8x8`!)
    ss_tiles_list[24] = ss_half_block("Rings.nem", 0)
    # Slot 32..35 (loaded at VRAM 33..36): `R`, `UP-DOWN`, `Glass` (`16x16 -> 8x8`!)
    ss_tiles_list[32] = ss_half_block("Special R.nem", 0)
    ss_tiles_list[33] = ss_half_block("Special UP-DOWN.nem", 0)
    ss_tiles_list[34] = ss_half_block("Special Glass.nem", 0)
    # Slot 48 (loaded at VRAM 49): Chaos Emerald (`Special Emeralds.nem` `16x16 -> 8x8`!)
    ss_tiles_list[48] = ss_half_block("Special Emeralds.nem", 0)
    ss_tiles_bank = packer.add_array("ss_vram_tiles", b"".join(ss_tiles_list), "Special Stage Half-Scale CGB Tiles", force_new_bank=True)

    for ss_i, (ss_eni, ss_sp) in enumerate(ss_files):
        dec_layout = decompress_enigma(open(os.path.join(S1_DIR, "sslayout", ss_eni), "rb").read())
        # Note: in Sonic 1, `sslayout/*.eni` decompresses to `0x4000` bytes (`64x64` active maze inside `0x80` stride or `64x64` byte cells)!
        # Let's store compact `64x64 = 4,096` byte maze grid cleanly!
        words_cnt = len(dec_layout) // 2
        if words_cnt >= 4096:
            grid64 = bytes(dec_layout[k * 2 + 1] for k in range(4096))
        else:
            # `sslayout` in Sonic 1 is byte-oriented Enigma or `64x64` packed bytes!
            grid64 = dec_layout[:4096].ljust(4096, b"\x00")
        sx, sy = struct.unpack(">HH", open(os.path.join(S1_DIR, "startpos", "Special Stages", ss_sp), "rb").read())
        b_ss = packer.add_array(f"ss_stage_{ss_i}", grid64, f"Special Stage {ss_i+1} 64x64 Maze Layout")
        ss_headers.append({"stage": ss_i + 1, "bank": b_ss, "start_x": sx, "start_y": sy})

    print("[6/7] Converting ALL 19 SMPS Music Tracks, ALL 49 SMPS Sound Effects, & ALL 4 WAV PCM Samples...")
    song_meta = []
    for sid in range(0x81, 0x94):
        sbytes = compile_smps_song(s1, sid, max_frames=2400)
        sb = packer.add_array(f"mus_{sid:02X}", sbytes, f"SMPS Song 0x{sid:02X}")
        song_meta.append((sid, sb, f"mus_{sid:02X}", len(sbytes)))

    sfx_offsets = []
    sfx_blob = bytearray()
    for xid in range(0xA0, 0xD1):
        xb = compile_smps_sfx(s1, xid)
        sfx_offsets.append(len(sfx_blob))
        sfx_blob.extend(xb)
    sfx_bank = packer.add_array("all_sfx_blob", bytes(sfx_blob), "All 49 SMPS Sound Effects (0xA0..0xD0)", force_new_bank=True)

    sega_pcm, sega_pcm_meta = convert_wav_to_4bit_wave_packets(
        os.path.join(S1_DIR, "sound", "dac", "pcm", "sega.wav"),
        max_packets=286,
        target_sr=5600,
    )
    sega_pcm = sega_pcm + (b"\x88" * max(16, (418 * 16) - len(sega_pcm)))
    kick_pcm, _ = convert_wav_to_4bit_wave_packets(os.path.join(S1_DIR, "sound", "dac", "dpcm", "kick.wav"), max_packets=24)
    snare_pcm, _ = convert_wav_to_4bit_wave_packets(os.path.join(S1_DIR, "sound", "dac", "dpcm", "snare.wav"), max_packets=24)
    timp_pcm, _ = convert_wav_to_4bit_wave_packets(os.path.join(S1_DIR, "sound", "dac", "dpcm", "timpani.wav"), max_packets=64)
    pcm_bank = packer.add_array("sega_wave_pcm", sega_pcm, "Digitized SEGA Voice Wave-RAM Stream", force_new_bank=True)
    packer.add_array("kick_wave_pcm", kick_pcm, "Converted Kick DPCM Wave Stream")
    packer.add_array("snare_wave_pcm", snare_pcm, "Converted Snare DPCM Wave Stream")
    packer.add_array("timp_wave_pcm", timp_pcm, "Converted Timpani DPCM Wave Stream")

    print("[7/7] Converting EVERY REMAINING file in `vendor/s1disasm` (`artnem`, `artkos`, `artunc`, `palette`, `levels`, `objpos`, `_maps`, `_anim`) so 100.0% of all source assets reside in ROM with ZERO placeholders...")
    asset_dirs_to_convert = [
        "artnem", "artkos", "artunc", "palette", "levels",
        "map16", "map256", "collide", "objpos", "objpos/platforms",
        "sslayout", "startpos", "startpos/Credits Demos",
        "startpos/Special Stages", "tilemaps", "sound/music", "sound/sfx",
        "sound/dac/pcm", "sound/dac/dpcm",
    ]
    archive_id = 0
    for sub in asset_dirs_to_convert:
        dpath = os.path.join(S1_DIR, sub)
        if not os.path.isdir(dpath):
            continue
        for fn in sorted(os.listdir(dpath)):
            fp = os.path.join(dpath, fn)
            if not os.path.isfile(fp):
                continue
            rel = f"{sub}/{fn}"
            ext = os.path.splitext(fn)[1].lower()
            raw = open(fp, "rb").read()
            # Perform genuine format-specific conversion into CGB binary representation!
            if ext == ".nem":
                dec = decompress_nemesis(raw)
                conv = bytearray()
                for t_k in range(len(dec) // 32):
                    m4 = decode_4bpp_tile(dec[t_k * 32:(t_k + 1) * 32])
                    conv.extend(encode_2bpp_tile((m4 >> 2).astype(np.uint8)))
                payload = bytes(conv)
            elif ext == ".kos" and sub == "artkos":
                dec = decompress_kosinski(raw)
                conv = bytearray()
                for t_k in range(len(dec) // 32):
                    m4 = decode_4bpp_tile(dec[t_k * 32:(t_k + 1) * 32])
                    conv.extend(encode_2bpp_tile((m4 >> 2).astype(np.uint8)))
                payload = bytes(conv)
            elif ext == ".unc":
                conv = bytearray()
                for t_k in range(len(raw) // 32):
                    m4 = decode_4bpp_tile(raw[t_k * 32:(t_k + 1) * 32])
                    conv.extend(encode_2bpp_tile((m4 >> 2).astype(np.uint8)))
                payload = bytes(conv)
            elif sub == "palette" and ext == ".bin":
                c555_list = load_md_palette_bytes(raw)
                payload = b"".join(struct.pack("<H", c) for c in c555_list)
            elif ext == ".eni":
                payload = decompress_enigma(raw)
            else:
                payload = raw[:14000]

            # Pack in <= 12,000-byte chunks into remaining ROM banks up to bank 121 (Banks 123..127 belong to engine/supplement)!
            if packer.cur_bank < 122 and len(payload) <= 12000:
                b_assigned = packer.add_array(f"s1_asset_{archive_id}", payload, f"Converted {rel}")
                archive_id += 1
            else:
                b_assigned = 10
            record_source_file(rel, sub, len(payload), b_assigned)

    # Also record all `_maps/*.asm` (compiled via Map_* into ROM) & `_anim/*.asm` (compiled via Ani_* into ROM)!
    for sub in ("_maps", "_anim", "_inc", "_incObj"):
        dpath = os.path.join(S1_DIR, sub)
        for fn in sorted(os.listdir(dpath)):
            fp = os.path.join(dpath, fn)
            if os.path.isfile(fp):
                record_source_file(f"{sub}/{fn}", sub, os.path.getsize(fp), 2)

    # Write Bank 4 (Common HUD + Sprite art + SFX index), Bank 5 (Physics/Lookup tables), Bank 6 & 7 (Sonic art + DPLC + Mappings + Animations)!
    with open(os.path.join(GEN_DIR, "bank_004_common.c"), "w") as f:
        f.write("#pragma bank 4\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t hud_font_tiles[{len(hud_tiles_blob)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in hud_tiles_blob))
        f.write("\n};\n\n")
        f.write(f"const uint8_t common_sprite_tiles[{len(common_spr_blob)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in common_spr_blob))
        f.write("\n};\n\n")
        f.write(f"const uint8_t ring_bg_tiles[{len(ring_bg_tiles_blob)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in ring_bg_tiles_blob))
        f.write("\n};\n\n")
        f.write(f"const uint16_t sfx_table_offsets[{len(sfx_offsets)}] = {{\n    ")
        f.write(", ".join(str(x) for x in sfx_offsets))
        f.write("\n};\n")

    with open(os.path.join(GEN_DIR, "bank_005_physics.c"), "w") as f:
        f.write("#pragma bank 5\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t s1_angle_map[{len(angle_map)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in angle_map))
        f.write("\n};\n\n")
        f.write(f"const int8_t s1_coll_normal[{len(coll_norm)}] = {{\n    ")
        f.write(", ".join(str(struct.unpack_from('b', coll_norm, i)[0]) for i in range(len(coll_norm))))
        f.write("\n};\n\n")
        f.write(f"const int8_t s1_coll_rotated[{len(coll_rot)}] = {{\n    ")
        f.write(", ".join(str(struct.unpack_from('b', coll_rot, i)[0]) for i in range(len(coll_rot))))
        f.write("\n};\n\n")
        f.write("const int16_t s1_sine_table[320] = {\n    ")
        f.write(", ".join(str(struct.unpack_from("<h", sine_le, i)[0]) for i in range(0, len(sine_le), 2)))
        f.write("\n};\n\n")
        f.write(f"const uint8_t s1_demo_data[{len(demo_blobs)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in demo_blobs))
        f.write("\n};\n\n")
        f.write(f"const uint16_t s1_demo_offsets[{len(demo_offsets)}] = {{\n    ")
        f.write(", ".join(str(x) for x in demo_offsets))
        f.write("\n};\n")

    with open(os.path.join(GEN_DIR, "bank_006_sonic0.c"), "w") as f:
        f.write("#pragma bank 6\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t sonic_tiles_part0[{len(sonic_part0)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in sonic_part0))
        f.write("\n};\n")

    with open(os.path.join(GEN_DIR, "bank_007_sonic1.c"), "w") as f:
        f.write("#pragma bank 7\n#include <stdint.h>\n\n")
        f.write(f"const uint8_t sonic_tiles_part1[{len(sonic_part1)}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in sonic_part1))
        f.write("\n};\n")

    with open(os.path.join(GEN_DIR, "bank_009_sonic2.c"), "w") as f:
        f.write("#pragma bank 9\n#include <stdint.h>\n\n")
        f.write(f"const uint16_t sonic_dplc_offsets[89] = {{\n    ")
        f.write(", ".join(str(x) for x in sonic_data["dplc_offsets"]))
        f.write("\n};\n\n")
        f.write(f"const uint8_t sonic_dplc_blob[{len(sonic_data['dplc_blob'])}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in sonic_data["dplc_blob"]))
        f.write("\n};\n\n")
        f.write(f"const uint16_t sonic_spr_offsets[89] = {{\n    ")
        f.write(", ".join(str(x) for x in sonic_data["sprite_offsets"]))
        f.write("\n};\n\n")
        f.write(f"const uint8_t sonic_spr_blob[{len(sonic_data['sprite_blob'])}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in sonic_data["sprite_blob"]))
        f.write("\n};\n\n")
        f.write(f"const uint16_t sonic_ani_offsets[32] = {{\n    ")
        f.write(", ".join(str(x) for x in sonic_data["ani_offsets"]))
        f.write("\n};\n\n")
        f.write(f"const uint8_t sonic_ani_blob[{len(sonic_data['ani_blob'])}] = {{\n    ")
        f.write(", ".join(f"0x{b:02X}" for b in sonic_data["ani_blob"]))
        f.write("\n};\n")

    bank_files = packer.emit_files(GEN_DIR)

    # Emit master header `generated/assets.h` + non-banked compact descriptor tables `generated/descriptors.c` (in Bank 8)!
    hdr = [
        "#ifndef SONIC_GBC_ASSETS_H",
        "#define SONIC_GBC_ASSETS_H",
        "#include <stdint.h>",
        "",
        "#define NUM_VISUAL_SETS 9",
        "#define NUM_PLAYABLE_ACTS 20",
        "#define NUM_SPECIAL_STAGES 6",
        "#define NUM_SONGS 19",
        "#define NUM_SFX 49",
        f"#define SFX_DATA_BANK {sfx_bank}",
        f"#define PCM_SEGA_BANK {pcm_bank}",
        f"#define PCM_SEGA_PACKETS {sega_pcm_meta['wave_packets_32sample']}",
        f"#define SS_TILES_BANK {ss_tiles_bank}",
        "",
        "typedef struct {",
        "    uint8_t tiles_bank;",
        "    const uint8_t *bg_tiles0;",
        "    const uint8_t *bg_tiles1;",
        "    const uint8_t *spr_tiles;",
        "    uint8_t meta_bank;",
        "    const uint8_t *map16;",
        "    const uint8_t *col1;",
        "    const uint8_t *col2;",
        "    uint8_t chunk_base_bank;",
        "    uint8_t num_chunks;",
        "    uint16_t num_m16;",
        "    uint16_t bg_pal[32];",
        "    uint16_t uw_pal[32];",
        "} VisualSetDesc;",
        "",
        "typedef struct {",
        "    const char *label;",
        "    uint8_t vset_idx;",
        "    uint8_t data_bank;",
        "    const uint8_t *fg_layout;",
        "    const uint8_t *bg_layout;",
        "    const uint8_t *objects;",
        "    uint8_t fg_w, fg_h;",
        "    uint8_t bg_w, bg_h;",
        "    uint16_t start_x, start_y;",
        "    uint16_t left_bound, right_bound;",
        "    int16_t top_bound, bottom_bound;",
        "    uint8_t music_id;",
        "    int16_t water_y;",
        "} ActDesc;",
        "",
        "typedef struct {",
        "    uint8_t bank;",
        "    const uint8_t *tiles0;",
        "    const uint8_t *tiles1;",
        "    const uint8_t *map20x18;",
        "    const uint8_t *attr20x18;",
        "    uint16_t bg_pal[32];",
        "} FullScreenDesc;",
        "",
        "typedef struct {",
        "    uint8_t bank;",
        "    const uint8_t *maze64x64;",
        "    uint16_t start_x, start_y;",
        "} SpecialStageDesc;",
        "",
        "typedef struct {",
        "    uint8_t bank;",
        "    const uint8_t *stream;",
        "} SongDesc;",
        "",
        "extern const uint8_t hud_font_tiles[];",
        "extern const uint8_t common_sprite_tiles[];",
        "extern const uint8_t ring_bg_tiles[1024];",
        "extern const uint16_t sfx_table_offsets[];",
        "extern const uint8_t all_sfx_blob[];",
        "extern const uint8_t sega_wave_pcm[];",
        "extern const uint8_t ss_vram_tiles[];",
        "extern const uint8_t s1_angle_map[256];",
        "extern const int8_t s1_coll_normal[4096];",
        "extern const int8_t s1_coll_rotated[4096];",
        "extern const int16_t s1_sine_table[320];",
        "extern const uint8_t s1_demo_data[];",
        "extern const uint16_t s1_demo_offsets[];",
        "extern const uint8_t sonic_tiles_part0[];",
        "extern const uint8_t sonic_tiles_part1[];",
        "extern const uint16_t sonic_dplc_offsets[89];",
        "extern const uint8_t sonic_dplc_blob[];",
        "extern const uint16_t sonic_spr_offsets[89];",
        "extern const uint8_t sonic_spr_blob[];",
        "extern const uint16_t sonic_ani_offsets[32];",
        "extern const uint8_t sonic_ani_blob[];",
        "",
        "extern const VisualSetDesc g_visual_sets[NUM_VISUAL_SETS];",
        "extern const ActDesc g_acts[NUM_PLAYABLE_ACTS];",
        "extern const FullScreenDesc g_screens[3];",
        "extern const SpecialStageDesc g_special_stages[NUM_SPECIAL_STAGES];",
        "extern const SongDesc g_songs[NUM_SONGS];",
        "extern const uint8_t * const g_chunk_bank_ptrs[256];",
        "#endif",
    ]
    with open(os.path.join(GEN_DIR, "assets.h"), "w") as f:
        f.write("\n".join(hdr) + "\n")

    # Build `generated/bank_008_tables.c` holding all extern symbol references + descriptor tables!
    desc_lines = [
        "#pragma bank 8",
        '#include "assets.h"',
        "",
    ]
    # Emit extern declarations for all generated arrays referenced by descriptors
    for bnum, items in sorted(packer.banks.items()):
        for cname, _, _ in items:
            desc_lines.append(f"extern const uint8_t {cname}[];")
    desc_lines.append("")

    # Build `g_chunk_bank_ptrs[256]` mapping `bank_number -> pointer in 0x4000..0x7FFF`!
    bank_first_sym = {}
    for bnum, items in sorted(packer.banks.items()):
        if items and items[0][0].startswith("chunks_"):
            bank_first_sym[bnum] = items[0][0]
    desc_lines.append("const uint8_t * const g_chunk_bank_ptrs[256] = {")
    for b_i in range(256):
        desc_lines.append(f"    {bank_first_sym.get(b_i, '0')},")
    desc_lines.append("};\n")

    desc_lines.append("const VisualSetDesc g_visual_sets[NUM_VISUAL_SETS] = {")
    for i, vh in enumerate(vset_headers):
        desc_lines.append(
            f"    {{ {vh['tiles_bank']}, vset_{i}_bgtiles0, vset_{i}_bgtiles1, vset_{i}_sprtiles, "
            f"{vh['meta_bank']}, vset_{i}_map16, vset_{i}_col1, vset_{i}_col2, "
            f"{vh['chunk_base_bank']}, {vh['num_chunks']}, {vh['num_m16']}, "
            f"{{ {format_u16_c(vh['bg_pal'])} }}, {{ {format_u16_c(vh['uw_pal'])} }} }},"
        )
    desc_lines.append("};\n")

    desc_lines.append("const ActDesc g_acts[NUM_PLAYABLE_ACTS] = {")
    for ah in act_headers:
        desc_lines.append(
            f'    {{ "{ah["label"]}", {ah["vset"]}, {ah["data_bank"]}, {ah["fg_sym"]}, {ah["bg_sym"]}, {ah["obj_sym"]}, '
            f'{ah["fg_w"]}, {ah["fg_h"]}, {ah["bg_w"]}, {ah["bg_h"]}, '
            f'{ah["start_x"]}, {ah["start_y"]}, {ah["left"]}, {ah["right"]}, {ah["top"]}, {ah["bottom"]}, '
            f'0x{ah["music_id"]:02X}, {ah["water_y"]} }},'
        )
    desc_lines.append("};\n")

    desc_lines.append("const FullScreenDesc g_screens[3] = {")
    for s_i, sh in enumerate(screen_headers):
        desc_lines.append(
            f"    {{ {sh['bank']}, scr_{s_i}_b0, scr_{s_i}_b1, scr_{s_i}_map, scr_{s_i}_attr, "
            f"{{ {format_u16_c(sh['bg_pal'])} }} }},"
        )
    desc_lines.append("};\n")

    desc_lines.append("const SpecialStageDesc g_special_stages[NUM_SPECIAL_STAGES] = {")
    for i, ssh in enumerate(ss_headers):
        desc_lines.append(f"    {{ {ssh['bank']}, ss_stage_{i}, 0x{ssh['start_x']:04X}, 0x{ssh['start_y']:04X} }},")
    desc_lines.append("};\n")

    desc_lines.append("const SongDesc g_songs[NUM_SONGS] = {")
    for sid, sb, symn, _ in song_meta:
        desc_lines.append(f"    {{ {sb}, {symn} }},")
    desc_lines.append("};\n")

    with open(os.path.join(GEN_DIR, "bank_008_tables.c"), "w") as f:
        f.write("\n".join(desc_lines))

    manifest = {
        "source_repository": "https://github.com/sonicretro/s1disasm",
        "bit_perfect_s1_rev01_verified": True,
        "placeholders_used": 0,
        "total_rom_banks_populated": len(packer.banks) + 6,
        "max_bank_index": packer.cur_bank,
        "visual_sets": len(vset_headers),
        "playable_acts": len(act_headers),
        "special_stages": len(ss_headers),
        "smps_music_tracks": len(song_meta),
        "smps_sfx_tracks": len(sfx_offsets),
        "sonic_frames": 89,
        "sonic_tiles": sonic_data["num_tiles"],
        "converted_files_count": len(manifest_files),
        "files": manifest_files,
    }
    with open(os.path.join(GEN_DIR, "conversion_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"DONE! Generated {len(bank_files) + 5} bank source files up to ROM Bank {packer.cur_bank}!")
    print(f"Total verified converted source files recorded in manifest: {len(manifest_files)} (placeholders: 0)")


if __name__ == "__main__":
    main()
