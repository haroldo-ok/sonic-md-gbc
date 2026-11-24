#!/usr/bin/env python3
"""
End-to-end hardware verification of the 3-voice Channel 3 Wave-RAM Layered PCM SFX Mixer
on sonic.gbc using PyBoy (window='null', sound_emulated=True).
"""
from __future__ import annotations

import os
import wave

import numpy as np
from pyboy import PyBoy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM_PATH = os.path.join(ROOT, "sonic.gbc")
NOI_PATH = os.path.join(ROOT, "sonic.noi")
VER_DIR = os.path.join(ROOT, "verification")

BASS_WAVE = [0xAC, 0xEF, 0xFF, 0xFF, 0xFF, 0xFE, 0xDC, 0xA8,
             0x53, 0x10, 0x00, 0x00, 0x00, 0x01, 0x24, 0x68]
SLAP_WAVE = [0xFF, 0xFE, 0xDD, 0xCE, 0xFF, 0xEC, 0xBA, 0x87,
             0x64, 0x21, 0x00, 0x01, 0x22, 0x10, 0x00, 0x03]
VALID_CH3_WAVES = (
    BASS_WAVE,
    SLAP_WAVE,
    [0x8D, 0xFE, 0xB9, 0x88, 0x75, 0x32, 0x48, 0xAA, 0x85, 0x57, 0xBD, 0xCA, 0x87, 0x76, 0x41, 0x02],
    [0x8D, 0xFE, 0xDB, 0xBB, 0xAA, 0x99, 0x99, 0x88, 0x87, 0x76, 0x66, 0x65, 0x54, 0x44, 0x21, 0x02],
)


def load_noi(path: str) -> dict[str, int]:
    syms: dict[str, int] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3 and parts[0] == "DEF":
                syms[parts[1]] = int(parts[2], 16)
    return syms


def read_u16(pb: PyBoy, addr: int) -> int:
    return pb.memory[addr] | (pb.memory[addr + 1] << 8)


def read_i16(pb: PyBoy, addr: int) -> int:
    v = read_u16(pb, addr)
    return v - 0x10000 if v & 0x8000 else v


def read_bytes(pb: PyBoy, addr: int, n: int = 16) -> list[int]:
    return [pb.memory[addr + i] for i in range(n)]


def peek_wave_ram(pb: PyBoy) -> list[int]:
    """Read actual CGB Wave RAM ($FF30..$FF3F) safely by momentarily clearing NR30 DAC bit."""
    old_nr30 = pb.memory[0xFF1A]
    pb.memory[0xFF1A] = 0x00
    w = [pb.memory[0xFF30 + i] for i in range(16)]
    pb.memory[0xFF1A] = old_nr30
    return w


def tap(pb: PyBoy, btn: str, hold: int = 2, rel: int = 2, rec: list | None = None) -> None:
    pb.button_press(btn)
    for _ in range(hold):
        pb.tick()
        if rec is not None:
            rec.append(pb.sound.ndarray.copy())
    pb.button_release(btn)
    for _ in range(rel):
        pb.tick()
        if rec is not None:
            rec.append(pb.sound.ndarray.copy())


def screech_ratio(pcm_stereo: np.ndarray, sr: int = 48000) -> float:
    mono = pcm_stereo.mean(axis=1).astype(np.float64) if pcm_stereo.ndim == 2 else pcm_stereo.astype(np.float64)
    if len(mono) < 256:
        return 0.0
    spec = np.abs(np.fft.rfft(mono * np.hanning(len(mono)))) ** 2
    freqs = np.fft.rfftfreq(len(mono), 1.0 / sr)
    tot = float(spec.sum())
    if tot <= 1e-12:
        return 0.0
    return float(spec[freqs > 4500.0].sum() / tot)


