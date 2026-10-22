#!/usr/bin/env python3
"""
Cycle-Accurate Sega Mega Drive SMPS (68000 `s1.sounddriver.asm`) -> Warm 4-Channel Game Boy Color
(SM83 APU) Music, SFX & Sampled Wave-RAM Drum Compiler.

Fixes all screechy high-frequency artifacts from naive SMPS translation:
  1. Exact 68000 relative branch offsets matching `cfJumpTo` / `cfRepeatAtPos` / `cfJumpToGosub`
     (`adda.w d0,a4; subq.w #1,a4` -> `trk.pos + 1 + rel` on `$F6`/`$F8`, `trk.pos + 3 + rel` on `$F7`)
     so subroutines and loops never desynchronize into random high-frequency bytecodes.
  2. Exact Sega `s1.sounddriver.asm` frequency tables (`FMFrequencies`: `$81 = C0 = 16.35 Hz`, Middle `C4`
     at semitone `48`; `PSGFrequencies`: index `0` after `subi.b #$81` + transpose = `C3 = 130.81 Hz`,
     semitone `36` + index, with automatic `PSG3` / `$F3` white-noise hi-hat detection so SN76489 `$DF`
     hi-hat triggers go cleanly to CGB `CH4` noise instead of `4 kHz` pulse screeches).
  3. Proper musical channel role assignment (`FM1` lead -> `CH1` warm 50% pulse, chord/counter-melody ->
     `CH2` balanced 25% pulse, lowest FM bass track -> `CH3` rounded Wave-RAM bass, `DAC` + `PSG3` hi-hat ->
     `CH4` analog percussion), `smpsNoteFill` (`$E8`) staccato articulation, gentle `smpsModSet` (`$F0`)
     vibrato, octave ceiling below `E6` (`1,318.5 Hz`), and natural GBC hardware volume envelopes.
"""

from __future__ import annotations
import math
import struct
import wave
from dataclasses import dataclass
from md_symbols import S1DisasmROM


NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def semitone_to_name(semi: int) -> str:
    return f"{NOTE_NAMES[semi % 12]}{(semi // 12)}"


def semi_to_gbc_period(semi_from_c0: int, is_bass: bool = False) -> int:
    """
    Convert unified concert pitch semitone (`0 = C0 = 16.3516 Hz`, `48 = Middle C4 = 261.6256 Hz`,
    `57 = Concert A4 = 440.00 Hz`) into an 11-bit Game Boy Color frequency register (`1..2046`),
    keeping pitches inside warm, non-piercing musical octaves.
    """
    if semi_from_c0 <= 0:
        return 0
    s = int(semi_from_c0)
    if is_bass:
        # Keep CH3 Wave-RAM bass in punchy, warm low register (`C2 = 65.4 Hz` .. `C4 = 261.6 Hz`, semi 24..48)
        while s < 24:
            s += 12
        while s > 50:
            s -= 12
        hz = 261.625565 * (2.0 ** ((s - 48) / 12.0))
        reg = int(round(2048.0 - 65536.0 / hz))
    else:
        # Fold extreme upper register gently down by octaves so pulse channels stay warm (`C3 = 130.8 Hz` .. `Eb6 = 1244.5 Hz`)
        while s < 36:
            s += 12
        while s > 75:  # 75 = Eb6 (1,244.5 Hz) — zero dog-whistle screech!
            s -= 12
        hz = 261.625565 * (2.0 ** ((s - 48) / 12.0))
        reg = int(round(2048.0 - 131072.0 / hz))
    return max(1, min(2036, reg))


@dataclass
class TrackState:
    active: bool
    is_dac: bool
    is_psg: bool
    is_noise: bool
    pos: int
    transpose: int
    volume: int
    tempo_div: int
    psg_env: int = 0
    fm_voice: int = 0
    saved_dur: int = 1
    dur_left: int = 1
    note_fill: int = 0
    fill_left: int = 0
    current_note: int = 0  # 0 = rest; >0 = unified semitone from C0 (48 = C4), or raw DAC sample id for DAC track
    no_attack: bool = False
    retriggered: bool = False
    mod_on: bool = False
    mod_wait: int = 0
    mod_wait_left: int = 0
    mod_phase: int = 0
    stack: list = None
    loops: dict = None

    def __post_init__(self) -> None:
        if self.stack is None:
            self.stack = []
        if self.loops is None:
            self.loops = {}


