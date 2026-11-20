#!/usr/bin/env python3
"""
Compile the Sonic 1 jump sound effect (`sound/sfx/SndA0 - Jump.asm`) straight from the MD disassembly
(sonicretro/s1disasm) into the GBC SFX frame format used by `hw_sound_tick()` in `src/main.c`.

Everything that defines the sound is read from the disassembly source, not typed in:
  * the SFX header: tempo divider, PSG channel, track transposition (`$F4` = -12 semitones);
  * the PSG1 track: `smpsPSGvoice`, note / duration pairs, `smpsModSet`, `smpsStop`;
  * the PSG frequency table (`PSGFrequencies`, built by the `MakePSGFrequency` formula with `PSG_Sample_Rate`);
  * the PSG voice 0 volume envelope (`PSG1:` table in `s1.sounddriver.asm`).

Driver model (`PSGSetFreq`, `PSGUpdateFreq`, the modulation routine and `VolEnvIndex`):
  * a note sets the SN76489 tone value N = table entry, index = note - nC0 + track transposition;
  * modulation (`smpsModSet wait, speed, change, steps`): after `wait` frames, N += change every `speed` frames,
    for `steps` changes; the modulation and the volume-envelope index restart on every new note;
  * the volume envelope gives an attenuation per frame; the last value is held.

GBC output frame (per 60 Hz frame):  [duration=1, mode, period_lo, period_hi, volume]
  mode bit 0 = tone on CH2, bit 2 = volume byte present (see `hw_sound_tick`).  Terminated by a 0 byte.
"""
import os
import re
import sys

SN_CLOCK = 3579545                 # NTSC Z80 clock (`_Constants.asm`: Z80_Clock)
PSG_SAMPLE_RATE = SN_CLOCK / 16.0  # `PSG_Sample_Rate`
GBC_VOLUME = [15, 12, 10, 8, 6, 5, 4, 3]   # SN attenuation 0..7 (2 dB steps) -> 4-bit GBC volume

NOTE_NAMES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def note_index(name):
    """`nC0` = 0 (driver note byte `nC0` = $81). Accepts names such as nF2, nBb2, nCs3."""
    m = re.fullmatch(r'n([A-G])([sb]?)(\d)', name)
    if not m:
        raise ValueError(f"unknown note name {name}")
    semi = NOTE_NAMES[m.group(1)] + (1 if m.group(2) == 's' else -1 if m.group(2) == 'b' else 0)
    return int(m.group(3)) * 12 + semi


def parse_psg_table(psg_asm_text):
    """Read the 60 frequencies listed under `PSGFrequencies:` (uncommented `MakePSGFrequencies` rows)."""
    block = psg_asm_text.split('PSGFrequencies:', 1)[1]
    freqs = []
    for line in block.splitlines():
        if not line.strip().startswith('MakePSGFrequencies'):
            if freqs and len(freqs) >= 60:
                break
            continue
        nums = re.findall(r'\d+\.\d+|\d+', line.split('MakePSGFrequencies', 1)[1])
        freqs.extend(float(x) for x in nums)
    if len(freqs) < 60:
        raise ValueError(f"PSG frequency table incomplete ({len(freqs)})")
    return freqs[:60]


def parse_psg_voice0(psg_asm_text):
    """`PSG1: dc.b 0,0,0,1,...,$80` = PSG voice 0 attenuation envelope (`$80` = hold last value)."""
    m = re.search(r'^PSG1:\s*dc\.b\s*([^\n]+)$', psg_asm_text, re.M)
    vals = []
    for tok in m.group(1).split(','):
        tok = tok.strip()
        if tok.startswith('$80'):
            break
        vals.append(int(tok.lstrip('$'), 16) if tok.startswith('$') else int(tok))
    return vals


