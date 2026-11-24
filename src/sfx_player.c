#pragma bank 8
/* 3-voice real-time layered PCM SFX slot allocator + Bank 8 expressive BGM wave/timbre/drum helpers.
 * All Sonic 1 SFX ($A0..$D0) mix together onto CGB Channel 3 (Wave RAM at 8,192 Hz) without stealing
 * CH1, CH2, or CH4 from the music driver. */
#include "engine.h"
#include "sfx_player.h"
#include "../generated/sfx_pcm.h"

/* 4 selectable 32-sample CH3 Wave RAM timbres (Bank 8):
 *   0 (0x10): Punchy FM Slap Bass (fundamental + 2nd..4th harmonics)
 *   1 (0x11): Warm Classic Bass (default for all standard SMPS songs)
 *   2 (0x12): Major Triad Chord Wave (octave + 5th + major 3rd)
 *   3 (0x13): Bright Synth / Brass Wave */
const uint8_t g_ch3_waves[4][16] = {
    { 0xFF, 0xFE, 0xDD, 0xCE, 0xFF, 0xEC, 0xBA, 0x87,
      0x64, 0x21, 0x00, 0x01, 0x22, 0x10, 0x00, 0x03 },
    { 0xAC, 0xEF, 0xFF, 0xFF, 0xFF, 0xFE, 0xDC, 0xA8,
      0x53, 0x10, 0x00, 0x00, 0x00, 0x01, 0x24, 0x68 },
    { 0x8D, 0xFE, 0xB9, 0x88, 0x75, 0x32, 0x48, 0xAA,
      0x85, 0x57, 0xBD, 0xCA, 0x87, 0x76, 0x41, 0x02 },
    { 0x8D, 0xFE, 0xDB, 0xBB, 0xAA, 0x99, 0x99, 0x88,
      0x87, 0x76, 0x66, 0x65, 0x54, 0x44, 0x21, 0x02 }
};

void bgm_install_wave_b8(void) {
    NR30_REG = 0x00;
    memcpy((void *)0xFF30, g_ch3_waves[g_ch3_wave_idx & 3U], 16);
    NR30_REG = 0x80;
}

uint8_t bgm_cmd_b8(uint8_t cmd, uint8_t can_wave_drum) {
    if (cmd == 5U) {
        /* Crisp closed hi-hat */
        NR41_REG = 0x3EU; NR42_REG = 0x41U; NR43_REG = 0x32U; NR44_REG = 0xC0U;
        return 0;
    }
    if (cmd == 6U) {
        /* Open hi-hat / crash accent */
        NR41_REG = 0x2CU; NR42_REG = 0x62U; NR43_REG = 0x33U; NR44_REG = 0xC0U;
        return 0;
    }
    if (cmd == 7U) {
        /* Soft rimshot / ghost snare */
        NR41_REG = 0x3AU; NR42_REG = 0x51U; NR43_REG = 0x43U; NR44_REG = 0xC0U;
        return 0;
    }
    if (cmd >= 1U && cmd <= 4U) {
        if (cmd == 1U) {
            NR41_REG = 0x38U; NR42_REG = 0xA1U; NR43_REG = 0x56U; NR44_REG = 0xC0U;
        } else if (cmd == 2U) {
            NR41_REG = 0x32U; NR42_REG = 0x92U; NR43_REG = 0x44U; NR44_REG = 0xC0U;
        } else if (cmd == 3U) {
            NR41_REG = 0x34U; NR42_REG = 0x82U; NR43_REG = 0x54U; NR44_REG = 0xC0U;
        } else {
            NR41_REG = 0x34U; NR42_REG = 0x82U; NR43_REG = 0x64U; NR44_REG = 0xC0U;
        }
        if (can_wave_drum) return cmd;
        g_dac_drum_packets++;
        return 0;
    }
    if (cmd >= 0x10U && cmd <= 0x13U) {
        g_ch3_wave_idx = (uint8_t)(cmd & 3U);
        if (!g_pcm_sfx_active) bgm_install_wave_b8();
        else g_ch3_restore_bass = 1;
        return 0;
    }
    if (cmd >= 0x20U && cmd <= 0x2FU) {
        g_ch1_duty = (uint8_t)((cmd & 0x0CU) << 4);
        g_ch1_env = (uint8_t)((cmd & 0x03U) << 1);
        return 0;
    }
    if (cmd >= 0x30U && cmd <= 0x3FU) {
        g_ch2_duty = (uint8_t)((cmd & 0x0CU) << 4);
        g_ch2_env = (uint8_t)((cmd & 0x03U) << 1);
        return 0;
    }
    return 0;
}

