#!/usr/bin/env python3
"""
Compile the Sonic 1 sound effects ($A1..$D0; the jump $A0 keeps `tools/jump_sfx.py`) from the s1disasm sources
into Game Boy APU streams, played by `hw_sound_tick()` in `src/main.c`.

Pipeline:
  1. `sfx_smps.py` runs each SFX through a model of the Sonic 1 SMPS driver: notes, durations x tempo divider,
     modulation, detune, volume, PSG volume envelopes, noise, loops, holds, per frame.
  2. `md_synth.py` renders every track (YM2612 operators/envelopes, SN76489 tones/noise). From the render we take
     each frame's loudness and how noisy the track is (spectral flatness).
  3. Every source becomes a Game Boy voice:
       * an FM track that sounds like noise (median flatness >= 0.45: bomb, roll, saw, electric...) -> noise,
         clocked from the render's spectral centroid;
       * other FM tracks -> one tone voice per carrier operator, at note frequency x the carrier's multiple,
         with that carrier's envelope + TL + track volume as its loudness (the ring's x2 and x9 partials, etc.);
       * PSG tones -> tone voices at the driver's PSG frequency; PSG noise ($E7, clocked by PSG3) -> noise.
     The two loudest tone voices get CH1 / CH2 (a quieter one fills a channel while that one is silent), the
     loudest noise voice gets CH4. Panning follows `smpsPan`.
  4. Loudness becomes GB volume 0..15 (amplitude x 15; PSG attenuation 0 = 15, as in the jump table). The GB
     volume envelope (NRx2) is fitted to each note, so a channel is only re-triggered on a new note or when the
     hardware envelope can no longer follow; frequencies change between triggers without restarting the note.

Stream format (per SFX):  header [owned channels mask, priority], then groups  [mask, data..., delay]
  mask bit0 CH1, bit1 CH2, bit3 CH4, bit4 NR51 pan bits, bit5 clear the push-block flag; mask 0 = end.
  tone  : ctl (bit7 trigger, bit6 mute, bits0-2 period high), lo (unless mute), NRx2 + NRx1 (if trigger)
  noise : ctl (bit7 trigger, bit6 mute, bit0 NR43 follows), NR43, NR42 (if trigger)
  delay : frames to wait before the next group (1..255).
"""
from __future__ import annotations

import math
import os
import sys
import wave

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
from sfx_smps import load_sfx, simulate, fm_hz, psg_hz, PSG_SAMPLE_RATE   # noqa: E402
from md_synth import render_tracks, voice_params, carrier_slots, SR as MD_SR, FRAME as MD_FRAME   # noqa: E402

NOISE_FLATNESS = 0.45
BUZZ_RATIO = 16.0          # FM brightness / fundamental above this -> 7-bit LFSR buzz on CH4
TAIL_FRAMES = 24
DUTY_50 = 0x80

# GB noise clocks: 524288 / r / 2^(s+1), r = 0 counts as 0.5
NOISE_CLOCKS = []
for s_ in range(14):
    for r_ in range(8):
        NOISE_CLOCKS.append(((1048576 if r_ == 0 else 524288 / r_) / (2 ** (s_ + 1)), s_, r_))


def tone_reg(f: float) -> int:
    if f <= 0:
        return 0
    return max(0, min(2047, round(2048 - 131072.0 / f)))


def noise_nr43(clock: float, buzz: bool = False) -> int:
    best = min(NOISE_CLOCKS, key=lambda c: abs(math.log(c[0]) - math.log(max(1.0, clock))))
    return (best[1] << 4) | (0x08 if buzz else 0) | best[2]   # bit 3: 7-bit LFSR (periodic buzz)


def pulse_centroid_ratio(duty: float, f0: float, top: float = 12000.0) -> float:
    """Spectral centroid of a pulse wave, in multiples of its fundamental (harmonics up to `top` Hz)."""
    n = np.arange(1, max(2, int(top / max(f0, 1.0))) + 1)
    a = np.abs(np.sin(np.pi * n * duty)) / n
    return float((a * n).sum() / max(1e-9, a.sum()))


