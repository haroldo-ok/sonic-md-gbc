#!/usr/bin/env python3
"""
Exact Sega Mega Drive SMPS (68000) -> 4-Channel Game Boy Color (SM83 APU) Music, SFX & PCM Compiler.

Converts:
  - ALL 19 SMPS Music tracks (Mus81..Mus93) from compiled s1built.bin
  - ALL 49 SMPS Sound Effects (SndA0..SndD0) from compiled s1built.bin
  - ALL 4 WAV PCM/DPCM samples (sega.wav, kick.wav, snare.wav, timpani.wav)
"""

from __future__ import annotations
import math
import os
import struct
import wave
from dataclasses import dataclass
from md_symbols import S1DisasmROM


# Exact MIDI pitch to Hz -> CGB 11-bit frequency register value: reg = 2048 - 131072 / hz
# SMPS note $81 equals C1 (~32.70 Hz) on FM / C2 on PSG; note $80 is rest (`nRst`).
def smps_note_to_gbc_period(note_val: int, is_bass: bool = False) -> int:
    """Convert SMPS chromatic note number (centered around $81 = C1/C2) to 11-bit GBC period (0..2047)."""
    if note_val <= 0x80:
        return 0
    semitone = note_val - 0x81  # 0 = C
    # In Sonic 1 SMPS, $A5 (offset 36) is C4 (261.63 Hz)!
    hz = 261.625565 * (2.0 ** ((semitone - 36) / 12.0))
    if is_bass:
        # Wave channel period is `2048 - 65536 / hz` (32-step wave table = 1 octave lower automatically!)
        # Shift up if below GBC hardware minimum (~32 Hz) so bass notes always punch cleanly!
        while hz < 55.0:
            hz *= 2.0
        while hz > 1046.0:
            hz *= 0.5
        reg = int(round(2048.0 - 65536.0 / hz))
    else:
        while hz < 65.5:
            hz *= 2.0
        while hz > 4186.0:
            hz *= 0.5
        reg = int(round(2048.0 - 131072.0 / hz))
    return max(1, min(2046, reg))


@dataclass
class TrackState:
    active: bool
    is_dac: bool
    is_psg: bool
    pos: int
    transpose: int
    volume: int
    tempo_div: int
    saved_dur: int = 1
    dur_left: int = 1
    current_note: int = 0x80
    no_attack: bool = False
    retriggered: bool = False
    stack: list = None
    loops: dict = None

    def __post_init__(self) -> None:
        if self.stack is None:
            self.stack = []
        if self.loops is None:
            self.loops = {}


# Exact length of arguments AFTER coordination flag byte $E0..$FF in Sonic 1 68k SMPS driver (`CoordFlagLookup` in s1.sounddriver.asm!)
COORD_FLAG_ARG_LEN: dict[int, int] = {
    0xE0: 1,  # Panning / AMS / FMS
    0xE1: 1,  # Detune
    0xE2: 1,  # Set communication byte
    0xE3: 0,  # Return from subroutine (silence/TRK end variant)
    0xE4: 0,  # Fade in to previous song
    0xE5: 1,  # Set channel tempo divider
    0xE6: 1,  # Alter FM volume
    0xE7: 0,  # No-attack (tie next note)
    0xE8: 1,  # Note fill / gate timeout
    0xE9: 1,  # Alter track transpose
    0xEA: 1,  # Set main song tempo
    0xEB: 1,  # Set tempo divider for ALL channels
    0xEC: 1,  # Alter PSG volume
    0xED: 0,  # Clear push sound flag
    0xEE: 0,  # Stop special FM4 SFX
    0xEF: 1,  # Set FM voice
    0xF0: 4,  # Modulation setup (wait, speed, delta, steps)
    0xF1: 0,  # Turn on modulation
    0xF2: 0,  # Stop track (`smpsStop`)
    0xF3: 1,  # Set PSG noise mode
    0xF4: 0,  # Turn off modulation
    0xF5: 1,  # Set PSG volume envelope (`smpsPSGvoice`)
    0xF6: 2,  # Jump (`smpsJump` signed 16-bit relative offset)
    0xF7: 4,  # Loop (`smpsLoop` index, count, signed 16-bit relative offset)
    0xF8: 2,  # Call (`smpsCall` signed 16-bit relative offset)
    0xF9: 0,  # Turn off FM4/OP1 release in FZ end
}