def parse_track(jump_asm_text):
    """Return (tempo_div, transpose, notes[(index, duration)], modset(wait,speed,change,steps), env_index)."""
    tempo = int(re.search(r'smpsHeaderTempoSFX\s+\$([0-9A-Fa-f]+)', jump_asm_text).group(1), 16)
    hdr = re.search(r'smpsHeaderSFXChannel\s+cPSG1,[^,]+,\s*\$([0-9A-Fa-f]+),', jump_asm_text)
    transpose = int(hdr.group(1), 16)
    if transpose >= 0x80:
        transpose -= 0x100
    body = jump_asm_text.split('SndA0_Jump_PSG1:', 1)[1].split('; Song seems', 1)[0]
    env = 0
    modset = None
    notes = []
    pending = None
    for raw in body.splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        if line.startswith('smpsPSGvoice'):
            env = int(line.split()[1].lstrip('$'), 16)
        elif line.startswith('smpsModSet'):
            vals = [int(v.strip().lstrip('$'), 16) for v in line.split(None, 1)[1].split(',')]
            wait, speed, change, steps = vals
            if change >= 0x80:
                change -= 0x100
            modset = (wait, speed, change, steps)
        elif line.startswith('dc.b'):
            parts = [p.strip() for p in line[4:].split(',')]
            for i in range(0, len(parts), 2):
                name, dur = parts[i], parts[i + 1]
                d = int(dur.lstrip('$'), 16)
                notes.append((note_index(name), d))
        elif line.startswith('smpsStop'):
            break
    return tempo, transpose, notes, modset, env


def compile_jump(asm_dir):
    jump_asm = open(os.path.join(asm_dir, 'sound', 'sfx', 'SndA0 - Jump.asm'), encoding='utf-8', errors='replace').read()
    driver_asm = open(os.path.join(asm_dir, 's1.sounddriver.asm'), encoding='utf-8', errors='replace').read()

    tempo, transpose, notes, modset, env_idx = parse_track(jump_asm)
    table = parse_psg_table(driver_asm)
    envelope = parse_psg_voice0(driver_asm)
    wait, speed, change, steps_max = modset

    out = bytearray()
    for note, dur in notes:
        idx = note + transpose                       # `PSGSetFreq`: index = note - $81 + Transpose (nC0 = 0)
        idx &= 0x7F
        n0 = min(0x3FF, round(PSG_SAMPLE_RATE / (table[idx] * 2)))   # `MakePSGFrequency`
        for j in range(dur * tempo):
            steps = 0
            if j >= wait and speed > 0:
                steps = min(steps_max, (j - wait) // speed + 1)
            n = max(1, min(0x3FF, n0 + change * steps))
            # GBC pulse period: f = 131072 / (2048 - reg), SN tone f = PSG_SAMPLE_RATE / (2 * N)
            reg = 2048 - round(n * 262144.0 / PSG_SAMPLE_RATE)
            reg = max(0, min(2047, reg))
            att = envelope[min(j, len(envelope) - 1)]
            vol = GBC_VOLUME[min(att, 7)]
            out.extend((reg & 0xFF, (reg >> 8) & 0x07, vol))
    return bytes(out), {'notes': notes, 'transpose': transpose, 'modset': modset, 'env_index': env_idx,
                        'frames': len(out) // 3}


def emit_header(blob, info, path):
    """Write `generated/sfx_jump.h`: one {NR23, NR24-high-bits, NR22 volume} triple per 60 Hz frame."""
    rows = []
    for i in range(0, len(blob), 3):
        lo, hi, vol = blob[i], blob[i + 1], blob[i + 2]
        rows.append(f"    {{ 0x{lo:02X}, 0x{hi:02X}, {vol} }},")
    text = ("/* Auto-generated by tools/jump_sfx.py from s1disasm `sound/sfx/SndA0 - Jump.asm`.\n"
            " * Do not edit by hand. Frame count: %d. Source notes: %s, modset (wait,speed,change,steps) = %s. */\n"
            "#define JUMP_SFX_FRAMES %d\n"
            "static const uint8_t jump_sfx_frames[JUMP_SFX_FRAMES][3] = {\n%s\n};\n") % (
        info['frames'], info['notes'], info['modset'], info['frames'], "\n".join(rows))
    with open(path, 'w') as f:
        f.write(text)


if __name__ == '__main__':
    asm_dir = sys.argv[1] if len(sys.argv) > 1 else '/tmp/s1disasm'
    blob, info = compile_jump(asm_dir)
    print(info)
    emit_header(blob, info, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'sfx_jump.h'))
