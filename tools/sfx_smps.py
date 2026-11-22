#!/usr/bin/env python3
"""
Sonic 1 sound effects, straight from the s1disasm sources: a small assembler for the SMPS2ASM macros the SFX
files use, and a frame-by-frame model of the Sonic 1 68k SMPS driver (`s1.sounddriver.asm`) that plays them.

The model follows the driver routines line by line where they shape the sound:
  * `FMUpdateTrack` / `PSGUpdateTrack`: duration countdown, `FMDoNext` / `PSGDoNext` byte parsing, `SetDuration`
    (duration x tempo divider), `FinishTrackUpdate` (note fill, envelope index and modulation reset per note);
  * `DoModulation`: wait, speed, delta, steps (halved at note start; when they run out the delta is negated);
  * `PSGDoVolFX`: PSG volume envelopes (`PSG1`..`PSG9`, `$80` = hold), clamped to attenuation $F;
  * coordination flags $E0..$F9 used by the SFX (pan, detune, volume, transposition, voice, modulation, PSG noise
    `$E7`, PSG voice, jump, loop, stop, clear-push, stop-special);
  * FM voices: SetVoice byte order, `FMSlotMask` carriers, track volume added to the carrier TLs.

Every frame of every track is reported as a dict; `tools/sfx_gbc.py` turns those into Game Boy APU frames.
"""
from __future__ import annotations

import os
import re

NOTE_NAMES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}
CHANNELS = {'cFM1': 0x00, 'cFM2': 0x01, 'cFM3': 0x02, 'cFM4': 0x04, 'cFM5': 0x05, 'cFM6': 0x06,
            'cPSG1': 0x80, 'cPSG2': 0xA0, 'cPSG3': 0xC0, 'cNoise': 0xE0}
SYMS = {'smpsNoAttack': 0xE7, 'panNone': 0x00, 'panRight': 0x40, 'panLeft': 0x80, 'panCentre': 0xC0, 'panCenter': 0xC0}

MASTER_CLOCK = 53693175                 # `_Constants.asm` (NTSC)
M68000_CLOCK = MASTER_CLOCK / 7
Z80_CLOCK = MASTER_CLOCK / 15
FM_SAMPLE_RATE = M68000_CLOCK / (6 * 6 * 4)
PSG_SAMPLE_RATE = Z80_CLOCK / 16

# carriers in smpsVc operator numbering (op1 = register $4C slot, see SetVoice / FMSlotMask)
CARRIERS = {0: [1], 1: [1], 2: [1], 3: [1], 4: [1, 3], 5: [1, 2, 3], 6: [1, 2, 3], 7: [1, 2, 3, 4]}


def note_value(name: str) -> int | None:
    """Driver note byte for nC0..nB7 (nC0 = $81) / nRst = $80 / nMaxPSG (= nA5 for the Sonic 1 driver)."""
    if name == 'nRst':
        return 0x80
    if name == 'nMaxPSG':
        name = 'nA5'
    m = re.fullmatch(r'n([A-G])([sb]?)(\d)', name)
    if not m:
        return None
    semi = NOTE_NAMES[m.group(1)] + (1 if m.group(2) == 's' else -1 if m.group(2) == 'b' else 0)
    return 0x81 + int(m.group(3)) * 12 + semi


def num(tok: str, labels=None) -> int:
    tok = tok.strip()
    if tok in SYMS:
        return SYMS[tok]
    if re.fullmatch(r'[fs]Tone_[0-9A-Fa-f]{2}', tok):     # PSG envelope equates: fTone_01 = $01 ...
        return int(tok[-2:], 16)
    if tok in CHANNELS:
        return CHANNELS[tok]
    nv = note_value(tok)
    if nv is not None:
        return nv
    if '+' in tok[1:] or ('-' in tok[1:]):
        # tiny expression support: a+b, a-b
        m = re.fullmatch(r'(-?[^+-]+)([+-])(.+)', tok)
        a, op, b = num(m.group(1)), m.group(2), num(m.group(3))
        return a + b if op == '+' else a - b
    neg = tok.startswith('-')
    if neg:
        tok = tok[1:]
    v = int(tok[1:], 16) if tok.startswith('$') else int(tok, 0)
    return -v if neg else v