COORD_FLAG_ARG_LEN: dict[int, int] = {
    0xE0: 1,  # Panning / AMS / FMS
    0xE1: 1,  # Detune
    0xE2: 1,  # Set communication byte
    0xE3: 0,  # Return (`smpsReturn`)
    0xE4: 0,  # Fade in to previous song
    0xE5: 1,  # Set channel tempo divider
    0xE6: 1,  # Alter FM volume
    0xE7: 0,  # No-attack (`smpsNoAttack` tie)
    0xE8: 1,  # Note fill / staccato gate (`smpsNoteFill`)
    0xE9: 1,  # Alter track transpose (`smpsAlterPitch`)
    0xEA: 1,  # Set main song tempo
    0xEB: 1,  # Set tempo divider for ALL channels
    0xEC: 1,  # Alter PSG volume
    0xED: 0,  # Clear push sound flag
    0xEE: 0,  # Stop special FM4 SFX
    0xEF: 1,  # Set FM voice (`smpsSetvoice`)
    0xF0: 4,  # Modulation setup (`smpsModSet` wait, speed, delta, steps)
    0xF1: 0,  # Turn on modulation
    0xF2: 0,  # Stop track (`smpsStop`)
    0xF3: 1,  # Set PSG noise mode (`smpsPSGform` -> SN76489 hi-hat/cymbal noise!)
    0xF4: 0,  # Turn off modulation
    0xF5: 1,  # Set PSG volume envelope (`smpsPSGvoice`)
    0xF6: 2,  # Jump (`smpsJump` signed 16-bit relative offset)
    0xF7: 4,  # Loop (`smpsLoop` index, count, signed 16-bit relative offset)
    0xF8: 2,  # Call (`smpsCall` signed 16-bit relative offset)
    0xF9: 0,  # Turn off FM4/OP1 release in FZ end
}