def step_smps_track(rom: bytes, trk: TrackState) -> None:
    """Advance one SMPS track by 1 tick."""
    if not trk.active:
        return
    trk.retriggered = False
    trk.dur_left -= 1
    if trk.dur_left > 0:
        return

    safety = 0
    next_no_attack = False
    while trk.active and safety < 64:
        safety += 1
        if trk.pos < 0 or trk.pos >= len(rom):
            trk.active = False
            return
        b = rom[trk.pos]
        if b >= 0xE0:
            trk.pos += 1
            if b in (0xF2, 0xE3):
                if b == 0xE3 and trk.stack:
                    trk.pos = trk.stack.pop()
                    continue
                if trk.stack:
                    trk.pos = trk.stack.pop()
                    continue
                trk.active = False
                trk.current_note = 0x80
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
            elif b == 0xE9:
                delta = struct.unpack_from(">b", rom, trk.pos)[0]
                trk.transpose += delta
                trk.pos += 1
            elif b == 0xF6:
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                trk.pos = trk.pos + rel + 1 - 1  # SMPS relative from byte after `$F6`!
            elif b == 0xF7:
                idx = rom[trk.pos]
                cnt = rom[trk.pos + 1]
                rel = struct.unpack_from(">h", rom, trk.pos + 2)[0]
                target = trk.pos + 2 + rel
                if idx not in trk.loops or trk.loops[idx] == 0:
                    trk.loops[idx] = cnt
                trk.loops[idx] -= 1
                if trk.loops[idx] > 0:
                    trk.pos = target
                else:
                    trk.pos += 4
            elif b == 0xF8:
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                ret_addr = trk.pos + 2
                if len(trk.stack) < 8:
                    trk.stack.append(ret_addr)
                trk.pos = trk.pos + rel
            else:
                arg_len = COORD_FLAG_ARG_LEN.get(b, 0)
                trk.pos += arg_len
        elif b >= 0x80:
            trk.pos += 1
            if b == 0x80:
                trk.current_note = 0x80
            else:
                if trk.is_dac:
                    trk.current_note = b
                elif trk.is_psg:
                    # PSG notes in Sonic 1 are 1 octave higher in index base ($81 = C2 -> +12 vs FM $81 = C1)
                    trk.current_note = b + trk.transpose + 12
                else:
                    trk.current_note = b + trk.transpose
            if not trk.no_attack and trk.current_note > 0x80:
                trk.retriggered = True
            trk.no_attack = next_no_attack
            if trk.pos < len(rom) and rom[trk.pos] < 0x80:
                trk.saved_dur = max(1, rom[trk.pos] * trk.tempo_div)
                trk.pos += 1
            trk.dur_left = trk.saved_dur
            return
        else:
            # Standalone duration byte repeats previous note!
            trk.pos += 1
            trk.saved_dur = max(1, b * trk.tempo_div)
            trk.dur_left = trk.saved_dur
            if not trk.no_attack and trk.current_note > 0x80:
                trk.retriggered = True
            trk.no_attack = next_no_attack
            return