def pick_duty(centroid_hz: float, f0: float) -> int:
    """NRx1 duty bits whose pulse spectrum is closest (log centroid) to the FM render's brightness."""
    if f0 <= 0 or centroid_hz <= 0:
        return DUTY_50
    want = math.log(centroid_hz / f0)
    best = min(((0x80, 0.5), (0x40, 0.25), (0x00, 0.125)),
               key=lambda d: abs(math.log(pulse_centroid_ratio(d[1], max(64.0, f0))) - want))
    return best[0]


BANDS = np.geomspace(80, 15000, 33)


def band_spectrum(sig, sr):
    """Average log power in 32 log-spaced bands (80 Hz..15 kHz), level-normalised."""
    n = 2048
    acc = None
    cnt = 0
    for i in range(0, max(1, len(sig) - n), n // 2):
        seg = sig[i:i + n]
        if len(seg) < n or np.sqrt((seg ** 2).mean()) < 1e-3:
            continue
        m = np.abs(np.fft.rfft(seg * np.hanning(n))) ** 2
        acc = m if acc is None else acc + m
        cnt += 1
    if acc is None:
        return None
    f = np.fft.rfftfreq(n, 1 / sr)
    b = np.array([(acc[(f >= lo) & (f < hi)].mean() if ((f >= lo) & (f < hi)).any() else 0.0) + 1e-12
                  for lo, hi in zip(BANDS[:-1], BANDS[1:])])
    lb = 10 * np.log10(b)
    return lb - lb.max()


_GB_NOISE_SPECTRA = {}


def gb_noise_spectrum(nr43):
    """Band spectrum of the GB noise channel for one NR43 value (rendered once, cached)."""
    if nr43 not in _GB_NOISE_SPECTRA:
        g = GBRender()
        s_, r_ = nr43 >> 4, nr43 & 7
        clk = (1048576 if r_ == 0 else 524288 / r_) / (2 ** (s_ + 1))
        g.width7 = bool(nr43 & 8)
        g.trigger(4, 0xF0, clk)
        sig = np.concatenate([g.run(GB_SR // 60) for _ in range(12)])
        _GB_NOISE_SPECTRA[nr43] = band_spectrum(sig, GB_SR)
    return _GB_NOISE_SPECTRA[nr43]


NOISE_MIN_CLOCK = 5000.0
MD_LOWPASS_HZ = 3400.0     # Model 1 Mega Drive audio output: ~3.4 kHz first-order low-pass


def md_output_filter(sig):
    """One-pole low-pass of the Mega Drive's output stage (darkens FM noise: 'whoosh' rather than 'shhh')."""
    a = math.exp(-2 * math.pi * MD_LOWPASS_HZ / MD_SR)
    out = np.empty_like(sig)
    y = 0.0
    for i, x in enumerate(sig):
        y = (1 - a) * x + a * y
        out[i] = y
    return out


def best_noise_setting(md_sig):
    """NR43 (clock + LFSR width) whose spectrum is closest to an FM noise carrier as heard through the Mega Drive
    output filter (the GB has no such filter, so the colour has to come from the LFSR clock)."""
    target = band_spectrum(md_output_filter(md_sig), MD_SR)
    if target is None:
        return None
    best = None
    for s_ in range(14):
        for w in (0, 8):
            for r_ in range(8):
                nr = (s_ << 4) | w | r_
                clk = (1048576 if r_ == 0 else 524288 / r_) / (2 ** (s_ + 1))
                if clk < NOISE_MIN_CLOCK:
                    continue       # slower LFSR clocks flutter audibly at frame rate instead of hissing
                sp = gb_noise_spectrum(nr)
                if sp is None:
                    continue
                d = float(np.mean(np.abs(sp - target)))
                if best is None or d < best[0]:
                    best = (d, nr)
    return best[1]


def frame_features(sig):
    n = int(MD_FRAME)
    rms, flat, cent = [], [], []
    for i in range(0, len(sig), n):
        seg = sig[i:i + n]
        if len(seg) < n:
            seg = np.pad(seg, (0, n - len(seg)))
        r = float(np.sqrt((seg ** 2).mean()))
        rms.append(r)
        if r < 1e-4:
            flat.append(0.0)
            cent.append(0.0)
            continue
        m = np.abs(np.fft.rfft(seg * np.hanning(n))) + 1e-9
        f = np.fft.rfftfreq(n, 1 / MD_SR)
        flat.append(float(np.exp(np.log(m).mean()) / m.mean()))
        cent.append(float((m * f).sum() / m.sum()))
    return np.array(rms), np.array(flat), np.array(cent)


class Voice:
    def __init__(self, kind, key, nfr):
        self.kind = kind            # 'tone' / 'noise'
        self.key = key
        self.amp = np.zeros(nfr)
        self.freq = np.zeros(nfr)   # Hz (tone) or LFSR clock (noise)
        self.trig = np.zeros(nfr, bool)
        self.pan = np.full(nfr, 0xC0)
        self.duty = DUTY_50
        self.buzz = False
        self.nr43 = None           # fixed NR43 picked by spectrum match (FM noise)

    @property
    def energy(self):
        return float((self.amp ** 2).sum())


def build_voices(src, tables):
    fr = simulate(src, tables)
    nfr = len(fr) + TAIL_FRAMES
    sigs, car, parts = render_tracks(fr, nfr, want_parts=True)
    voices = []
    info = []
    for k, sig in enumerate(sigs):
        rms, flat, cent = frame_features(sig)
        rms, flat, cent = rms[:nfr], flat[:nfr], cent[:nfr]
        t0 = fr[0][k]
        aud = rms > 0.01
        med_flat = float(np.median(flat[aud])) if aud.any() else 0.0
        snaps = [fr[f][k] if f < len(fr) else None for f in range(nfr)]
        if t0['fm']:
            # Every carrier operator is judged on its own render: a clean carrier is a tone, a carrier fed by a
            # chaotic modulator is noise, a very bright one is a 7-bit buzz (e.g. the roll = rising whistle + hiss).
            last = snaps[len(fr) - 1]
            tones, kinds, track_noise = [], [], []
            for slot, psig in sorted(parts[k].items()):
                prms, pflat, pcent = frame_features(psig)
                prms, pflat, pcent = prms[:nfr], pflat[:nfr], pcent[:nfr]
                paud = prms > 0.01
                if not paud.any():
                    continue
                fund = np.zeros(nfr)
                mulv = None
                for f in range(nfr):
                    sn = snaps[f] or last
                    if sn['voice'] is None or not sn['freq']:
                        continue
                    pv = voice_params(sn['voice'])
                    if slot not in carrier_slots(pv['alg']):
                        continue
                    mulv = 0.5 if pv['mul'][slot] == 0 else pv['mul'][slot]
                    fund[f] = fm_hz(sn['freq']) * mulv
                ok = paud & (fund > 0)
                if not ok.any():
                    continue
                pf = float(np.median(pflat[ok]))
                ratio = float(np.median(pcent[ok])) / float(np.median(fund[ok]))
                kind = 'noise' if pf >= NOISE_FLATNESS else ('buzz' if ratio > BUZZ_RATIO else 'tone')
                v = Voice('tone' if kind == 'tone' else 'noise', (k, slot, kind), nfr)
                for f in range(nfr):
                    sn = snaps[f]
                    if sn:
                        v.trig[f] = sn['keyon']
                        v.pan[f] = sn['pan']
                    elif f:
                        v.pan[f] = v.pan[f - 1]
                if kind == 'tone':
                    for f in range(nfr):
                        cl = car[k][f] if f < len(car[k]) else {}
                        v.amp[f] = min(1.0, cl.get(slot, 0.0)) if fund[f] > 0 else 0.0
                    v.freq[:] = fund
                    act = (v.amp > 0.05) & paud & (pcent > 0)
                    if act.any():
                        v.duty = pick_duty(float(np.median(pcent[act])), float(np.median(fund[act])))
                    tones.append(v)
                else:
                    # level as heard through the MD output low-pass: broadband hiss loses far more than a tone,
                    # so unfiltered RMS made noise carriers too loud (the roll's whoosh became a 'splash')
                    frms = frame_features(md_output_filter(psig))[0][:nfr]
                    v.amp[:] = np.minimum(1.0, frms * 1.5)
                    v.raw_peak = float(np.max(prms)) * 1.5
                    track_noise.append(v)
                    if kind == 'buzz':
                        v.buzz = True
                        v.freq[:] = np.where(fund > 0, fund, np.max(fund)) * 127.0
                    else:
                        v.freq[:] = np.maximum(64.0, pcent * 3.0)
                        v.nr43 = best_noise_setting(psig)
                    voices.append(v)
                kinds.append(f"S{slot}:{kind}")
            # a track with no tonal carrier keeps its unfiltered loudness (only its colour follows the filter)
            if not tones:
                for v in track_noise:
                    pk = float(v.amp.max())
                    if pk > 0:
                        v.amp[:] = np.minimum(1.0, v.amp * min(v.raw_peak, 1.0) / pk)
            # carriers at (nearly) the same pitch play as one voice
            merged = []
            for v in sorted(tones, key=lambda x: -x.energy):
                same = None
                for m in merged:
                    both = (m.freq > 0) & (v.freq > 0)
                    if both.any() and np.median(np.abs(np.log2(v.freq[both] / m.freq[both]))) < 0.04:
                        same = m
                        break
                if same is not None:
                    same.amp = np.minimum(1.0, same.amp + v.amp)
                else:
                    merged.append(v)
            voices += merged
            info.append(f"tr{k} FM " + " ".join(kinds))
        else:
            is_noise = any(sn and sn['noise'] is not None for sn in snaps)
            v = Voice('noise' if is_noise else 'tone', (k, 'psg'), nfr)
            for f, sn in enumerate(snaps):
                if not sn or not sn['keyed'] or sn['freq'] is None or sn['psg_att'] >= 15:
                    continue
                v.amp[f] = 10 ** (-2 * sn['psg_att'] / 20.0)
                N = max(1, sn['freq'])
                v.freq[f] = (PSG_SAMPLE_RATE / (2 * N)) if is_noise else psg_hz(N)
                v.trig[f] = sn['keyon']
            voices.append(v)
            info.append(f"tr{k} PSG->{v.kind}")
    return fr, voices, info


def allocate(voices, nfr):
    """Channel -> per-frame voice (or None). The two loudest tone voices own CH1 / CH2 and the loudest noise voice
    owns CH4; while an owner is silent, the loudest other active voice of the same kind borrows the channel."""
    tones = sorted([v for v in voices if v.kind == 'tone' and v.energy > 0], key=lambda v: -v.energy)
    noises = sorted([v for v in voices if v.kind == 'noise' and v.energy > 0], key=lambda v: -v.energy)

    def fill(primary, pool, taken):
        seq = []
        for f in range(nfr):
            v = primary if (primary is not None and primary.amp[f] > 0) else None
            if v is None:
                cand = [x for x in pool if x.amp[f] > 0 and x is not primary and (taken is None or x is not taken[f])]
                v = max(cand, key=lambda x: x.amp[f]) if cand else primary
            seq.append(v)
        return seq

    plan = {}
    plan[1] = fill(tones[0] if tones else None, tones[2:], None)
    plan[2] = fill(tones[1] if len(tones) > 1 else None, tones[2:], plan[1])
    plan[4] = fill(noises[0] if noises else None, noises[1:], None)
    return plan


def env_predict(v0, direction, period, j):
    if period == 0:
        return v0
    steps = int(j * 64 / 60 / period)
    return max(0, min(15, v0 + (steps if direction else -steps)))


def fit_envelope(target, start, end):
    """Best (v0, dir, period, length) for target[start:end] (ints, >0)."""
    v0 = int(target[start])
    best = (v0, 0, 0, 1)
    for direction in (0, 1):
        for period in range(0, 8):
            if period == 0 and direction == 1:
                continue
            n = 0
            for j in range(end - start):
                t = int(target[start + j])
                if t == 0 or abs(env_predict(v0, direction, period, j) - t) > (1 if t >= 3 else 0):
                    break
                n += 1
            if n > best[3]:
                best = (v0, direction, period, n)
    return best


def encode(src_id, voices, plan, nfr, priority, clear_push_frame=None):
    """Return the byte stream for one SFX (header + groups)."""
    owned = 0
    for ch in (1, 2, 4):
        if any(v is not None and v.amp.any() for v in set(plan[ch])):
            owned |= {1: 1, 2: 2, 4: 8}[ch]
    # per channel per frame: target vol, freq, trigger
    events = [dict() for _ in range(nfr)]
    for ch in (1, 2, 4):
        if not owned & {1: 1, 2: 2, 4: 8}[ch]:
            continue
        seq = plan[ch]
        vol = np.zeros(nfr, int)
        frq = np.zeros(nfr)
        trg = np.zeros(nfr, bool)
        pan = np.full(nfr, 0xC0)
        duty = np.full(nfr, DUTY_50)
        buzz = np.zeros(nfr, bool)
        fixed = [None] * nfr
        prev_v = None
        for f in range(nfr):
            v = seq[f]
            if v is None:
                prev_v = None
                continue
            vol[f] = min(15, int(round(v.amp[f] * 15)))
            if v.amp[f] > 0.02 and vol[f] == 0:
                vol[f] = 1
            frq[f] = v.freq[f]
            trg[f] = bool(v.trig[f]) or (v is not prev_v)
            pan[f] = v.pan[f]
            duty[f] = v.duty
            buzz[f] = v.buzz
            fixed[f] = v.nr43
            prev_v = v
        # walk frames and emit register state changes
        hw_on = False
        seg_start = seg_len = 0
        env = None
        last_freq_byte = None
        last_pan = None
        f = 0
        while f < nfr:
            ev = {}
            if vol[f] == 0:
                if hw_on:
                    ev['mute'] = True
                    hw_on = False
                    last_freq_byte = None
            else:
                need = (not hw_on) or trg[f] or (f >= seg_start + seg_len)
                if need:
                    end = f + 1
                    while end < nfr and vol[end] > 0 and not trg[end]:
                        end += 1
                    v0, d, p, n = fit_envelope(vol, f, end)
                    seg_start, seg_len = f, n
                    ev['trig'] = (v0 << 4) | (d << 3) | p
                    ev['duty'] = int(duty[f])
                    hw_on = True
                if ch == 4:
                    b = fixed[f] if fixed[f] is not None else noise_nr43(frq[f], bool(buzz[f]))
                else:
                    b = tone_reg(frq[f])
                if 'trig' in ev or b != last_freq_byte:
                    ev['freq'] = b
                    last_freq_byte = b
            pv = pan[f]
            if pv != last_pan and vol[f] > 0:
                ev['pan'] = pv
                last_pan = pv
            if ev:
                events[f][ch] = ev
            f += 1
    # serialise
    out = bytearray((owned, priority))
    last_f = -1
    pending_mask_bytes = None
    groups = []
    for f in range(nfr):
        evs = events[f]
        cp = clear_push_frame is not None and f == clear_push_frame
        if not evs and not cp:
            continue
        mask = 0
        data = bytearray()
        pan_bits = None
        for ch in (1, 2, 4):
            ev = evs.get(ch)
            if not ev:
                continue
            if 'pan' in ev:
                bits = {1: 0x11, 2: 0x22, 4: 0x88}[ch]
                l = ev['pan'] & 0x80
                r = ev['pan'] & 0x40
                val = (bits & 0x0F if r else 0) | (bits & 0xF0 if l else 0)
                pan_bits = (pan_bits or 0) | val
                if len(ev) == 1:
                    continue
            if ch == 4:
                mask |= 8
                ctl = (0x80 if 'trig' in ev else 0) | (0x40 if ev.get('mute') else 0) | (1 if 'freq' in ev else 0)
                data.append(ctl)
                if 'freq' in ev:
                    data.append(ev['freq'])
                if 'trig' in ev:
                    data.append(ev['trig'])
            else:
                mask |= 1 if ch == 1 else 2
                if ev.get('mute'):
                    data.append(0x40)
                    continue
                reg = ev.get('freq', 0)
                ctl = (0x80 if 'trig' in ev else 0) | ((reg >> 8) & 7)
                data.append(ctl)
                data.append(reg & 0xFF)
                if 'trig' in ev:
                    data.append(ev['trig'])
                    data.append(ev.get('duty', DUTY_50))
        if pan_bits is not None:
            mask |= 0x10
            data.append(pan_bits)
        if cp:
            mask |= 0x20
        groups.append((f, mask, data))
    prev_f = 0
    for gi, (f, mask, data) in enumerate(groups):
        if gi == 0 and f > 0:
            # leading silence: an empty group (mask with only... ) -> use a pan-less no-op: mask 0x80
            out += bytes((0x80, f))
        out.append(mask)
        out += data
        nxt = groups[gi + 1][0] if gi + 1 < len(groups) else nfr
        delay = max(1, nxt - f)
        while delay > 255:
            out += bytes((255, 0x80))
            delay -= 255
        out.append(delay)
    out.append(0)
    return bytes(out), owned


# ------------------------------------------------------------------------------------------------------------
# Game Boy APU model (for checking and A/B WAVs)
# ------------------------------------------------------------------------------------------------------------
GB_SR = 32768
DUTY = {0x00: 0.125, 0x40: 0.25, 0x80: 0.5, 0xC0: 0.75}


class GBRender:
    def __init__(self):
        self.ch = {c: dict(on=False, vol=0, env_dir=0, env_p=0, env_t=0.0, freq=0.0, phase=0.0, duty=0.5)
                   for c in (1, 2, 4)}
        self.lfsr = 0x7FFF
        self.nacc = 0.0
        self.width7 = False

    def trigger(self, c, env, freq, duty=0.5):
        s = self.ch[c]
        s.update(on=(env >> 4) > 0 or (env & 8) > 0, vol=env >> 4, env_dir=(env >> 3) & 1, env_p=env & 7,
                 env_t=0.0, freq=freq, duty=duty)
        if c != 4:
            s['phase'] = 0.0

    def run(self, n):
        out = np.zeros(n)
        dt = 1.0 / GB_SR
        for c, s in self.ch.items():
            if not s['on']:
                continue
            vols = np.zeros(n)
            for i in range(n):
                if s['env_p']:
                    s['env_t'] += dt * 64
                    if s['env_t'] >= s['env_p']:
                        s['env_t'] -= s['env_p']
                        s['vol'] = max(0, min(15, s['vol'] + (1 if s['env_dir'] else -1)))
                vols[i] = s['vol']
            if c == 4:
                sig = np.zeros(n)
                step = s['freq'] / GB_SR
                st = 1.0 if (self.lfsr & 1) == 0 else -1.0
                for i in range(n):
                    self.nacc += step
                    while self.nacc >= 1:
                        self.nacc -= 1
                        b = (self.lfsr ^ (self.lfsr >> 1)) & 1
                        self.lfsr = (self.lfsr >> 1) | (b << 14)
                        if self.width7:
                            self.lfsr = (self.lfsr & ~0x40) | (b << 6)
                        st = 1.0 if (self.lfsr & 1) == 0 else -1.0
                    sig[i] = st
            else:
                ph = s['phase'] + s['freq'] / GB_SR * np.arange(1, n + 1)
                s['phase'] = ph[-1] % 1.0
                sig = np.where((ph % 1.0) < s['duty'], 1.0, -1.0)
            out += sig * vols / 15.0
        return out


def render_stream(stream: bytes, extra_frames=10):
    g = GBRender()
    pos = 2
    out = []
    wait = 0
    frames = 0
    while True:
        if wait == 0:
            mask = stream[pos]
            pos += 1
            if mask == 0:
                break
            if mask == 0x80:
                wait = stream[pos]
                pos += 1
            else:
                for c in (1, 2):
                    if mask & (1 if c == 1 else 2):
                        ctl = stream[pos]
                        pos += 1
                        if ctl & 0x40:
                            g.ch[c]['on'] = False
                            continue
                        lo = stream[pos]
                        pos += 1
                        reg = ((ctl & 7) << 8) | lo
                        f = 131072.0 / (2048 - reg)
                        if ctl & 0x80:
                            env, duty = stream[pos], stream[pos + 1]
                            pos += 2
                            g.trigger(c, env, f, DUTY[duty & 0xC0])
                        else:
                            g.ch[c]['freq'] = f
                if mask & 8:
                    ctl = stream[pos]
                    pos += 1
                    if ctl & 0x40:
                        g.ch[4]['on'] = False
                    else:
                        clk = None
                        if ctl & 1:
                            nr43 = stream[pos]
                            pos += 1
                            s_, r_ = nr43 >> 4, nr43 & 7
                            clk = (1048576 if r_ == 0 else 524288 / r_) / (2 ** (s_ + 1))
                            g.width7 = bool(nr43 & 8)
                        if ctl & 0x80:
                            env = stream[pos]
                            pos += 1
                            g.trigger(4, env, clk or g.ch[4]['freq'])
                        elif clk:
                            g.ch[4]['freq'] = clk
                if mask & 0x10:
                    pos += 1
                wait = stream[pos]
                pos += 1
        out.append(g.run(GB_SR // 60))
        frames += 1
        wait -= 1
    for _ in range(extra_frames):
        out.append(g.run(GB_SR // 60))
    return np.concatenate(out) if out else np.zeros(1)


def render_old_blob(blob: bytes, extra_frames=10):
    """The previous converter's format: [dur, mode, lo, hi]... 0 (NR22=$A2, NR42=$92, NR43=$33 fixed)."""
    g = GBRender()
    pos = 0
    out = []
    while pos < len(blob) and blob[pos]:
        dur, mode, lo, hi = blob[pos:pos + 4]
        pos += 4
        if mode & 1 and (lo | hi):
            g.trigger(2, 0xA2, 131072.0 / (2048 - (lo | (hi & 7) << 8)))
        if mode & 2:
            g.trigger(4, 0x92, 524288 / 3 / (2 ** 4))
        for _ in range(dur):
            out.append(g.run(GB_SR // 60))
    for _ in range(extra_frames):
        out.append(g.run(GB_SR // 60))
    return np.concatenate(out)


def write_wav(path, sig, sr):
    sig = np.asarray(sig, float)
    peak = max(1e-6, float(np.abs(sig).max()))
    sig = sig / peak * 0.8
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((sig * 32767).astype('<i2').tobytes())


def compile_all(asm_dir):
    srcs, tables = load_sfx(asm_dir)
    pri = tables['priorities']
    result = {}
    for sid, src in sorted(srcs.items()):
        if sid == 0xA0:
            continue
        fr, voices, info = build_voices(src, tables)
        nfr = len(fr) + TAIL_FRAMES
        plan = allocate(voices, nfr)
        cp = None
        if any(b == 0xED for b in src.code):           # smpsClearPush: the push sound frees itself at its end
            cp = len(fr) - 1
        prio = pri[sid - 0x81] if sid - 0x81 < len(pri) else 0x70
        stream, owned = encode(sid, voices, plan, nfr, prio, cp)
        result[sid] = dict(stream=stream, owned=owned, info=info, frames=fr, voices=voices, src=src)
    return result, tables


def emit_c(result, out_dir, bank):
    ids = list(range(0xA0, 0xD1))
    blob = bytearray()
    offs = []
    for sid in ids:
        if sid in result:
            offs.append(len(blob))
            blob += result[sid]['stream']
        else:
            offs.append(0xFFFF)
    lines = [f"#pragma bank {bank}", "#include <stdint.h>",
             "/* Generated by tools/sfx_gbc.py from the s1disasm sound/sfx sources and s1.sounddriver.asm. Do not edit. */",
             "", f"const uint16_t sfx_gb_offsets[{len(ids)}] = {{"]
    for i in range(0, len(offs), 12):
        lines.append("    " + ", ".join(f"0x{o:04X}" for o in offs[i:i + 12]) + ",")
    lines.append("};")
    lines.append(f"const uint8_t sfx_gb_blob[{len(blob)}] = {{")
    for i in range(0, len(blob), 24):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 24]) + ",")
    lines.append("};")
    open(os.path.join(out_dir, "sfx_gb.c"), "w").write("\n".join(lines) + "\n")
    hdr = ["/* Generated by tools/sfx_gbc.py. */", "#ifndef SFX_GB_H", "#define SFX_GB_H", "#include <stdint.h>",
           f"#define SFX_GB_BANK {bank}", "extern const uint16_t sfx_gb_offsets[];", "extern const uint8_t sfx_gb_blob[];",
           "#endif"]
    open(os.path.join(out_dir, "sfx_gb.h"), "w").write("\n".join(hdr) + "\n")
    return len(blob)


if __name__ == '__main__':
    root = os.path.dirname(TOOLS)
    asm = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'vendor', 's1disasm')
    res, _ = compile_all(asm)
    for sid, r in sorted(res.items()):
        print(f"${sid:02X} {r['src'].name:32s} {len(r['stream']):4d} bytes  owned={r['owned']:#x}  " + "; ".join(r['info']))
    n = emit_c(res, os.path.join(root, 'generated'), int(os.environ.get('SFX_GB_BANK', '8')))
    print("blob bytes", n)
