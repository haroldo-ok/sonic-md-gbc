#!/usr/bin/env python3
"""
Strategic 4-Channel Game Boy Color Arranger for Green Hill Zone (`Mus81_GHZ`).

Reads the authentic compiled `Mus81_GHZ` SMPS binary straight from the Sonic 1 Mega Drive ROM,
simulates all 9 tracks (`DAC`, `FM1..FM5`, `PSG1..PSG3`) with fixed `smpsNoAttack` (`$E7`) and
exact YM2612 carrier `MUL` octave shifts, and arranges them strategically for the 4 CGB channels:

  - CH1 (Pulse 1): Singing lead melody with dynamic duty (25%/50%), sustain vs pluck envelopes,
                   1-tick pitch-bend slides, and subtle vibrato.
  - CH2 (Pulse 2): Zero-click 30 Hz chord arpeggiator + counter-melody engine compressing
                   FM3 + FM4 + FM5 + PSG1 + PSG2 (3-note intro brass chords + PSG2 sparkle bells,
                   FM3 marimba fills + PSG1/2 off-beat chord stabs + FM4/5 ostinato/strings,
                   and FM5 bridge arpeggios + FM3/4 brass chords).
  - CH3 (Wave RAM): Selectable Wave RAM timbres (Wave 2 Major Chord opening swell, Wave 0 Punchy
                    FM Slap Bass for Intro/Section A, Wave 1 Warm Walking Bass for Bridge) with
                    2-stage dynamic volume shaping (`100% -> 50%`).
  - CH4 (Noise):   Punchy kick, crisp snare, timpani, closed hi-hats, and open hi-hat/crash drops.
"""
from __future__ import annotations

import os
import re
import struct
import sys
import zipfile
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from smps_converter import COORD_FLAG_ARG_LEN  # noqa: E402

NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# Exact lowest-pitch carrier `MUL` octave shift (in semitones) for GHZ Voices $00..$08
# Verified against `Mus81_GHZ_Voices` and `s1.sounddriver.asm` `FMInstrumentOperatorTable`:
#   V$00 (Walking Bass): OP4 MUL=0 (0.5x) -> -12
#   V$01 (Slap Bass):    OP4 MUL=1 (1x)   ->   0
#   V$02 (Intro Bells):  OP2/3/4 MUL=1    ->   0
#   V$03:                MUL=2            -> +12
#   V$04 (Strings/Arps): OP4 MUL=4 (4x)   -> +24
#   V$05 (Marimba):      OP3/4 MUL=4      -> +24
#   V$06 (Main Lead):    OP3/4 MUL=2      -> +12
#   V$07 (Bridge Brass): OP2/3/4 MUL=2    -> +12
#   V$08 (Intro Chords): OP3/4 MUL=4      -> +24
VOICE_OCT_SHIFT = {0: -12, 1: 0, 2: 0, 3: 12, 4: 24, 5: 24, 6: 12, 7: 12, 8: 24}

# Bank 8 timbre / wave command codes (passed through CH4 stream byte when ch4 drum isn't active):
WAVE_SLAP_BASS = 0x10
WAVE_WARM_BASS = 0x11
WAVE_MAJ_CHORD = 0x12
WAVE_BRIGHT    = 0x13

# Timbre encoding: 0x20 | (duty_2bit << 2) | (env >> 1) for CH1; 0x30 | ... for CH2
# duty_2bit: 0=12.5%, 1=25%, 2=50%, 3=75%; env_code: 0->env0(sustain), 1->env2, 2->env4, 3->env6
def ch1_timbre(duty_code: int, env_code: int) -> int:
    return 0x20 | ((duty_code & 3) << 2) | (env_code & 3)

def ch2_timbre(duty_code: int, env_code: int) -> int:
    return 0x30 | ((duty_code & 3) << 2) | (env_code & 3)


def semi_to_name(semi: int) -> str:
    return "--" if semi <= 0 else f"{NOTE_NAMES[semi % 12]}{semi // 12}"


def semi_to_period_pulse(semi: int, max_hz: float = 1580.0) -> int:
    """Convert semitone (C4=48) to GBC pulse period, folding cleanly so Hz <= max_hz."""
    if semi <= 0:
        return 0
    s = int(semi)
    while s < 24:
        s += 12
    while 261.625565 * (2.0 ** ((s - 48) / 12.0)) > max_hz:
        s -= 12
    hz = 261.625565 * (2.0 ** ((s - 48) / 12.0))
    return max(1, min(1966, int(round(2048.0 - 131072.0 / hz))))


def semi_to_period_bass(semi: int) -> int:
    """Convert semitone (C4=48) to GBC Wave RAM period strictly within 55..340 Hz (C2..E4)."""
    if semi <= 0:
        return 0
    s = int(semi)
    while s < 24:  # C2 = 65.41 Hz (safely above 55 Hz audit floor)
        s += 12
    while s > 52:  # E4 = 329.63 Hz (safely below 340 Hz audit ceiling)
        s -= 12
    hz = 261.625565 * (2.0 ** ((s - 48) / 12.0))
    return max(857, min(1855, int(round(2048.0 - 65536.0 / hz))))