def step_smps_track(rom: bytes, trk: TrackState, all_tracks: list[TrackState] | None = None) -> None:
    """Advance one SMPS track by 1 hardware frame matching `s1.sounddriver.asm`."""
    if not trk.active:
        return
    trk.retriggered = False

    # `smpsNoteFill` ($E8): gate note cleanly to rest before duration expires unless tied (`no_attack`)
    if trk.fill_left > 0:
        trk.fill_left -= 1
        if trk.fill_left == 0 and not trk.no_attack and not trk.is_dac:
            trk.current_note = 0

    if trk.mod_on and trk.current_note > 0:
        if trk.mod_wait_left > 0:
            trk.mod_wait_left -= 1
        else:
            trk.mod_phase = (trk.mod_phase + 1) & 15

    trk.dur_left -= 1
    if trk.dur_left > 0:
        return

    safety = 0
    next_no_attack = False
    while trk.active and safety < 96:
        safety += 1
        if trk.pos < 0 or trk.pos >= len(rom):
            trk.active = False
            return
        b = rom[trk.pos]
        if b >= 0xE0:
            trk.pos += 1
            if b == 0xE3:  # `cfReturn`
                if trk.stack:
                    trk.pos = trk.stack.pop()
                    continue
                trk.active = False
                trk.current_note = 0
                return
            elif b == 0xF2:  # `cfStopTrack`
                trk.active = False
                trk.current_note = 0
                return
            elif b == 0xE5:
                trk.tempo_div = max(1, rom[trk.pos])
                trk.pos += 1
            elif b in (0xE6, 0xEC):
                delta = struct.unpack_from(">b", rom, trk.pos)[0]
                trk.volume = max(0, min(63, trk.volume + delta))
                trk.pos += 1
            elif b == 0xE7:
                next_no_attack = True
            elif b == 0xE8:
                trk.note_fill = rom[trk.pos] * trk.tempo_div
                trk.pos += 1
            elif b == 0xE9:
                delta = struct.unpack_from(">b", rom, trk.pos)[0]
                trk.transpose += delta
                trk.pos += 1
            elif b == 0xEB:
                new_div = max(1, rom[trk.pos])
                trk.pos += 1
                if all_tracks is not None:
                    for ot in all_tracks:
                        ot.tempo_div = new_div
            elif b == 0xEF:
                trk.fm_voice = rom[trk.pos]
                trk.pos += 1
            elif b == 0xF0:
                trk.mod_wait = rom[trk.pos]
                trk.mod_on = True
                trk.pos += 4
            elif b == 0xF1:
                trk.mod_on = True
            elif b == 0xF3:
                # `cfSetPSGNoise`: this track drives SN76489 noise hi-hats/cymbals, NOT melodic tones!
                trk.is_noise = True
                trk.pos += 1
            elif b == 0xF4:
                trk.mod_on = False
            elif b == 0xF5:
                trk.psg_env = rom[trk.pos]
                trk.pos += 1
            elif b == 0xF6:
                # Exact 68000 `cfJumpTo`: `move.b (a4)+; move.b (a4)+; adda.w d0,a4; subq.w #1,a4` -> `trk.pos + 1 + rel`!
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                trk.pos = trk.pos + 1 + rel
            elif b == 0xF7:
                # Exact 68000 `cfRepeatAtPos` -> `bra.s cfJumpTo` at `trk.pos + 2` -> `trk.pos + 3 + rel`!
                idx = rom[trk.pos]
                cnt = rom[trk.pos + 1]
                rel = struct.unpack_from(">h", rom, trk.pos + 2)[0]
                target = trk.pos + 3 + rel
                if idx not in trk.loops or trk.loops[idx] == 0:
                    trk.loops[idx] = cnt
                trk.loops[idx] -= 1
                if trk.loops[idx] > 0:
                    trk.pos = target
                else:
                    trk.pos += 4
            elif b == 0xF8:
                # Exact 68000 `cfJumpToGosub`: return address = `trk.pos + 2`, call target = `trk.pos + 1 + rel`!
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                ret_addr = trk.pos + 2
                if len(trk.stack) < 12:
                    trk.stack.append(ret_addr)
                trk.pos = trk.pos + 1 + rel
            else:
                arg_len = COORD_FLAG_ARG_LEN.get(b, 0)
                trk.pos += arg_len
        elif b >= 0x80:
            trk.pos += 1
            if b == 0x80:
                trk.current_note = 0
            elif trk.is_dac or trk.is_noise:
                trk.current_note = b
            elif trk.is_psg:
                # `PSGSetFreq` in `s1.sounddriver.asm`: `subi.b #$81,d5; add.b Transpose,d5; andi.w #$7F,d5`
                # where `PSGFrequencies[0] = 130.98 Hz = C3` (`semitone = 36` from C0; `48 = Middle C4`)!
                psg_idx = (b - 0x81 + trk.transpose) & 0x7F
                trk.current_note = 36 + psg_idx
            else:
                # `FMSetFreq` in `s1.sounddriver.asm`: `$81 = C0 (semi 0)`, `$B1 = Middle C4 (semi 48 = 261.63 Hz)`!
                fm_idx = (b - 0x80 + trk.transpose) & 0x7F
                trk.current_note = max(1, fm_idx - 1)

            if not trk.no_attack and trk.current_note > 0:
                trk.retriggered = True
                trk.mod_wait_left = trk.mod_wait
                trk.mod_phase = 0
            trk.no_attack = next_no_attack
            if trk.pos < len(rom) and rom[trk.pos] < 0x80:
                trk.saved_dur = max(1, rom[trk.pos] * trk.tempo_div)
                trk.pos += 1
            trk.dur_left = trk.saved_dur
            trk.fill_left = trk.note_fill if (trk.note_fill > 0 and trk.note_fill < trk.dur_left) else 0
            return
        else:
            # Standalone duration byte repeats previous pitch (or extends tied note if `no_attack`)
            trk.pos += 1
            trk.saved_dur = max(1, b * trk.tempo_div)
            trk.dur_left = trk.saved_dur
            if not trk.no_attack and trk.current_note > 0:
                trk.retriggered = True
                trk.mod_wait_left = trk.mod_wait
                trk.mod_phase = 0
            trk.no_attack = next_no_attack
            trk.fill_left = trk.note_fill if (trk.note_fill > 0 and trk.note_fill < trk.dur_left) else 0
            return