static uint8_t s_ring_toggle = 0;
static uint8_t s_voice_age[SFX_MAX_VOICES];

static uint8_t is_single_instance_sfx(uint8_t sfx_id) {
    switch (sfx_id) {
        case SFX_JUMP:
        case SFX_SKID:
        case SFX_ROLL:
        case SFX_CHAIN:
        case SFX_SAW:
        case SFX_ELECTRIC:
        case SFX_FLAMETHROW:
        case SFX_SPIKESMOVE:
            return 1;
        default:
            return 0;
    }
}

void sfx_pcm_start_b8(uint8_t sfx_id) {
    const SfxPcmDesc *d;
    SfxPcmVoice *slot = 0;
    uint8_t slot_idx = 0xFFU;
    uint8_t i;
    uint8_t cnt = 0;

    if (sfx_id < SFX_PCM_FIRST || sfx_id > SFX_PCM_LAST) return;
    if (sfx_id == SFX_RING) {
        if (!s_ring_toggle) sfx_id = 0xCE;
        s_ring_toggle ^= 1U;
    }

    d = &sfx_pcm_table[sfx_id - SFX_PCM_FIRST];
    if (d->bank == 0U || d->blocks == 0U || d->data == 0) return;

    CRITICAL {
        uint8_t blocked = 0;
        if (sfx_id == SFX_PUSH) {
            for (i = 0; i < SFX_MAX_VOICES; i++) {
                if (g_sfx_voices[i].active && g_sfx_voices[i].sfx_id == SFX_PUSH) {
                    blocked = 1;
                    break;
                }
            }
        }
        if (!blocked) {
            if (is_single_instance_sfx(sfx_id)) {
                for (i = 0; i < SFX_MAX_VOICES; i++) {
                    if (g_sfx_voices[i].active && g_sfx_voices[i].sfx_id == sfx_id) {
                        slot_idx = i;
                        break;
                    }
                }
            }

            if (slot_idx == 0xFFU) {
                for (i = 0; i < SFX_MAX_VOICES; i++) {
                    if (!g_sfx_voices[i].active) {
                        slot_idx = i;
                        break;
                    }
                }
            }

            if (slot_idx == 0xFFU) {
                uint8_t vidx = 0;
                for (i = 1; i < SFX_MAX_VOICES; i++) {
                    SfxPcmVoice *v = &g_sfx_voices[i];
                    SfxPcmVoice *vic = &g_sfx_voices[vidx];
                    if (v->prio < vic->prio ||
                        (v->prio == vic->prio && (s_voice_age[i] > s_voice_age[vidx] ||
                         (s_voice_age[i] == s_voice_age[vidx] && v->blocks < vic->blocks)))) {
                        vidx = i;
                    }
                }
                if ((d->prio & 0x80U) || d->prio >= g_sfx_voices[vidx].prio) {
                    slot_idx = vidx;
                }
            }

            if (slot_idx != 0xFFU) {
                for (i = 0; i < SFX_MAX_VOICES; i++) {
                    if (g_sfx_voices[i].active && s_voice_age[i] < 255U) {
                        s_voice_age[i]++;
                    }
                }
                slot = &g_sfx_voices[slot_idx];
                slot->bank = d->bank;
                slot->ptr = d->data;
                slot->blocks = d->blocks;
                slot->prio = d->prio;
                slot->sfx_id = sfx_id;
                slot->active = 1;
                s_voice_age[slot_idx] = 0;

                for (i = 0; i < SFX_MAX_VOICES; i++) {
                    if (g_sfx_voices[i].active) cnt++;
                }
                g_pcm_sfx_active = cnt;
                g_ch3_restore_bass = 0;
                NR51_REG |= 0x44U;
                if (!(TAC_REG & 0x04U) && !g_sega_pcm_playing) {
                    /* CGB Double Speed: TAC=0x04 clocks TIMA at 8,192 Hz -> TMA=224 gives exact 256 Hz Wave reloads. */
                    TMA_REG = 224U;
                    TIMA_REG = 255U;
                    TAC_REG = 0x04U;
                }
            }
        }
    }

    if (slot) {
        g_cur_sfx = sfx_id;
        g_sound_events++;
    }
}
