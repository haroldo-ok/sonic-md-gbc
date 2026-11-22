#pragma bank 8
/* Sound effects $A1..$D0 (the jump $A0 is `jump_tick` in main.c), played from the streams tools/sfx_gbc.py builds
 * from the s1disasm SFX sources (generated/sfx_gb.c, same bank). Stream format: see tools/sfx_gbc.py.
 *
 * Driver behaviour kept from `s1.sounddriver.asm`:
 *   - CycleSoundQueue: an SFX whose SoundPriorities entry is below the current v_sndprio is ignored; entries with
 *     bit 7 set play without changing it; it is cleared when the SFX stops;
 *   - Sound_PlaySFX: the ring sound alternates between the right ($B5) and left ($CE) speakers, and the push
 *     sound ($A7) does not restart until its smpsClearPush has run.
 * These functions are not BANKED: main.c maps SFX_GB_BANK and calls them directly. */
#include "engine.h"
#include "sfx_player.h"
#include "../generated/sfx_gb.h"

uint8_t g_sfx_own = 0;
const uint8_t *g_sfx_ptr = 0;
uint8_t g_sfx_prio = 0;
static uint8_t s_wait = 0;
static uint8_t s_flags = 0;        /* bit0: ring speaker (v_ring_speaker), bit1: push playing (f_push_playing) */

/* Give channels back to the music: silence them and restore their panning. */
static void release(uint8_t keep, uint8_t jump_playing) {
    uint8_t drop = (uint8_t)(g_sfx_own & ~keep);
    if (drop & 1) { NR12_REG = 0x00; NR51_REG |= 0x11; }
    if ((drop & 2) && !jump_playing) { NR22_REG = 0x00; NR51_REG |= 0x22; }
    if (drop & 8) { NR42_REG = 0x00; NR51_REG |= 0x88; }
    g_sfx_own = (uint8_t)(g_sfx_own & keep);
}

void sfx_start_b8(uint8_t sfx_id, uint8_t jump_playing) {
    uint16_t off;
    const uint8_t *h;
    uint8_t prio;
    if (sfx_id < 0xA1 || sfx_id > 0xD0) return;
    off = sfx_gb_offsets[sfx_id - 0xA0];
    if (off == 0xFFFFU) return;
    h = &sfx_gb_blob[off];
    prio = h[1];
    if (prio < g_sfx_prio) return;
    if (sfx_id == SFX_RING) {
        if (!(s_flags & 1)) {
            sfx_id = 0xCE;
            h = &sfx_gb_blob[sfx_gb_offsets[0xCE - 0xA0]];
        }
        s_flags ^= 1;
    }
    if (sfx_id == SFX_PUSH) {
        if (s_flags & 2) return;
        s_flags |= 2;
    } else {
        s_flags &= (uint8_t)~2;
    }
    if (!(prio & 0x80)) g_sfx_prio = prio;
    release(h[0], jump_playing);
    g_sfx_own = h[0];
    g_sfx_ptr = h + 2;
    s_wait = 0;
    g_cur_sfx = sfx_id;
    g_sound_events++;
}

void sfx_tick_b8(uint8_t jump_playing) {
    const uint8_t *p = g_sfx_ptr;
    uint8_t m, c, lo, env, duty, v;
    if (p == 0) return;
    if (s_wait) {
        s_wait--;
        return;
    }
    m = *p++;
    if (m == 0) {                              /* end: smpsStop clears the SFX priority */
        release(0, jump_playing);
        g_sfx_ptr = 0;
        g_sfx_prio = 0;
        s_flags &= (uint8_t)~2;
        return;
    }
    if (m != 0x80) {
        if (m & 1) {                           /* CH1 tone */
            c = *p++;
            if (c & 0x40) {
                NR12_REG = 0x00;
            } else {
                lo = *p++;
                if (c & 0x80) {
                    env = *p++;
                    duty = *p++;
                    NR10_REG = 0x00;
                    NR11_REG = duty;
                    NR12_REG = env;
                    NR13_REG = lo;
                    NR14_REG = (uint8_t)(0x80 | (c & 7));
                } else {
                    NR13_REG = lo;
                    NR14_REG = (uint8_t)(c & 7);
                }
            }
        }
        if (m & 2) {                           /* CH2 tone (the jump keeps CH2 while it plays) */
            c = *p++;
            if (c & 0x40) {
                if (!jump_playing) NR22_REG = 0x00;
            } else {
                lo = *p++;
                if (c & 0x80) {
                    env = *p++;
                    duty = *p++;
                    if (!jump_playing) {
                        NR21_REG = duty;
                        NR22_REG = env;
                        NR23_REG = lo;
                        NR24_REG = (uint8_t)(0x80 | (c & 7));
                    }
                } else if (!jump_playing) {
                    NR23_REG = lo;
                    NR24_REG = (uint8_t)(c & 7);
                }
            }
        }
        if (m & 8) {                           /* CH4 noise / 7-bit buzz */
            c = *p++;
            if (c & 0x40) {
                NR42_REG = 0x00;
            } else {
                if (c & 1) NR43_REG = *p++;
                if (c & 0x80) {
                    NR41_REG = 0x00;
                    NR42_REG = *p++;
                    NR44_REG = 0x80;
                }
            }
        }
        if (m & 0x10) {                        /* smpsPan: replace the panning bits of the channels named */
            v = *p++;
            if (v & 0x11) NR51_REG = (uint8_t)((NR51_REG & ~0x11) | (v & 0x11));
            if (v & 0x22) NR51_REG = (uint8_t)((NR51_REG & ~0x22) | (v & 0x22));
            if (v & 0x88) NR51_REG = (uint8_t)((NR51_REG & ~0x88) | (v & 0x88));
        }
        if (m & 0x20) s_flags &= (uint8_t)~2;  /* smpsClearPush */
    }
    s_wait = (uint8_t)(*p++ - 1);
    g_sfx_ptr = p;
}
