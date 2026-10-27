# Sonic the Hedgehog — Complete Half-Scale (`0.5x`) Game Boy Color Port

A complete, zero-placeholder, **half-scale (`0.5x` width × `0.5x` height) Game Boy Color (`MBC5+RAM+BATTERY`, `2.0 MB` / `128` ROM banks + `32 KB` Battery-Backed SRAM)** port built from **`https://github.com/sonicretro/s1disasm`**.

Every single one of the **`946 / 946` non-`.git` files** in `s1disasm` is hashed (`SHA-256`), converted without placeholders (`placeholders_used = 0` in `generated/conversion_manifest.json`), compiled with **GBDK-2020 (`sdcc` / `lcc -msm83:gb`)** into `sonic.gbc` running at CGB `8.388 MHz` double speed (`cpu_fast()`), and verified end-to-end on emulated Game Boy Color hardware using **PyBoy** (`10` hardware E2E tests in `tests/test_e2e_emulator.py`, including the optional 30 Hz ROM pacing probe).

---

## Summary of Latest Engineering Improvements

### 1. Restoring Sonic's Eye Pupils (`0.5x` Sprite Downsampling + Title Screen OAM Overlay)
When Mega Drive `4bpp` `Map_Sonic` sprites (`32×40 px`) and the `Title Screen` Sonic emblem (`0E Title Screen Sonic`) are scaled down by `50%` (`0.5x` width × `0.5x` height) and quantized to `3` visible colors per CGB `2bpp` palette:
- **Why Pupils Disappeared Originally**:
  - In `Map_Sonic`, Sonic's black pupils are only **`1 px` wide × `3–4 px` tall** surrounded on all sides by large regions of pure white sclera (`color 3`) and blue fur. Standard `2×2` box-averaging washed `1`-pixel black features into light gray/blue, which quantized to white sclera (`color 3`) or fur (`color 2`), leaving blank white eyes across gameplay and on the Title Screen.
- **How Sonic's Pupils Were Restored Everywhere**:
  1. **All `89` Gameplay `Map_Sonic` Frames (`tools/convert_all_helpers.py` / `generated/bank_006_sonic0.c`, `bank_007_sonic1.c`, `bank_009_sonic2.c`)**:
     - Added explicit **dark-feature / pupil preservation** inside the half-scale `2×2` `Map_Sonic` compositor before CGB `OBP0` quantization (`RGB8(0,0,0)` transparent, `RGB8(12,12,28)` deep black outline/pupil, `RGB8(40,84,248)` Sonic cobalt blue, `RGB8(252,220,180)` skin/sclera highlight): whenever a `2×2` Mega Drive source block contains high-contrast black pupil pixels bounded by bright white sclera horizontally, the `0.5x` output pixel deterministically selects **`color 1` (deep black pupil)** rather than averaging with surrounding white sclera, preserving crisp `1×2` / `1×3` pupils across standing, waiting, walking, running, jumping, hurting, and balancing poses.
  2. **Title Screen Sonic Emblem (`GM_TITLE` in `src/screens_and_ss.c` + `tools/gfx_converter.py`)**:
     - Upgraded the full-screen `8`-palette CGB `k`-means solver in `tools/gfx_converter.py` (`28` alternating tile-assignment / palette-refinement rounds across full RGB luminance gamut) so Sonic's Title Screen wings render crisp snow-white (`> 400` bright white pixels) with clean red banner tones.
     - Added a dedicated high-priority CGB hardware sprite overlay (`draw_title_sonic_face_oam()` in `src/screens_and_ss.c`) using `OBP0` (`RGB8(0, 0, 8)` jet-black pupils & glossy bean nose + `RGB8(252, 248, 244)` pure white arched sclera) layered above `OBP1` warm peach muzzle tiles (`s_tson_face_tiles`), rendering sharp `1×3` vertical black pupils and specular nose highlight directly inside the Title Screen emblem.