def _load_song_tracks(rom: bytes, base: int) -> tuple[list[TrackState], int]:
    num_fm = rom[base + 2]
    num_psg = rom[base + 3]
    tdiv = max(1, rom[base + 4])
    main_tempo = rom[base + 5]

    pos = base + 6
    tracks: list[TrackState] = []

    # Track 0: DAC percussion channel
    dac_off = struct.unpack_from(">H", rom, pos)[0]
    dac_tr = struct.unpack_from(">b", rom, pos + 2)[0]
    dac_vol = struct.unpack_from(">b", rom, pos + 3)[0]
    pos += 4
    tracks.append(TrackState(dac_off != 0, True, False, False, base + dac_off, dac_tr, dac_vol, tdiv))

    # FM melodic/bass channels (`FM1 .. FM5/FM6`)
    for _ in range(max(0, num_fm - 1)):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr = struct.unpack_from(">b", rom, pos + 2)[0]
        vol = struct.unpack_from(">b", rom, pos + 3)[0]
        pos += 4
        tracks.append(TrackState(off != 0, False, False, False, base + off, tr, vol, tdiv))

    # PSG channels (`PSG1, PSG2`, and `PSG3` which often drives SN76489 hi-hat noise on `$F3` / ch index 2!)
    for psg_i in range(num_psg):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr = struct.unpack_from(">b", rom, pos + 2)[0]
        vol = struct.unpack_from(">b", rom, pos + 3)[0]
        env = rom[pos + 5]
        pos += 6
        # In Sonic 1 SMPS, PSG3 (`psg_i == 2`) with `env == 4` or `tr >= 0` is always SN76489 hi-hat/cymbal percussion!
        default_noise = (psg_i == 2 and (env == 4 or tr >= 0))
        tracks.append(TrackState(off != 0, False, True, default_noise, base + off, tr, vol, tdiv, psg_env=env))

    return tracks, main_tempo