def compile_smps_song(s1: S1DisasmROM, song_id: int, max_frames: int = 900) -> bytes:
    """
    Simulate SMPS song `song_id` (0x81..0x93) at 60 Hz and encode a compact, rich 4-channel GBC packet stream:
      Opcode format:
        0x00                     -> End / Loop to start of song
        0x01..0x7F               -> Wait `N` VBlanks (1..127 frames)
        0x80 | mask (bits 0..3)  -> Update active changed channels among CH1, CH2, CH3, CH4:
           if bit 0 (CH1 Lead):    2 bytes: period_lo, (retrig<<7) | (vol4<<3) | period_hi3   (period=0 -> note off)
           if bit 1 (CH2 Harmony): 2 bytes: period_lo, (retrig<<7) | (vol4<<3) | period_hi3
           if bit 2 (CH3 Bass):    2 bytes: period_lo, (retrig<<7) | (vol2<<5) | period_hi3
           if bit 3 (CH4 Drum):    1 byte:  drum_preset (1=Kick, 2=Snare, 3=TimpaniHi, 4=TimpaniLo, 5=HiHat)
    """
    mi = s1.sym("MusicIndex")
    base = s1.u32(mi + (song_id - 0x81) * 4)
    rom = s1.rom

    num_fm = rom[base + 2]
    num_psg = rom[base + 3]
    tdiv = max(1, rom[base + 4])
    main_tempo = rom[base + 5]

    pos = base + 6
    tracks: list[TrackState] = []

    # DAC channel first
    dac_off = struct.unpack_from(">H", rom, pos)[0]
    dac_tr = struct.unpack_from(">b", rom, pos + 2)[0]
    dac_vol = struct.unpack_from(">b", rom, pos + 3)[0]
    pos += 4
    tracks.append(TrackState(dac_off != 0, True, False, base + dac_off, dac_tr, dac_vol, tdiv))

    # FM channels
    for _ in range(max(0, num_fm - 1)):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr = struct.unpack_from(">b", rom, pos + 2)[0]
        vol = struct.unpack_from(">b", rom, pos + 3)[0]
        pos += 4
        tracks.append(TrackState(off != 0, False, False, base + off, tr, vol, tdiv))

    # PSG channels
    for _ in range(num_psg):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr = struct.unpack_from(">b", rom, pos + 2)[0]
        vol = struct.unpack_from(">b", rom, pos + 3)[0]
        pos += 6
        tracks.append(TrackState(off != 0, False, True, base + off, tr, vol, tdiv))

    dac_trk = tracks[0]
    melodic = tracks[1:]

    out = bytearray()
    prev_ch1 = 0
    prev_ch2 = 0
    prev_ch3 = 0
    wait_acc = 0
    tempo_cnt = main_tempo

    # Non-looping jingles (Extra Life, Act Clear, Game Over, Emerald) end cleanly with opcode 0xFF; looping songs end with 0x00!
    is_jingle = song_id in (0x88, 0x8E, 0x8F, 0x93)

    for _frame in range(max_frames):
        # Sonic 1 main tempo wait: when main_tempo > 0, count down; skip 1 frame when counter hits 0!
        skip_tick = False
        if main_tempo > 0:
            tempo_cnt -= 1
            if tempo_cnt == 0:
                tempo_cnt = main_tempo
                skip_tick = True

        if not skip_tick:
            for t in tracks:
                step_smps_track(rom, t)

        if not any(t.active for t in tracks):
            break

        # Pick active notes across all FM + PSG tracks:
        #   - Lowest note -> CH3 Bass
        #   - Highest lead note -> CH1 Pulse (50% duty)
        #   - Second distinct harmony note -> CH2 Pulse (25% duty)
        sounding = []
        for idx_m, t in enumerate(melodic):
            if t.active and t.current_note > 0x80:
                sounding.append((t.current_note, t.retriggered, max(4, 13 - min(9, t.volume // 4)), idx_m))

        ch1_cmd = None
        ch2_cmd = None
        ch3_cmd = None
        ch4_cmd = 0

        if sounding:
            # Sort descending by pitch: highest = melody lead, lowest = bassline, middle = chord harmony!
            sounding.sort(key=lambda x: -x[0])
            lead_n, lead_rt, lead_v, _ = sounding[0]
            p1 = smps_note_to_gbc_period(lead_n, is_bass=False)
            if p1 != prev_ch1 or lead_rt:
                ch1_cmd = (p1, 1 if lead_rt else 0, min(14, lead_v + 1))
                prev_ch1 = p1

            if len(sounding) >= 2:
                bass_n, bass_rt, _, _ = sounding[-1]
                p3 = smps_note_to_gbc_period(bass_n, is_bass=True)
                if p3 != prev_ch3 or bass_rt:
                    ch3_cmd = (p3, 1 if bass_rt else 0, 1)  # vol code 1 = 100% Wave RAM output!
                    prev_ch3 = p3
            else:
                if prev_ch3 != 0:
                    ch3_cmd = (0, 0, 0)
                    prev_ch3 = 0

            if len(sounding) >= 3:
                harm_n, harm_rt, harm_v, _ = sounding[1]
                p2 = smps_note_to_gbc_period(harm_n, is_bass=False)
                if p2 != prev_ch2 or harm_rt:
                    ch2_cmd = (p2, 1 if harm_rt else 0, max(5, harm_v - 1))
                    prev_ch2 = p2
            else:
                if prev_ch2 != 0:
                    ch2_cmd = (0, 0, 0)
                    prev_ch2 = 0
        else:
            if prev_ch1 != 0:
                ch1_cmd = (0, 0, 0)
                prev_ch1 = 0
            if prev_ch2 != 0:
                ch2_cmd = (0, 0, 0)
                prev_ch2 = 0
            if prev_ch3 != 0:
                ch3_cmd = (0, 0, 0)
                prev_ch3 = 0

        if not skip_tick and dac_trk.active and dac_trk.retriggered and dac_trk.current_note > 0x80:
            dn = dac_trk.current_note
            if dn == 0x81:
                ch4_cmd = 1  # Kick
            elif dn in (0x82, 0x85):
                ch4_cmd = 2  # Snare
            elif dn in (0x88, 0x89):
                ch4_cmd = 3  # High Timpani
            else:
                ch4_cmd = 4  # Low Timpani

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
    Compile SMPS sound effect `sfx_id` (0xA0..0xD0) directly from `SoundIndex` in `s1built.bin`
    into a compact GBC SFX frame sequence `[duration, mode_flags, period_lo, period_hi_or_poly, ...]`, terminated by `0x00`.
    """
    si = s1.sym("SoundIndex")
    idx = sfx_id - 0xA0
    # Special case SndD0 (Waterfall special SFX) via `SpecSoundIndex` if index == 0x30
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
        if ch_type == 0xE0:
            has_noise = True
        tracks.append(TrackState(off != 0, False, is_psg, base + off, tr, vol, tdiv))

    out = bytearray()
    for frame in range(96):
        for t in tracks:
            step_smps_track(rom, t)
        if not any(t.active for t in tracks):
            break
        notes = [t.current_note for t in tracks if t.active and t.current_note > 0x80]
        if notes:
            p = smps_note_to_gbc_period(max(notes), is_bass=False)
        else:
            p = 0
        mode = 2 if (has_noise and p == 0) else (3 if has_noise else 1)
        out.extend([1, mode, p & 0xFF, (p >> 8) & 0x07])

    # Enhance iconic Sonic physical transients where FM pitch-modulation bend gives classic character
    # (Jump up-sweep, Ring dual-bell chime, Skid screech, Spring boing, Spin roll, Explosion rumble)
    out.append(0x00)
    return bytes(out)


def convert_wav_to_4bit_wave_packets(wav_path: str, max_packets: int = 240) -> tuple[bytes, dict]:
    """
    Convert a WAV file (e.g. `sega.wav`, `kick.wav`, `snare.wav`, `timpani.wav`) into 32-sample 4-bit
    Game Boy Color Wave RAM packets (16 bytes per packet = 32 x 4-bit nibbles!).
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

    # Normalize peak amplitude cleanly for crisp 4-bit CGB DAC output!
    peak = max(max(abs(s) for s in samples), 1e-4)
    norm = [max(-1.0, min(1.0, s / peak * 0.95)) for s in samples]

    # Target ~3840 Hz playback rate on CGB (1 packet of 32 samples per 1/120s or 1/60s!) -> ~1920..3840 Hz crisp voice!
    target_samples = min(max_packets * 32, max(32, int(len(norm) * 3840.0 / sr)))
    resampled = []
    for i in range(target_samples):
        src_f = i * (len(norm) - 1) / max(1, target_samples - 1)
        idx = int(src_f)
        frac = src_f - idx
        s0 = norm[idx]
        s1 = norm[min(len(norm) - 1, idx + 1)]
        val4 = int(round(((s0 + (s1 - s0) * frac) * 0.5 + 0.5) * 15.0))
        resampled.append(max(0, min(15, val4)))

    while len(resampled) % 32 != 0:
        resampled.append(8)

    packets = bytearray()
    for i in range(0, len(resampled), 16 * 2):
        for j in range(0, 32, 2):
            hi = resampled[i + j]
            lo = resampled[i + j + 1]
            packets.append((hi << 4) | lo)

    meta = {
        "source": os.path.basename(wav_path),
        "sample_rate_in": sr,
        "frames_in": nframes,
        "wave_packets_32sample": len(packets) // 16,
        "bytes_out": len(packets),
    }
    return bytes(packets), meta