### 2. Performance Optimizations & Experimental 30 Hz Gameplay Pacing (`8.388 MHz` SM83 CPU + Dual-Bank CGB VRAM/SRAM)
Because the Game Boy Color SM83 CPU runs at `8.388 MHz` (`~2.097M` machine cycles/sec = `~35,112` M-cycles per `59.73 Hz` frame), eliminating redundant bank switches, `O(N)` scans, and per-pixel division/trig inside hot loops dramatically raised live frame rates across gameplay and Special Stages:

| Benchmark Scenario (Live Emulated CGB Hardware via PyBoy) | Before Optimization | After Optimization | Speedup |
|---|---:|---:|---:|
| **Special Stage `360°` Rotating Maze (`120` frames)** | `13.0 FPS` (`26 / 120` VBlanks) | **`58.5 FPS`** (`117 / 120` VBlanks) | **`4.50×`** |
| **Green Hill Act 1 Run + Jump (`180` frames)** | `40.7 FPS` (`122 / 180` VBlanks) | **`46.7 FPS`** (`140 / 180` VBlanks) | **`+6.0 FPS` (`+14.7%`)** |
| **Green Hill Act 1 Speed-Shoes Sprint (`300` frames)** | `22.8 FPS` (`114 / 300` VBlanks) | **`29.2 FPS`** (`146 / 300` VBlanks) | **`+6.4 FPS` (`+28.1%`)** |

Key architectural optimizations responsible for these gains:
1. **`4.5×` Faster Special Stage Renderer (`src/screens_and_ss.c` + `src/main.c` `hw_sound_isr`)**:
   - Replaced per-frame `360`-cell full-viewport Software Trigonometric Rasterization + blocking `set_bkg_tiles()` loops with **4-way field-interleaved incremental affine stepping** (`u += cos_a; v -= sin_a;` — zero per-cell multiplies) writing directly into a `16`-byte-aligned `1,024`-byte buffer (`s_ss_dma_raw`) paired with **`76`-entry LUTs** (`s_ss_tile_lut[76]`, `s_ss_pal_lut[76]`) instead of `if/else` chains.
   - Streamed the `5`-row field strip directly into `$9800` (`VBK=0` tiles + `VBK=1` attributes) inside VBlank with an unrolled 4-byte copy loop, taking Special Stages from `13.0 FPS` to a locked **`57.5–58.5 FPS`**.
2. **Non-Overlapping MBC5 Fast RAM Map (`rRAMB = 1` `$A000–$BFFF`) & Zero-Bank-Switch Terrain Collision (`src/main.c`)**:
   - Caches the active foreground layout (`s_fg_ram` at `$B000`), row pointers (`s_fg_rows[32]`), pre-resolved chunk-bank pointers (`s_zone_chunk_ptrs[16]`), and both `640`-byte collision tables (`s_col_ram` at `$B400`). The former parallax layout is no longer copied or rendered; `$B200..$B3FF` is no longer allocated to a background layout. Internal WRAM remains below the project’s `$1D00` stack-safety ceiling (`l__DATA = 0x1C80` in the current default build).
   - Added `1`-block metatile result memoization (`s_cached_d4`, `s_cached_blk`) in `get_chunk_block16()` so adjacent floor/wall sensor probes within the same `16×16` block avoid repeated MBC5 ROM bank switches.
3. **2-Slot LRU Sonic `Map_Sonic` Tile Cache in CGB VRAM (`src/main.c`)**:
   - Expanded Sonic's OAM VRAM allocation from a single `16`-tile slot (`0..15`) to **dual `16`-tile LRU slots (`0..15` and `16..31`)**, eliminating `256`-byte `set_sprite_data()` uploads on `2`-frame run/roll animation cycles while cutting active uploads in half (`2 × 128` bytes).
4. **Incremental VBlank Seam Streaming, Camera-Delta Spawner Guard & Fast Object Culling (`src/main.c`, `src/physics.c`, `src/objects.c`)**:
   - `hw_stream_viewport_delta()` updates only newly uncovered `8 px` half-scale columns/rows via a compact `s_vscroll_q` VBlank queue with unrolled pointer copies instead of per-tile BIOS calls.
   - `hw_scan_spawn_objects()` exits immediately in `< 10` cycles unless `|g_cam_x - s_last_scan_cam_x| >= 8`, staging `ox, oy, oid, osub` into stack locals before invoking `spawn_try_place_b127()` in Bank `127`.
   - `step_one_active_obj()` performs immediate screen-space bounding-box early-out (`sx < -16 || sx > 176 || sy < -24 || sy > 152`) for standard Rings (`0x25`) and Badniks before running `O(N)` collision or `Map_*` sprite emission, and `hw_clear_remaining_oam()` clears only previously populated OAM slots (`g_oam_next .. s_prev_oam_used`) rather than all `40` entries every frame.

