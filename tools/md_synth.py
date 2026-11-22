#!/usr/bin/env python3
"""
A small YM2612 + SN76489 model, good enough to judge what a Sonic 1 sound effect sounds like frame by frame
(loudness, pitch, how noisy it is) and to render reference WAVs. Driven by the per-frame track snapshots of
`tools/sfx_smps.py`.

YM2612 side (after MAME's fm.c): 4 operators, the 8 algorithms, operator-1 feedback (pi/16 .. 4pi), a full-scale
modulator moving the phase by 8*pi, and the envelope generator with its rate shift / increment tables
(attack, decay 1 to the sustain level, decay 2, release; key scaling from the block / F-number key code).
Operator multiples are applied, detune is ignored. SN76489 side: square tones and the 15-bit white noise LFSR,
2 dB per attenuation step.
"""
from __future__ import annotations

import math

import numpy as np

from sfx_smps import FM_SAMPLE_RATE, PSG_SAMPLE_RATE, CARRIERS

SR = FM_SAMPLE_RATE
FRAME = SR / 60.0

# MAME eg_inc rows, selected by effective rate
EG_INC = [
    [0, 1, 0, 1, 0, 1, 0, 1], [0, 1, 0, 1, 1, 1, 0, 1], [0, 1, 1, 1, 0, 1, 1, 1], [0, 1, 1, 1, 1, 1, 1, 1],
    [1, 1, 1, 1, 1, 1, 1, 1], [1, 1, 1, 2, 1, 1, 1, 2], [1, 2, 1, 2, 1, 2, 1, 2], [1, 2, 2, 2, 1, 2, 2, 2],
    [2, 2, 2, 2, 2, 2, 2, 2], [2, 2, 2, 4, 2, 2, 2, 4], [2, 4, 2, 4, 2, 4, 2, 4], [2, 4, 4, 4, 2, 4, 4, 4],
    [4, 4, 4, 4, 4, 4, 4, 4], [4, 4, 4, 8, 4, 4, 4, 8], [4, 8, 4, 8, 4, 8, 4, 8], [4, 8, 8, 8, 4, 8, 8, 8],
    [8, 8, 8, 8, 8, 8, 8, 8], [0, 0, 0, 0, 0, 0, 0, 0],
]


def rate_params(r: int):
    """(shift, row) for an effective rate 0..63."""
    if r <= 1:
        return 0, 17                     # no change
    if r < 48:
        return 11 - (r >> 2), r & 3
    if r < 60:
        return 0, 4 + (r - 48)
    return 0, 16


# operator numbering below: slot S1..S4 (YM); smpsVc 'op k' = slot 5-k
def slot_of(op_k: int) -> int:
    return 5 - op_k


class Operator:
    def __init__(self):
        self.att = 1023          # 10-bit attenuation
        self.state = 'off'       # attack / decay / sustain / release / off
        self.phase = 0.0

    def key_on(self):
        self.state = 'attack'
        self.phase = 0.0

    def key_off(self):
        if self.state != 'off':
            self.state = 'release'


def voice_params(v: dict):
    """Per slot (1..4): mul, tl, ar, d1r, d2r, dl, rr, rs; plus alg, fb."""
    def per(key, default=0):
        vals = v.get(key, [default] * 4)
        if not isinstance(vals, list):
            vals = [vals] * 4
        return {slot_of(k + 1): vals[k] for k in range(4)}
    mul = per('CoarseFreq')
    tl = {k: v & 0x7F for k, v in per('TotalLevel').items()}   # bit 7 = S1 driver's algorithm mask bit
    ar, d1r, d2r = per('AttackRate'), per('DecayRate1'), per('DecayRate2')
    dl, rr, rs = per('DecayLevel'), per('ReleaseRate'), per('RateScale')
    return dict(alg=v.get('Algorithm', 0), fb=v.get('Feedback', 0), mul=mul, tl=tl, ar=ar, d1r=d1r, d2r=d2r,
                dl=dl, rr=rr, rs=rs)


def carrier_slots(alg: int):
    return [slot_of(k) for k in CARRIERS[alg]]