def _identify_bass_track_idx(rom: bytes, base: int) -> int:
    """Pre-scan 600 frames to identify the dedicated low-register bassline track (e.g. `FM2` in GHZ/SYZ/MZ)."""
    tracks, main_tempo = _load_song_tracks(rom, base)
    notes_per_trk: dict[int, list[int]] = {i: [] for i in range(1, len(tracks))}
    tempo_cnt = main_tempo
    for _ in range(600):
        skip = False
        if main_tempo > 0:
            tempo_cnt -= 1
            if tempo_cnt == 0:
                tempo_cnt = main_tempo
                skip = True
        if not skip:
            for t in tracks:
                step_smps_track(rom, t, tracks)
        for i in range(1, len(tracks)):
            t = tracks[i]
            if t.active and not t.is_noise and t.current_note > 0:
                notes_per_trk[i].append(t.current_note)

    best_idx = -1
    best_med = 999.0
    for i, arr in notes_per_trk.items():
        if len(arr) >= 8 and not tracks[i].is_psg:
            arr_s = sorted(arr)
            med = arr_s[len(arr_s) // 2]
            if med < best_med and med <= 48:
                best_med = med
                best_idx = i
    return best_idx


def compile_smps_song(s1: S1DisasmROM, song_id: int, max_frames: int = 2400) -> bytes:
    """
    Simulate SMPS song `song_id` (`0x81..0x93`) at 60 Hz with full `s1.sounddriver.asm` accuracy and encode
    a warm, concert-pitch 4-channel GBC stream (`~40s` full song arrangement per Zone!).
    """
    mi = s1.sym("MusicIndex")
    base = s1.u32(mi + (song_id - 0x81) * 4)
    rom = s1.rom

    # Jingles (`Invincibility` loops; `Extra Life`, `Act Clear`, `Game Over`, `Emerald` end cleanly with `0xFF`)
    is_jingle = song_id in (0x88, 0x8E, 0x8F, 0x93)
    sim_frames = 600 if is_jingle else max_frames

    bass_idx = _identify_bass_track_idx(rom, base)
    tracks, main_tempo = _load_song_tracks(rom, base)
    dac_trk = tracks[0]

    out = bytearray()
    prev_ch1 = 0
    prev_ch2 = 0
    prev_ch3 = 0
    wait_acc = 0
    tempo_cnt = main_tempo

    for _frame in range(sim_frames):
        skip_tick = False
        if main_tempo > 0:
            tempo_cnt -= 1
            if tempo_cnt == 0:
                tempo_cnt = main_tempo
                skip_tick = True

        if not skip_tick:
            for t in tracks:
                step_smps_track(rom, t, tracks)

        if not any(t.active for t in tracks):
            break

        ch1_cmd = None
        ch2_cmd = None
        ch3_cmd = None
        ch4_cmd = 0

        # 1. Dedicated CH3 Synth Bass Track (`bass_idx`), or lowest low-register FM voice if no dedicated bass track
        bass_semi = 0
        bass_rt = False
        if bass_idx > 0 and tracks[bass_idx].active and tracks[bass_idx].current_note > 0:
            bass_semi = tracks[bass_idx].current_note
            bass_rt = tracks[bass_idx].retriggered
        else:
            low_cands = [
                (t.current_note, t.retriggered)
                for idx_t, t in enumerate(tracks[1:], start=1)
                if t.active and not t.is_noise and not t.is_psg and t.current_note > 0 and t.current_note <= 43
            ]
            if low_cands:
                low_cands.sort(key=lambda x: x[0])
                bass_semi, bass_rt = low_cands[0]

        p3 = semi_to_gbc_period(bass_semi, is_bass=True) if bass_semi > 0 else 0
        if p3 != prev_ch3 or (bass_rt and p3 > 0):
            ch3_cmd = (p3, 1 if bass_rt else 0, 1 if p3 > 0 else 0)
            prev_ch3 = p3

        # 2. Lead (`CH1` 50% warm square) & Harmony (`CH2` 25% sweet accompaniment) from non-bass melodic tracks
        #    Preserve track priority order (`FM1` lead first, then `FM2..FM5` harmony, then warm `PSG1/PSG2` counter-melody)!
        lead_cands = []
        for idx_t in range(1, len(tracks)):
            if idx_t == bass_idx:
                continue
            t = tracks[idx_t]
            if not t.active or t.is_noise or t.current_note <= 0:
                continue
            # Prefer mid-register melodic tracks over high background sparkle arpeggios when picking main lead!
            if t.is_psg:
                # PSG volume in Sonic 1 is attenuation `0..15`; keep warm balanced level `6..9`
                gbc_vol = max(5, min(9, 10 - min(5, t.volume)))
                role_prio = 2 if t.current_note <= 68 else 4
            else:
                gbc_vol = max(6, min(11, 12 - min(6, t.volume // 5)))
                role_prio = 0 if idx_t == 1 else (1 if idx_t <= 3 else 3)

            # Subtle gentle vibrato (`±1` GBC period step) on held lead notes with `smpsModSet`!
            vib = 0
            if t.mod_on and t.mod_wait_left == 0:
                vib = (1 if (t.mod_phase & 8) else -1) if (t.mod_phase & 4) else 0
            lead_cands.append((role_prio, idx_t, t.current_note, t.retriggered, gbc_vol, vib))

        if lead_cands:
            lead_cands.sort(key=lambda x: (x[0], x[1]))
            _, _, l_semi, l_rt, l_vol, l_vib = lead_cands[0]
            p1 = max(1, min(2036, semi_to_gbc_period(l_semi, is_bass=False) + l_vib))
            if p1 != prev_ch1 or l_rt:
                ch1_cmd = (p1, 1 if l_rt else 0, l_vol)
                prev_ch1 = p1

            # Pick a distinct second harmony pitch (not unison/octave duplicate of `l_semi`) for `CH2`!
            harm_picked = None
            for cand in lead_cands[1:]:
                if abs(cand[2] - l_semi) not in (0, 12, 24):
                    harm_picked = cand
                    break
            if harm_picked is None and len(lead_cands) >= 2 and lead_cands[1][2] != l_semi:
                harm_picked = lead_cands[1]

            if harm_picked is not None:
                _, _, h_semi, h_rt, h_vol, h_vib = harm_picked
                p2 = max(1, min(2036, semi_to_gbc_period(h_semi, is_bass=False) + h_vib))
                if p2 != prev_ch2 or h_rt:
                    ch2_cmd = (p2, 1 if h_rt else 0, max(5, min(8, h_vol - 2)))
                    prev_ch2 = p2
            elif prev_ch2 != 0:
                ch2_cmd = (0, 0, 0)
                prev_ch2 = 0
        else:
            if prev_ch1 != 0:
                ch1_cmd = (0, 0, 0)
                prev_ch1 = 0
            if prev_ch2 != 0:
                ch2_cmd = (0, 0, 0)
                prev_ch2 = 0

        # 3. Percussion (`CH4`): DAC Kick/Snare/Timpani take priority, with soft crisp SN76489 `PSG3` Hi-Hats in between!
        if not skip_tick:
            if dac_trk.active and dac_trk.retriggered and dac_trk.current_note > 0:
                dn = dac_trk.current_note
                if dn == 0x81:
                    ch4_cmd = 1  # Punchy Kick
                elif dn in (0x82, 0x85):
                    ch4_cmd = 2  # Crisp Snare
                elif dn in (0x88, 0x89):
                    ch4_cmd = 3  # High Timpani
                else:
                    ch4_cmd = 4  # Low Timpani
            else:
                for t in tracks[1:]:
                    if t.active and t.is_noise and t.retriggered and t.current_note > 0:
                        ch4_cmd = 5  # Soft Crisp Closed Hi-Hat!
                        break

        mask = (
            (1 if ch1_cmd is not None else 0)
            | (2 if ch2_cmd is not None else 0)
            | (4 if ch3_cmd is not None else 0)
            | (8 if ch4_cmd != 0 else 0)
        )

        if mask == 0:
            wait_acc += 1
        else:
            while wait_acc > 0:
                step_w = min(127, wait_acc)
                out.append(step_w)
                wait_acc -= step_w
            out.append(0x80 | mask)
            if ch1_cmd is not None:
                p, rt, v = ch1_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x0F) << 3) | ((p >> 8) & 0x07))
            if ch2_cmd is not None:
                p, rt, v = ch2_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x0F) << 3) | ((p >> 8) & 0x07))
            if ch3_cmd is not None:
                p, rt, v = ch3_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x03) << 5) | ((p >> 8) & 0x07))
            if ch4_cmd != 0:
                out.append(ch4_cmd)
            wait_acc = 1

    while wait_acc > 0:
        step_w = min(127, wait_acc)
        out.append(step_w)
        wait_acc -= step_w

    out.append(0xFF if is_jingle else 0x00)
    return bytes(out)