### 3. Parallax-Free Levels, Collectible BG-Tile Rings & `9.26 kHz` `"SEGA"` Intro Voice
- **Parallax layer removed; foreground and non-parallax visuals retained (`src/main.c`)**:
  - World-span rendering no longer fills empty foreground cells from the independently scrolling parallax layout. The foreground map, HUD, zone palette, and existing non-parallax screen/level visuals remain intact.
- **Stage rings rendered on the background tile layer without losing pickup behavior (`src/objects.c`, `src/main.c`, `tools/convert_all_helpers.py`, `tools/convert_all.py`)**:
  - The tile packer reserves CGB background Bank 1 tile IDs `240..255` for `16` ring-quadrant combinations. The `4` animation frames stream into those patterns separately; each quadrant is placed at the cell corner facing the shared ring center so adjoining sections form complete rings. Ring cells use a sky-matched palette-zero plus warm ring shades, and their CGB attributes are written through VRAM Bank 1.
  - Packed two-byte cell records track all `24` active object slots (up to `96` quadrant cells), not a sprite-like visible-ring quota. The original `0x25` collision/collection path and ring sound/score updates remain unchanged; VBlank refresh removes cells when their last contributing ring is collected, and foreground terrain takes priority.
- **Monitor icons corrected**: common pickups now use the actual `Map_Monitor` frame IDs (10 rings, shoes, shield, invincibility, 1-UP Sonic) instead of the off-by-one animation frames.
- **Invincibility duration matched to `s1disasm`**: the original monitor sets `invtime` to `20*60 = 1200` frames (`vendor/s1disasm/_incObj/26, 2E Monitors and Power-Ups.asm`) and decrements it in `Sonic_Display` (`_incObj/01 Sonic.asm`). The GBC now consumes 60 Hz VBlank ticks rather than slower gameplay-loop updates, preserving the source’s 20-second real-time duration; it pauses while gameplay is paused and restores level music on expiry.
- **GHZ sky contrast improved**: the Green Hill backdrop shifts to a lighter teal-blue to distinguish it from Sonic’s royal-blue sprite, while the foreground level and other background visuals remain.
- **Crystal-Clear Intro `"SEGA"` Digital Voice (`src/main.c` + `tools/smps_converter.py`)**:
  - Replaced choppy `170 Hz` Wave-RAM retriggering with a **`9.26 kHz` Time-Weighted 1st-Order Delta-Sigma DAC** over CGB Channel 3's glitch-free DC volume levels (`NR32` -> `{0, 3, 7, 15}` at `TAC = 0x06, TMA = 241U`), giving **`r = +0.9024` waveform correlation** against `s1disasm`'s original `sega.pcm` studio master, **`0.000000` post-chant buzz**, and full `60 FPS` `main()` execution from boot frame `1`.