class FMChannel:
    """One YM2612 channel driven frame by frame."""

    def __init__(self):
        self.ops = {s: Operator() for s in (1, 2, 3, 4)}
        self.fb_hist = [0.0, 0.0]
        self.eg_counter = 0
        self.eg_sub = 0
        self.p = None
        self.volume = 0

    def set_voice(self, v: dict, volume: int):
        self.p = voice_params(v)
        self.volume = volume

    def keycode(self, word):
        block = (word >> 11) & 7
        fn = word & 0x7FF
        return (block << 2) | (fn >> 9)

    def _eg_tick(self, word):
        self.eg_counter += 1
        kc = self.keycode(word or 0)
        p = self.p
        for s, op in self.ops.items():
            if op.state == 'off':
                continue
            ks = kc >> (3 - p['rs'][s])
            if op.state == 'attack':
                rate = p['ar'][s] * 2
            elif op.state == 'decay':
                rate = p['d1r'][s] * 2
            elif op.state == 'sustain':
                rate = p['d2r'][s] * 2
            else:
                rate = p['rr'][s] * 4 + 2
            r = min(63, rate + ks) if rate else 0
            if op.state == 'attack' and r >= 62:
                op.att = 0
                op.state = 'decay'
                continue
            shift, row = rate_params(r)
            if self.eg_counter & ((1 << shift) - 1):
                continue
            inc = EG_INC[row][(self.eg_counter >> shift) & 7]
            if op.state == 'attack':
                op.att += ((~op.att) * inc) >> 4
                if op.att <= 0:
                    op.att = 0
                    op.state = 'decay'
            else:
                op.att = min(1023, op.att + inc)
                sl = 1023 if p['dl'][s] == 15 else p['dl'][s] << 5
                if op.state == 'decay' and op.att >= sl:
                    op.state = 'sustain'
                if op.state == 'release' and op.att >= 1023:
                    op.state = 'off'

    def level(self, s):
        """Linear amplitude of slot s (envelope + TL, + track volume on carriers)."""
        p = self.p
        att = self.ops[s].att + (p['tl'][s] << 3)
        if s in carrier_slots(p['alg']):
            att += (self.volume & 0x7F) << 3
        return 2.0 ** (-min(1023 * 2, att) / 64.0) if self.ops[s].state != 'off' or self.ops[s].att < 1023 else 0.0

    def render(self, n, word, keyed):
        """n samples at F-number word; returns (signal, per-carrier mean levels)."""
        out = np.zeros(n)
        if self.p is None:
            return out, {}
        p = self.p
        f = 0.0
        if word:
            fnum = word & 0x7FF
            block = (word >> 11) & 7
            f = fnum * (2 ** block) * SR / (2 ** 21)
        # envelope per sample block of 3
        levels = {s: np.zeros(n) for s in (1, 2, 3, 4)}
        for i in range(0, n, 3):
            self._eg_tick(word)
            for s in (1, 2, 3, 4):
                levels[s][i:i + 3] = self.level(s)
        mul = {s: (0.5 if p['mul'][s] == 0 else p['mul'][s]) for s in (1, 2, 3, 4)}
        t = np.arange(n)
        ph = {}
        for s in (1, 2, 3, 4):
            inc = 2 * math.pi * f * mul[s] / SR
            ph[s] = self.ops[s].phase + inc * t
            self.ops[s].phase = (self.ops[s].phase + inc * n) % (2 * math.pi)
        # slot 1 with feedback (sample loop)
        o1 = np.zeros(n)
        fb = p['fb']
        h0, h1 = self.fb_hist
        fbscale = 0.0 if fb == 0 else (2 ** (fb + 6)) * 8192 / 65536 / 1024 * 2 * math.pi / 8192 * 8192
        lv1 = levels[1]
        ph1 = ph[1]
        for i in range(n):
            m = (h0 + h1) * fbscale if fb else 0.0
            v = math.sin(ph1[i] + m) * lv1[i]
            h0, h1 = h1, v
            o1[i] = v
        self.fb_hist = [h0, h1]
        K = 8 * math.pi
        alg = p['alg']
        sinp = lambda s, m: np.sin(ph[s] + m) * levels[s]
        parts = {}
        if alg == 0:
            o2 = sinp(2, K * o1); o3 = sinp(3, K * o2); parts[4] = sinp(4, K * o3)
        elif alg == 1:
            o2 = sinp(2, 0); o3 = sinp(3, K * (o1 + o2)); parts[4] = sinp(4, K * o3)
        elif alg == 2:
            o2 = sinp(2, 0); o3 = sinp(3, K * o2); parts[4] = sinp(4, K * (o1 + o3))
        elif alg == 3:
            o2 = sinp(2, K * o1); o3 = sinp(3, 0); parts[4] = sinp(4, K * (o2 + o3))
        elif alg == 4:
            parts[2] = sinp(2, K * o1); parts[4] = sinp(4, K * sinp(3, 0))
        elif alg == 5:
            parts[2] = sinp(2, K * o1); parts[3] = sinp(3, K * o1); parts[4] = sinp(4, K * o1)
        elif alg == 6:
            parts[2] = sinp(2, K * o1); parts[3] = sinp(3, 0); parts[4] = sinp(4, 0)
        else:
            parts[1] = o1; parts[2] = sinp(2, 0); parts[3] = sinp(3, 0); parts[4] = sinp(4, 0)
        out = sum(parts.values())
        self.last_parts = parts
        car = {s: float(levels[s].mean()) for s in carrier_slots(alg)}
        return out, car

    def key(self, on):
        for op in self.ops.values():
            (op.key_on if on else op.key_off)()