@dataclass
class Trk:
    name: str
    active: bool
    is_dac: bool
    is_psg: bool
    is_noise: bool
    pos: int
    transpose: int
    volume: int
    tdiv: int
    psg_env: int = 0
    fm_voice: int = 0
    saved_dur: int = 1
    dur_left: int = 1
    note_fill: int = 0
    fill_left: int = 0
    note: int = 0
    retrig: bool = False
    jumps: int = 0
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


def load_md_rom() -> bytes:
    zpath = os.path.join(os.environ.get("USERPROFILE", ""), "Downloads", "Sonic The Hedgehog (USA, Europe).zip")
    if not os.path.exists(zpath):
        raise FileNotFoundError(f"Sonic 1 MD ROM zip not found: {zpath}")
    with zipfile.ZipFile(zpath) as zf:
        for name in zf.namelist():
            if name.lower().endswith((".md", ".bin", ".gen")):
                return zf.read(name)
        return zf.read(zf.namelist()[0])


def find_ghz_offset(rom: bytes) -> int:
    for off in range(0x70000, len(rom) - 24):
        if rom[off + 2:off + 6] == b"\x06\x03\x01\x03":
            voff = struct.unpack_from(">H", rom, off)[0]
            if 0x100 < voff < 0x1200 and rom[off + 12:off + 14] == b"\xF4\x12" and rom[off + 16:off + 18] == b"\x00\x0B":
                return off
    raise RuntimeError("Could not locate Mus81_GHZ header in MD ROM")


def load_ghz_tracks(rom: bytes, base: int) -> tuple[list[Trk], int]:
    tdiv = rom[base + 4]
    tempo = rom[base + 5]
    pos = base + 6
    names = ["DAC", "FM1", "FM2", "FM3", "FM4", "FM5", "PSG1", "PSG2", "PSG3"]
    trks: list[Trk] = []

    off = struct.unpack_from(">H", rom, pos)[0]
    tr, vol = struct.unpack_from(">bb", rom, pos + 2)
    pos += 4
    trks.append(Trk("DAC", off != 0, True, False, False, base + off, tr, vol, tdiv))

    for i in range(5):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr, vol = struct.unpack_from(">bb", rom, pos + 2)
        pos += 4
        trks.append(Trk(names[1 + i], off != 0, False, False, False, base + off, tr, vol, tdiv))

    for i in range(3):
        off = struct.unpack_from(">H", rom, pos)[0]
        tr, vol = struct.unpack_from(">bb", rom, pos + 2)
        env = rom[pos + 5]
        pos += 6
        trks.append(Trk(names[6 + i], off != 0, False, True, i == 2, base + off, tr, vol, tdiv, psg_env=env))

    return trks, tempo


def step_track(rom: bytes, trk: Trk, all_trks: list[Trk]) -> None:
    """Advance one track by 1 tick, applying smpsNoAttack ($E7) accurately to the NEXT note."""
    if not trk.active:
        return
    trk.retrig = False

    if trk.fill_left > 0:
        trk.fill_left -= 1
        if trk.fill_left == 0 and not trk.is_dac:
            trk.note = 0

    if trk.mod_on and trk.note > 0:
        if trk.mod_wait_left > 0:
            trk.mod_wait_left -= 1
        else:
            trk.mod_phase = (trk.mod_phase + 1) & 15

    trk.dur_left -= 1
    if trk.dur_left > 0:
        return

    no_atk = False
    for _ in range(96):
        b = rom[trk.pos]
        if b >= 0xE0:
            trk.pos += 1
            if b == 0xE3:
                if trk.stack:
                    trk.pos = trk.stack.pop()
                    continue
                trk.active = False
                trk.note = 0
                return
            elif b == 0xF2:
                trk.active = False
                trk.note = 0
                return
            elif b == 0xE5:
                trk.tdiv = max(1, rom[trk.pos])
                trk.pos += 1
            elif b in (0xE6, 0xEC):
                trk.volume = max(0, min(63, trk.volume + struct.unpack_from(">b", rom, trk.pos)[0]))
                trk.pos += 1
            elif b == 0xE7:
                no_atk = True
            elif b == 0xE8:
                trk.note_fill = rom[trk.pos] * trk.tdiv
                trk.pos += 1
            elif b == 0xE9:
                trk.transpose += struct.unpack_from(">b", rom, trk.pos)[0]
                trk.pos += 1
            elif b == 0xEB:
                nd = max(1, rom[trk.pos])
                trk.pos += 1
                for ot in all_trks:
                    ot.tdiv = nd
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
                trk.is_noise = True
                trk.pos += 1
            elif b == 0xF4:
                trk.mod_on = False
            elif b == 0xF5:
                trk.psg_env = rom[trk.pos]
                trk.pos += 1
            elif b == 0xF6:
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                trk.pos = trk.pos + 1 + rel
                trk.jumps += 1
            elif b == 0xF7:
                idx, cnt, rel = rom[trk.pos], rom[trk.pos + 1], struct.unpack_from(">h", rom, trk.pos + 2)[0]
                if idx not in trk.loops or trk.loops[idx] == 0:
                    trk.loops[idx] = cnt
                trk.loops[idx] -= 1
                trk.pos = (trk.pos + 3 + rel) if trk.loops[idx] > 0 else (trk.pos + 4)
            elif b == 0xF8:
                rel = struct.unpack_from(">h", rom, trk.pos)[0]
                trk.stack.append(trk.pos + 2)
                trk.pos = trk.pos + 1 + rel
            else:
                trk.pos += COORD_FLAG_ARG_LEN.get(b, 0)
        elif b >= 0x80:
            trk.pos += 1
            if b == 0x80:
                trk.note = 0
            elif trk.is_dac or trk.is_noise:
                trk.note = b
            elif trk.is_psg:
                trk.note = 36 + ((b - 0x81 + trk.transpose) & 0x7F)
            else:
                trk.note = max(1, ((b - 0x80 + trk.transpose) & 0x7F) - 1)
            if not no_atk and trk.note > 0:
                trk.retrig = True
                trk.mod_wait_left = trk.mod_wait
                trk.mod_phase = 0
            if trk.pos < len(rom) and rom[trk.pos] < 0x80:
                trk.saved_dur = max(1, rom[trk.pos] * trk.tdiv)
                trk.pos += 1
            trk.dur_left = trk.saved_dur
            trk.fill_left = trk.note_fill if (0 < trk.note_fill < trk.dur_left) else 0
            return
        else:
            trk.pos += 1
            trk.saved_dur = max(1, b * trk.tdiv)
            trk.dur_left = trk.saved_dur
            if not no_atk and trk.note > 0:
                trk.retrig = True
                trk.mod_wait_left = trk.mod_wait
                trk.mod_phase = 0
            trk.fill_left = trk.note_fill if (0 < trk.note_fill < trk.dur_left) else 0
            return


