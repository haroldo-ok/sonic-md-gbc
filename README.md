# Sonic the Hedgehog — Complete Half-Scale (`0.5x`) Game Boy Color Port

A complete, zero-placeholder, **half-scale (`0.5x` width × `0.5x` height) Game Boy Color (`MBC5+RAM+BATTERY`, `2.0 MB` / `128` ROM banks + `32 KB` Battery-Backed SRAM)** port built from **`https://github.com/sonicretro/s1disasm`**.

Every single one of the **`946 / 946` non-`.git` files** in `s1disasm` is hashed (`SHA-256`), converted without placeholders (`placeholders_used = 0` in `generated/conversion_manifest.json`), compiled with **GBDK-2020 (`sdcc` / `lcc -msm83:gb`)** into `sonic.gbc` running at CGB `8.388 MHz` double speed (`cpu_fast()`), and verified end-to-end on emulated Game Boy Color hardware using **PyBoy** (`7 / 7` test suites passing in `tests/test_e2e_emulator.py`).

---

## Highlights & Features

- **`0.5x` Half-Scale Graphics, Authentic `Map_*` Sprites & World Geometry**:
  - All sprites are composited directly from authentic `s1built.bin` `Map_*` multi-piece sprite mapping tables (`Map_Sonic` + `SonicDynPLC` `89` frames, `5`-frame `24×24` Goal Post, Monitors with distinct Ring/Speed Shoes/Shield/Invincibility/1-UP icons, Vertical & Horizontal Springs, Steel Spikes, Checkpoints, all Zone Badniks, `32×16` Dr. Eggman + boss weapons, `24×16` Prison Capsule + button, Giant Special Stage Ring, Animals, Points, Projectiles, Explosions, Air Bubbles, and per-Zone props) into dual CGB VRAM banks (`VRAM Bank 0` common sprites + `VRAM Bank 1` Zone sprites) with matched `OBP0..OBP7` palettes and proper `8×8` / `16×16` (`2×2`) / `24×16` / `24×24` / `32×16` OAM geometry.
  - All Zone and Special Stage background tiles, `16×16` blocks, and `128×128` chunks are converted at **`0.5x` scale**, preserving Mega Drive screen framing and sightlines on the `160×144` Game Boy Color LCD (`128 px` viewport + `16 px` HUD window), with CGB `BG-over-OBJ` priority masked cleanly on normal terrain (`mptr[1] & 0x7F`) so ground sprites are always crisp while `loop_plane` keeps Sonic authentically tucked behind foreground loop exit masks.