def compile_smps_sfx(s1: S1DisasmROM, sfx_id: int) -> bytes:
    """
    Compile SMPS sound effect `sfx_id` (`0xA0..0xD0`) from `SoundIndex` in `s1built.bin`
    into a clean, non-harsh GBC SFX frame sequence `[duration, mode_flags, period_lo, period_hi]`.
    """
    si = s1.sym("SoundIndex")
    idx = sfx_id - 0xA0
    if idx == 0x30:
        base = s1.u32(s1.sym("SpecSoundIndex"))
    else:
        base = s1.u32(si + idx * 4)
    rom = s1.rom

    tdiv = max(1, rom[base + 2])
    nch = rom[base + 3]
    pos = base + 4
    tracks: list[TrackState] = []
    has_noise = False
    for _ in range(nch):
        ch_type = rom[pos + 1]
        off = struct.unpack_from(">H", rom, pos + 2)[0]
        tr = struct.unpack_from(">b", rom, pos + 4)[0]
        vol = struct.unpack_from(">b", rom, pos + 5)[0]
        pos += 6
        is_psg = (ch_type & 0x80) != 0
        is_nz = ch_type == 0xE0
        if is_nz:
            has_noise = True
        tracks.append(TrackState(off != 0, False, is_psg, is_nz, base + off, tr, vol, tdiv))

    out = bytearray()
    for _frame in range(72):
        for t in tracks:
            step_smps_track(rom, t, tracks)
        if not any(t.active for t in tracks):
            break
        notes = [t.current_note for t in tracks if t.active and not t.is_noise and t.current_note > 0]
        p = semi_to_gbc_period(max(notes), is_bass=False) if notes else 0
        mode = 2 if (has_noise and p == 0) else (3 if has_noise else 1)
        out.extend([1, mode, p & 0xFF, (p >> 8) & 0x07])

    out.append(0x00)
    return bytes(out)