class SfxSource:
    """One assembled SFX file: header, voices, and a byte image with labels."""

    def __init__(self, path: str):
        self.path = path
        self.name = os.path.basename(path)
        self.tempo = 1
        self.tracks = []          # (channel id, label, transpose, volume)
        self.voices = []
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups = []          # (offset, label) for absolute 2-byte targets
        self._assemble(open(path, encoding='utf-8', errors='replace').read())

    def _assemble(self, text: str) -> None:
        skip = False
        cur_voice = None
        for raw in text.splitlines():
            line = raw.split(';', 1)[0].rstrip()
            s = line.strip()
            if not s:
                continue
            if s.startswith('if '):
                skip = True          # use the original (unfixed) data: take the `else` branch
                continue
            if s == 'else':
                skip = False
                continue
            if s == 'endif':
                skip = False
                continue
            if skip:
                continue
            m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$', s)
            if m:
                self.labels[m.group(1)] = len(self.code)
                s = m.group(2).strip()
                if not s:
                    continue
            parts = s.split(None, 1)
            op = parts[0]
            args = [a.strip() for a in parts[1].split(',')] if len(parts) > 1 else []
            if op.startswith('smpsVc'):
                if op == 'smpsVcAlgorithm':
                    cur_voice = {}
                    self.voices.append(cur_voice)
                key = op[6:]
                vals = [num(a) for a in args]
                cur_voice[key] = vals if len(vals) > 1 else vals[0]
                continue
            if op == 'smpsHeaderStartSong' or op == 'smpsHeaderVoice' or op == 'smpsHeaderChanSFX':
                continue
            if op == 'smpsHeaderTempoSFX':
                self.tempo = num(args[0])
                continue
            if op == 'smpsHeaderSFXChannel':
                tr = num(args[2]) & 0xFF
                if tr >= 0x80:
                    tr -= 0x100
                self.tracks.append((CHANNELS[args[0]], args[1], tr, num(args[3])))
                continue
            if op == 'dc.b':
                for a in args:
                    self.code.append(num(a) & 0xFF)
                continue
            if op == 'even':
                continue
            simple = {
                'smpsSetvoice': 0xEF, 'smpsFMvoice': 0xEF, 'smpsAlterVol': 0xE6, 'smpsPSGAlterVol': 0xEC,
                'smpsPSGform': 0xF3, 'smpsPSGvoice': 0xF5, 'smpsAlterNote': 0xE1, 'smpsDetune': 0xE1,
                'smpsAlterPitch': 0xE9, 'smpsChangeTransposition': 0xE9, 'smpsNoteFill': 0xE8,
                'smpsChanTempoDiv': 0xE5,
            }
            if op in simple:
                self.code.append(simple[op])
                self.code.append(num(args[0]) & 0xFF)
                continue
            if op == 'smpsPan':
                self.code += bytes((0xE0, (num(args[0]) + num(args[1])) & 0xFF))
                continue
            if op == 'smpsModSet':
                self.code.append(0xF0)
                self.code += bytes(num(a) & 0xFF for a in args)
                continue
            single = {'smpsStop': 0xF2, 'smpsModOff': 0xF4, 'smpsModOn': 0xF1, 'smpsClearPush': 0xED,
                      'smpsStopSpecial': 0xEE, 'smpsReturn': 0xE3, 'smpsNoAttack': 0xE7}
            if op in single:
                self.code.append(single[op])
                continue
            if op == 'smpsJump':
                self.code.append(0xF6)
                self.fixups.append((len(self.code), args[0]))
                self.code += b'\0\0'
                continue
            if op == 'smpsCall':
                self.code.append(0xF8)
                self.fixups.append((len(self.code), args[0]))
                self.code += b'\0\0'
                continue
            if op == 'smpsLoop':
                self.code += bytes((0xF7, num(args[0]) & 0xFF, num(args[1]) & 0xFF))
                self.fixups.append((len(self.code), args[2]))
                self.code += b'\0\0'
                continue
            raise ValueError(f"{self.name}: unsupported line: {s}")
        for off, lab in self.fixups:
            t = self.labels[lab]
            self.code[off] = t >> 8
            self.code[off + 1] = t & 0xFF