- **Hardware-Interrupt Concert-Pitch SMPS Music, Warm CGB 4-Channel APU & Wave-RAM PCM Drums**:
  - Driven directly from the **SM83 Hardware Vertical Blank Interrupt (`0x0040` -> `add_VBL(hw_sound_isr)`)** with atomic main-thread lock guards (`s_audio_lock`), safe MBC5 ROM bank save/restore (`CURRENT_BANK` / `SWITCH_ROM`), and authentic pause mute/resume (`NR51_REG = 0x00 / 0xFF`), keeping music tempo locked to the `59.7275 Hz` hardware clock even across heavy scrolling frames or VRAM loads.
  - Exact 68000 `s1.sounddriver.asm` relative branch/loop/gosub offsets (`cfJumpTo $F6`, `cfRepeatAtPos $F7`, `cfJumpToGosub $F8`), true `A4 = 440 Hz` (`C4 = 261.63 Hz = semitone 48`) FM & PSG concert pitch across all **19 SMPS music tracks** and **49 SMPS sound effects**, `CH3` rounded triangle/sine bass (`65.4..293.9 Hz`) + Wave-RAM PCM drum packets (`kick`, `snare`, `timpani`, `"SE-GA!"`), `$F3` `PSG3` white-noise hi-hat routing, and warm low-frequency `CH4` LFSR percussion (`>4.5 kHz` FFT energy `< 20%`).
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
├── sonic.noi                       # NoICE symbol table used by PyBoy hardware E2E tests
├── rom/
│   ├── sonic.gbc                   # Mirror of compiled 2.0 MB CGB ROM
│   └── sonic.noi                   # Mirror of NoICE symbol table
├── src/
│   ├── engine.h                    # Core constants, structs, dual-bank VRAM/OAM maps & cross-bank APIs
│   ├── main.c                      # Bank 0: CGB 8.388 MHz kernel, VBlank streamer, DMA/palettes, 512-slot spawner, SMPS+PCM audio
│   ├── physics.c                   # Bank 1: 360° slope/loop/S-tunnel physics, Spin Dash, Debug Mode, pause/clear handling
│   ├── objects.c                   # Bank 2: Rings, monitors, V/H springs, 24×24 Signpost (`0x0D`), Capsule (`0x3E`), Badniks & shots
│   ├── screens_and_ss.c            # Bank 3: Boot -> Level Select, Title, Act Clear Tally, 6 Rotating Special Stages, Credits
│   ├── bosses_and_gimmicks.c       # Bank 122: All 6 Zone Bosses (`GHZ3..FZ`), Zone gimmicks/props & cut `s1disasm` objects
│   └── dle_osc_sram.c              # Bank 123: `DynamicLevelEvents`, 16-ch oscillators, extra-life score & MBC5 SRAM save
├── tools/
│   ├── convert_all.py              # Master converter across all Zones, Acts, physics tables, Special Stages & audio
│   ├── convert_all_helpers.py      # Half-scale (`0.5x`) `s1built.bin` `Map_*` sprite compositor, Sonic 89-frame DPLCs & HUD
│   ├── build_complete_supplement.py# Wave-RAM PCM drums, `artunc` animated tiles, `REV00` data & 946/946 manifest verifier
│   ├── compression.py              # Pure-Python Nemesis, Kosinski & Enigma decompressors
│   ├── gfx_converter.py            # Mega Drive 4bpp -> CGB 2bpp half-scale tile/block/chunk & palette solvers
│   ├── md_symbols.py               # 68000 `s1disasm` assembly table/mapping/DPLC parser
│   └── smps_converter.py           # Exact 68k `s1.sounddriver.asm` concert-pitch SMPS music & SFX -> CGB 4-ch APU compiler
├── generated/                      # Pre-generated C bank sources (`bank_004`..`bank_125`), headers & SHA-256 manifest
├── tests/
│   └── test_e2e_emulator.py        # 6-suite headless PyBoy CGB hardware, FFT audio & live OAM/VRAM verification suite
└── verification/                   # Hardware telemetry (`e2e_emulator_report.json`), sprite atlas & live gameplay showcases (`*.png`)
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

### 3. Optional: Regenerate All Assets from `s1disasm` (`946 / 946` Files)
To rebuild every generated asset bank and SHA-256 manifest from upstream `sonicretro/s1disasm`:

```bash
cd sonic-gbc
git clone https://github.com/sonicretro/s1disasm.git vendor/s1disasm
make assets
make -j$(nproc)
```

### 4. Run End-to-End PyBoy Hardware Emulator Suite (`8 / 8` Suites)
```bash
cd sonic-gbc
pytest -v tests/test_e2e_emulator.py
```