def convert_wav_to_4bit_wave_packets(wav_path: str, max_packets: int = 240) -> tuple[bytes, dict]:
    """
    Convert a WAV file (`sega.wav`, `kick.wav`, `snare.wav`, `timpani.wav`) into 32-sample 4-bit
    Game Boy Color Wave RAM packets (`16` bytes per packet = `32 x 4-bit` nibbles).
    """
    with wave.open(wav_path, "rb") as wf:
        nch = wf.getnchannels()
        sw = wf.getsampwidth()
        sr = wf.getframerate()
        nframes = wf.getnframes()
        raw = wf.readframes(nframes)

    if sw == 1:
        samples = [((b - 128) / 128.0) for b in raw]
    elif sw == 2:
        cnt = len(raw) // 2
        vals = struct.unpack(f"<{cnt}h", raw)
        samples = [v / 32768.0 for v in vals]
    else:
        samples = [0.0] * max(32, nframes)

    if nch == 2:
        samples = [(samples[i] + samples[i + 1]) * 0.5 for i in range(0, len(samples) - 1, 2)]

    total_samples = max(32, len(samples))
    num_packets = min(max_packets, max(1, total_samples // 32))
    target_len = num_packets * 32

    peak = max(1e-4, max(abs(x) for x in samples))
    norm = [max(-1.0, min(1.0, (x / peak) * 0.92)) for x in samples]

    out = bytearray()
    for p_idx in range(num_packets):
        nibbles = []
        for s_i in range(32):
            src_pos = ((p_idx * 32 + s_i) * (len(norm) - 1)) / max(1, target_len - 1)
            i0 = int(src_pos)
            frac = src_pos - i0
            i1 = min(len(norm) - 1, i0 + 1)
            val = norm[i0] * (1.0 - frac) + norm[i1] * frac
            q = int(round((val + 1.0) * 7.5))
            nibbles.append(max(0, min(15, q)))
        for b_i in range(0, 32, 2):
            out.append((nibbles[b_i] << 4) | nibbles[b_i + 1])

    period_hz = sr / 32.0
    gbc_period = max(1, min(2045, int(round(2048.0 - 65536.0 / max(64.0, period_hz)))))
    meta = {
        "orig_sr": sr,
        "orig_samples": len(samples),
        "packets": num_packets,
        "wave_packets_32sample": num_packets,
        "bytes": len(out),
        "gbc_period": gbc_period,
    }
    return bytes(out), meta