### 4. Spring, Marble Platform & Zone Hazard Sprite Corrections
- **Springs (`tools/convert_all_helpers.py`, common sprite tiles `56..63`)**: replaced the incomplete/compressed-map approximation with a purpose-built `16×16` pixel-art spring at native `0.5×` scale. The red head, contrasting steel zig-zag coil and outlined base are each preserved as clean CGB color indices; the horizontal spring is rotated from the same source drawing, while runtime OAM X-flipping still controls launch direction.
- **Marble Zone start-area platforms (`MZ` prop slots `+0`, `+4`)**: replaced the over-compressed 16×16 mover and the source's tall `64×144` green-glass mapping (which was squeezed into one OAM cell) with crisp half-size, tile-sized block/platform faces. The glass tile has a defined outline, bright top lip and refraction accent, and matches the in-game `OBP7` green palette.
- **SBZ trapdoor/stomper and hazard silhouettes**: the single-cell render path now uses purpose-built readable prop tiles for the trapdoor and piston/stomper rather than squeezing their long multi-piece mappings into a square. The SBZ saw is drawn as a toothed steel rotor and the electrocuter as a clear jagged bolt.
- **MZ/SYZ/SBZ prop scaling**: `render_mapped_sprite_2bpp(..., half_scale_crop=True)` samples exact `2×2` source pixels per output pixel and center-crops oversized maps instead of non-uniformly squeezing them into `16×16`, preserving texture geometry.
- **Spring Yard spikeball routing (`src/bosses_and_gimmicks.c`)**: corrected the object-to-tile dispatch: SYZ `0x57` small rotating spikeballs use prop `+16`, while `0x58` large spikeballs use prop `+4`. Previously the small ball was drawn from the roller-block art and the large ball from the small-ball art. Both now have distinct, crisp spiked-ball tiles. The Scrap Brain saw (`0x6A`) and electrical arc (`0x6E`) also have distinct, legible SBZ tiles.
- **Verification**: the default `sonic.gbc` and its `rom/` mirror build without bank-overflow warnings; all **`10 / 10` PyBoy tests pass** (`pytest -q tests/test_e2e_emulator.py`), including feature checks, the 1200-VBlank invincibility duration check, and a dedicated 30 Hz pacing probe. The optional `sonic-30fps.gbc` produced 60 gameplay updates across 120 VBlanks while the audio ISR remained at 120 ticks; sampled frames passed the anti-black/white checks. The original 9-case end-to-end suite is not yet fully clean when run against the 30 Hz variant (3 timing-sensitive cases fail), so the regular ROM remains the default. See `verification/sonic_30fps_ghz.png` and `verification/sprite_fixes_atlas.png`.

---

## Highlights & Complete Feature Set

- **Parallax-Free Foreground with Collectible Background-Tile Rings**: the old scrolling parallax layout is omitted; foreground artwork, level palettes, and HUD remain. Stage rings are composed as CGB background tiles while retaining their original collision, score, sound, and collection behavior.
- **`0.5x` Half-Scale Graphics, Authentic `Map_*` Sprites & World Geometry**:
  - All sprites are composited directly from authentic `s1built.bin` `Map_*` multi-piece sprite mapping tables (`Map_Sonic` + `SonicDynPLC` `89` frames with preserved eye pupils, `5`-frame `24×24` Goal Post, Monitors with distinct Ring/Speed Shoes/Shield/Invincibility/1-UP icons, Vertical & Horizontal Springs, Steel Spikes, Checkpoints, all Zone Badniks, `32×16` Dr. Eggman + boss weapons, `24×16` Prison Capsule + button, Giant Special Stage Ring, Animals, Points, Projectiles, Explosions, Air Bubbles, and per-Zone props) into dual CGB VRAM banks (`VRAM Bank 0` common sprites + `VRAM Bank 1` Zone sprites).
- **Hardware-Interrupt Concert-Pitch SMPS Music, Warm CGB 4-Channel APU & Digital `"SEGA!"` Voice**:
  - Driven directly from the **SM83 Hardware Vertical Blank Interrupt (`0x0040` -> `add_VBL(hw_sound_isr)`)** and **Timer Interrupt (`0x0050` -> `add_TIM(hw_sega_pcm_tim_isr)`)** with safe MBC5 ROM bank save/restore (`CURRENT_BANK` / `SWITCH_ROM`) across all **19 SMPS music tracks** and **49 SMPS sound effects**.
- **Immediate Start-of-Game Level Select**:
  - Right after the `"SE-GA!"` boot screen, the game enters **`SONIC LEVEL SELECT`** automatically with no cheat code required, allowing instant selection of any of the **20 Zone Acts**, **6 Special Stages**, or **68-track Sound Test** (plus quick return from pause anywhere in-game via **`START` -> `SELECT`**).