# ------------------------------------------------------------------------------------------------------------
def parse_driver_tables(driver_text: str):
    """PSG envelopes (PSG1..PSG9 via PSG_Index order), PSG and FM frequency tables, SoundPriorities."""
    # envelopes
    env = {}
    for m in re.finditer(r'^(PSG\d):\s*dc\.b\s*(.+?)(?=^\S|\Z)', driver_text, re.M | re.S):
        vals = []
        for tok in re.findall(r'\$?[0-9A-Fa-f]+', re.sub(r';[^\n]*', '', m.group(2).replace('dc.b', ','))):
            vals.append(int(tok[1:], 16) if tok.startswith('$') else int(tok))
        env[m.group(1)] = vals
    idx = re.search(r'PSG_Index:\s*(.+?)\n\s*\n', driver_text, re.S)
    order = re.findall(r'PSG\d', idx.group(1)) if idx else sorted(env)
    envelopes = [env[n] for n in order]
    # PSG table
    block = driver_text.split('PSGFrequencies:', 1)[1]
    freqs = []
    for line in block.splitlines():
        if line.strip().startswith('MakePSGFrequencies'):
            freqs += [float(x) for x in re.findall(r'\d+\.\d+|\d+', line.split('MakePSGFrequencies', 1)[1])]
        elif freqs and not line.strip().startswith(';'):
            break
    psg_n = [min(0x3FF, round(PSG_SAMPLE_RATE / (f * 2))) for f in freqs]
    # FM table: MakeFMFrequenciesOctave 0..7 over the base octave list
    base = [float(x) for x in re.search(r'irp op,\s*([0-9., ]+)', driver_text.split('MakeFMFrequenciesOctave macro', 1)[1]).group(1).split(',')]
    fm = []
    for octv in range(8):
        for f in base:
            fm.append(round(f * 1024 * 1024 * 2 / FM_SAMPLE_RATE) + octv * 0x800)
    # Indices past the 96-entry table (SS Goal keeps transposing up) read code bytes on the real driver;
    # continue the scale at block 7 instead (F-number doubled per octave, capped at $7FF).
    for i in range(96, 128):
        f = base[i % 12] * (2 ** (i // 12 - 7))
        fm.append(min(0x7FF, round(f * 1024 * 1024 * 2 / FM_SAMPLE_RATE)) + 7 * 0x800)
    # priorities
    pri_block = re.split(r'^SoundPriorities:', driver_text, maxsplit=1, flags=re.M)[1]
    pri = []
    for line in pri_block.splitlines():
        line = line.split(';', 1)[0]
        if 'dc.b' not in line:
            if pri:
                break
            continue
        pri += [int(t[1:], 16) for t in re.findall(r'\$[0-9A-Fa-f]+', line.split('dc.b', 1)[1])]
    return envelopes, psg_n, fm, pri


def psg_hz(n: int) -> float:
    return PSG_SAMPLE_RATE / (2 * max(1, n))


def fm_hz(word: int) -> float:
    fnum = word & 0x7FF
    block = (word >> 11) & 7
    return fnum * (2 ** block) * FM_SAMPLE_RATE / (2 ** 21)


# ------------------------------------------------------------------------------------------------------------
class Track:
    def __init__(self, src: SfxSource, chan: int, label: str, transpose: int, volume: int, tempo: int):
        self.src = src
        self.chan = chan
        self.fm = (chan & 0x80) == 0
        self.ptr = src.labels[label]
        self.transpose = transpose
        self.volume = volume
        self.tempo = tempo
        self.active = True
        self.timeout = 1
        self.saved = 1
        self.freq = 0 if self.fm else -1
        self.rest = False
        self.no_attack = False
        self.note_fill = 0
        self.note_fill_master = 0
        self.env_index = 0
        self.psg_voice = 0
        self.noise = None          # PSG noise form byte, or None
        self.voice = None          # FM voice dict
        self.voice_idx = 0
        self.pan = 0xC0
        self.detune = 0
        self.mod_on = False
        self.mod_data = None
        self.mod_wait = self.mod_speed = self.mod_delta = self.mod_steps = 0
        self.mod_val = 0
        self.loops = [0] * 8
        self.stack = []
        self.keyon_event = False   # new note attacked this frame
        self.keyed = False         # FM key / PSG audible
        self.out_freq = None       # frequency written this frame (FM word / PSG N incl. detune & modulation)
        self.psg_att = 15
        self.clear_push = False

    def _read(self):
        b = self.src.code[self.ptr]
        self.ptr += 1
        return b

    def _coord(self, b):
        c = self.src.code
        if b == 0xE0:
            self.pan = self._read() & 0xC0 or self.pan
        elif b == 0xE1:
            v = self._read()
            self.detune = v - 256 if v >= 128 else v
        elif b == 0xE6:
            v = self._read()
            self.volume = (self.volume + (v - 256 if v >= 128 else v)) & 0xFF
        elif b == 0xE7:
            self.no_attack = True
        elif b == 0xE8:
            self.note_fill = self.note_fill_master = self._read()
        elif b == 0xE9:
            v = self._read()
            self.transpose += v - 256 if v >= 128 else v
        elif b == 0xEC:
            v = self._read()
            self.volume = (self.volume + (v - 256 if v >= 128 else v)) & 0xFF
        elif b == 0xED:
            self.clear_push = True
        elif b == 0xEE:
            self.active = False
            self.keyed = False
        elif b == 0xEF:
            self.voice_idx = self._read()
            self.voice = self.src.voices[self.voice_idx]
        elif b == 0xF0:
            self.mod_on = True
            self.mod_data = tuple(c[self.ptr:self.ptr + 4])
            self.ptr += 4
            self._mod_reset()
        elif b == 0xF1:
            self.mod_on = True
        elif b == 0xF2:
            self.active = False
            self.keyed = False
        elif b == 0xF3:
            self.noise = self._read()
        elif b == 0xF4:
            self.mod_on = False
        elif b == 0xF5:
            self.psg_voice = self._read()
        elif b == 0xF6:
            self.ptr = (c[self.ptr] << 8) | c[self.ptr + 1]
        elif b == 0xF7:
            idx, cnt = self._read(), self._read()
            if self.loops[idx] == 0:
                self.loops[idx] = cnt
            self.loops[idx] -= 1
            if self.loops[idx]:
                self.ptr = (c[self.ptr] << 8) | c[self.ptr + 1]
            else:
                self.ptr += 2
        elif b == 0xF8:
            self.stack.append(self.ptr + 2)
            self.ptr = (c[self.ptr] << 8) | c[self.ptr + 1]
        elif b == 0xE3:
            self.ptr = self.stack.pop()
        else:
            raise ValueError(f"{self.src.name}: coord flag ${b:02X} not modelled")

    def _mod_reset(self):
        w, s, d, st = self.mod_data
        self.mod_wait, self.mod_speed = w, s
        self.mod_delta = d - 256 if d >= 128 else d
        self.mod_steps = st >> 1
        self.mod_val = 0

    def _set_duration(self, d):
        self.saved = self.timeout = (d * self.tempo) & 0xFF

    def _finish(self):
        self.timeout = self.saved
        if not self.no_attack:
            self.note_fill = self.note_fill_master
            self.env_index = 0
            if self.mod_on and self.mod_data:
                self._mod_reset()

    def _do_modulation(self):
        """Returns True when the frequency changed (DoModulation 'returns to caller')."""
        if not self.mod_on or not self.mod_data:
            return False
        if self.mod_wait:
            self.mod_wait -= 1
            return False
        self.mod_speed = (self.mod_speed - 1) & 0xFF
        if self.mod_speed:
            return False
        self.mod_speed = self.mod_data[1]
        if self.mod_steps == 0:
            self.mod_steps = self.mod_data[3]
            self.mod_delta = -self.mod_delta
            return False
        self.mod_steps -= 1
        self.mod_val += self.mod_delta
        return True

    def _note_fill_update(self):
        if self.note_fill == 0:
            return False
        self.note_fill -= 1
        if self.note_fill:
            return False
        self.rest = True
        self.keyed = False
        return True

    def step(self, ctx):
        self.keyon_event = False
        if not self.active:
            return
        self.timeout = (self.timeout - 1) & 0xFF
        if self.timeout == 0:
            self.no_attack = False       # bclr #4 before *DoNext; $E7 inside the parse sets it again
            self.rest = False
            # *DoNext
            while True:
                b = self._read()
                if b >= 0xE0:
                    self._coord(b)
                    if not self.active:
                        if self.clear_push:
                            ctx['clear_push'] = True
                        return
                    continue
                break
            attack = not self.no_attack
            if self.fm and attack:
                self.keyed = False       # FMNoteOff before every new byte (skipped under $E7)
            if b & 0x80:
                if self.fm:
                    i = b - 0x80
                    if i == 0:
                        self.rest = True
                        self.freq = 0
                    else:
                        self.freq = ctx['fm'][((i + self.transpose) & 0x7F)]
                else:
                    if b == 0x80:
                        self.rest = True
                        self.freq = -1
                    else:
                        self.freq = ctx['psg'][((b - 0x81 + self.transpose) & 0x7F) % len(ctx['psg'])]
                nb = self.src.code[self.ptr]
                if nb < 0x80:
                    self.ptr += 1
                    self._set_duration(nb)
            else:
                self._set_duration(b)
            self._finish()
            if self.clear_push:
                ctx['clear_push'] = True
            # note on
            if self.fm:
                if not self.rest and self.freq:
                    self.out_freq = (self.freq + self.detune) & 0xFFFF
                    if attack:
                        self.keyed = True
                        self.keyon_event = True
                    else:
                        self.keyed = True
                else:
                    self.rest = True
            else:
                if self.freq >= 0 and not self.rest:
                    self.out_freq = self.freq + self.detune
                    self.keyed = True
                    self.keyon_event = attack
                else:
                    self.rest = True
                    self.keyed = False
                self._psg_volume(force=True)
            return
        # note going
        if self._note_fill_update():
            return
        if not self.fm:
            self._psg_volume(force=False)
        if self._do_modulation():
            base = self.freq
            if self.fm:
                if base:
                    self.out_freq = (base + self.mod_val + self.detune) & 0xFFFF
            else:
                if base >= 0:
                    self.out_freq = base + self.mod_val + self.detune

    def _psg_volume(self, force):
        if not force and self.psg_voice == 0:
            return
        v = self.volume & 0xFF
        if self.psg_voice:
            envs = self.src_env
            e = envs[self.psg_voice - 1]
            i = self.env_index
            val = e[i] if i < len(e) else 0x80
            self.env_index += 1
            if val & 0x80:
                if val == 0x80:
                    self.env_index -= 1
                    # hold: previous attenuation stays
                    return
            v += val
            if v >= 0x10:
                v = 0x0F
        if self.rest:
            return
        self.psg_att = v & 0x0F


def load_sfx(asm_dir: str):
    """Return (sources by id, driver tables)."""
    drv = open(os.path.join(asm_dir, 's1.sounddriver.asm'), encoding='utf-8', errors='replace').read()
    envelopes, psg_n, fm, pri = parse_driver_tables(drv)
    sfx_dir = os.path.join(asm_dir, 'sound', 'sfx')
    srcs = {}
    for f in sorted(os.listdir(sfx_dir)):
        m = re.match(r'Snd([0-9A-F]{2})', f)
        if m and f.endswith('.asm'):
            srcs[int(m.group(1), 16)] = SfxSource(os.path.join(sfx_dir, f))
    return srcs, dict(envelopes=envelopes, psg=psg_n, fm=fm, priorities=pri)


def simulate(src: SfxSource, tables: dict, max_frames: int = 600):
    """Run the SFX; returns a list of frames, each a list of per-track snapshots (dicts)."""
    tracks = []
    for chan, label, tr, vol in src.tracks:
        t = Track(src, chan, label, tr, vol, src.tempo)
        t.src_env = tables['envelopes']
        tracks.append(t)
    ctx = {'fm': tables['fm'], 'psg': tables['psg']}
    frames = []
    for _ in range(max_frames):
        ctx['clear_push'] = False
        for t in tracks:
            t.step(ctx)
        snap = []
        for t in tracks:
            snap.append(dict(chan=t.chan, fm=t.fm, active=t.active, keyed=t.keyed and t.active and not t.rest,
                             keyon=t.keyon_event, freq=t.out_freq, voice=t.voice, volume=t.volume,
                             psg_att=t.psg_att, noise=t.noise, pan=t.pan))
        frames.append(snap)
        if not any(t.active for t in tracks):
            break
    return frames