def simulate_ghz_frames(rom: bytes, base: int) -> tuple[list[dict], int]:
    """Simulate full intro + 1 complete main loop pass (0..3168), returning per-frame track snapshots and loop_frame."""
    trks, tempo = load_ghz_tracks(rom, base)
    tcnt = tempo
    frames: list[dict] = []
    loop_frame = None

    for f in range(4000):
        skip = False
        if tempo > 0:
            tcnt -= 1
            if tcnt == 0:
                tcnt = tempo
                skip = True
        if not skip:
            jb = trks[1].jumps
            for t in trks:
                step_track(rom, t, trks)
            if trks[1].jumps > jb:
                loop_frame = f
                break

        snap = {"skip": skip}
        for t in trks:
            if t.is_dac or t.is_noise:
                snap[t.name] = {
                    "note": t.note if (t.active and not skip) else 0,
                    "retrig": t.retrig and not skip,
                }
            else:
                semi = 0
                if t.active and t.note > 0:
                    semi = t.note + (0 if t.is_psg else VOICE_OCT_SHIFT.get(t.fm_voice, 0))
                vib = 0
                if t.mod_on and t.mod_wait_left == 0 and semi > 0:
                    vib = (1 if (t.mod_phase & 8) else -1) if (t.mod_phase & 4) else 0
                snap[t.name] = {
                    "semi": semi,
                    "retrig": t.retrig and not skip,
                    "voice": t.psg_env if t.is_psg else t.fm_voice,
                    "vol": t.volume,
                    "vib": vib,
                    "dur": t.dur_left,
                }
        frames.append(snap)

    assert loop_frame is not None and loop_frame > 1000, f"Unexpected GHZ loop_frame={loop_frame}"
    clean_intro_grace_scoops(frames)
    # Mus81_GHZ_Jump04 begins at frame 864 (where FM1 hits its main loop body)
    return frames, 864


def clean_intro_grace_scoops(frames: list[dict]) -> None:
    """Strip 1-4 frame SMPS chromatic pitch-bend scoops and 1-2 frame FM3 lag in Intro chords (`96..863`)."""
    n = min(864, len(frames))
    for k in ("FM3", "FM4", "FM5"):
        f = 96
        while f < n:
            s = frames[f][k]["semi"]
            if s <= 0:
                f += 1
                continue
            end = f + 1
            while end < n and frames[end][k]["semi"] == s and (frames[end]["skip"] or not frames[end][k]["retrig"]):
                end += 1
            if end - f <= 4 and end < n:
                nxt = frames[end][k]["semi"]
                if nxt > 0 and not frames[end][k]["retrig"] and 1 <= abs(nxt - s) <= 2:
                    for j in range(f, end):
                        frames[j][k]["semi"] = nxt
            f = end

    for f in range(96, n - 2):
        if frames[f]["FM3"]["semi"] == 0 and (frames[f]["FM4"]["retrig"] or frames[f]["FM5"]["retrig"]):
            for look in (1, 2):
                nxt_s = frames[f + look]["FM3"]["semi"]
                if nxt_s > 0:
                    frames[f]["FM3"]["semi"] = nxt_s
                    frames[f]["FM3"]["retrig"] = True
                    frames[f + look]["FM3"]["retrig"] = False
                    break