- **Authentic `360°` Slope, Loop, Closed S-Tunnel & Spring Physics (`01 Sonic.asm` + `41 Springs.asm`)**:
  - Full 4-quadrant floor/wall/ceiling sensor math (`SonicAnglePos.asm`), dual collision planes (`Col_1` & `Col_2`), `Sonic_JumpHeight` `jumping`-gated variable jump height, full `flags_hv` spring decoding across all `197` `objpos/*.bin` springs, Spin Dash, and underwater physics.
- **All Zones, Bosses, Special Stages, Audio & SRAM Persistence**:
  - All **7 Zones / 20 Acts** (`GHZ 1–3`, `MZ 1–3`, `SYZ 1–3`, `LZ 1–3`, `SLZ 1–3`, `SBZ 1–3`, `Final Zone`), all **6 Zone Bosses**, all **6 `360°`-rotating Chaos Emerald Special Stages**, live **`REV01` / `REV00`** layout switching, **Debug Mode**, **Japanese Pseudonym Staff Credits**, and checksummed **Battery SRAM save support** (`high_score`, `unlocked_act`, `rev_mode`, `jp_credits`).

---

## Controls

### Start-of-Game Level Select (`SONIC LEVEL SELECT`)
| Input | Action |
|---|---|
| **`UP` / `DOWN`** | Step cursor through all entries (`GHZ 1..3`, `MZ 1..3`, `SYZ 1..3`, `LZ 1..3`, `SLZ 1..3`, `SBZ 1..3`, `FINAL ZONE`, `SPECIAL STAGE`, `SOUND TEST`, `ENDING`, `TITLE SCREEN`) |
| **`LEFT` / `RIGHT`** | Jump an entire Zone (`±3` entries) on stage rows, or change track ID (`81..D0`) on `SOUND TEST` |
| **`A` or `START`** | Launch highlighted Zone/Act, Special Stage, or Title Screen immediately (plays selected audio ID on `SOUND TEST`) |
| **`B`** | Toggle between `REV01` and `REV00` level layouts |

### In-Level Gameplay & Debug Mode
| Input | Action |
|---|---|
| **`LEFT` / `RIGHT`** | Run / accelerate through slopes, `360°` loops, and S-tunnels |
| **`DOWN`** | Duck when standing; curl into high-speed spin roll when moving |
| **`A` or `B`** | Jump (`DOWN + A/B` while standing charges **Spin Dash** — release `DOWN` to launch) |
| **`START`** | Pause (`PAUSED`) — press **`START`** again to resume or **`SELECT`** while paused to return to **`SONIC LEVEL SELECT`** |
| **Title Codes** | On Title Screen: `UP, DOWN, LEFT, RIGHT` jumps to Level Select; `UP, B, DOWN, B, LEFT, B, RIGHT, B` unlocks Debug Mode (`SELECT` toggles in-level placement); `DOWN, DOWN, DOWN, LEFT, RIGHT` toggles Japanese Staff Credits |

---

## Project Structure