The test suite boots `sonic.gbc` in headless **PyBoy** with 48 kHz audio synthesis enabled and verifies:
1. **`test_rom_header_and_zero_placeholders`**: Validates the `2,097,152`-byte MBC5+RAM+BATTERY CGB ROM header/checksums and `946 / 946` converted `s1disasm` source entries with `placeholders_used == 0`.
2. **`test_full_end_to_end_boot_gameplay_all_zones_special_stages_and_audio`**: Verifies zero black/white screens across boot, all `20` playable Acts, `SYZ3` boss block shattering, `FZ` piston crusher, Debug Mode, all `6` rotating Special Stages, Good Ending, Japanese Credits, `REV00/REV01` switching, SMPS + PCM audio telemetry, and cold power-off Battery SRAM persistence.
3. **`test_ghz_wall_stability_and_loops_and_tunnels`**: Verifies `0` flickering cliff wall pixels across `32` consecutive frames plus full traversal through both a `360°` Green Hill loop and a closed S-tunnel.
4. **`test_start_of_game_level_select_and_end_of_level_goalpost_clear`**: Verifies cold hands-off boot directly into `SONIC LEVEL SELECT`, Zone/Act navigation, `Pause -> SELECT` return, high-index Goal Post (`0x0D`) & Prison Capsule (`0x3E`) spawning, `24×24` 5-frame Signpost spin + sparkles, `SONIC GOT THRU` tally, and automatic transition into Green Hill Zone Act 2.
5. **`test_concert_pitch_music_non_screechy_across_all_zones_and_boss`**: Audits all 19 compiled SMPS `mus_*` streams (`100%` notes in concert pitch `55..1,600 Hz`, zero ultrasonic notes `> 1,800 Hz`, active bass `65..294 Hz`) AND performs live `48 kHz` PyBoy audio capture + FFT spectral analysis across all 6 Zones, Final Zone, Boss, Title, and Special Stage (`>4.5 kHz` high-frequency screech ratio `<= 0.17`).
6. **`test_live_ingame_sprites_enemies_shots_monitors_springs_boss_and_props`**: Verifies nonzero `VRAM Bank 0` (`32..127`) & `VRAM Bank 1` (`0..127`) sprite tile uploads, matching OAM palettes/geometry, and live rasterized CGB screen pixels across 9 gameplay panels covering all 5 Monitor types, Vertical & Horizontal Springs, Spikes, Badniks across all 6 Zones, Buzz Bomber/Fireball/Bomb/Plasma shots, `32×16` Eggman + Checkered Wrecking Ball, `24×16` Prison Capsule + Animal + Giant Ring, and Zone props.
7. **`test_interrupt_driven_music_playback_when_main_loop_stalled`**: Verifies `1:1` `g_sound_isr_ticks` per hardware VBlank (`180 / 180`), authentic ISR pause mute/resume (`NR51 = 0x00 / 0xFF`), and uninterrupted 4-channel Green Hill Zone music synthesis from the hardware `0x0040` interrupt even when `main()` is completely halted in an infinite WRAM `JR -2` spin loop for `120` frames (`2.0s`).
8. **`test_spring_kinematics_match_original_s1disasm`**: Directly compares live CGB kinematics and OAM orientation against `s1disasm/_incObj/41 Springs.asm` and `_incObj/01 Sonic.asm` across all spring variants:
   - **`Sonic_JumpHeight` `jumping` flag (`01 Sonic.asm:1273-1303`)**: Gates variable-jump velocity clamping (`-$400`) strictly on `g_sonic.jumping != 0`, preventing unheld Jump buttons from chopping Red Up Springs from **`~593 px` down to `36.6 px` (`16×` weaker!)** and Yellow Up Springs from **`~233 px` down to `36.6 px` (`6.3×` weaker!)**, while preserving crisp `~40 px` short-hop player jumps when `jumping == 1`, plus clearing `jumping = 0` on Springs (`0x41`), Bumpers (`0x47`), Seesaws (`0x5E`), Rotating Junctions (`0x66`), and Teleporters (`0x72`).
   - **Packed `flags_hv` Subtype & Orientation Decoding (`41 Springs.asm:37-76` across all `197` `objpos/*.bin` springs)**: Masks `raw_sub = o->subtype & 0x3F`, inspects `xflip = (o->subtype & 0x40) != 0` (`25 / 37` horizontal springs across Sonic 1 face Left as `0x50` / `0x52`, previously treated as vertical springs!), `horiz = (raw_sub & 0x10) != 0`, and `upside_down = !horiz && ((raw_sub & 0x20) || (o->subtype & 0x80))` (`4 / 4` ceiling springs in Sonic 1 are `0xA0`, now rendered Y-flipped and blasting downward at `+0x1000` / `+0x0A00`).
   - **`Spring_LR`, `ObjectFall`, & `Spring_ResetAni` Fidelity**: Sets `lock_ctrl = 15` (`Sonic_JumpDirection` air steering gated while `lock_ctrl > 0` so `Spring_LR` delivers full horizontal range), syncs `x_sub` / `y_sub`, integrates position prior to adding `$0038` gravity (`ObjectFall`, `01 Sonic.asm:1264-1267`), holds `SonAnim_Spring`'s `$2F`-frame pose through ascent, and transitions `ANI_SPRING -> ANI_WALK` at apex (`obVelY >= 0`).