def arrange_ghz(frames: list[dict], loop_frame: int) -> bytes:
    """Transform the 9-track GHZ simulation into an expressive 4-channel GBC stream."""
    n = len(frames)

    # Pre-plan desired CH1/CH2/CH3/CH4 state per frame
    ch1_plan: list[tuple[int, int, int] | None] = [None] * n  # (semi_with_vib_per, rt, vol) -> store (per, rt, vol)
    ch2_plan: list[tuple[int, int, int] | None] = [None] * n
    ch3_plan: list[tuple[int, int, int] | None] = [None] * n  # (per, rt, vol3)
    ch4_plan: list[int] = [0] * n
    timbre_cmds: dict[int, list[int]] = {}

    def queue_cmd(frame_idx: int, cmd: int) -> None:
        if 0 <= frame_idx < n:
            timbre_cmds.setdefault(frame_idx, []).append(cmd)

    # Initial section timbres and section boundaries:
    #   0..95:     intro_pickup (FM1/3 bells, Wave 2 major chord swell f=0..11, Slap Bass at f=12)
    #   96..863:   intro_chords (FM3/4/5 bent brass chords + PSG2 sparkle bells)
    #   864..1560: section_a1 (Main Theme Part 1 + FM3 marimba fills + PSG1/2 stabs + FM4/5 ostinato)
    #   1561..2399: section_a2 (Main Theme Part 2 + low-priority FM4 strings + FM3 fills + PSG stabs)
    #   2400..3047: section_b (Bridge: FM1+PSG2 sustained melody + Walking Bass + FM5 rapid arps + FM3/4 chords)
    #   3048..end: turnaround (FM1 Voice $05 riff back to Jump04)
    queue_cmd(0, WAVE_MAJ_CHORD)
    queue_cmd(10, WAVE_SLAP_BASS)
    queue_cmd(94, ch1_timbre(2, 3))    # Intro chords CH1: 50% duty, slow fade (env=6)
    queue_cmd(95, ch2_timbre(1, 2))    # Intro chords CH2: 25% duty, med fade (env=4)
    queue_cmd(862, ch1_timbre(2, 2))   # Main theme CH1: 50% duty, env=4
    queue_cmd(863, ch2_timbre(1, 1))   # Main theme CH2: 25% duty, snappy env=2 (marimba/stabs)
    queue_cmd(1560, ch2_timbre(1, 1))  # Section A2 CH2: 25% duty, snappy env=2 so long FM4 strings fade out gently
    queue_cmd(2398, WAVE_WARM_BASS)    # Bridge CH3: Warm Walking Bass
    queue_cmd(2399, ch1_timbre(2, 3))  # Bridge CH1: 50% duty, smooth env=6 fade (avoid flat env=0 drone)
    queue_cmd(2400, ch2_timbre(1, 2))  # Bridge CH2: 25% duty, med fade env=4
    queue_cmd(3047, ch1_timbre(1, 1))  # Turnaround CH1: 25% duty, plucky env=2
    # Reset back to Main Theme timbres right before looping to frame 864:
    queue_cmd(n - 3, WAVE_SLAP_BASS)
    queue_cmd(n - 2, ch1_timbre(2, 2))
    queue_cmd(n - 1, ch2_timbre(1, 1))

    # --- 1. CH1 (Singing Lead Melody, rebalanced below CH3 Bass headroom) ---
    prev_ch1_semi = 0
    for f in range(n):
        fr = frames[f]
        fm1 = fr["FM1"]
        fm3 = fr["FM3"]
        lead = None

        if f < 96:
            if fm1["semi"] > 0:
                lead = (fm1["semi"], fm1["retrig"], 8, fm1["vib"])
        elif f < 864:
            # Intro: keep CH1 strictly on pitch-cleaned FM3 brass melody (no octave jumps to FM1/PSG1)
            if fm3["semi"] > 0:
                lead = (fm3["semi"], fm3["retrig"], 8, 0)
        elif f < 2400:
            # Main Theme A1/A2: FM1 is king!
            if fm1["semi"] > 0:
                lead = (fm1["semi"], fm1["retrig"], 9, fm1["vib"])
        elif f < 3048:
            # Bridge: FM1 held notes (reinforced by PSG2), smooth decay (env=6)
            if fm1["semi"] > 0:
                lead = (fm1["semi"], fm1["retrig"], 8, fm1["vib"])
        else:
            # Turnaround: FM1 Voice $05 riff
            if fm1["semi"] > 0:
                lead = (fm1["semi"], fm1["retrig"], 9, 0)

        if lead is not None:
            s, rt, vol, vib = lead
            p = max(1, min(1966, semi_to_period_pulse(s, 1580.0) + vib))
            # If note changed without retrigger (`smpsNoAttack`), keep rt=0 for smooth pitch bend!
            ch1_plan[f] = (p, 1 if rt else 0, vol)
            prev_ch1_semi = s
        else:
            ch1_plan[f] = (0, 0, 0)
            prev_ch1_semi = 0

    # --- 2. CH2 (Strategic 30 Hz Chord Arpeggiator + Counter-Melody, Low Priority for Long Holds) ---
    arp_idx = 0
    prev_chord_sig: tuple[int, ...] = ()
    prev_intro_sig: tuple[int, ...] = ()
    intro_chord_age = 0
    prev_ch2_mode = ""
    fm4_age = 0
    prev_fm4_s = 0
    fm3_age = 0
    prev_fm3_s = 0

    for f in range(n):
        fr = frames[f]
        fm1, fm3, fm4, fm5 = fr["FM1"], fr["FM3"], fr["FM4"], fr["FM5"]
        psg1, psg2 = fr["PSG1"], fr["PSG2"]

        if fm4["semi"] == 0 or fm4["retrig"] or fm4["semi"] != prev_fm4_s:
            fm4_age = 0
        else:
            fm4_age += 1
        prev_fm4_s = fm4["semi"]

        if fm3["semi"] == 0 or fm3["retrig"] or fm3["semi"] != prev_fm3_s:
            fm3_age = 0
        else:
            fm3_age += 1
        prev_fm3_s = fm3["semi"]

        picked_semi = 0
        picked_rt = False
        picked_vol = 8
        mode = "rest"

        if f < 96:
            # Intro pickup: single-channel bell melody on CH1 only (keep CH2 silent to preserve clean diatonic pitch)
            prev_chord_sig = ()
            mode = "rest"
        elif f < 864:
            # Intro chords: prioritize FM4+FM5+FM3 chord arp; long held chords (>=24f) yield to PSG2 sparkle cascade!
            chord_notes = []
            for tr in (fm5, fm4, fm3):
                if tr["semi"] > 0 and tr["semi"] not in chord_notes:
                    chord_notes.append(tr["semi"])
            has_intro_chord = len(chord_notes) >= 2 and (fm4["semi"] > 0 or fm5["semi"] > 0)
            sig = tuple(chord_notes) if has_intro_chord else ()
            chord_onset = any(tr["retrig"] for tr in (fm5, fm4, fm3) if tr["semi"] > 0) if has_intro_chord else False
            if not has_intro_chord or sig != prev_intro_sig or chord_onset:
                intro_chord_age = 0
                prev_intro_sig = sig
            else:
                intro_chord_age += 1

            if has_intro_chord and not (intro_chord_age >= 24 and psg2["semi"] > 0):
                if sig != prev_chord_sig:
                    if chord_onset or prev_ch2_mode != "arp_intro":
                        arp_idx = 0
                        picked_rt = True
                    prev_chord_sig = sig
                elif f % 2 == 0:
                    arp_idx = (arp_idx + 1) % len(chord_notes)
                picked_semi = chord_notes[arp_idx % len(chord_notes)]
                picked_vol = 6
                mode = "arp_intro"
            elif psg2["semi"] > 0:
                picked_semi = psg2["semi"]
                picked_rt = psg2["retrig"] or (prev_ch2_mode != "psg2_intro")
                picked_vol = 5
                prev_chord_sig = ()
                mode = "psg2_intro"
            elif psg1["semi"] > 0:
                picked_semi = psg1["semi"] + 12
                picked_rt = psg1["retrig"]
                picked_vol = 5
                prev_chord_sig = ()
                mode = "psg1_intro"
            else:
                prev_chord_sig = ()
        elif f < 1561:
            # Section A1: FM3 marimba fills > PSG1+PSG2 chord stab arps > FM4 marimba ostinato
            if fm3["semi"] > 0:
                picked_semi = fm3["semi"]
                picked_rt = fm3["retrig"] or (prev_ch2_mode != "fm3_a1")
                picked_vol = 6
                prev_chord_sig = ()
                mode = "fm3_a1"
            elif psg1["semi"] > 0 and psg2["semi"] > 0:
                chord_notes = [psg1["semi"], psg2["semi"]]
                if fm4["semi"] > 0 and fm4["semi"] not in chord_notes:
                    chord_notes.insert(0, fm4["semi"])
                sig = tuple(chord_notes)
                if sig != prev_chord_sig:
                    arp_idx = 0
                    picked_rt = psg1["retrig"] or psg2["retrig"] or (prev_ch2_mode != "psg_stab")
                    prev_chord_sig = sig
                elif f % 2 == 0:
                    arp_idx = (arp_idx + 1) % len(chord_notes)
                picked_semi = chord_notes[arp_idx % len(chord_notes)]
                picked_vol = 5
                mode = "psg_stab"
            elif fm4["semi"] > 0:
                picked_semi = fm4["semi"]
                picked_rt = fm4["retrig"] or (prev_ch2_mode != "fm4_ost")
                picked_vol = 5
                prev_chord_sig = ()
                mode = "fm4_ost"
            else:
                prev_chord_sig = ()
        elif f < 2400:
            # Section A2: FM3 fills & ostinato > PSG1+PSG2 full chord stabs > low-priority FM4 strings (<24f)
            if fm3["semi"] > 0:
                picked_semi = fm3["semi"]
                picked_rt = fm3["retrig"] or (prev_ch2_mode != "fm3_a2")
                picked_vol = 6
                prev_chord_sig = ()
                mode = "fm3_a2"
            elif psg1["semi"] > 0 and psg2["semi"] > 0:
                chord_notes = [psg1["semi"], psg2["semi"]]
                if fm4["semi"] > 0 and fm4["semi"] not in chord_notes:
                    chord_notes.insert(0, fm4["semi"])
                sig = tuple(chord_notes)
                if sig != prev_chord_sig:
                    arp_idx = 0
                    picked_rt = psg1["retrig"] or psg2["retrig"] or (prev_ch2_mode != "psg_stab")
                    prev_chord_sig = sig
                elif f % 2 == 0:
                    arp_idx = (arp_idx + 1) % len(chord_notes)
                picked_semi = chord_notes[arp_idx % len(chord_notes)]
                picked_vol = 5
                mode = "psg_stab"
            elif fm4["semi"] > 0 and fm4_age < 24:
                # Fold soaring C7 strings down 1 octave and fade out smoothly after 24f so they never drone!
                s4 = fm4["semi"] - 12 if fm4["semi"] > 79 else fm4["semi"]
                picked_semi = s4
                picked_rt = fm4["retrig"]
                picked_vol = 4
                prev_chord_sig = ()
                mode = "fm4_str"
            else:
                prev_chord_sig = ()
        elif f < 3048:
            # Bridge: Mix FM3+FM4 brass chord onset with FM5's iconic rapid sparkling arpeggios!
            chord_notes = []
            for tr in (fm4, fm3):
                if tr["semi"] > 0 and tr["semi"] not in chord_notes:
                    chord_notes.append(tr["semi"])
            if fm5["semi"] > 0:
                s5 = fm5["semi"]
                while s5 > 79:
                    s5 -= 12
                if s5 not in chord_notes:
                    chord_notes.append(s5)
            # When brass chord hits (`fm3` or `fm4` retrigger), cycle full chord at 30 Hz for first 12 ticks,
            # then let `fm5`'s fast 8-frame arpeggio sparkle cleanly underneath `FM1`'s sustained note!
            if (fm3["retrig"] or fm4["retrig"]) and len(chord_notes) >= 2:
                arp_idx = 0
                picked_semi = chord_notes[0]
                picked_rt = True
                picked_vol = 6
                prev_chord_sig = tuple(chord_notes)
                mode = "bridge_chord"
            elif prev_ch2_mode == "bridge_chord" and len(prev_chord_sig) >= 2 and (fm3["dur"] > 38 or fm4["dur"] > 38):
                if f % 2 == 0:
                    arp_idx = (arp_idx + 1) % len(prev_chord_sig)
                picked_semi = prev_chord_sig[arp_idx % len(prev_chord_sig)]
                picked_rt = False
                picked_vol = 6
                mode = "bridge_chord"
            elif fm5["semi"] > 0:
                s5 = fm5["semi"]
                while s5 > 79:
                    s5 -= 12
                picked_semi = s5
                picked_rt = fm5["retrig"] or (prev_ch2_mode not in ("bridge_arp", "bridge_chord"))
                picked_vol = 5
                mode = "bridge_arp"
            elif chord_notes and min(fm3_age, fm4_age) < 24:
                picked_semi = chord_notes[0]
                picked_rt = fm3["retrig"] or fm4["retrig"]
                picked_vol = 4
                mode = "bridge_pad"
        else:
            # Turnaround: FM4 brass / PSG1 echo beneath FM1's turnaround riff
            if fm4["semi"] > 0:
                picked_semi = fm4["semi"]
                picked_rt = fm4["retrig"] or (prev_ch2_mode != "turn_fm4")
                picked_vol = 5
                mode = "turn_fm4"
            elif psg1["semi"] > 0:
                picked_semi = psg1["semi"]
                picked_rt = psg1["retrig"]
                picked_vol = 5
                mode = "turn_psg1"

        prev_ch2_mode = mode
        if picked_semi > 0:
            p2 = semi_to_period_pulse(picked_semi, 1580.0)
            ch2_plan[f] = (p2, 1 if picked_rt else 0, picked_vol)
        else:
            ch2_plan[f] = (0, 0, 0)

    # --- 3. CH3 (Wave 2 Opening Chord Swell + Full-Power 100% Slap/Warm Bass) ---
    bass_age = 0
    prev_bass_p = 0
    for f in range(n):
        fr = frames[f]
        fm2 = fr["FM2"]
        if f >= 1 and f < 12:
            # Wave 2 Major Chord opening swell (`F2` -> `175 Hz + 262 Hz + 437 Hz` major triad)
            p_open = semi_to_period_bass(29)  # F2
            ch3_plan[f] = (p_open, 1 if f == 1 else 0, 1)
            prev_bass_p = p_open
            continue

        if fm2["semi"] > 0:
            p3 = semi_to_period_bass(fm2["semi"])
            if fm2["retrig"] or p3 != prev_bass_p:
                if fm2["retrig"]:
                    bass_age = 0
                else:
                    bass_age += 1
                rt3 = 1 if fm2["retrig"] else 0
            else:
                bass_age += 1
                rt3 = 0
            # Keep Wave RAM bass at 100% (`1`) across regular notes (<24f), stepping to 50% (`2`) only on very long holds!
            vol3 = 1 if bass_age < 24 else 2
            ch3_plan[f] = (p3, rt3, vol3)
            prev_bass_p = p3
        else:
            ch3_plan[f] = (0, 0, 0)
            prev_bass_p = 0
            bass_age = 0

    # --- 4. CH4 (Percussion Groove + Crash Drops + Timbre/Wave Commands) ---
    drop_frames = {96, 864, 1560, 2400, 3048}
    for f in range(n):
        fr = frames[f]
        dac = fr["DAC"]
        nz = fr["PSG3"]
        if dac["retrig"] and dac["note"] > 0:
            dn = dac["note"]
            if dn == 0x81:
                ch4_plan[f] = 1
            elif dn in (0x82, 0x85):
                ch4_plan[f] = 2
            elif dn in (0x88, 0x89):
                ch4_plan[f] = 3
            else:
                ch4_plan[f] = 4
        elif nz["retrig"] and nz["note"] > 0:
            # Upgrade closed hi-hat to Open Hi-Hat / Crash accent on major section transitions!
            if any(abs(f - df) <= 10 for df in drop_frames):
                ch4_plan[f] = 6
            else:
                ch4_plan[f] = 5

    # Inject pending timbre/wave commands into CH4 stream on idle CH4 slots near target frame
    for target_f, cmds in sorted(timbre_cmds.items()):
        for cmd in cmds:
            placed = False
            for off in (0, -1, -2, -3, -4, -5, -6, -7, -8, 1, 2):
                cf = target_f + off
                if 0 <= cf < n and ch4_plan[cf] == 0:
                    ch4_plan[cf] = cmd
                    placed = True
                    break
            assert placed, f"Could not schedule timbre command 0x{cmd:02X} near frame {target_f}"

    # --- 5. Delta-Compress & Encode Stream ---
    out = bytearray()
    prev_ch1 = (0, 0)  # (period, vol)
    prev_ch2 = (0, 0)
    prev_ch3 = (0, 0)  # (period, vol3)
    wait_acc = 0
    loop_off = 0

    for f in range(n):
        if f == loop_frame:
            while wait_acc > 0:
                w = min(127, wait_acc)
                out.append(w)
                wait_acc -= w
            loop_off = len(out)
            # Force clean re-sync of current pitches right at loop start
            prev_ch1 = (-1, -1)
            prev_ch2 = (-1, -1)
            prev_ch3 = (-1, -1)

        c1_cmd = None
        p1, rt1, v1 = ch1_plan[f]
        if p1 == 0:
            if prev_ch1[0] != 0:
                c1_cmd = (0, 0, 0)
                prev_ch1 = (0, 0)
        elif rt1 or p1 != prev_ch1[0]:
            c1_cmd = (p1, rt1, v1)
            prev_ch1 = (p1, v1)

        c2_cmd = None
        p2, rt2, v2 = ch2_plan[f]
        if p2 == 0:
            if prev_ch2[0] != 0:
                c2_cmd = (0, 0, 0)
                prev_ch2 = (0, 0)
        elif rt2 or p2 != prev_ch2[0]:
            c2_cmd = (p2, rt2, v2)
            prev_ch2 = (p2, v2)

        c3_cmd = None
        p3, rt3, v3 = ch3_plan[f]
        if p3 == 0:
            if prev_ch3[0] != 0:
                c3_cmd = (0, 0, 0)
                prev_ch3 = (0, 0)
        elif rt3 or p3 != prev_ch3[0] or v3 != prev_ch3[1]:
            c3_cmd = (p3, rt3, v3)
            prev_ch3 = (p3, v3)

        c4_cmd = ch4_plan[f]
        mask = (
            (1 if c1_cmd is not None else 0)
            | (2 if c2_cmd is not None else 0)
            | (4 if c3_cmd is not None else 0)
            | (8 if c4_cmd != 0 else 0)
        )
        if mask == 0:
            wait_acc += 1
        else:
            while wait_acc > 0:
                w = min(127, wait_acc)
                out.append(w)
                wait_acc -= w
            out.append(0x80 | mask)
            if c1_cmd is not None:
                p, rt, v = c1_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x0F) << 3) | ((p >> 8) & 0x07))
            if c2_cmd is not None:
                p, rt, v = c2_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x0F) << 3) | ((p >> 8) & 0x07))
            if c3_cmd is not None:
                p, rt, v = c3_cmd
                out.append(p & 0xFF)
                out.append(((rt & 1) << 7) | ((v & 0x03) << 5) | ((p >> 8) & 0x07))
            if c4_cmd != 0:
                out.append(c4_cmd)
            wait_acc = 1

    while wait_acc > 0:
        w = min(127, wait_acc)
        out.append(w)
        wait_acc -= w

    out.extend((0x00, loop_off & 0xFF, (loop_off >> 8) & 0xFF))
    return bytes(out)