def save_wav(path: str, pcm_stereo: np.ndarray, sr: int = 48000) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    arr = np.asarray(pcm_stereo, dtype=np.float64)
    if arr.ndim == 1:
        arr = np.stack([arr, arr], axis=1)
    peak = max(1.0, float(np.max(np.abs(arr))))
    scaled = np.clip((arr / peak) * 28000.0, -32767, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(arr.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(scaled.tobytes())


def main() -> int:
    syms = load_noi(NOI_PATH)
    sym_mode = syms["_g_game_mode"]
    sym_frame = syms["_g_frame_counter"]
    sym_snd_ev = syms["_g_sound_events"]
    sym_sfx_act = syms["_g_pcm_sfx_active"]
    sym_restore = syms["_g_ch3_restore_bass"]
    sym_cur_sfx = syms["_g_cur_sfx"]
    sym_voices = syms["_g_sfx_voices"]
    sym_mix_buf = syms["_g_sfx_mix_buf"]
    sym_dac_pkts = syms["_g_dac_drum_packets"]
    sym_sonic = syms["_g_sonic"]
    sym_rings = syms["_g_rings"]
    sym_cheats = syms["_g_cheats"]

    demo_audio: list[np.ndarray] = []

    with PyBoy(ROM_PATH, window="null", sound_emulated=True) as pb:
        # 1. Boot & SEGA Chant
        boot_ticks = 0
        while read_u16(pb, sym_frame) < 1 and boot_ticks < 240:
            pb.tick()
            boot_ticks += 1
        assert read_u16(pb, sym_frame) >= 1, "Cartridge did not reach main loop!"
        assert (pb.memory[0xFF4D] & 0x80) == 0x80, "CGB Double Speed not enabled!"

        sega_bufs = []
        for _ in range(55):
            pb.tick()
            sega_bufs.append(pb.sound.ndarray.copy())
        sega_ev = read_u16(pb, sym_snd_ev)
        sega_pcm = np.concatenate(sega_bufs, axis=0)
        sega_std = float(np.std(sega_pcm))
        print(f"[1] SEGA Boot: sound_events={sega_ev}, audio_std={sega_std:.2f}")
        assert sega_ev >= 10, f"SEGA PCM did not play (events={sega_ev})"
        assert sega_std > 0.3, f"SEGA audio is silent (std={sega_std})"
        demo_audio.extend(sega_bufs)

        # 2. Title -> Level Select Cheat -> Sound Test
        tap(pb, "start", hold=4, rel=20)
        assert pb.memory[sym_mode] == 1, f"Expected Title (1), got {pb.memory[sym_mode]}"
        for btn in ("up", "down", "left", "right"):
            tap(pb, btn, hold=2, rel=2)
        tap(pb, "start", hold=4, rel=10)
        assert pb.memory[sym_mode] == 2, f"Expected Level Select (2), got {pb.memory[sym_mode]}"

        # Navigate up 3x from row 0 -> row 20 (SOUND TEST, default 0x81 = GHZ BGM)
        for _ in range(3):
            tap(pb, "up", hold=3, rel=6)
        # Wait 15 frames so UP's SFX_SWITCH finishes and verify mixer stopped cleanly
        for _ in range(15):
            pb.tick()
        assert pb.memory[sym_sfx_act] == 0 and pb.memory[sym_restore] == 0 and (pb.memory[0xFF07] & 0x07) == 0

        tap(pb, "a", hold=3, rel=6)  # Start GHZ BGM (0x81)
        assert pb.memory[sym_mode] == 2, f"Expected to remain on Level Select Sound Test (2), got {pb.memory[sym_mode]}"
        bgm_bufs = []
        saw_bass_wave = False
        for f_i in range(60):
            pb.tick()
            bgm_bufs.append(pb.sound.ndarray.copy())
            if f_i % 10 == 9 and peek_wave_ram(pb) in VALID_CH3_WAVES:
                saw_bass_wave = True
        bgm_pcm = np.concatenate(bgm_bufs, axis=0)
        bgm_sr = getattr(pb.sound, "sample_rate", 48000) or 48000
        bgm_screech = screech_ratio(bgm_pcm, bgm_sr)
        bgm_std = float(np.std(bgm_pcm))
        print(f"[2] GHZ BGM idle: audio_std={bgm_std:.2f}, screech(>4.5kHz)={bgm_screech:.3f}, saw_bass_wave={saw_bass_wave}")
        assert bgm_std > 0.3, "GHZ BGM is silent!"
        assert bgm_screech < 0.20, f"GHZ BGM screech too high: {bgm_screech:.3f}"
        assert saw_bass_wave, "GHZ BGM never installed s_bass_wave on CH3!"
        demo_audio.extend(bgm_bufs)

        # 3. Deterministic 3-Voice Layered SFX Overlap in Sound Test
        # Step s_ls_sound from 0x81 backwards 5x -> 0xCC (Spring, 0.855s)
        for _ in range(5):
            tap(pb, "left", hold=3, rel=6)
        layer_bufs = []
        mix_snapshots = []
        max_voices = 0
        saw_voice_counts = set()

        def sample_step():
            nonlocal max_voices
            act = pb.memory[sym_sfx_act]
            real_cnt = sum(1 for k in range(3) if pb.memory[sym_voices + k * 8] != 0)
            max_voices = max(max_voices, real_cnt)
            saw_voice_counts.add(real_cnt)
            if act:
                mix_snapshots.append((real_cnt, read_bytes(pb, sym_mix_buf, 16), pb.memory[0xFF07]))

        # Trigger SFX #1: Spring (0xCC)
        pb.button_press("a")
        for _ in range(3):
            pb.tick(); layer_bufs.append(pb.sound.ndarray.copy()); sample_step()
        pb.button_release("a")
        for _ in range(5):
            pb.tick(); layer_bufs.append(pb.sound.ndarray.copy()); sample_step()

        # Step 1x left -> 0xCB (Wall Smash) & trigger SFX #2
        tap(pb, "left", hold=3, rel=6, rec=layer_bufs)
        sample_step()
        pb.button_press("a")
        for _ in range(3):
            pb.tick(); layer_bufs.append(pb.sound.ndarray.copy()); sample_step()
        pb.button_release("a")
        for _ in range(5):
            pb.tick(); layer_bufs.append(pb.sound.ndarray.copy()); sample_step()

        # Trigger SFX #3 via SELECT -> Giant Ring (0xC3)
        pb.button_press("select")
        for _ in range(3):
            pb.tick(); layer_bufs.append(pb.sound.ndarray.copy()); sample_step()
        pb.button_release("select")

        # Observe 3-voice -> 2-voice -> 1-voice -> 0-voice decay over 170 frames (~2.83s)
        for _ in range(170):
            pb.tick()
            layer_bufs.append(pb.sound.ndarray.copy())
            sample_step()

        layer_pcm = np.concatenate(layer_bufs, axis=0)
        layer_std = float(np.std(layer_pcm))
        layer_screech = screech_ratio(layer_pcm, bgm_sr)
        unique_waves = len({tuple(w) for _, w, _ in mix_snapshots})
        nibbles = []
        for cnt, w, _ in mix_snapshots:
            if cnt >= 2:
                for b in w:
                    nibbles.append((b >> 4) & 0xF)
                    nibbles.append(b & 0xF)
        wave_post = peek_wave_ram(pb)
        tac_post = pb.memory[0xFF07]
        act_post = pb.memory[sym_sfx_act]
        rest_post = pb.memory[sym_restore]
        last_mix_wave = mix_snapshots[-1][1]

        print(
            f"[3] 3-Voice Layered SFX: max_voices={max_voices}, counts={sorted(saw_voice_counts)}, "
            f"unique_mix_blocks={unique_waves}, layered_nibbles=({min(nibbles)}..{max(nibbles)}), "
            f"audio_std={layer_std:.2f}, screech={layer_screech:.3f}, "
            f"post(act={act_post}, tac=0x{tac_post:02X}, restore_flag={rest_post}, wave_head={[hex(b) for b in wave_post[:4]]})"
        )
        assert max_voices == 3, f"Expected 3 simultaneous voices, got {max_voices} ({saw_voice_counts})"
        assert unique_waves >= 20, f"Mix buffer did not stream dynamically: {unique_waves} blocks"
        assert min(nibbles) <= 4 and max(nibbles) >= 10, f"Unexpected mixed nibble range: {min(nibbles)}..{max(nibbles)}"
        assert max(nibbles) <= 14, f"Mixed nibble overflowed > 14: {max(nibbles)}"
        assert act_post == 0 and (tac_post & 0x07) == 0 and rest_post == 0, (
            f"Mixer did not stop/restore cleanly: act={act_post}, tac={tac_post}, restore={rest_post}"
        )
        # Confirm CH3 bass wave returns cleanly within 60 frames after SFX finish
        saw_post_bass = (wave_post in VALID_CH3_WAVES)
        for f_i in range(60):
            pb.tick()
            layer_bufs.append(pb.sound.ndarray.copy())
            if f_i % 6 == 0 and peek_wave_ram(pb) in VALID_CH3_WAVES:
                saw_post_bass = True
        assert saw_post_bass, "CH3 BGM wave did not return to Wave RAM after layered SFX finished!"
        assert layer_screech < 0.20, f"Layered SFX audio screech too high: {layer_screech:.3f}"
        demo_audio.extend(layer_bufs)

        # 4. Launch GHZ1 Gameplay & Verify Natural Layered SFX (Jump + Rings + Break)
        for _ in range(3):
            tap(pb, "down", hold=3, rel=6)  # row 20 -> 21 -> 22 -> 0 (GHZ1)
        tap(pb, "start", hold=4, rel=6)
        for _ in range(70):
            pb.tick()
        assert pb.memory[sym_mode] == 3, f"Expected GM_LEVEL (3), got {pb.memory[sym_mode]}"

        pb.memory[sym_cheats] = 0x01  # Invincibility so Moto Bug doesn't halt scripted run
        start_x = read_i16(pb, sym_sonic + 8)
        ghz_bufs = []
        ghz_max_voices = 0
        ghz_sfx_seen = set()

        pb.button_press("right")
        for step in range(340):
            if step in (45, 138, 215, 290):
                pb.button_press("a")
            elif step in (75, 174, 250, 322):
                pb.button_release("a")
            pb.tick()
            ghz_bufs.append(pb.sound.ndarray.copy())
            real_cnt = sum(1 for k in range(3) if pb.memory[sym_voices + k * 8] != 0)
            ghz_max_voices = max(ghz_max_voices, real_cnt)
            if pb.memory[sym_cur_sfx]:
                ghz_sfx_seen.add(hex(pb.memory[sym_cur_sfx]))
        pb.button_release("right")
        pb.button_release("a")

        end_x = read_i16(pb, sym_sonic + 8)
        rings = read_u16(pb, sym_rings)
        dac_pkts = read_u16(pb, sym_dac_pkts)
        ghz_pcm = np.concatenate(ghz_bufs, axis=0)
        ghz_std = float(np.std(ghz_pcm))
        ghz_screech = screech_ratio(ghz_pcm, bgm_sr)
        demo_audio.extend(ghz_bufs)

        print(
            f"[4] GHZ1 Gameplay: X={start_x}->{end_x}, rings={rings}, dac_pkts={dac_pkts}, "
            f"max_voices={ghz_max_voices}, sfx_seen={sorted(ghz_sfx_seen)}, "
            f"audio_std={ghz_std:.2f}, screech={ghz_screech:.3f}"
        )
        assert end_x > start_x + 250, f"Sonic did not advance in GHZ1: {start_x}->{end_x}"
        assert rings > 0, "Sonic collected 0 rings in GHZ1!"
        assert ghz_max_voices >= 2, f"Expected >=2 layered SFX during GHZ1 run, got {ghz_max_voices}"
        assert ghz_std > 0.3 and ghz_screech < 0.20

    full_audio = np.concatenate(demo_audio, axis=0)
    wav_path = os.path.join(VER_DIR, "layered_sfx_demo.wav")
    save_wav(wav_path, full_audio, 48000)
    print(f"Saved live PyBoy audio demo -> {wav_path} ({len(full_audio)/48000:.2f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