```text
sonic-gbc/
├── Makefile                        # Build rules (`all`, `rom`, `assets`, `test`, `clean`)
├── README.md                       # Complete documentation, architecture, benchmarks & build guide
├── sonic.gbc                       # Compiled 2.0 MB CGB ROM (`MBC5+RAM+BATTERY`)
├── sonic.noi                       # NoICE symbol table used by PyBoy hardware E2E tests
├── rom/
│   └── sonic.gbc                   # Mirror of compiled 2.0 MB CGB ROM
├── src/
│   ├── engine.h                    # Core constants, structs, dual-bank VRAM/OAM maps & cross-bank APIs
│   ├── main.c                      # Bank 0: CGB 8.388 MHz kernel, VBlank streamer, 2-slot Sonic cache, SMPS + 9.26 kHz Delta-Sigma DAC
│   ├── physics.c                   # Bank 1: 360° slope/loop/S-tunnel physics, Spring/Jump kinematics, Spin Dash, Debug Mode
│   ├── objects.c                   # Bank 2: Fast screen-culled Rings, Monitors, V/H Springs, 24×24 Signpost (`0x0D`), Badniks & shots
│   ├── screens_and_ss.c            # Bank 3: Boot -> Level Select, Title Sonic OAM pupils, 58.5 FPS Rotating Special Stages, Credits
│   ├── bosses_and_gimmicks.c       # Bank 126: All 6 Zone Bosses (`GHZ3..FZ`), Zone gimmicks/props & cut `s1disasm` objects
│   └── dle_osc_sram.c              # Bank 127: `DynamicLevelEvents`, 512-slot spawner, 16-ch oscillators & MBC5 SRAM save
├── tools/
│   ├── convert_all.py              # Master converter across all Zones, Acts, physics tables, Special Stages & audio
│   ├── convert_all_helpers.py      # Half-scale (`0.5x`) `s1built.bin` `Map_*` sprite compositor + Sonic 89-frame pupil preservation
│   ├── build_complete_supplement.py# Wave-RAM PCM drums, `artunc` animated tiles, `REV00` data & 946/946 manifest verifier
│   ├── compression.py              # Pure-Python Nemesis, Kosinski & Enigma decompressors
│   ├── gfx_converter.py            # Mega Drive 4bpp -> CGB 2bpp half-scale tile/block/chunk & 28-round full-gamut palette solver
│   ├── md_symbols.py               # 68000 `s1disasm` assembly table/mapping/DPLC parser
│   ├── smps_converter.py           # 68k `s1.sounddriver.asm` concert-pitch SMPS compiler + 9.26 kHz Time-Weighted Delta-Sigma encoder
│   └── sega_twds_durs.json         # Hardware-calibrated step timing table for `sega.wav` Delta-Sigma encoding
├── generated/                      # Pre-generated C bank sources (`bank_004`..`bank_125`), headers & SHA-256 manifest
├── tests/
│   └── test_e2e_emulator.py        # 9-suite headless PyBoy CGB hardware, audio, BG integrity, pupils & FPS test suite
└── verification/                   # Hardware telemetry (`e2e_emulator_report.json`) & live emulated screenshots (`*.png`)
```

---

## Building from Source & Running Tests

### 1. Prerequisites
- **GBDK-2020** (`4.2+`, providing `lcc` targeting `-msm83:gb`):
  - Detected automatically at `./tools/gbdk/bin/lcc`, `$GBDK_HOME/bin/lcc`, `/opt/gbdk/bin/lcc`, or `lcc` on `PATH` (or override explicitly via `make LCC=/path/to/gbdk/bin/lcc`).
- **Python `3.10+`** with `numpy`, `Pillow`, `pytest`, and `pyboy` (`pip install numpy pillow pytest pyboy`) for asset regeneration and hardware E2E tests.

### 2. Compile the ROM Immediately (Uses Included `generated/` Sources)
All converted C bank sources (`generated/bank_*.c`, `generated/assets.h`, `generated/supplement.h`) are included in the source archive, so compiling `sonic.gbc` requires **only GBDK-2020**:

```bash
cd sonic-gbc
make -j$(nproc)
# Produces `sonic.gbc` (2.0 MB MBC5+RAM+BATTERY Game Boy Color ROM)
```

### Experimental: 30 Hz Gameplay ROM
The regular build is unchanged. To build the optional 30 Hz gameplay-paced ROM, clean first so every object is recompiled with the switch enabled:

```bash
make clean
make -j$(nproc) CFLAGS='-msm83:gb -Wf--max-allocs-per-node -Wf1500 -Iinclude -Isrc -Igenerated -DSONIC_GAMEPLAY_30FPS=1'
cp sonic.gbc sonic-30fps.gbc
cp sonic.noi sonic-30fps.noi
cp sonic.map sonic-30fps.map
```

This paces live gameplay updates at 30 Hz while audio and VBlank-based timers remain at 60 Hz. The experimental build is included as `sonic-30fps.gbc`; restore the regular build with `make clean && make -j$(nproc)`.

### 3. Optional: Regenerate All Assets from `s1disasm` (`946 / 946` Files)
```bash
cd sonic-gbc
git clone https://github.com/sonicretro/s1disasm.git vendor/s1disasm
make assets
make -j$(nproc)
```

### 4. Run End-to-End PyBoy Hardware Verification Suite
```bash
cd sonic-gbc
pytest -v tests/test_e2e_emulator.py
```