def audit_stream(stream: bytes) -> dict:
    """Validate all frequency bounds matching tests/test_e2e_emulator.py."""
    pos = 0
    ch12_hz: list[float] = []
    ch3_hz: list[float] = []
    events = 0
    while pos < len(stream):
        b = stream[pos]
        pos += 1
        if b in (0x00, 0xFF):
            break
        if b < 0x80:
            continue
        events += 1
        mask = b & 0x0F
        if mask & 1:
            p = stream[pos] | ((stream[pos + 1] & 0x07) << 8)
            v = (stream[pos + 1] >> 3) & 0x0F
            pos += 2
            if p > 0 and v > 0:
                ch12_hz.append(131072.0 / (2048 - p))
        if mask & 2:
            p = stream[pos] | ((stream[pos + 1] & 0x07) << 8)
            v = (stream[pos + 1] >> 3) & 0x0F
            pos += 2
            if p > 0 and v > 0:
                ch12_hz.append(131072.0 / (2048 - p))
        if mask & 4:
            p = stream[pos] | ((stream[pos + 1] & 0x07) << 8)
            v3 = (stream[pos + 1] >> 5) & 0x03
            pos += 2
            if p > 0 and v3 > 0:
                ch3_hz.append(65536.0 / (2048 - p))
        if mask & 8:
            pos += 1

    assert ch12_hz, "No CH1/CH2 notes in arranged stream!"
    assert max(ch12_hz) <= 1600.0, f"CH1/CH2 max {max(ch12_hz):.1f} Hz > 1600 Hz"
    assert ch3_hz, "No CH3 bass notes!"
    assert 55.0 <= min(ch3_hz) and max(ch3_hz) <= 340.0, (
        f"CH3 out of 55..340 Hz: {min(ch3_hz):.1f}..{max(ch3_hz):.1f}"
    )
    return {
        "bytes": len(stream),
        "events": events,
        "ch12_min_hz": min(ch12_hz),
        "ch12_max_hz": max(ch12_hz),
        "ch3_min_hz": min(ch3_hz),
        "ch3_max_hz": max(ch3_hz),
    }


