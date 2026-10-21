# Sonic the Hedgehog — Complete Half-Scale (`0.5x`) Game Boy Color Port

A complete, zero-placeholder, **half-scale (`0.5x` width × `0.5x` height) Game Boy Color (`MBC5+RAM+BATTERY`, `2.0 MB` / `128` ROM banks + `32 KB` Battery-Backed SRAM)** port built from **`https://github.com/sonicretro/s1disasm`**.

Every single one of the **`946 / 946` non-`.git` files** in `s1disasm` is hashed (`SHA-256`), converted without placeholders (`placeholders_used = 0` in `generated/conversion_manifest.json`), compiled with **GBDK-2020 (`sdcc` / `lcc -msm83:gb`)** into `sonic.gbc`, and verified end-to-end on emulated Game Boy Color hardware using **PyBoy** (`4 / 4` test suites passing in `tests/test_e2e_emulator.py`).

---

## Highlights & Features

- **`0.5x` Half-Scale Graphics & World Geometry**:
  - All sprites (Sonic's `89` `Map_Sonic` + `SonicDynPLC` frames, `5`-frame `24×24` Goal Post + post, Badniks, bosses, rings, monitors, springs, capsules, animals, projectiles) and all Zone/Special Stage background tiles, `16×16` blocks, and `128×128` chunks are converted at **`0.5x` scale**, preserving Mega Drive screen framing and sightlines on the `160×144` Game Boy Color LCD (`128 px` viewport + `16 px` HUD window).
- **Immediate Start-of-Game Level Select**:
  - Right after the `"SE-GA!"` boot screen, the game enters **`SONIC LEVEL SELECT`** automatically with no cheat code required, allowing instant selection of any of the **20 Zone Acts**, **6 Special Stages**, or **68-track Sound Test** (plus quick return from pause anywhere in-game via **`START` -> `SELECT`**).
- **Authentic `360°` Slope, Loop & Closed S-Tunnel Physics (`01 Sonic.asm`)**:
  - Full 4-quadrant floor/wall/ceiling sensor math (`SonicAnglePos.asm`), dual collision planes (`Col_1` primary & `Col_2` loop alternate plane) with dynamic entrance/crown/exit plane switching (`2E` loop chunks), spin-tunnel auto-roll triggers, Spin Dash, and underwater physics.
- **Stable Multi-Palette Zone Rendering + Live Animated Tiles**:
  - Dedicated stable sub-palettes separate solid terrain/cliff walls from water/waterfall cycling (`PaletteCycle.asm`), paired with live VBlank tile animation uploads (`AnimateLevelGfx.asm` for GHZ flowers/waterfalls, MZ lava/torches, SBZ smoke, Ending flowers) and per-Zone parallax scrolling (`DeformLayers (REV01).asm`).
- **Guaranteed End-of-Act Goal Posts (`0x0D`) & Act 3 Prison Capsules (`0x3E`)**:
  - Full `512`-bit per-Act object state tracking (`g_obj_taken[64]` + `uint16_t spawn_slot`) across all `20` Acts (up to `461` objects per Act), priority object slot eviction, off-screen non-despawn protection, top CGB `10-sprites/scanline` OAM priority right after Sonic, full `5`-frame `24×24` Signpost spin animation (`Dr. Eggman -> 45° -> 90° rim -> 135° -> Sonic`) with twinkling sparkles, **`SONIC GOT THRU`** bonus tally (`GM_ACT_CLEAR`), and seamless progression into the next Act.
- **All Zones, Bosses, Special Stages, Audio & SRAM Persistence**:
  - All **7 Zones / 20 Acts** (`GHZ 1–3`, `MZ 1–3`, `SYZ 1–3`, `LZ 1–3`, `SLZ 1–3`, `SBZ 1–3`, `Final Zone`), all **6 Zone Bosses**, all **6 `360°`-rotating Chaos Emerald Special Stages**, **19 SMPS music tracks + 49 SMPS sound effects + `Wave-RAM` PCM drum & `"SE-GA!"` voice playback**, live **`REV01` / `REV00`** layout switching, **Debug Mode**, **Japanese Pseudonym Staff Credits**, and checksummed **Battery SRAM save support** (`high_score`, `unlocked_act`, `rev_mode`, `jp_credits`).

---

## Controls

### Start-of-Game Level Select (`SONIC LEVEL SELECT`)
| Input | Action |
|---|---|
| **`UP` / `DOWN`** | Step cursor through all `25` entries (`GHZ 1..3`, `MZ 1..3`, `SYZ 1..3`, `LZ 1..3`, `SLZ 1..3`, `SBZ 1..3`, `FINAL ZONE`, `SPECIAL STAGE 1..6`, `SOUND TEST`) |
| **`LEFT` / `RIGHT`** | Jump an entire Zone (`±3` entries) on stage rows, or change track ID (`00..43`) on `SOUND TEST` |
| **`A` or `START`** | Launch highlighted Zone/Act or Special Stage immediately (plays selected audio ID on `SOUND TEST`) |
| **`B`** | View classic Sonic Title Screen (`START` on Title Screen returns to `SONIC LEVEL SELECT`) |
| **`SELECT`** | Toggle between `REV01` and `REV00` level layouts |

### In-Level Gameplay & Debug Mode
| Input | Action |
|---|---|
| **`LEFT` / `RIGHT`** | Run / accelerate through slopes, `360°` loops, and S-tunnels |
| **`DOWN`** | Duck when standing; curl into high-speed spin roll when moving |
| **`A`** | Jump (`DOWN + A` while standing charges **Spin Dash** — release `DOWN` to launch) |
| **`START`** | Pause (`PAUSED  SEL:LVSEL`) — press **`START`** again to resume or **`SELECT`** while paused to return to **`SONIC LEVEL SELECT`** |
| **Title Codes** | On Title Screen: `UP, DOWN, LEFT, RIGHT` enables Debug flag; `DOWN, DOWN, DOWN, LEFT, RIGHT` toggles Japanese Staff Credits; hold **`B + START`** when starting a stage with Debug enabled (or toggle in Debug HUD: `A` cycles object, `B` exits free-flight / places item) |

---

## Project Structure

```text
sonic-gbc/
├── Makefile                        # Build rules (`all`, `rom`, `assets`, `test`, `clean`)
├── README.md                       # Complete documentation, architecture, controls & build guide
├── sonic.gbc                       # Compiled 2.0 MB CGB ROM (`MBC5+RAM+BATTERY`)
├── rom/
│   ├── sonic.map                   # SDCC/GBDK linker bank allocation map
│   └── sonic.noi                   # NoICE symbol table used by PyBoy hardware E2E tests
├── src/
│   ├── engine.h                    # Core constants, structs, VRAM/OAM maps & cross-bank APIs
│   ├── main.c                      # Bank 0: CGB kernel, VBlank streamer, DMA/palettes, 512-slot spawner, SMPS+PCM audio
│   ├── physics.c                   # Bank 1: 360° slope/loop/S-tunnel physics, Spin Dash, Debug Mode, pause/clear handling
│   ├── objects.c                   # Bank 2: Rings, monitors, springs, 24×24 Signpost (`0x0D`), Capsule (`0x3E`), 16 Badnik AIs
│   ├── screens_and_ss.c            # Bank 3: Boot -> Level Select, Title, Act Clear Tally, 6 Rotating Special Stages, Credits
│   ├── bosses_and_gimmicks.c       # Bank 122: All 6 Zone Bosses (`GHZ3..FZ`), Zone gimmicks & cut `s1disasm` objects
│   └── dle_osc_sram.c              # Bank 123: `DynamicLevelEvents`, 16-ch oscillators, extra-life score & MBC5 SRAM save
├── tools/
│   ├── convert_all.py              # Master converter across all Zones, Acts, physics tables, Special Stages & audio
│   ├── convert_all_helpers.py      # Half-scale (`0.5x`) font/HUD, 89-frame Sonic DPLC & 17-tile `Signpost.nem` converters
│   ├── build_complete_supplement.py# Wave-RAM PCM drums, `artunc` animated tiles, `REV00` data & 946/946 manifest verifier
│   ├── compression.py              # Pure-Python Nemesis, Kosinski & Enigma decompressors
│   ├── gfx_converter.py            # Mega Drive 4bpp -> CGB 2bpp half-scale tile/block/chunk & palette solvers
│   ├── md_symbols.py               # 68000 `s1disasm` assembly table/mapping/DPLC parser
│   └── smps_converter.py           # Mega Drive Z80/68k SMPS music & SFX -> CGB 4-channel APU stream compiler
├── generated/                      # Pre-generated C bank sources (`bank_004`..`bank_125`), headers & SHA-256 manifest
├── tests/
│   └── test_e2e_emulator.py        # 4-suite headless PyBoy CGB hardware verification & screenshot generator
└── verification/                   # Hardware telemetry report (`e2e_emulator_report.json`) & PyBoy proof sheets (`*.png`)
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
# Produces `sonic.gbc` (2.0 MB MBC5+RAM+BATTERY Game Boy Color ROM; ~1-2 min full SDCC register allocation for 60 FPS SM83 speed)
```

### 3. Optional: Regenerate All Assets from `s1disasm` (`946 / 946` Files)
To rebuild every generated asset bank and SHA-256 manifest from upstream `sonicretro/s1disasm`:

```bash
cd sonic-gbc
git clone https://github.com/sonicretro/s1disasm.git vendor/s1disasm
make assets
make -j$(nproc)
```

### 4. Run End-to-End PyBoy Hardware Emulator Suite (`4 / 4` Suites)
```bash
cd sonic-gbc
pytest -v tests/test_e2e_emulator.py
```

The test suite boots `sonic.gbc` in headless **PyBoy** with audio enabled and verifies:
1. **`test_rom_header_and_zero_placeholders`**: Validates the `2,097,152`-byte MBC5+RAM+BATTERY CGB ROM header/checksums and `946 / 946` converted `s1disasm` source entries with `placeholders_used == 0`.
2. **`test_full_end_to_end_boot_gameplay_all_zones_special_stages_and_audio`**: Verifies zero black/white screens across boot, all `20` playable Acts, `SYZ3` boss block shattering, `FZ` piston crusher, Debug Mode, all `6` rotating Special Stages, Good Ending, Japanese Credits, `REV00/REV01` switching, SMPS + PCM audio telemetry, and cold power-off Battery SRAM persistence.
3. **`test_ghz_wall_stability_and_loops_and_tunnels`**: Verifies `0` flickering cliff wall pixels across `32` consecutive frames plus full traversal through both a `360°` Green Hill loop and a closed S-tunnel.
4. **`test_start_of_game_level_select_and_end_of_level_goalpost_clear`**: Verifies cold hands-off boot directly into `SONIC LEVEL SELECT`, Zone/Act navigation, `Pause -> SELECT` return, high-index Goal Post (`0x0D`) & Prison Capsule (`0x3E`) spawning even when all `256` early object slots are marked taken, `24×24` 5-frame Signpost spin + sparkles, `SONIC GOT THRU` tally, and automatic transition into Green Hill Zone Act 2.