class PSGChannel:
    def __init__(self, noise=False):
        self.noise = noise
        self.phase = 0.0
        self.lfsr = 0x8000
        self.acc = 0.0
        self.state = 1.0

    def render(self, n, N, att):
        if N is None or att >= 15:
            return np.zeros(n)
        amp = 10 ** (-2 * att / 20.0)
        f = PSG_SAMPLE_RATE / (2 * max(1, N))
        if not self.noise:
            t = self.phase + f / SR * np.arange(n)
            self.phase = (self.phase + f / SR * n) % 1.0
            return np.where((t % 1.0) < 0.5, amp, -amp)
        out = np.zeros(n)
        step = f / SR                      # LFSR shifts once per tone-3 period
        for i in range(n):
            self.acc += step
            while self.acc >= 1.0:
                self.acc -= 1.0
                bit = (self.lfsr ^ (self.lfsr >> 3)) & 1     # Sega SN76489: 16-bit register, taps 0 and 3
                self.lfsr = (self.lfsr >> 1) | (bit << 15)
                self.state = 1.0 if (self.lfsr & 1) else -1.0
            out[i] = self.state * amp
        return out


def render_tracks(frames, n_frames=None, want_parts=False):
    """Render every track separately. Returns (list of per-track signals, per-track per-frame carrier levels)."""
    if not frames:
        return [], []
    ntr = len(frames[0])
    n_frames = n_frames or len(frames) + 20          # let release tails ring out
    chans = []
    for k in range(ntr):
        t0 = frames[0][k]
        if t0['fm']:
            chans.append(FMChannel())
        else:
            chans.append(PSGChannel(noise=False))
    sigs = [[] for _ in range(ntr)]
    car_levels = [[] for _ in range(ntr)]
    parts = [dict() for _ in range(ntr)]      # per track: slot -> list of per-frame signals
    acc = 0.0
    prev_keyed = [False] * ntr
    for fi in range(n_frames):
        acc += FRAME
        n = int(acc)
        acc -= n
        snap = frames[fi] if fi < len(frames) else None
        for k in range(ntr):
            ch = chans[k]
            tr = snap[k] if snap else None
            if isinstance(ch, FMChannel):
                if tr is not None and tr['voice'] is not None:
                    if ch.p is None or ch.volume != tr['volume'] or ch._voice is not tr['voice']:
                        ch.set_voice(tr['voice'], tr['volume'])
                        ch._voice = tr['voice']
                keyed = bool(tr and tr['keyed'])
                if tr and tr['keyon']:
                    ch.key(True)
                elif prev_keyed[k] and not keyed:
                    ch.key(False)
                prev_keyed[k] = keyed
                word = (tr['freq'] if tr else getattr(ch, '_word', 0)) or 0
                ch._word = word
                ch.last_parts = {}
                sig, car = ch.render(n, word, keyed)
                car_levels[k].append(car)
                for sl in (1, 2, 3, 4):
                    parts[k].setdefault(sl, []).append(ch.last_parts.get(sl, np.zeros(n)))
            else:
                if tr is not None and tr['noise'] is not None and not ch.noise:
                    ch.noise = True
                N = tr['freq'] if (tr and tr['keyed']) else None
                att = tr['psg_att'] if tr else 15
                sig = ch.render(n, N, att)
                car_levels[k].append({0: (10 ** (-2 * att / 20.0) if N is not None and att < 15 else 0.0)})
            sigs[k].append(sig)
    track_parts = [{sl: np.concatenate(v) for sl, v in pk.items()} for pk in parts]
    if want_parts:
        return [np.concatenate(s) for s in sigs], car_levels, track_parts
    return [np.concatenate(s) for s in sigs], car_levels