def update_banks(stream: bytes) -> None:
    gen_dir = os.path.join(ROOT, "generated")
    # 1. Write generated/bank_141_ghz_bgm.c
    out_c = os.path.join(gen_dir, "bank_141_ghz_bgm.c")
    lines = [
        "#pragma bank 141",
        "#include <stdint.h>",
        "/* Strategic 4-channel GBC arrangement of Mus81_GHZ (tools/ghz_arrange.py). */",
        "",
        f"const uint8_t mus_81[{len(stream)}] = {{",
    ]
    for i in range(0, len(stream), 24):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in stream[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    with open(out_c, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    # 2. Remove legacy mus_81 from generated/bank_076.c (keep mus_82 and mus_83)
    b76_path = os.path.join(gen_dir, "bank_076.c")
    b76_txt = open(b76_path, encoding="utf-8").read()
    new_b76 = re.sub(r"const uint8_t mus_81\[\d+\]\s*=\s*\{[^}]*\};\s*", "", b76_txt, count=1)
    if new_b76 != b76_txt:
        with open(b76_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new_b76)

    # 3. Point g_songs[0] (`0x81` GHZ) to Bank 141 in generated/bank_008_tables.c
    b8_path = os.path.join(gen_dir, "bank_008_tables.c")
    b8_txt = open(b8_path, encoding="utf-8").read()
    new_b8 = re.sub(r"\{\s*\d+\s*,\s*mus_81\s*\}", "{ 141, mus_81 }", b8_txt, count=1)
    assert "{ 141, mus_81 }" in new_b8, "Failed to update g_songs[0] bank to 141 in bank_008_tables.c!"
    with open(b8_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(new_b8)


def audit_intro_pitch(frames: list[dict], stream: bytes) -> None:
    """Assert zero out-of-key accidentals (`pc in {1, 3, 6, 8, 10}`) across `f=0..863` on CH1/CH2."""
    import math
    for f in range(min(864, len(frames))):
        for k in ("FM3", "FM4", "FM5"):
            s = frames[f][k]["semi"]
            if s > 0:
                pc = s % 12
                assert pc not in (1, 3, 6, 8, 10), f"Out-of-key intro {k} pc={pc} at frame {f}"

    pos, cur_f = 0, 0
    ch = [0, 0]
    while pos < len(stream) and cur_f < 864:
        b = stream[pos]
        pos += 1
        if b in (0x00, 0xFF):
            break
        if b < 0x80:
            for _ in range(b):
                if cur_f < 864:
                    for ch_idx, p in enumerate(ch):
                        if p > 0:
                            hz = 131072.0 / (2048 - p)
                            s = round(48 + 12 * math.log2(hz / 261.625565))
                            pc = s % 12
                            assert pc not in (1, 3, 6, 8, 10), (
                                f"Out-of-key intro CH{ch_idx + 1} semi={s} (pc={pc}) at frame {cur_f}"
                            )
                cur_f += 1
            continue
        mask = b & 0x0F
        if mask & 1:
            p = stream[pos] | ((stream[pos + 1] & 0x07) << 8)
            v = (stream[pos + 1] >> 3) & 0x0F
            pos += 2
            ch[0] = p if v > 0 else 0
        if mask & 2:
            p = stream[pos] | ((stream[pos + 1] & 0x07) << 8)
            v = (stream[pos + 1] >> 3) & 0x0F
            pos += 2
            ch[1] = p if v > 0 else 0
        if mask & 4:
            pos += 2
        if mask & 8:
            pos += 1


def main() -> None:
    rom = load_md_rom()
    base = find_ghz_offset(rom)
    frames, loop_frame = simulate_ghz_frames(rom, base)
    stream = arrange_ghz(frames, loop_frame)
    stats = audit_stream(stream)
    audit_intro_pitch(frames, stream)
    assert stats["bytes"] <= 16000, f"Stream exceeds 16 KB bank: {stats['bytes']}"
    update_banks(stream)
    print(
        f"Arranged GHZ (Mus81) -> bank_141_ghz_bgm.c: {stats['bytes']} bytes, {stats['events']} events, "
        f"frames={len(frames)} (loop_frame={loop_frame}), "
        f"CH1/2={stats['ch12_min_hz']:.1f}..{stats['ch12_max_hz']:.1f} Hz, "
        f"CH3={stats['ch3_min_hz']:.1f}..{stats['ch3_max_hz']:.1f} Hz"
    )


if __name__ == "__main__":
    main()
