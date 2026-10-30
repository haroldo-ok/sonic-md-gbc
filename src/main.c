#include "engine.h"
#include "ghz_pals.h"
#include <stddef.h>

#ifndef SONIC_GAMEPLAY_30FPS
#define SONIC_GAMEPLAY_30FPS 0
#endif

/* Interrupt-safe `memcpy` replacement: GBDK's default `___memcpy` loads
 * hardware `SP = src` without disabling `IME`, which corrupts `SP` and RAM if
 * our `60 Hz` `hw_sound_isr` or `5,461 Hz` `hw_sega_pcm_tim_isr` fires mid-copy! */
void *safe_copy_bytes(void *dst, const void *src, uint16_t n) __naked {
    /* Forward byte copy, 8x unrolled, no SP tricks (interrupt-safe). sdcccall(1): dst DE, src BC, n at [SP+2];
       returns dst in BC; callee pops 2 bytes. */
    (void)dst; (void)src; (void)n;
    __asm
    ldhl sp, #2
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    push de
    push hl
    ld   h, b
    ld   l, c
    pop  bc
    ld   a, c
    and  a, #7
    push af
    srl  b
    rr   c
    srl  b
    rr   c
    srl  b
    rr   c
    ld   a, b
    or   a, c
    jr   z, 603$
    ld   a, c
    or   a, a
    jr   z, 601$
    inc  b
601$:
602$:
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    dec  c
    jr   nz, 602$
    dec  b
    jr   nz, 602$
603$:
    pop  af
    or   a, a
    jr   z, 605$
    ld   c, a
604$:
    ld   a, (hl+)
    ld   (de), a
    inc  de
    dec  c
    jr   nz, 604$
605$:
    pop  bc
    pop  hl
    pop  af
    jp   (hl)
    __endasm;
}

void *safe_set_bytes(void *dst, uint8_t val, uint16_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n >= 8U) {
        d[0] = val; d[1] = val; d[2] = val; d[3] = val;
        d[4] = val; d[5] = val; d[6] = val; d[7] = val;
        d += 8;
        n -= 8U;
    }
    while (n != 0U) {
        *d++ = val;
        n--;
    }
    return dst;
}

/* Global Engine State in WRAM Bank 0 */
volatile uint8_t g_game_mode = GM_SEGA;
volatile uint8_t g_next_mode = GM_SEGA;
volatile uint8_t g_mode_init_done = 0;
volatile uint16_t g_frame_counter = 0;
volatile uint8_t g_invinc_vblanks_pending = 0;
volatile uint8_t g_cur_act = 0;
volatile uint8_t g_cur_ss = 0;
volatile uint8_t g_demo_mode = 0;
uint16_t g_demo_timer = 0;
uint16_t g_demo_pos = 0;
uint8_t g_demo_btn = 0;
uint8_t g_demo_btn_timer = 0;

uint8_t g_joy = 0;
uint8_t g_joy_prev = 0;
uint8_t g_joy_pressed = 0;
uint8_t g_paused = 0;

volatile uint8_t g_debug_unlocked = 0;
volatile uint8_t g_debug_active = 0;
uint8_t g_debug_item = 0;
uint8_t g_button_state = 0;
volatile uint16_t g_pal_cycles = 0;
volatile uint16_t g_anim_tile_uploads = 0;
volatile uint16_t g_dac_drum_packets = 0;
volatile uint8_t g_rev_mode = 0;
volatile uint8_t g_jp_credits = 0;

int16_t g_dyn_left_bound = 0;
int16_t g_dyn_right_bound = 0x24BF;
int16_t g_dyn_top_bound = 0;
int16_t g_dyn_bottom_bound = 0x0300;
uint16_t g_osc_val[16];
uint16_t g_osc_dir = 0;

SonicState g_sonic;
ActiveObj g_objs[MAX_ACTIVE_OBJS];
LostRing g_lost_rings[MAX_LOST_RINGS];
ProjObj g_projs[MAX_PROJS];
ScorePop g_pops[MAX_SCORE_POPS];
uint8_t g_obj_taken[64];

int16_t g_cam_x = 0;
int16_t g_cam_y = 0;
uint32_t g_score = 0;
uint32_t g_high_score = 50000UL;
uint8_t g_score_life_awarded = 0;
uint16_t g_rings = 0;
uint8_t g_lives = 3;
uint8_t g_continues = 0;
uint8_t g_emeralds = 0;
uint8_t g_unlocked_act = 0;
uint8_t g_ss_got_emerald = 0;
uint16_t g_level_frames = 0;
uint8_t g_level_sec_div = 0;
uint8_t g_hud_dirty = 1;
uint8_t g_next_ring_life = 1;

uint8_t g_checkpoint_active = 0;
uint8_t g_checkpoint_act = 0;
int16_t g_checkpoint_x = 0;
int16_t g_checkpoint_y = 0;

uint8_t g_signpost_passed = 0;
uint8_t g_act_clear_done = 0;
uint16_t g_time_bonus = 0;
uint16_t g_ring_bonus = 0;
volatile uint8_t g_boss_active = 0;
volatile uint8_t g_boss_defeated = 0;
uint16_t g_syz_blocks_alive = 0x03FF;
int16_t g_water_y = 0x7FFF;

VisualSetDesc g_ram_vset;
ActDesc g_ram_act;
uint8_t g_oam_next = 0;
static uint8_t s_loaded_sonic_frame = 0xFF;

volatile uint8_t g_cur_bgm = 0;
volatile uint8_t g_cur_sfx = 0;
volatile uint16_t g_sound_events = 0;
volatile uint16_t g_sound_isr_ticks = 0;

static volatile uint8_t s_audio_lock = 0;
static uint8_t s_paused_muted = 0;
static uint8_t s_bgm_bank = 0;
static const uint8_t *s_bgm_base = 0;
static const uint8_t *s_bgm_ptr = 0;
static uint8_t s_bgm_wait = 0;
static uint8_t s_sfx_active = 0;
static const uint8_t *s_sfx_ptr = 0;
static uint8_t s_sfx_wait = 0;
static uint8_t s_dac_drum_kind = 0;
static uint8_t s_dac_drum_pkt = 0;
static uint8_t s_bass_dirty = 0;
static uint16_t s_ch3_bass_per = 0;

static void play_wave_drum_packet(uint8_t kind, uint8_t pkt) {
    uint8_t prev = CURRENT_BANK;
    const uint8_t *wsrc;
    uint16_t period = 1120; /* Warm 70.6 Hz sub-bass thump — never high-frequency 1.17 kHz buzz! */
    uint8_t i;

    SWITCH_ROM(SUPPLEMENT_ANIM_BANK);
    if (kind == 1) {
        wsrc = &dac_kick_wave[(pkt & 15) << 4];
        period = 1120;
    } else if (kind == 2) {
        wsrc = &dac_snare_wave[(pkt & 15) << 4];
        period = 1480;
    } else {
        wsrc = &dac_timpani_wave[(pkt & 15) << 4];
        period = (kind == 3) ? 1548u : 1380u; /* Concert C3 (131 Hz) / G2 (98 Hz) timpani */
    }
    NR30_REG = 0x00;
    for (i = 0; i < 16; i++) {
        ((volatile uint8_t *)0xFF30)[i] = wsrc[i];
    }
    NR30_REG = 0x80;
    NR32_REG = 0x20;
    NR33_REG = (uint8_t)(period & 0xFF);
    NR34_REG = 0x80 | ((period >> 8) & 0x07);
    s_bass_dirty = 1;
    g_dac_drum_packets++;
    SWITCH_ROM(prev);
}

void hw_install_bass_wave(void);

static void start_bgm_unlocked(uint8_t bgm_id) {
    uint8_t prev = CURRENT_BANK;
    SongDesc sd;
    if (bgm_id < 0x81 || bgm_id > 0x93) return;
    g_cur_bgm = bgm_id;
    s_ch3_bass_per = 0;
    s_dac_drum_kind = 0;
    hw_install_bass_wave();
    SWITCH_ROM(8);
    sd = g_songs[bgm_id - 0x81];
    s_bgm_bank = sd.bank;
    s_bgm_base = sd.stream;
    s_bgm_ptr = sd.stream;
    s_bgm_wait = 0;
    g_sound_events++;
    SWITCH_ROM(prev);
}

void hw_play_bgm(uint8_t bgm_id) {
    s_audio_lock = 1;
    start_bgm_unlocked(bgm_id);
    s_audio_lock = 0;
}

void hw_play_sfx(uint8_t sfx_id) {
    uint8_t prev;
    uint16_t off;
    if (sfx_id < 0xA0 || sfx_id > 0xD0) return;
    s_audio_lock = 1;
    prev = CURRENT_BANK;
    g_cur_sfx = sfx_id;
    SWITCH_ROM(4);
    off = sfx_table_offsets[sfx_id - 0xA0];
    s_sfx_ptr = &all_sfx_blob[off];
    s_sfx_active = 1;
    s_sfx_wait = 0;
    g_sound_events++;

    /* Warm, authentic hardware sweep transients for signature Sonic SFX (`Jump`, `Ring`, `Spring`) */
    if (sfx_id == SFX_JUMP) {
        NR10_REG = 0x2B;
        NR11_REG = 0x80;
        NR12_REG = 0xB2;
        NR13_REG = 0x88;
        NR14_REG = 0x85;
    } else if (sfx_id == SFX_RING) {
        NR21_REG = 0x80;
        NR22_REG = 0xB2;
        NR23_REG = 0x4E;
        NR24_REG = 0x87;
    } else if (sfx_id == SFX_SPRING) {
        NR10_REG = 0x1D;
        NR11_REG = 0x80;
        NR12_REG = 0xB2;
        NR13_REG = 0x18;
        NR14_REG = 0x86;
    }
    SWITCH_ROM(prev);
    s_audio_lock = 0;
}

void hw_sound_tick(void) {
    uint8_t prev = CURRENT_BANK;

    /* 1. Step 4-channel Concert-Pitch SMPS BGM Stream */
    if (s_bgm_ptr != 0 && s_bgm_bank != 0) {
        if (s_bgm_wait > 0) {
            s_bgm_wait--;
        } else {
            uint8_t op;
            SWITCH_ROM(s_bgm_bank);
            op = *s_bgm_ptr++;
            if (op == 0x00) {
                s_bgm_ptr = s_bgm_base;
                op = *s_bgm_ptr++;
            } else if (op == 0xFF) {
                s_bgm_ptr = 0;
                if (g_game_mode == GM_LEVEL) {
                    SWITCH_ROM(prev);
                    start_bgm_unlocked(g_ram_act.music_id);
                    return;
                }
                op = 0;
            }
            if (op >= 1 && op <= 0x7F) {
                s_bgm_wait = op - 1;
            } else if (op & 0x80) {
                if (op & 1) { /* CH1 Warm 50% Lead Melody */
                    uint8_t lo = *s_bgm_ptr++;
                    uint8_t hi = *s_bgm_ptr++;
                    uint16_t per = lo | ((uint16_t)(hi & 7) << 8);
                    if (per == 0) {
                        NR12_REG = 0x00;
                    } else if (hi & 0x80) {
                        uint8_t vol = (hi >> 3) & 0x0F;
                        NR10_REG = 0x00;
                        NR11_REG = 0x80;
                        NR12_REG = (uint8_t)((vol << 4) | 0x04);
                        NR13_REG = lo;
                        NR14_REG = 0x80 | (hi & 0x07);
                    } else {
                        NR13_REG = lo;
                        NR14_REG = (hi & 0x07);
                    }
                }
                if (op & 2) { /* CH2 Balanced 25% Harmony / Counter-Melody */
                    uint8_t lo = *s_bgm_ptr++;
                    uint8_t hi = *s_bgm_ptr++;
                    if (!s_sfx_active) {
                        uint16_t per = lo | ((uint16_t)(hi & 7) << 8);
                        if (per == 0) {
                            NR22_REG = 0x00;
                        } else if (hi & 0x80) {
                            uint8_t vol = (hi >> 3) & 0x0F;
                            NR21_REG = 0x40;
                            NR22_REG = (uint8_t)((vol << 4) | 0x04);
                            NR23_REG = lo;
                            NR24_REG = 0x80 | (hi & 0x07);
                        } else {
                            NR23_REG = lo;
                            NR24_REG = (hi & 0x07);
                        }
                    }
                }
                if (op & 4) { /* CH3 Warm Rounded Wave-RAM Bass */
                    uint8_t lo = *s_bgm_ptr++;
                    uint8_t hi = *s_bgm_ptr++;
                    uint16_t per = lo | ((uint16_t)(hi & 7) << 8);
                    s_ch3_bass_per = per;
                    if (s_bass_dirty) {
                        s_bass_dirty = 0;
                        s_dac_drum_kind = 0;
                        hw_install_bass_wave();
                        SWITCH_ROM(s_bgm_bank);
                    }
                    if (per == 0) {
                        NR32_REG = 0x00;
                    } else {
                        NR30_REG = 0x80;
                        NR32_REG = 0x20;
                        NR33_REG = lo;
                        NR34_REG = ((hi & 0x80) ? 0x80 : 0x00) | (hi & 0x07);
                    }
                }
                if (op & 8) { /* CH4 Warm Non-Screechy Analog Percussion + Optional Sub-Bass Transient */
                    uint8_t drum = *s_bgm_ptr++;
                    if (drum == 5) {
                        /* Soft brushed closed hi-hat (low LFSR clock + short 15 ms length gate) */
                        NR41_REG = 0x3C; NR42_REG = 0x21; NR43_REG = 0x43; NR44_REG = 0xC0;
                    } else {
                        if (s_ch3_bass_per == 0) {
                            s_dac_drum_kind = drum ? drum : 1;
                            s_dac_drum_pkt = 0;
                            play_wave_drum_packet(s_dac_drum_kind, 0);
                            SWITCH_ROM(s_bgm_bank);
                        } else {
                            g_dac_drum_packets++;
                        }
                        if (drum == 1) {        /* Warm Punchy Kick (`1.1 kHz` LFSR thump) */
                            NR41_REG = 0x36; NR42_REG = 0x61; NR43_REG = 0x57; NR44_REG = 0xC0;
                        } else if (drum == 2) { /* Warm Crisp Snare (`3.2 kHz` body, zero white hiss) */
                            NR41_REG = 0x34; NR42_REG = 0x51; NR43_REG = 0x44; NR44_REG = 0xC0;
                        } else if (drum == 3) { /* Warm High Timpani */
                            NR41_REG = 0x34; NR42_REG = 0x51; NR43_REG = 0x55; NR44_REG = 0xC0;
                        } else {                /* Warm Low Timpani */
                            NR41_REG = 0x34; NR42_REG = 0x51; NR43_REG = 0x65; NR44_REG = 0xC0;
                        }
                    }
                }
                g_sound_events++;
            }
        }
    }

    /* Brief 2-frame low sub-bass drum transient when CH3 bass is idle */
    if (s_dac_drum_kind != 0) {
        s_dac_drum_pkt++;
        if (s_dac_drum_pkt < 2 && s_ch3_bass_per == 0) {
            play_wave_drum_packet(s_dac_drum_kind, s_dac_drum_pkt);
        } else {
            s_dac_drum_kind = 0;
            if (s_bass_dirty && s_ch3_bass_per == 0) {
                NR32_REG = 0x00;
            }
        }
    }

    /* 2. Step SMPS Sound Effect Overlay on CH2 / CH4 */
    if (s_sfx_active && s_sfx_ptr != 0) {
        if (s_sfx_wait > 0) {
            s_sfx_wait--;
        } else {
            uint8_t dur;
            SWITCH_ROM(SFX_DATA_BANK);
            dur = *s_sfx_ptr++;
            if (dur == 0) {
                s_sfx_active = 0;
            } else {
                uint8_t mode = *s_sfx_ptr++;
                uint8_t lo = *s_sfx_ptr++;
                uint8_t hi = *s_sfx_ptr++;
                s_sfx_wait = dur - 1;
                if ((mode & 1) && (lo | hi)) {
                    NR21_REG = 0x80;
                    NR22_REG = 0xA2;
                    NR23_REG = lo;
                    NR24_REG = 0x80 | (hi & 0x07);
                }
                if (mode & 2) {
                    NR41_REG = 0x20;
                    NR42_REG = 0x92;
                    NR43_REG = 0x33;
                    NR44_REG = 0xC0;
                }
            }
        }
    }
    SWITCH_ROM(prev);
}

/* 8 Crisp Simultaneous CGB Hardware Sprite Palettes (OBP0..OBP7) */
const uint16_t g_default_obj_pals[32] = {
    /* OBP0: Sonic Ink-Black Eye Pupils (`1`) + Royal Cobalt Blue (`2`) + Bright Cream-White Sclera/Gloves (`3`) */
    0x0000, RGB8(8, 8, 24),     RGB8(36, 76, 248),  RGB8(252, 240, 212),
    /* OBP1: Crimson + Pure White + Black Outline (Sonic Shoes, Springs, Lamp, Eggman, Fireballs) */
    0x0000, RGB8(16, 16, 24),   RGB8(244, 28, 24),  RGB8(252, 252, 252),
    /* OBP2: Gleaming Gold + White + Dark Bronze (Rings, Monitors, Giant Ring, Capsule, Sparkles) */
    0x0000, RGB8(108, 52, 0),   RGB8(252, 204, 0),  RGB8(252, 252, 252),
    /* OBP3: Red/Gold Badniks (Moto Bug, Crabmeat, Chopper, Caterkiller, Yadrin, Splats) */
    0x0000, RGB8(16, 16, 28),   RGB8(244, 36, 24),  RGB8(252, 248, 232),
    /* OBP4: Cyan/Cobalt Blue Badniks & Aura (Buzz Bomber, Burrobot, Roller, Shield, Bubbles, Animals) */
    0x0000, RGB8(16, 24, 56),   RGB8(36, 148, 248), RGB8(248, 252, 252),
    /* OBP5: Chrome Steel Silver + Pure White + Charcoal (Spikes, Bomb, Orbinaut, Gargoyle, Saws, Switch) */
    0x0000, RGB8(24, 28, 36),   RGB8(148, 168, 192),RGB8(252, 252, 252),
    /* OBP6: Chestnut Wood & Golden Tan (Bridges, Checkered Wrecking Ball, Rocks, Seesaws, Bumpers) */
    0x0000, RGB8(56, 28, 8),    RGB8(208, 128, 36), RGB8(252, 244, 192),
    /* OBP7: Vivid Emerald Chameleon Green (Newtron Chameleon, Green Glass & Push Blocks, Emeralds) */
    0x0000, RGB8(12, 48, 16),   RGB8(44, 212, 64),  RGB8(244, 252, 224)
};

/* Warm Rounded Fundamental Synth-Bass 32-Sample Waveform for CGB Wave RAM ($FF30..$FF3F) */
static const uint8_t s_bass_wave[16] = {
    0x8B, 0xDF, 0xFE, 0xDC, 0xB9, 0x87, 0x54, 0x32,
    0x10, 0x01, 0x12, 0x34, 0x56, 0x78, 0x89, 0x8A
};

void hw_install_bass_wave(void) {
    uint8_t i;
    NR30_REG = 0x00;
    for (i = 0; i < 16; i++) {
        ((volatile uint8_t *)0xFF30)[i] = s_bass_wave[i];
    }
    NR30_REG = 0x80;
}

volatile uint8_t g_sega_pcm_playing = 0;
static volatile uint16_t s_sega_idx = 0;
static uint8_t s_sega_prev_b = 0xFF;

static const uint8_t s_sega_nr32_lut[4] = { 0x00U, 0x60U, 0x40U, 0x20U };
static uint8_t s_sega_shift = 0;
static uint8_t s_sega_sub = 0;

void hw_stop_sega_chant(void) {
    if (!g_sega_pcm_playing) return;
    g_sega_pcm_playing = 0;
    s_sega_sub = 0;
    s_sega_prev_b = 0xFF;
    TAC_REG = 0x00;
    NR32_REG = 0x00;
    hw_install_bass_wave();
    NR30_REG = 0x00;
    NR51_REG = 0xFF;
}

void hw_start_sega_chant(void) {
    volatile uint8_t *w = (volatile uint8_t *)0xFF30U;
    uint8_t i;
    s_sega_idx = 0;
    s_sega_sub = 0;
    s_sega_shift = 0;
    s_sega_prev_b = 0xFF;
    g_sega_pcm_playing = 1;
    s_bgm_ptr = 0;
    s_sfx_active = 0;
    NR12_REG = 0x00;
    NR22_REG = 0x00;
    NR42_REG = 0x00;
    NR50_REG = 0x77;
    NR51_REG = 0x44;
    NR30_REG = 0x00;
    for (i = 0; i < 16U; i++) w[i] = 0xFFU;
    NR32_REG = 0x00;
    NR30_REG = 0x80;
    NR33_REG = 0x00U;
    NR34_REG = 0x87U;
    /* Hardware `8,738 Hz` 1st-order Delta-Sigma DAC over CH3 `[0, 3, 7, 15]` (`TMA = 241`, `TAC = 0x06`) —
     * below the `9,257 Hz` ISR ceiling so `main()` never starves and runs at full `60 FPS` from frame 1! */
    TMA_REG = 241U;
    TIMA_REG = 254U;
    TAC_REG = 0x06U;
}

void hw_sega_pcm_tim_isr(void) {
    uint8_t prev;
    if (!g_sega_pcm_playing || g_game_mode != GM_SEGA) {
        hw_stop_sega_chant();
        return;
    }
    if (s_sega_sub == 0U) {
        uint16_t idx = s_sega_idx;
        if (idx >= 3610U) {
            hw_stop_sega_chant();
            return;
        }
        s_sega_idx = idx + 1U;
        prev = CURRENT_BANK;
        SWITCH_ROM(PCM_SEGA_BANK);
        s_sega_shift = sega_wave_pcm[idx];
        SWITCH_ROM(prev);
        s_sega_sub = 4U;
        g_sound_events++;
    }
    NR32_REG = s_sega_nr32_lut[s_sega_shift & 3U];
    s_sega_shift >>= 2;
    s_sega_sub--;
}

static volatile uint8_t s_vbl_joy_latch = 0;

void hw_sound_isr(void) {
    g_sound_isr_ticks++;
    /* Sonic 1's invincibility timer is 20*60 VBlanks, not rendered game-loop iterations. */
    if (g_game_mode == GM_LEVEL && !g_paused &&
        g_invinc_vblanks_pending != 0xFF) {
        g_invinc_vblanks_pending++;
    }
    if (g_game_mode == GM_LEVEL && !g_paused) {
        s_vbl_joy_latch |= (joypad() & J_SELECT);
    }
    if (g_sega_pcm_playing) return;
    if (s_audio_lock) return;
    if (g_paused && g_game_mode == GM_LEVEL) {
        if (!s_paused_muted) {
            NR51_REG = 0x00;
            s_paused_muted = 1;
        }
        return;
    }
    if (s_paused_muted) {
        NR51_REG = 0xFF;
        s_paused_muted = 0;
    }
    hw_sound_tick();
}

BANKREF_EXTERN(print_win_ram_b127)
extern void print_win_ram_b127(uint8_t x, uint8_t y, const char *ram_str) BANKED;
BANKREF_EXTERN(flush_vscroll_b127)
extern void flush_vscroll_b127(uint8_t cnt) BANKED;

void hw_print_win(uint8_t x, uint8_t y, const char *str) {
    char buf[21];
    uint8_t n = 0;
    while (n < 20U && str[n] != '\0') {
        buf[n] = str[n];
        n++;
    }
    buf[n] = '\0';
    print_win_ram_b127(x, y, buf);
}

static uint8_t s_prev_oam_used = 40;

void hw_clear_remaining_oam(void) {
    uint8_t i;
    uint8_t lim = s_prev_oam_used;
    for (i = g_oam_next; i < lim; i++) {
        shadow_OAM[i].y = 0;
    }
    s_prev_oam_used = g_oam_next ? g_oam_next : 1;
}

/* One 8x8 sprite into shadow OAM (visible iff -8 < sx < 160 and -8 < sy < 128). Hand-written: this is called
   for most object pieces every frame. sdcccall(1): sx in DE, sy in BC, tile at [SP+2], attr at [SP+3]; the callee
   pops the two stack bytes. shadow_OAM is 256-byte aligned (OAM DMA requirement). */
void hw_put_sprite8(int16_t sx, int16_t sy, uint8_t tile, uint8_t attr) __naked {
    (void)sx; (void)sy; (void)tile; (void)attr;
    __asm
    ld   a, (_g_oam_next)
    cp   a, #40
    jr   nc, 401$
    ld   hl, #7
    add  hl, de
    ld   a, h
    or   a, a
    jr   nz, 401$
    ld   a, l
    cp   a, #167
    jr   nc, 401$
    ld   hl, #7
    add  hl, bc
    ld   a, h
    or   a, a
    jr   nz, 401$
    ld   a, l
    cp   a, #135
    jr   nc, 401$
    ld   a, (_g_oam_next)
    ld   l, a
    inc  a
    ld   (_g_oam_next), a
    ld   a, l
    add  a, a
    add  a, a
    ld   l, a
    ld   h, #(_shadow_OAM >> 8)
    ld   a, c
    add  a, #16
    ld   (hl+), a
    ld   a, e
    add  a, #8
    ld   (hl+), a
    ld   d, h
    ld   e, l
    ldhl sp, #2
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl)
    ld   (de), a
401$:
    pop  hl
    pop  af
    jp   (hl)
    __endasm;
}

/* Same result as emitting 2*ncols `hw_put_sprite8` calls (column by column, top then bottom; X/Y flips mirror the
   column order / swap top and bottom tiles). Hand-written; scratch state lives in c2_*.
   sdcccall(1): sx in DE, sy in BC, base_tile [SP+2], ncols [SP+3], attr [SP+4]; callee pops 3 bytes. */
static uint8_t c2_ok0, c2_ok1, c2_oy0, c2_oy1, c2_base, c2_ncols, c2_attr, c2_cx;
void hw_put_sprite_cols2rows(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t ncols, uint8_t attr) __naked {
    (void)sx; (void)sy; (void)base_tile; (void)ncols; (void)attr;
    __asm
    ld   hl, #7
    add  hl, bc
    ld   a, h
    or   a, a
    jr   nz, 410$
    ld   a, l
    cp   a, #135
    jr   nc, 410$
    ld   a, #1
    jr   411$
410$:
    xor  a, a
411$:
    ld   (_c2_ok0), a
    ld   hl, #15
    add  hl, bc
    ld   a, h
    or   a, a
    jr   nz, 412$
    ld   a, l
    cp   a, #135
    jr   nc, 412$
    ld   a, #1
    jr   413$
412$:
    xor  a, a
413$:
    ld   (_c2_ok1), a
    ld   a, (_g_oam_next)
    cp   a, #40
    jp   nc, 499$
    ld   a, (_c2_ok0)
    ld   h, a
    ld   a, (_c2_ok1)
    or   a, h
    jp   z, 499$
    ld   a, c
    add  a, #16
    ld   (_c2_oy0), a
    add  a, #8
    ld   (_c2_oy1), a
    ldhl sp, #2
    ld   a, (hl+)
    ld   (_c2_base), a
    ld   a, (hl+)
    ld   (_c2_ncols), a
    ld   a, (hl)
    ld   (_c2_attr), a
    xor  a, a
    ld   (_c2_cx), a
420$:
    ld   a, (_c2_ncols)
    ld   b, a
    ld   a, (_c2_cx)
    cp   a, b
    jp   z, 499$
    ld   hl, #7
    add  hl, de
    ld   a, h
    or   a, a
    jp   nz, 430$
    ld   a, l
    cp   a, #167
    jp   nc, 430$
    ld   a, (_c2_attr)
    and  a, #0x20
    ld   a, (_c2_cx)
    jr   z, 421$
    ld   c, a
    ld   a, b
    dec  a
    sub  a, c
421$:
    add  a, a
    ld   c, a
    ld   a, (_c2_base)
    add  a, c
    ld   c, a
    ld   b, a
    inc  b
    ld   a, (_c2_attr)
    and  a, #0x40
    jr   z, 422$
    ld   a, c
    ld   c, b
    ld   b, a
422$:
    push de
    ld   a, e
    add  a, #8
    ld   e, a
    ld   a, (_c2_ok0)
    or   a, a
    jr   z, 423$
    ld   a, (_g_oam_next)
    cp   a, #40
    jr   nc, 423$
    ld   l, a
    inc  a
    ld   (_g_oam_next), a
    ld   a, l
    add  a, a
    add  a, a
    ld   l, a
    ld   h, #(_shadow_OAM >> 8)
    ld   a, (_c2_oy0)
    ld   (hl+), a
    ld   a, e
    ld   (hl+), a
    ld   a, c
    ld   (hl+), a
    ld   a, (_c2_attr)
    ld   (hl), a
423$:
    ld   a, (_c2_ok1)
    or   a, a
    jr   z, 424$
    ld   a, (_g_oam_next)
    cp   a, #40
    jr   nc, 424$
    ld   l, a
    inc  a
    ld   (_g_oam_next), a
    ld   a, l
    add  a, a
    add  a, a
    ld   l, a
    ld   h, #(_shadow_OAM >> 8)
    ld   a, (_c2_oy1)
    ld   (hl+), a
    ld   a, e
    ld   (hl+), a
    ld   a, b
    ld   (hl+), a
    ld   a, (_c2_attr)
    ld   (hl), a
424$:
    pop  de
430$:
    ld   a, (_c2_cx)
    inc  a
    ld   (_c2_cx), a
    ld   hl, #8
    add  hl, de
    ld   d, h
    ld   e, l
    jp   420$
499$:
    pop  hl
    add  sp, #3
    jp   (hl)
    __endasm;
}

/* 16x16 sprite centred on (sx, sy): exactly hw_put_sprite_cols2rows(sx - 8, sy - 8, base_tile, 2, attr), unrolled
   for two columns. sdcccall(1): sx DE, sy BC, base_tile [SP+2], attr [SP+3]; callee pops 2 bytes. */
void hw_put_sprite16(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t attr) __naked {
    (void)sx; (void)sy; (void)base_tile; (void)attr;
    __asm
    ld   a, (_g_oam_next)
    cp   a, #40
    jp   nc, 909$
    ld   h, b
    ld   l, c
    dec  hl
    ld   a, h
    or   a, a
    jr   nz, 901$
    ld   a, l
    cp   a, #135
    jr   nc, 901$
    ld   a, #1
    jr   902$
901$:
    xor  a, a
902$:
    ld   (_c2_ok0), a
    ld   hl, #7
    add  hl, bc
    ld   a, h
    or   a, a
    jr   nz, 903$
    ld   a, l
    cp   a, #135
    jr   nc, 903$
    ld   a, #1
    jr   904$
903$:
    xor  a, a
904$:
    ld   (_c2_ok1), a
    ld   h, a
    ld   a, (_c2_ok0)
    or   a, h
    jp   z, 909$
    ld   a, c
    add  a, #8
    ld   (_c2_oy0), a
    add  a, #8
    ld   (_c2_oy1), a
    ldhl sp, #2
    ld   a, (hl+)
    ld   (_c2_base), a
    ld   a, (hl)
    ld   (_c2_attr), a
    ld   h, d
    ld   l, e
    dec  hl
    ld   a, h
    or   a, a
    jr   nz, 906$
    ld   a, l
    cp   a, #167
    jr   nc, 906$
    ld   b, e
    ld   a, (_c2_attr)
    and  a, #0x20
    ld   a, #0
    jr   z, 905$
    ld   a, #2
905$:
    call 950$
906$:
    ld   hl, #7
    add  hl, de
    ld   a, h
    or   a, a
    jr   nz, 909$
    ld   a, l
    cp   a, #167
    jr   nc, 909$
    ld   a, e
    add  a, #8
    ld   b, a
    ld   a, (_c2_attr)
    and  a, #0x20
    ld   a, #2
    jr   z, 907$
    ld   a, #0
907$:
    call 950$
909$:
    pop  hl
    pop  af
    jp   (hl)
950$:
    push de
    ld   c, a
    ld   a, (_c2_base)
    add  a, c
    ld   c, a
    ld   d, a
    inc  d
    ld   a, (_c2_attr)
    and  a, #0x40
    jr   z, 951$
    ld   a, c
    ld   c, d
    ld   d, a
951$:
    ld   e, b
    ld   a, (_c2_ok0)
    or   a, a
    jr   z, 952$
    ld   a, (_g_oam_next)
    cp   a, #40
    jr   nc, 952$
    ld   l, a
    inc  a
    ld   (_g_oam_next), a
    ld   a, l
    add  a, a
    add  a, a
    ld   l, a
    ld   h, #(_shadow_OAM >> 8)
    ld   a, (_c2_oy0)
    ld   (hl+), a
    ld   a, e
    ld   (hl+), a
    ld   a, c
    ld   (hl+), a
    ld   a, (_c2_attr)
    ld   (hl), a
952$:
    ld   a, (_c2_ok1)
    or   a, a
    jr   z, 953$
    ld   a, (_g_oam_next)
    cp   a, #40
    jr   nc, 953$
    ld   l, a
    inc  a
    ld   (_g_oam_next), a
    ld   a, l
    add  a, a
    add  a, a
    ld   l, a
    ld   h, #(_shadow_OAM >> 8)
    ld   a, (_c2_oy1)
    ld   (hl+), a
    ld   a, e
    ld   (hl+), a
    ld   a, d
    ld   (hl+), a
    ld   a, (_c2_attr)
    ld   (hl), a
953$:
    pop  de
    ret
    __endasm;
}

BANKREF_EXTERN(draw_transition_card_win_b127)
extern void draw_transition_card_win_b127(const char *line1, const char *line2, uint8_t act_num) BANKED;
BANKREF_EXTERN(spawn_try_place_b127)
extern void spawn_try_place_b127(uint16_t idx, int16_t ox, int16_t oy, uint8_t oid, uint8_t osub) BANKED;
BANKREF_EXTERN(sonic_anim_oct_b127)
extern uint8_t sonic_anim_oct_b127(uint8_t *out_fx, uint8_t *out_fy, uint8_t cmd, uint8_t do_oct) BANKED;

void hw_show_transition_card(const char *line1, const char *line2, uint8_t act_num) {
    char l1[20], l2[20];
    uint8_t i = 0, prev = CURRENT_BANK;
    l1[0] = '\0'; l2[0] = '\0';
    if (line1) { for (i = 0; i < 19 && line1[i]; i++) l1[i] = line1[i]; l1[i] = '\0'; }
    if (line2) { for (i = 0; i < 19 && line2[i]; i++) l2[i] = line2[i]; l2[i] = '\0'; }
    g_oam_next = 0;
    hw_clear_remaining_oam();

    SWITCH_ROM(4);
    VBK_REG = 1;
    set_bkg_data(80, 48, hud_font_tiles);
    SWITCH_ROM(prev);
    draw_transition_card_win_b127(l1, l2, act_num);
}

void hw_load_fullscreen_screen(uint8_t scr_idx) {
    uint8_t prev = CURRENT_BANK;
    FullScreenDesc desc;
    uint8_t y;

    /* Copy descriptor safely from Bank 8 into stack RAM */
    SWITCH_ROM(8);
    desc = g_screens[scr_idx];

    if (LCDC_REG & LCDCF_ON) {
        set_bkg_palette(7, 1, &desc.bg_pal[28]);
        hw_show_transition_card("SONIC THE HEDGEHOG", "1991 SEGA", 0);
    }

    SWITCH_ROM(desc.bank);
    VBK_REG = 0;
    set_bkg_data(0,   64, desc.tiles0 + 0);
    set_bkg_data(64,  64, desc.tiles0 + 1024);
    if (LCDC_REG & LCDCF_ON) vsync();
    set_bkg_data(128, 64, desc.tiles0 + 2048);
    set_bkg_data(192, 64, desc.tiles0 + 3072);
    if (LCDC_REG & LCDCF_ON) vsync();

    VBK_REG = 1;
    set_bkg_data(0,   64, desc.tiles1 + 0);
    set_bkg_data(64,  16, desc.tiles1 + 1024);
    set_bkg_data(128, 64, desc.tiles1 + 1280);
    set_bkg_data(192, 64, desc.tiles1 + 2304);
    if (LCDC_REG & LCDCF_ON) vsync();

    for (y = 0; y < 18; y++) {
        VBK_REG = 1;
        set_bkg_tiles(0, y, 20, 1, desc.attr20x18 + (uint16_t)y * 20);
        VBK_REG = 0;
        set_bkg_tiles(0, y, 20, 1, desc.map20x18 + (uint16_t)y * 20);
    }
    SCX_REG = 0;
    SCY_REG = 0;
    set_bkg_palette(0, 8, desc.bg_pal);
    WY_REG = 144; /* Hide window overlay to reveal crisp full-bleed 160x144 screen! */
    s_vbl_joy_latch = 0;
    SWITCH_ROM(prev);
}

/* Pre-cached WRAM Zone Chunk Pointers (`0..15` pages = up to `256` chunks) — ZERO `SWITCH_ROM(8)` calls in gameplay! */
static const uint8_t *s_zone_chunk_ptrs[16];

/* MBC5 SRAM Bank 1 (`rRAMB = 1`) runtime map after removing the parallax BG plane:
 *   `0xA000..0xAFFF`: Special Stage 64x64 maze (Special Stage only); in level mode:
 *       `0xA000..0xA7FF` ring-overlay cell grids (objects.c), `0xA800..0xA9FF` spawn slot hints
 *   `0xB000..0xB1FF`: active foreground level layout only
 *   `0xB200..0xB22F`: ring-overlay cell sequence (objects.c); `0xB240..0xB267`: pending HUD tile rows
 *   `0xB400..0xBBFF`: primary (`+0`) and secondary (`+0x400`) collision tables
 *   `0xBC00..0xBFFF`: VBlank span queue */

#define s_fg_ram  ((uint8_t *)0xB000U)
#define s_col_ram ((uint8_t *)0xB400U)
static const uint8_t *s_fg_rows[32];
static uint8_t s_cached_chunk = 0xFF, s_cached_tile, s_cached_d4l, s_cached_d4h, s_cached_cid2;
static uint8_t s_scan_force = 1;
static uint8_t s_scache_frame[2] = { 0xFF, 0xFF };
static uint8_t s_cur_sonic_base = 0;

uint8_t hw_get_raw_chunk_id(int16_t world_x, int16_t world_y) {
    uint8_t cx, cy;
    if (world_x < 0 || world_y < 0) return 0;
    cx = (uint8_t)((uint16_t)world_x >> 8);
    cy = (uint8_t)((uint16_t)world_y >> 8);
    if (cx >= g_ram_act.fg_w || cy >= g_ram_act.fg_h) return 0;
    return s_fg_rows[cy][cx];
}

/* Look up 16-bit chunk block word at `(world_x, world_y)` matching `FindNearestTile` (`sub FindNearestTile.asm`) */
uint16_t hw_get_chunk_word(int16_t world_x, int16_t world_y, uint8_t loop_alt) {
    uint8_t prev, cx, cy, chunk_id, page_idx, bx, by;
    const uint8_t *cptr;
    uint16_t bw;

    if (world_x < 0 || world_y < 0) return 0;
    cx = (uint8_t)((uint16_t)world_x >> 8);
    cy = (uint8_t)((uint16_t)world_y >> 8);
    if (cx >= g_ram_act.fg_w || cy >= g_ram_act.fg_h) return 0;

    prev = CURRENT_BANK;
    chunk_id = s_fg_rows[cy][cx];
    if (chunk_id == 0) {
        SWITCH_ROM(prev);
        return 0;
    }
    if (chunk_id & 0x80) {
        chunk_id &= 0x7F;
        if (loop_alt) {
            chunk_id++;
            if (chunk_id == 0x29) chunk_id = 0x51;
        }
    }
    if (chunk_id == 0 || chunk_id > g_ram_vset.num_chunks) {
        SWITCH_ROM(prev);
        return 0;
    }
    chunk_id--;
    page_idx = chunk_id >> 4;
    cptr = s_zone_chunk_ptrs[page_idx & 15U];
    SWITCH_ROM(g_ram_vset.chunk_base_bank + page_idx);
    bx = ((uint8_t)world_x >> 4) & 0x0F;
    by = ((uint8_t)world_y >> 4) & 0x0F;
    bw = ((const uint16_t *)cptr)[((uint16_t)(chunk_id & 0x0F) << 8) + ((uint16_t)by << 4) + bx];
    SWITCH_ROM(prev);
    return bw;
}

/* Fast 2-pass span resolver (`is_col=1`: 20-tile column; `is_col=0`: 23-tile row) — zero FG/BG bank ping-pong! */
/* Block words -> (tile, attr) pairs. Hand-written loop; attrs are stored 23 bytes after the tiles
   (`VScrollSpan.a` directly follows `VScrollSpan.t[23]`). Per entry: bidx = bw & 0x3FF; 0 or >= num_m16 -> (0,0);
   else tile = map16[2*bidx], attr = (map16[2*bidx+1] & 0x7F) ^ ((bw >> 11 & 1) << 5 | (bw >> 12 & 1) << 6). */
static uint8_t s_bt_cnt, s_bt_hi, s_bt_nml, s_bt_nmh;
static const uint8_t *s_bt_map16;
static void bt_core(const uint8_t *src, uint8_t *out_t) __naked {
    (void)src; (void)out_t;
    __asm
501$:
    ld   a, (_s_bt_cnt)
    or   a, a
    ret  z
    dec  a
    ld   (_s_bt_cnt), a
    ld   a, (de)
    inc  de
    ld   l, a
    ld   a, (de)
    inc  de
    ld   (_s_bt_hi), a
    and  a, #0x03
    ld   h, a
    or   a, l
    jr   z, 510$
    ld   a, (_s_bt_nmh)
    cp   a, h
    jr   c, 510$
    jr   nz, 502$
    ld   a, (_s_bt_nml)
    cp   a, l
    jr   c, 510$
    jr   z, 510$
502$:
    add  hl, hl
    ld   a, (_s_bt_map16)
    add  a, l
    ld   l, a
    ld   a, (_s_bt_map16 + 1)
    adc  a, h
    ld   h, a
    ld   a, (hl+)
    ld   (bc), a
    ld   a, (hl)
    and  a, #0x7F
    ld   l, a
    ld   a, (_s_bt_hi)
    and  a, #0x18
    add  a, a
    add  a, a
    xor  a, l
    ld   hl, #23
    add  hl, bc
    ld   (hl), a
    inc  bc
    jr   501$
510$:
    xor  a, a
    ld   (bc), a
    ld   hl, #23
    add  hl, bc
    ld   (hl), a
    inc  bc
    jr   501$
    __endasm;
}

static void blocks_to_tiles_batch(const uint16_t *bw_buf, uint8_t count, uint8_t *out_t) {
    uint16_t nm16 = g_ram_vset.num_m16;
    s_bt_cnt = count;
    s_bt_nml = (uint8_t)nm16;
    s_bt_nmh = (uint8_t)(nm16 >> 8);
    s_bt_map16 = g_ram_vset.map16;
    SWITCH_ROM(g_ram_vset.meta_bank);
    bt_core((const uint8_t *)bw_buf, out_t);
}

typedef struct {
    uint8_t is_col;
    uint8_t vx;
    uint8_t vy;
    uint8_t t[23];
    uint8_t a[23];
} VScrollSpan;
#define s_vscroll_q ((VScrollSpan *)0xBC00U)
static uint8_t s_vscroll_cnt = 0;

/* Span copy helpers for enqueue_world_span: copy s_sp_cnt (>= 1) block words from src (column: stride 16 words,
   row: contiguous) to dst, or zero-fill. sdcccall(1): dst in DE, src in BC. */
static uint8_t s_sp_cnt;
static void span_copy_col(uint16_t *dst, const uint16_t *src) __naked {
    (void)dst; (void)src;
    __asm
    ld   h, b
    ld   l, c
    ld   a, (_s_sp_cnt)
    ld   c, a
1501$:
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl-)
    ld   (de), a
    inc  de
    ld   a, l
    add  a, #32
    ld   l, a
    jr   nc, 1502$
    inc  h
1502$:
    dec  c
    jr   nz, 1501$
    ret
    __endasm;
}
static void span_copy_row(uint16_t *dst, const uint16_t *src) __naked {
    (void)dst; (void)src;
    __asm
    ld   h, b
    ld   l, c
    ld   a, (_s_sp_cnt)
    ld   c, a
1511$:
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    dec  c
    jr   nz, 1511$
    ret
    __endasm;
}
static void span_zero(uint16_t *dst) __naked {
    (void)dst;
    __asm
    ld   h, d
    ld   l, e
    ld   a, (_s_sp_cnt)
    ld   c, a
    xor  a, a
1521$:
    ld   (hl+), a
    ld   (hl+), a
    dec  c
    jr   nz, 1521$
    ret
    __endasm;
}

static void enqueue_world_span(int16_t wx0, int16_t wy0, uint8_t is_col) {
    uint8_t prev = CURRENT_BANK;
    uint8_t n = is_col ? 20U : 23U;
    uint16_t bw_buf[23];
    VScrollSpan *sp;

    if (s_vscroll_cnt >= 8U) {
        rRAMB = 1;
        flush_vscroll_b127(s_vscroll_cnt);
        s_vscroll_cnt = 0;
    }

    {
        /* Walk the span one chunk-run at a time: inside a 16x16-block chunk the block index just advances by
           16 (column) or 1 (row), so each run is a strided copy. Out-of-range cells produce 0 as before. */
        uint16_t *dst = bw_buf;
        uint8_t left = n;
        int16_t wx = wx0, wy = wy0;
        while (left) {
            int16_t mv = is_col ? wy : wx;   /* coordinate that advances along the span */
            int16_t fx = is_col ? wx : wy;   /* coordinate fixed for the whole span */
            uint8_t seg;
            const uint16_t *src = 0;
            if (mv < 0) {
                seg = (mv < -(int16_t)left) ? left : (uint8_t)(-mv);
            } else {
                seg = (uint8_t)(16U - ((uint8_t)mv & 0x0FU));
                if (seg > left) seg = left;
                if (fx >= 0) {
                    uint8_t cx = (uint8_t)((uint16_t)wx >> 4);
                    uint8_t cy = (uint8_t)((uint16_t)wy >> 4);
                    if (cx < g_ram_act.fg_w && cy < g_ram_act.fg_h) {
                        uint8_t cid = s_fg_rows[cy][cx] & 0x7FU;
                        if (cid > 0U && cid <= g_ram_vset.num_chunks) {
                            uint8_t pg = (uint8_t)((cid - 1U) >> 4);
                            SWITCH_ROM((uint8_t)(g_ram_vset.chunk_base_bank + pg));
                            src = (const uint16_t *)s_zone_chunk_ptrs[pg & 15U] + ((uint16_t)((cid - 1U) & 0x0FU) << 8) +
                                  (uint8_t)((((uint8_t)wy & 0x0FU) << 4) | ((uint8_t)wx & 0x0FU));
                        }
                    }
                }
            }
            s_sp_cnt = seg;
            if (src) {
                if (is_col) span_copy_col(dst, src);
                else        span_copy_row(dst, src);
            } else {
                span_zero(dst);
            }
            dst += seg;
            left = (uint8_t)(left - seg);
            if (is_col) wy += seg; else wx += seg;
        }
    }

    /* Original independently scrolling parallax BG plane removed by request. Empty FG cells stay blank;
       collectible stage rings are queued as true CGB background-tile overlays instead. */
    rRAMB = 1;
    sp = &s_vscroll_q[s_vscroll_cnt++];
    sp->is_col = is_col;
    sp->vx = (uint8_t)wx0 & 31U;
    sp->vy = (uint8_t)wy0 & 31U;
    /* Terrain streaming overwrites BG cells here; ring overlay must be re-applied on this column/row only. */
    if (is_col) g_ring_dirty_col[sp->vx >> 3] |= (uint8_t)(1U << (sp->vx & 7U));
    else        g_ring_dirty_row[sp->vy >> 3] |= (uint8_t)(1U << (sp->vy & 7U));
    blocks_to_tiles_batch(bw_buf, n, sp->t);
    SWITCH_ROM(prev);
}

#define draw_world_col(wx, wy) enqueue_world_span((wx), (wy), 1U)
#define draw_world_row(wx, wy) enqueue_world_span((wx), (wy), 0U)

void hw_refresh_full_viewport(void) {
    int16_t wx0 = (g_cam_x >> 4) - 1;
    int16_t wy0 = (g_cam_y >> 4) - 1;
    uint8_t r;
    g_ring_force_redraw = 1;
    s_vscroll_cnt = 0;
    for (r = 0; r < 20; r++) {
        draw_world_row(wx0, wy0 + r);
        if ((r & 3) == 3) {
            vsync();
            flush_vscroll_b127(s_vscroll_cnt);
            s_vscroll_cnt = 0;
        }
    }
    SCX_REG = (uint8_t)(g_cam_x >> 1);
    SCY_REG = (uint8_t)(g_cam_y >> 1);
}

void hw_stream_viewport_delta(int16_t old_cam_x, int16_t old_cam_y) {
    int16_t old_tx = old_cam_x >> 4;
    int16_t new_tx = g_cam_x >> 4;
    int16_t old_ty = old_cam_y >> 4;
    int16_t new_ty = g_cam_y >> 4;

    if (new_tx - old_tx > 8 || new_tx - old_tx < -8 || new_ty - old_ty > 8 || new_ty - old_ty < -8) {
        hw_refresh_full_viewport();
        return;
    }

    while (old_tx < new_tx) {
        old_tx++;
        draw_world_col(old_tx + 20, new_ty - 1);
    }
    while (old_tx > new_tx) {
        old_tx--;
        draw_world_col(old_tx - 1, new_ty - 1);
    }
    while (old_ty < new_ty) {
        old_ty++;
        draw_world_row(new_tx - 1, old_ty + 17);
    }
    while (old_ty > new_ty) {
        old_ty--;
        draw_world_row(new_tx - 1, old_ty - 1);
    }
    SCX_REG = (uint8_t)(g_cam_x >> 1);
    SCY_REG = (uint8_t)(g_cam_y >> 1);
}

/* Ring-overlay state/logic lives in bank 2 (`src/objects.c`); only the bank-4 tile upload must stay in fixed bank 0. */
uint8_t g_ring_last_frame = 0xFF;

static void reload_common_hud_and_sprites(void) {
    SWITCH_ROM(4);
    VBK_REG = 1;
    set_bkg_data(80, 48, hud_font_tiles);
    set_bkg_data(BKG_RING_TILE_BASE, 16, ring_bg_tiles);
    g_ring_last_frame = 0xFF;
    VBK_REG = 0;
    set_sprite_data(32, 64, common_sprite_tiles);
    set_sprite_data(96, 32, common_sprite_tiles + 1024);
    set_sprite_palette(0, 8, g_default_obj_pals);
}

void hw_load_act(uint8_t act_idx) {
    uint8_t prev = CURRENT_BANK;
    int16_t sx, sy;
    uint8_t hold;
    uint16_t pal_ram[32];
    char act_label[16];

    if (act_idx >= NUM_PLAYABLE_ACTS) act_idx = 0;
    g_cur_act = act_idx;

    /* Clear any previous act's ring overlay from wrapped BG-map cells before streaming new art. */
    hw_ring_reset();

    SWITCH_ROM(8);
    __critical {
        g_ram_act = g_acts[act_idx];
        g_ram_vset = g_visual_sets[g_ram_act.vset_idx];
    }
    for (hold = 0; hold < 15 && g_ram_act.label[hold]; hold++) {
        act_label[hold] = g_ram_act.label[hold];
    }
    act_label[hold] = '\0';
    for (hold = 0; hold < 16; hold++) {
        uint8_t b = g_ram_vset.chunk_base_bank + hold;
        s_zone_chunk_ptrs[hold] = (b < 128U) ? g_chunk_bank_ptrs[b] : 0;
    }
    SWITCH_ROM(g_ram_act.data_bank);
    {
        uint16_t flen = (uint16_t)g_ram_act.fg_w * g_ram_act.fg_h;
        if (flen > 384U) flen = 384U;
        rRAMB = 1;
        memcpy(s_fg_ram, g_ram_act.fg_layout, flen);
    }
    for (hold = 0; hold < 32; hold++) {
        s_fg_rows[hold] = &s_fg_ram[(uint16_t)hold * g_ram_act.fg_w];
    }
    s_cached_chunk = 0xFF;
    SWITCH_ROM(g_ram_vset.meta_bank);
    rRAMB = 1;
    memset(s_col_ram, 0, 2048U);
    memcpy(s_col_ram, g_ram_vset.col1, g_ram_vset.num_m16);
    memcpy(s_col_ram + 1024U, g_ram_vset.col2, g_ram_vset.num_m16);

    /* Display crisp colorful Sonic Title Card Window during seamless VRAM streaming */
    set_bkg_palette(7, 1, &g_ram_vset.bg_pal[28]);
    hw_show_transition_card(act_label, "ZONE", (uint8_t)((act_idx % 3) + 1));

    /* Stream 464 Zone BG tiles + 128 Zone Sprite tiles cleanly across VBlanks */
    SWITCH_ROM(g_ram_vset.tiles_bank);
    VBK_REG = 0;
    set_bkg_data(0,   64, g_ram_vset.bg_tiles0 + 0);
    set_bkg_data(64,  64, g_ram_vset.bg_tiles0 + 1024);
    vsync();
    set_bkg_data(128, 64, g_ram_vset.bg_tiles0 + 2048);
    set_bkg_data(192, 64, g_ram_vset.bg_tiles0 + 3072);
    vsync();

    VBK_REG = 1;
    set_bkg_data(0,   64, g_ram_vset.bg_tiles1 + 0);
    set_bkg_data(64,  16, g_ram_vset.bg_tiles1 + 1024);
    set_bkg_data(128, 64, g_ram_vset.bg_tiles1 + 1280);
    set_bkg_data(192, 64, g_ram_vset.bg_tiles1 + 2304);
    vsync();

    set_sprite_data(0,  64, g_ram_vset.spr_tiles + 0);
    set_sprite_data(64, 64, g_ram_vset.spr_tiles + 1024);
    VBK_REG = 0;
    vsync();

    reload_common_hud_and_sprites();
    vsync();

    /* Initialize level state, Sonic, and Objects */
    if (g_checkpoint_active && g_checkpoint_act == act_idx) {
        sx = g_checkpoint_x;
        sy = g_checkpoint_y;
    } else {
        sx = (int16_t)g_ram_act.start_x;
        sy = (int16_t)g_ram_act.start_y;
        g_level_frames = 0;
        g_level_sec_div = 0;
    }
    g_rings = 0;
    g_next_ring_life = 1;
    g_signpost_passed = 0;
    g_act_clear_done = 0;
    g_boss_active = 0;
    g_boss_defeated = 0;
    g_water_y = (act_idx >= 9 && act_idx <= 11) ? g_ram_act.water_y : 0x7FFF;
    hw_set_underwater_palette(0xFF);
    s_loaded_sonic_frame = 0xFF;
    s_scache_frame[0] = 0xFF;
    s_scache_frame[1] = 0xFF;

    g_debug_active = 0;
    g_button_state = 0;
    dle_init_for_act();
    osc_reset();
    physics_init_sonic(sx, sy);
    objects_reset_for_act();
    boss_and_gimmick_reset();
    s_scan_force = 1;
    hw_scan_spawn_objects();
    hw_step_sonic_anim();

    memcpy(pal_ram, g_ram_vset.bg_pal, sizeof(pal_ram));
    set_bkg_palette(0, 8, pal_ram);
    set_sprite_palette(0, 8, g_default_obj_pals);
    if (g_ram_act.vset_idx == 0) set_sprite_palette(6, 2, g_ghz_obj_pal67);   /* GHZ: dirt/wood + grass for bridge, ledge, platforms */
    hw_play_bgm(g_ram_act.music_id);
    hw_refresh_full_viewport();

    /* Hold Title Card briefly so player reads Zone & Act, then reveal HUD bar at WY=128! */
    for (hold = 0; hold < 2; hold++) {
        vsync();
    }
    hw_update_hud();
    WX_REG = 7;
    WY_REG = 128;
    g_paused = 0;
    g_joy = joypad();
    g_joy_prev = g_joy;
    g_joy_pressed = 0;
    s_vbl_joy_latch = 0;
    SWITCH_ROM(prev);
}

void hw_load_special_stage_vram(uint8_t ss_idx, int16_t *out_sx, int16_t *out_sy) {
    uint8_t prev = CURRENT_BANK;
    SpecialStageDesc d;
    static const uint16_t ss_pals[32] = {
        /* BGP0: Deep Special Stage Starry Sky + Color Walls */
        RGB8(24, 36, 148), RGB8(40, 128, 248), RGB8(116, 212, 252), RGB8(252, 252, 252),
        /* BGP1: Pink/Magenta Maze Walls */
        RGB8(24, 36, 148), RGB8(152, 24, 140), RGB8(244, 72, 208), RGB8(252, 224, 248),
        /* BGP2: Emerald Green Maze Walls */
        RGB8(24, 36, 148), RGB8(12, 116, 56),  RGB8(48, 228, 108), RGB8(224, 252, 224),
        /* BGP3: Golden Ring & Bumper Cells */
        RGB8(24, 36, 148), RGB8(148, 72, 0),   RGB8(252, 208, 0),  RGB8(252, 252, 252),
        /* BGP4: Crimson GOAL & UP/DOWN Spheres */
        RGB8(24, 36, 148), RGB8(148, 16, 16),  RGB8(248, 48, 36),  RGB8(252, 240, 180),
        /* BGP5: Cyan Crystal & R-Block */
        RGB8(24, 36, 148), RGB8(0, 108, 148),  RGB8(56, 228, 248), RGB8(252, 252, 252),
        /* BGP6: Chaos Emerald Prismatic Glow */
        RGB8(24, 36, 148), RGB8(88, 40, 220),  RGB8(176, 120, 252),RGB8(252, 252, 252),
        /* BGP7: HUD Window */
        RGB8(16, 32, 108), RGB8(228, 28, 24),  RGB8(252, 216, 0),  RGB8(252, 252, 252)
    };
    SWITCH_ROM(8);
    d = g_special_stages[ss_idx % NUM_SPECIAL_STAGES];
    /* Authentic `09 Sonic in Special Stage.asm` spawn mapping: 24px cell pitch (`$18`) minus `$14` (20-cell border)! */
    *out_sx = (int16_t)(((((int16_t)(d.start_x / 24U) - 20) & 63) << 4) + 8);
    *out_sy = (int16_t)(((((int16_t)(d.start_y / 24U) - 20) & 63) << 4) + 8);
    hw_show_transition_card("SPECIAL STAGE", "CHAOS EMERALD", (uint8_t)(ss_idx + 1));
    SWITCH_ROM(SS_TILES_BANK);
    VBK_REG = 0;
    /* Tile 0 = blank sky backdrop (`0x00 * 16`) */
    {
        uint8_t zero16[16];
        memset(zero16, 0, 16);
        set_bkg_data(0, 1, zero16);
    }
    set_bkg_data(1, 64, ss_vram_tiles);
    vsync();
    reload_common_hud_and_sprites();
    vsync();
    set_bkg_palette(0, 8, ss_pals);
    g_sonic.invuln = 0;
    s_loaded_sonic_frame = 0xFF;
    s_scache_frame[0] = 0xFF;
    s_scache_frame[1] = 0xFF;
    SCX_REG = 0;
    SCY_REG = 0;
    hw_update_hud();
    WX_REG = 7;
    WY_REG = 128;
    s_vbl_joy_latch = 0;
    SWITCH_ROM(prev);
}

void hw_copy_ss_maze(uint8_t ss_idx, uint8_t *dst4096) {
    uint8_t prev = CURRENT_BANK;
    SpecialStageDesc d;
    SWITCH_ROM(8);
    __critical {
        d = g_special_stages[ss_idx % NUM_SPECIAL_STAGES];
    }
    SWITCH_ROM(d.bank);
    memcpy(dst4096, d.maze64x64, 4096);
    SWITCH_ROM(prev);
}

/* Exact 64-entry Sonic 1 Quarter-Sine Table in `uint8_t` (`65 B` vs `130 B`) — exact `0..256` with ZERO bank switch! */
static const uint8_t s_qsin[64] = {
    0, 6, 12, 18, 25, 31, 37, 43, 49, 56, 62, 68, 74, 80, 86, 92,
    97, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167, 171, 176,
    181, 185, 189, 193, 197, 201, 205, 209, 212, 216, 219, 222, 225, 228, 231, 234,
    236, 238, 241, 243, 244, 246, 248, 249, 251, 252, 253, 254, 254, 255, 255, 255
};

int16_t hw_sin(uint8_t angle) {
    uint8_t q = angle & 63;
    int16_t v = (angle & 64) ? (q == 0 ? 256 : (int16_t)s_qsin[64 - q]) : (int16_t)s_qsin[q];
    return (angle & 128) ? -v : v;
}

int16_t hw_cos(uint8_t angle) {
    return hw_sin((uint8_t)(angle + 64U));
}

uint8_t hw_read_demo_input(uint8_t demo_idx) {
    uint8_t prev = CURRENT_BANK;
    uint16_t base;
    if (g_demo_btn_timer == 0) {
        SWITCH_ROM(5);
        base = s1_demo_offsets[demo_idx % 13];
        g_demo_btn = s1_demo_data[base + g_demo_pos];
        g_demo_btn_timer = s1_demo_data[base + g_demo_pos + 1] + 1;
        g_demo_pos += 2;
        SWITCH_ROM(prev);
    }
    g_demo_btn_timer--;
    /* Translate Mega Drive pad bits (`0x01=Up,0x02=Dn,0x04=L,0x08=R,0x70=ABC`) to GBDK joypad bits */
    {
        uint8_t j = 0;
        if (g_demo_btn & 0x01) j |= J_UP;
        if (g_demo_btn & 0x02) j |= J_DOWN;
        if (g_demo_btn & 0x04) j |= J_LEFT;
        if (g_demo_btn & 0x08) j |= J_RIGHT;
        if (g_demo_btn & 0x70) j |= J_A;
        return j;
    }
}

/* ============================================================================================
 * LITERAL INSTRUCTION-FOR-INSTRUCTION PORT OF SEGA GENESIS SONIC 1 COLLISION ENGINE
 * (`vendor/s1disasm/_incObj/sub FindNearestTile & FindFloor & FindWall.asm`)
 * ============================================================================================ */
/* Inputs/outputs of the sampler live in fixed WRAM slots instead of an 8-argument call: SDCC spills every
   stack argument, and direct-address globals are several times cheaper on the SM83. Semantics are unchanged. */
static int16_t s_cs_d2, s_cs_d3;
static uint8_t s_cs_solid_mask;   /* (1 << solid_bit) >> 8: solid_bit is always 12..15 */
static uint8_t s_cs_d6h;          /* high byte of d6: only bits 0x10 (0x1000) / 0x08 (0x0800) are tested */
static uint8_t s_cs_loop;
static uint8_t *s_cs_a4;
static int8_t s_cs_d0;

/* Hand-written collision sampler; same steps as the original C version:
 *   cy = d2 >> 8, cx = d3 >> 8 (negative or outside fg_w x fg_h -> 0); chunk = s_fg_rows[cy][cx]
 *   (bit 7: loop chunk, +1 on the loop plane with 0x29 -> 0x51); 0 or > num_chunks -> 0; chunk--
 *   tile = (d2 & 0xF0) | (d3 lo >> 4); d4 = chunk table word (one-entry cache); cid = col_ram[d4 & 0x3FF] if
 *   d4 & 0x7FF; no cid / solid bit clear / empty -> 0. Then (bank 5) the angle (flip order depends on `rotated`,
 *   using -(a+0x40)-0x40 == (-a)^0x80) and the height from the normal or rotated table, negated when the flip
 *   bit of d4 ^ d6 is set. sdcccall(1): rotated in A, result in A. */
typedef char col_layout_check[(offsetof(ActDesc, fg_w) == 10U && offsetof(ActDesc, fg_h) == 11U &&
                               offsetof(VisualSetDesc, chunk_base_bank) == 14U &&
                               offsetof(VisualSetDesc, num_chunks) == 15U) ? 1 : -1];
static uint8_t s_cs_rot, s_cs_prev;
static uint8_t sample_col_height_g(uint8_t rotated) __naked {
    (void)rotated;
    __asm
    ld   (_s_cs_rot), a
    ld   a, (_s_cs_d2 + 1)
    ld   b, a
    ld   a, (_s_cs_d3 + 1)
    ld   c, a
    or   a, b
    and  a, #0x80
    jp   nz, 1499$
    ld   a, (_g_ram_act + 10)
    ld   d, a
    ld   a, c
    cp   a, d
    jp   nc, 1499$
    ld   a, (_g_ram_act + 11)
    ld   d, a
    ld   a, b
    cp   a, d
    jp   nc, 1499$
    ld   l, b
    ld   h, #0
    add  hl, hl
    ld   de, #_s_fg_rows
    add  hl, de
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    ld   e, c
    ld   d, #0
    add  hl, de
    ld   a, (hl)
    bit  7, a
    jr   z, 1401$
    and  a, #0x7F
    ld   e, a
    ld   a, (_s_cs_loop)
    or   a, a
    ld   a, e
    jr   z, 1401$
    inc  a
    cp   a, #0x29
    jr   nz, 1401$
    ld   a, #0x51
1401$:
    or   a, a
    jp   z, 1499$
    ld   e, a
    ld   a, (_g_ram_vset + 15)
    cp   a, e
    jp   c, 1499$
    dec  e
    ld   a, (_s_cs_d3)
    swap a
    and  a, #0x0F
    ld   d, a
    ld   a, (_s_cs_d2)
    and  a, #0xF0
    or   a, d
    ld   d, a
    ldh  a, (__current_bank + 0)
    ld   (_s_cs_prev), a
    ld   a, (_s_cached_chunk)
    cp   a, e
    jr   nz, 1410$
    ld   a, (_s_cached_tile)
    cp   a, d
    jr   nz, 1410$
    ld   a, (_s_cached_d4l)
    ld   c, a
    ld   a, (_s_cached_d4h)
    ld   b, a
    ld   a, (_s_cached_cid2)
    jr   1420$
1410$:
    ld   a, e
    swap a
    and  a, #0x0F
    ld   l, a
    ld   a, (_g_ram_vset + 14)
    add  a, l
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
    ld   h, #0
    add  hl, hl
    ld   bc, #_s_zone_chunk_ptrs
    add  hl, bc
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    ld   a, e
    and  a, #0x0F
    add  a, a
    add  a, h
    ld   h, a
    ld   a, d
    add  a, a
    jr   nc, 1411$
    inc  h
1411$:
    add  a, l
    ld   l, a
    jr   nc, 1412$
    inc  h
1412$:
    ld   a, (hl+)
    ld   c, a
    ld   b, (hl)
    ld   a, b
    and  a, #0x07
    or   a, c
    jr   z, 1413$
    ld   a, b
    and  a, #0x03
    add  a, #0xB4
    ld   h, a
    ld   l, c
    ld   a, (hl)
1413$:
    ld   (_s_cached_cid2), a
    ld   a, e
    ld   (_s_cached_chunk), a
    ld   a, d
    ld   (_s_cached_tile), a
    ld   a, c
    ld   (_s_cached_d4l), a
    ld   a, b
    ld   (_s_cached_d4h), a
    ld   a, (_s_cached_cid2)
1420$:
    or   a, a
    jp   z, 1498$
    ld   e, a
    ld   a, (_s_cs_solid_mask)
    and  a, b
    jp   z, 1498$
    ld   a, b
    and  a, #0x07
    or   a, c
    jp   z, 1498$
    ld   a, #5
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
    ld   hl, #_s_cs_a4
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    or   a, h
    jr   z, 1440$
    push hl
    ld   hl, #_s1_angle_map
    ld   a, l
    add  a, e
    ld   l, a
    jr   nc, 1421$
    inc  h
1421$:
    ld   d, (hl)
    ld   a, (_s_cs_rot)
    or   a, a
    jr   nz, 1425$
    bit  3, b
    jr   z, 1422$
    xor  a, a
    sub  a, d
    ld   d, a
1422$:
    bit  4, b
    jr   z, 1430$
    xor  a, a
    sub  a, d
    xor  a, #0x80
    ld   d, a
    jr   1430$
1425$:
    bit  4, b
    jr   z, 1426$
    xor  a, a
    sub  a, d
    xor  a, #0x80
    ld   d, a
1426$:
    bit  3, b
    jr   z, 1430$
    xor  a, a
    sub  a, d
    ld   d, a
1430$:
    pop  hl
    ld   (hl), d
1440$:
    ld   a, (_s_cs_rot)
    or   a, a
    jr   nz, 1450$
    ld   a, (_s_cs_d3)
    bit  3, b
    jr   z, 1441$
    cpl
1441$:
    and  a, #0x0F
    ld   d, a
    ld   l, e
    ld   h, #0
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    ld   a, l
    or   a, d
    ld   l, a
    push bc
    ld   bc, #_s1_coll_normal
    add  hl, bc
    pop  bc
    ld   d, (hl)
    ld   a, (_s_cs_d6h)
    xor  a, b
    and  a, #0x10
    jr   z, 1460$
    xor  a, a
    sub  a, d
    ld   d, a
    jr   1460$
1450$:
    ld   a, (_s_cs_d2)
    bit  4, b
    jr   z, 1451$
    cpl
1451$:
    and  a, #0x0F
    ld   d, a
    ld   l, e
    ld   h, #0
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    ld   a, l
    or   a, d
    ld   l, a
    push bc
    ld   bc, #_s1_coll_rotated
    add  hl, bc
    pop  bc
    ld   d, (hl)
    ld   a, (_s_cs_d6h)
    xor  a, b
    and  a, #0x08
    jr   z, 1460$
    xor  a, a
    sub  a, d
    ld   d, a
1460$:
    ld   a, d
    ld   (_s_cs_d0), a
    ld   a, (_s_cs_prev)
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
    ld   a, #1
    ret
1498$:
    ld   a, (_s_cs_prev)
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
1499$:
    xor  a, a
    ret
    __endasm;
}

/* FindFloor2 / FindWall2: `lo` is the low nibble of the along-axis coordinate (d2 for floors, d3 for walls). */
static int8_t find2_s1(uint8_t rotated, uint8_t lo) {
    int8_t d0, d1;
    if (!sample_col_height_g(rotated) || (d0 = s_cs_d0) == 0) return (int8_t)(0x0F - lo);
    d1 = (int8_t)(lo + d0);
    if (d0 > 0) return (int8_t)(0x0F - d1);
    return (d1 >= 0) ? (int8_t)(0x0F - lo) : (int8_t)~d1;
}

static void cs_setup(uint8_t solid_bit, uint16_t d6, uint8_t *a4) {
    s_cs_solid_mask = (uint8_t)(1U << (uint8_t)(solid_bit - 8U));
    s_cs_d6h = (uint8_t)(d6 >> 8);
    s_cs_a4 = a4;
    s_cs_loop = g_sonic.loop_plane;
}

int16_t hw_find_floor(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, int16_t a3, uint8_t *a4) {
    int8_t d0;
    uint8_t lo = (uint8_t)d2 & 0x0FU;
    cs_setup(solid_bit, d6, a4);
    s_cs_d2 = d2; s_cs_d3 = d3;
    if (!sample_col_height_g(0) || (d0 = s_cs_d0) == 0) goto next;
    if (d0 < 0) {
        if ((int8_t)(lo + d0) >= 0) goto next;
        goto back;
    }
    if (d0 == 16) goto back;
    return (int16_t)(int8_t)(0x0F - (int8_t)(lo + d0));
next:
    s_cs_d2 = d2 + a3;
    return (int16_t)find2_s1(0, (uint8_t)s_cs_d2 & 0x0FU) + 16;
back:
    s_cs_d2 = d2 - a3;
    return (int16_t)find2_s1(0, (uint8_t)s_cs_d2 & 0x0FU) - 16;
}

int16_t hw_find_wall(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, int16_t a3, uint8_t *a4) {
    int8_t d0;
    uint8_t lo = (uint8_t)d3 & 0x0FU;
    cs_setup(solid_bit, d6, a4);
    s_cs_d2 = d2; s_cs_d3 = d3;
    if (!sample_col_height_g(1) || (d0 = s_cs_d0) == 0) goto next;
    if (d0 < 0) {
        if ((int8_t)(lo + d0) >= 0) goto next;
        goto back;
    }
    if (d0 == 16) goto back;
    return (int16_t)(int8_t)(0x0F - (int8_t)(lo + d0));
next:
    s_cs_d3 = d3 + a3;
    return (int16_t)find2_s1(1, (uint8_t)s_cs_d3 & 0x0FU) + 16;
back:
    s_cs_d3 = d3 - a3;
    return (int16_t)find2_s1(1, (uint8_t)s_cs_d3 & 0x0FU) - 16;
}

int16_t hw_floor_distance(int16_t px, int16_t py, uint8_t *out_angle) {
    uint8_t ang = 3;
    int16_t d = hw_find_floor(py, px, 13, 0, 16, &ang);
    if (out_angle) *out_angle = (ang & 1) ? 0 : ang;
    return d;
}

int16_t hw_ceil_surface_distance(int16_t px, int16_t py, uint8_t *out_angle) {
    return hw_find_floor(py ^ 0x0F, px, 13, 0x1000, -16, out_angle);
}

int16_t hw_rwall_distance(int16_t px, int16_t py, uint8_t *out_angle) {
    return hw_find_wall(py, px, 13, 0, 16, out_angle);
}

int16_t hw_lwall_distance(int16_t px, int16_t py, uint8_t *out_angle) {
    return hw_find_wall(py, px ^ 0x0F, 13, 0x0800, -16, out_angle);
}

int16_t hw_ceil_distance(int16_t px, int16_t py) {
    uint8_t dummy = 3;
    return hw_find_floor(py ^ 0x0F, px, 14, 0x1000, -16, &dummy);
}

int16_t hw_wall_distance(int16_t px, int16_t py, int8_t dir) {
    uint8_t dummy = 3;
    return (dir > 0)
        ? hw_find_wall(py, px, 14, 0, 16, &dummy)
        : hw_find_wall(py, px ^ 0x0F, 14, 0x0800, -16, &dummy);
}

static uint8_t s_dplc_raw[528];
static uint8_t s_dplc_pending_cnt;
static uint8_t s_dplc_pending_dst;
static const uint8_t s_scache_base[2] = { 0, 16 };
static uint8_t s_scache_rr = 0;
const uint8_t *g_ss_dma_ptr;

static void gdma_transfer(uint16_t src, uint8_t dst_hi, uint8_t blocks_m1) {
    HDMA1_REG = (uint8_t)(src >> 8);
    HDMA2_REG = (uint8_t)src;
    HDMA3_REG = dst_hi;
    HDMA4_REG = 0x00;
    HDMA5_REG = blocks_m1;
}

/* Upload the 16 animated ring-quadrant BG tiles (`BKG_RING_TILE_BASE`.., VRAM bank 1, 256 bytes).
   Called right after VBlank starts, so a single general-purpose DMA (16-byte blocks, CPU halted ~1/20th of the
   time a STAT-polling `set_bkg_data` needs) replaces ~21k cycles of byte-by-byte copying. The source table is
   16-byte aligned in bank 4; fall back to `set_bkg_data` if that ever stops being true. */
void hw_upload_ring_frame(uint8_t frame) {
    uint8_t prev = CURRENT_BANK;
    const uint8_t *src = ring_bg_tiles + ((uint16_t)frame << 8);
    SWITCH_ROM(4);
    VBK_REG = 1;
    if (((uint16_t)src & 0x0FU) == 0U && BKG_RING_TILE_BASE >= 128U) {
        gdma_transfer((uint16_t)src, (uint8_t)(0x88U + ((BKG_RING_TILE_BASE - 128U) >> 4)), 15U);
    } else {
        set_bkg_data(BKG_RING_TILE_BASE, 16, src);
    }
    VBK_REG = 0;
    SWITCH_ROM(prev);
}

uint8_t g_hud_pend = 0;
static uint8_t s_ring_upload_ok = 0;

/* Write the pending HUD rows exactly as print_win_ram_b127 would (per row: 20 attrs 0x0F in VBK 1, then 20 tiles
   in VBK 0, each write after STAT leaves mode 2/3). Leaves VBK = 0. */
static void hud_write_rows(void) __naked {
    __asm
    ld   de, #0xB240
    ld   hl, #0x9C00
    call 801$
    ld   hl, #0x9C20
    call 801$
    ret
801$:
    ld   a, #1
    ldh  (0x4F), a
    push hl
    ld   b, #20
802$:
    ldh  a, (0x41)
    and  a, #0x02
    jr   nz, 802$
    ld   a, #0x0F
    ld   (hl+), a
    dec  b
    jr   nz, 802$
    pop  hl
    xor  a, a
    ldh  (0x4F), a
    ld   b, #20
803$:
    ldh  a, (0x41)
    and  a, #0x02
    jr   nz, 803$
    ld   a, (de)
    inc  de
    ld   (hl+), a
    dec  b
    jr   nz, 803$
    ret
    __endasm;
}

void hud_flush_pending(void) {
    if (!g_hud_pend) return;
    g_hud_pend = 0;
    hud_write_rows();
}

static void flush_vblank_hdma(void) {
    /* VBlank is only ~10 scanlines (~1.1 ms) long and VRAM writes made while the PPU is drawing (mode 3) are
       silently dropped on real hardware. Anything that cannot wait for a safe STAT mode, i.e. the general-purpose
       DMAs, therefore goes FIRST while the whole VBlank is still ahead; the CPU-driven spans / HUD rows below
       wait for STAT mode 0/1 before every byte, so they stay correct even if they run past the end of VBlank. */
    if (LCDC_REG & LCDCF_ON) {
        /* The VBlank interrupt chain (music tick) occasionally runs long enough that vsync() returns after VBlank
           is already over (LY < 144) or nearly over. A DMA started there would be dropped by the PPU, leaving
           stale Sonic frames / ring tiles. Sit out one more frame instead of corrupting this one. */
        uint8_t ly = LY_REG;
        if (ly < 144U || ly > 148U) vsync();
    }
    if (s_dplc_pending_cnt) {
        uint16_t ssrc = ((uint16_t)s_dplc_raw + 15U) & 0xFFF0U;
        uint16_t vdst = 0x8000U + ((uint16_t)s_dplc_pending_dst << 4);
        VBK_REG = 0;
        HDMA1_REG = (uint8_t)(ssrc >> 8);
        HDMA2_REG = (uint8_t)ssrc;
        HDMA3_REG = (uint8_t)(vdst >> 8);
        HDMA4_REG = (uint8_t)vdst;
        HDMA5_REG = s_dplc_pending_cnt - 1U;
        s_dplc_pending_cnt = 0;
    }
    if (g_ss_dma_ptr) {
        uint16_t s = (uint16_t)g_ss_dma_ptr;
        g_ss_dma_ptr = 0;
        VBK_REG = 0;
        gdma_transfer(s, 0x98, 31U);
        VBK_REG = 1;
        gdma_transfer(s + 512U, 0x98, 31U);
        VBK_REG = 0;
    }
    /* Animated ring tiles (general-purpose DMA, VBK 1). */
    if (s_ring_upload_ok && g_game_mode == GM_LEVEL && !g_paused) {
        uint8_t rf = (uint8_t)((g_frame_counter >> 3) & 3U);
        if (rf != g_ring_last_frame) {
            hw_upload_ring_frame(rf);
            g_ring_last_frame = rf;
        }
    }
    if (s_vscroll_cnt) {
        flush_vscroll_b127(s_vscroll_cnt);
        s_vscroll_cnt = 0;
    }
    if (g_hud_pend) hud_flush_pending();
}

/* Copy `cnt` 16-byte Sonic tiles listed in `st` (tile < 768: bank 6 part0, else bank 7 part1 at tile - 768) to
   s_dp_dst. Switches ROM banks (keeping __current_bank in sync); the caller restores its bank.
   sdcccall(1): st in DE, cnt in A. */
static uint8_t *s_dp_dst;
static uint8_t s_dp_cnt;
static void dplc_copy(const uint16_t *st, uint8_t cnt) __naked {
    (void)st; (void)cnt;
    __asm
    or   a, a
    ret  z
    ld   (_s_dp_cnt), a
    ld   hl, #_s_dp_dst
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    push hl
1601$:
    ld   a, (de)
    inc  de
    ld   l, a
    ld   a, (de)
    inc  de
    ld   h, a
    push de
    ld   a, h
    cp   a, #3
    jr   nc, 1602$
    ld   a, #6
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    ld   de, #_sonic_tiles_part0
    add  hl, de
    jr   1603$
1602$:
    ld   a, #7
    ldh  (__current_bank + 0), a
    ld   (#0x2000), a
    ld   de, #0xFD00
    add  hl, de
    add  hl, hl
    add  hl, hl
    add  hl, hl
    add  hl, hl
    ld   de, #_sonic_tiles_part1
    add  hl, de
1603$:
    pop  bc
    pop  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    ld   a, (hl+)
    ld   (de), a
    inc  de
    push de
    ld   d, b
    ld   e, c
    ld   a, (_s_dp_cnt)
    dec  a
    ld   (_s_dp_cnt), a
    jp   nz, 1601$
    pop  de
    ret
    __endasm;
}

static void ensure_sonic_dplc_loaded(uint8_t frame) {
    uint8_t prev, cnt, slot;
    uint16_t off;
    uint16_t st_list[32];
    uint8_t *dst;
    if (frame >= 89) frame = 1;

    for (slot = 0; slot < 2U; slot++) {
        if (s_scache_frame[slot] == frame) {
            s_cur_sonic_base = s_scache_base[slot];
            return;
        }
    }

    prev = CURRENT_BANK;
    SWITCH_ROM(9);
    off = sonic_dplc_offsets[frame];
    cnt = sonic_dplc_blob[off];
    if (cnt > 32) cnt = 32;
    if (cnt == 0) { SWITCH_ROM(prev); return; }
    memcpy(st_list, &sonic_dplc_blob[off + 1], (uint16_t)cnt << 1);

    /* Non-overlapping 2-slot cache in `VBK=0` (`0..15` & `16..31`) — NEVER touches `32..127` or `VBK=1`! */
    if (cnt <= 16U) {
        slot = s_scache_rr & 1U;
        s_scache_rr = slot ^ 1U;
        s_scache_frame[slot] = frame;
        s_cur_sonic_base = s_scache_base[slot];
    } else {
        s_scache_frame[0] = frame;
        s_scache_frame[1] = 0xFF;
        s_cur_sonic_base = 0;
    }

    dst = (uint8_t *)(((uint16_t)s_dplc_raw + 15U) & 0xFFF0U);
    s_dp_dst = dst;
    dplc_copy(st_list, cnt);
    s_dplc_pending_dst = s_cur_sonic_base;
    s_dplc_pending_cnt = cnt;
    SWITCH_ROM(prev);
}

static uint8_t s_sonic_rot_fx = 0;
static uint8_t s_sonic_rot_fy = 0;

void hw_step_sonic_anim(void) {
    uint8_t prev = CURRENT_BANK;
    uint16_t base;
    uint8_t spd, cmd;

    if (g_sonic.anim >= 32) g_sonic.anim = ANI_WAIT;
    if (g_sonic.anim != g_sonic.prev_anim) {
        g_sonic.prev_anim = g_sonic.anim;
        g_sonic.anim_idx = 0;
        g_sonic.anim_timer = 0;
    }

    (void)sonic_anim_oct_b127(&s_sonic_rot_fx, &s_sonic_rot_fy, 0, 0);

    if (g_sonic.anim_timer > 0) {
        g_sonic.anim_timer--;
        return;
    }

    SWITCH_ROM(9);
    base = sonic_ani_offsets[g_sonic.anim];
    spd = sonic_ani_blob[base];
    if (g_sonic.anim == ANI_WALK || g_sonic.anim == ANI_RUN || g_sonic.anim == ANI_ROLL) {
        int16_t aspd = g_sonic.gsp < 0 ? -g_sonic.gsp : g_sonic.gsp;
        int16_t calc = 8 - (aspd >> 8);
        spd = (calc < 1) ? 1 : (uint8_t)calc;
    } else if (g_sonic.anim == ANI_WAIT && spd > 20) {
        spd = 6;
    }

    cmd = sonic_ani_blob[base + 1 + g_sonic.anim_idx];
    if (cmd >= 0xFC) {
        if (cmd == 0xFF) {
            g_sonic.anim_idx = 0;
            cmd = sonic_ani_blob[base + 1];
        } else if (cmd == 0xFE) {
            uint8_t back = sonic_ani_blob[base + 2 + g_sonic.anim_idx];
            if (g_sonic.anim_idx >= back) g_sonic.anim_idx -= back;
            else g_sonic.anim_idx = 0;
            cmd = sonic_ani_blob[base + 1 + g_sonic.anim_idx];
        } else if (cmd == 0xFD) {
            g_sonic.anim = sonic_ani_blob[base + 2 + g_sonic.anim_idx] & 0x1F;
            g_sonic.prev_anim = g_sonic.anim;
            g_sonic.anim_idx = 0;
            base = sonic_ani_offsets[g_sonic.anim];
            cmd = sonic_ani_blob[base + 1];
        } else {
            cmd = g_sonic.frame;
        }
    }
    SWITCH_ROM(prev);
    if (cmd > 0 && cmd < 89) {
        g_sonic.frame = sonic_anim_oct_b127(&s_sonic_rot_fx, &s_sonic_rot_fy, cmd, 1);
        g_sonic.anim_idx++;
    }
    g_sonic.anim_timer = spd;
}

/* Fast emitter for fully visible Sonic pieces (see hw_draw_sonic_sprite): copies `cnt` 4-byte pieces from
   `s_fe_src` to shadow OAM at `s_fe_dst`, then rewrites Y/X (`b + (d ^ m)`) and tile/attr in place. shadow_OAM is
   256-byte aligned and 160 bytes long, so the end test only compares the low address byte. */
static const uint8_t *s_fe_src;
static uint8_t *s_fe_dst;
static uint8_t s_fe_cnt, s_fe_end, s_fe_my, s_fe_by, s_fe_mx, s_fe_bx, s_fe_base, s_fe_xa, s_fe_oa;
static void sonic_emit_fast(void) __naked {
    __asm
    ld   a, (_s_fe_cnt)
    or   a, a
    ret  z
    add  a, a
    add  a, a
    ld   b, a
    ld   hl, #_s_fe_dst
    ld   a, (hl+)
    ld   e, a
    ld   d, (hl)
    ld   hl, #_s_fe_src
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
201$:
    ld   a, (hl+)
    ld   (de), a
    inc  de
    dec  b
    jr   nz, 201$
    ld   a, (_s_fe_my)
    ld   c, a
    ld   a, (_s_fe_by)
    ld   b, a
    ld   a, (_s_fe_mx)
    ld   e, a
    ld   a, (_s_fe_bx)
    ld   d, a
    ld   hl, #_s_fe_dst
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
202$:
    ld   a, (hl)
    xor  a, c
    add  a, b
    ld   (hl+), a
    ld   a, (hl)
    xor  a, e
    add  a, d
    ld   (hl+), a
    inc  hl
    inc  hl
    ld   a, (_s_fe_end)
    cp   a, l
    jr   nz, 202$
    ld   a, (_s_fe_base)
    ld   b, a
    ld   a, (_s_fe_xa)
    ld   c, a
    ld   a, (_s_fe_oa)
    ld   d, a
    ld   hl, #_s_fe_dst
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
203$:
    inc  hl
    inc  hl
    ld   a, (hl)
    add  a, b
    ld   (hl+), a
    ld   a, (hl)
    xor  a, c
    or   a, d
    ld   (hl+), a
    ld   a, (_s_fe_end)
    cp   a, l
    jr   nz, 203$
    ret
    __endasm;
}

void hw_draw_sonic_sprite(void) {
    uint8_t prev, cnt, i;
    uint16_t off;
    int16_t scr_x = (g_sonic.x - g_cam_x) >> 1;
    int16_t scr_y = (g_sonic.y - g_cam_y) >> 1;

    if (g_sonic.invuln > 0 && (g_frame_counter & 2)) {
        return; /* Classic invulnerability blink */
    }
    ensure_sonic_dplc_loaded(g_sonic.frame);

    prev = CURRENT_BANK;
    SWITCH_ROM(9);
    off = sonic_spr_offsets[g_sonic.frame < 89 ? g_sonic.frame : 1];
    cnt = sonic_spr_blob[off];
    {
        const uint8_t *q = &sonic_spr_blob[off + 1U];
        uint8_t n = g_oam_next;
        uint8_t *o = (uint8_t *)shadow_OAM + ((uint16_t)n << 2);
        uint8_t rfx = s_sonic_rot_fx, rfy = s_sonic_rot_fy;
        uint8_t xor_attr = (uint8_t)((rfx ? 0x20U : 0U) ^ (rfy ? 0x40U : 0U));
        uint8_t or_attr = g_sonic.loop_plane ? 0x80U : 0U;
        uint8_t base = s_cur_sonic_base;
        /* Piece offsets in `sonic_spr_blob` are within dx,dy in [-16, 8] (both orientations land in
           [scr-16, scr+8]), so with Sonic this far inside the screen every piece is visible: emit them with
           pure 8-bit math. Flips use -d == ~d + 1. Otherwise fall back to the per-piece clipped loop. */
        if ((uint16_t)(scr_x - 9) <= 142U && (uint16_t)(scr_y - 9) <= 110U) {
            uint8_t room = (uint8_t)(40U - n);
            if (n >= 40U) room = 0;
            if (cnt > room) cnt = room;
            s_fe_src = q;
            s_fe_dst = o;
            s_fe_cnt = cnt;
            s_fe_end = (uint8_t)((uint8_t)(uint16_t)o + (uint8_t)(cnt << 2));
            s_fe_my = rfy ? 0xFFU : 0U;
            s_fe_by = (uint8_t)((uint8_t)scr_y + (rfy ? 9U : 16U));
            s_fe_mx = rfx ? 0xFFU : 0U;
            s_fe_bx = (uint8_t)((uint8_t)scr_x + (rfx ? 1U : 8U));
            s_fe_base = base;
            s_fe_xa = xor_attr;
            s_fe_oa = or_attr;
            sonic_emit_fast();
            n = (uint8_t)(n + cnt);
            cnt = 0;
        }
        for (i = 0; i < cnt && n < 40U; i++, q += 4) {
            int8_t dy = (int8_t)q[0];
            int8_t dx = (int8_t)q[1];
            int16_t fx = rfx ? (int16_t)(scr_x - dx - 8) : (int16_t)(scr_x + dx);
            int16_t fy = rfy ? (int16_t)(scr_y - dy - 8) : (int16_t)(scr_y + dy);
            if ((uint16_t)(fx + 7) > 166U || (uint16_t)(fy + 7) > 134U) continue;
            *o++ = (uint8_t)((uint8_t)fy + 16U);
            *o++ = (uint8_t)((uint8_t)fx + 8U);
            *o++ = (uint8_t)(q[2] + base);
            *o++ = (uint8_t)((q[3] ^ xor_attr) | or_attr);
            n++;
        }
        g_oam_next = n;
    }
    SWITCH_ROM(prev);

    /* Shield / Invincibility Sparkle Halo around half-scale Sonic */
    if (g_sonic.invinc > 0) {
        uint8_t ang = (uint8_t)(g_frame_counter << 3);
        int16_t ox = (hw_cos(ang) * 8) >> 8;
        int16_t oy = (hw_sin(ang) * 8) >> 8;
        hw_put_sprite8(scr_x + ox - 4, scr_y + oy - 4, SPR_TILE_STARS + ((g_frame_counter >> 2) & 3), 2);
        hw_put_sprite8(scr_x - ox - 4, scr_y - oy - 4, SPR_TILE_STARS + (((g_frame_counter >> 2) + 2) & 3), 2);
    } else if (g_sonic.shield) {
        if (g_frame_counter & 1) {
            hw_put_sprite16(scr_x, scr_y, SPR_TILE_SHIELD, 4);
        }
    }
}

void hw_init_osc_from_rom(uint16_t *dst16) {
    uint8_t prev = CURRENT_BANK;
    SWITCH_ROM(SUPPLEMENT_ANIM_BANK);
    memcpy(dst16, s1_osc_init_values, 32);
    SWITCH_ROM(prev);
}

/* `_inc/AnimateLevelGfx.asm`: Stream Half-Scale Animated Zone Tiles During VBlank! */
void hw_animate_level_gfx(void) {
    uint8_t prev;
    uint8_t vset;
    uint8_t phase;

    if (g_game_mode == GM_TITLE) {
        /* Authentic `PalCycle_Title`: Cycle Title Screen Water Palette (`s1_title_water_cycle`) */
        if ((g_frame_counter & 7) == 0) {
            prev = CURRENT_BANK;
            SWITCH_ROM(SUPPLEMENT_ANIM_BANK);
            phase = ((g_frame_counter >> 3) & 3) << 2;
            set_bkg_palette(2, 1, &s1_title_water_cycle[phase]);
            g_pal_cycles++;
            SWITCH_ROM(prev);
        }
        return;
    }
    if ((g_game_mode != GM_LEVEL && g_game_mode != GM_ACT_CLEAR && g_game_mode != GM_ENDING) || WY_REG < 128) {
        return;
    }
    if ((g_frame_counter & 7) != 0) return;

    prev = CURRENT_BANK;
    vset = g_ram_act.vset_idx;
    phase = (g_frame_counter >> 3);
    /* Stream ONLY into dedicated unused VRAM Bank 1 slot `83` (`80..82` = solid UI, `84..127` = HUD font,
       `0..79` & `128..255` = live Zone terrain — ZERO terrain or HUD tiles are ever overwritten!) */
    SWITCH_ROM(SUPPLEMENT_ANIM_BANK);
    VBK_REG = 1;
    if (vset == 0) {
        uint16_t w_off = ((uint16_t)((phase & 1) * 2)) << 4;
        set_bkg_data(83, 1, &anim_ghz_waterfall[w_off]);
        g_anim_tile_uploads++;
    } else if (vset == 2) {
        uint16_t l_off = ((uint16_t)((phase % 3) * 4)) << 4;
        set_bkg_data(83, 1, &anim_mz_lava[l_off]);
        g_anim_tile_uploads++;
    } else if (vset == 5 || vset == 6) {
        uint16_t s_off = ((uint16_t)((phase & 3) * 2)) << 4;
        set_bkg_data(83, 1, &anim_sbz_smoke[s_off]);
        g_anim_tile_uploads++;
    } else if (vset == 8) {
        uint16_t e_off = ((uint16_t)((phase & 3) * 4)) << 4;
        set_bkg_data(83, 1, &anim_end_flower[e_off]);
        g_anim_tile_uploads++;
    }
    VBK_REG = 0;
    SWITCH_ROM(prev);
}


static int16_t s_last_scan_cam_x = -32767;
static uint8_t s_last_scan_tag = 0xFF;

extern const uint8_t rev00_ghz3_objs[];
extern const uint8_t rev00_lz1_objs[];

/* Spawn scan resumes from a cursor: entries [0, s_scan_skip) are known to be non-terminator entries with
   x < s_scan_skip_min, so with min_x >= s_scan_skip_min the original loop would skip them without effect.
   (A smaller min_x just rescans from the start; the result is identical to the original full scan.) */
static uint16_t s_scan_skip = 0;
static int16_t s_scan_skip_min = -32767;

/* Spawn-list walker for hw_scan_spawn_objects (hand-written). From (s_sc_p, s_sc_idx) it applies the scan rules
   (idx < 512, 0xFFFF terminator, x > max_x stops, x < min_x skipped and extends the s_scan_skip prefix, the taken
   bit and the spawn-hint "already spawned" test) and stops at the next entry that needs spawn_try_place_b127,
   returning 1, or returns 0 at the end. min/max are stored sign-biased (^0x8000) for unsigned compares. */
static const uint8_t *s_sc_p;
static uint16_t s_sc_idx, s_sc_minb, s_sc_maxb;
static uint8_t s_sc_prefix;
typedef char spawn_layout_check[(sizeof(ActiveObj) == 20U && offsetof(ActiveObj, spawn_slot) == 3U &&
                                 offsetof(ActiveObj, base_x) == 9U && MAX_ACTIVE_OBJS == 24) ? 1 : -1];
static uint8_t scan_next(void) __naked {
    __asm
    ld   hl, #_s_sc_p
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
    ld   a, (_s_sc_idx)
    ld   c, a
    ld   a, (_s_sc_idx + 1)
    ld   b, a
1001$:
    ld   a, b
    cp   a, #2
    jp   nc, 1090$
    ld   a, (hl+)
    ld   e, a
    ld   a, (hl-)
    ld   d, a
    and  a, e
    inc  a
    jp   z, 1090$
    ld   a, d
    xor  a, #0x80
    ld   d, a
    push hl
    ld   hl, #_s_sc_maxb
    ld   a, (hl+)
    sub  a, e
    ld   a, (hl)
    sbc  a, d
    pop  hl
    jp   c, 1090$
    push hl
    ld   hl, #_s_sc_minb
    ld   a, e
    sub  a, (hl)
    inc  hl
    ld   a, d
    sbc  a, (hl)
    pop  hl
    jr   nc, 1010$
    ld   a, (_s_sc_prefix)
    or   a, a
    jp   z, 1080$
    ld   a, c
    add  a, #1
    ld   (_s_scan_skip), a
    ld   a, b
    adc  a, #0
    ld   (_s_scan_skip + 1), a
    jp   1080$
1010$:
    xor  a, a
    ld   (_s_sc_prefix), a
    push hl
    ld   de, #4
    add  hl, de
    ld   a, (hl)
    pop  hl
    cp   a, #0x0D
    jr   z, 1020$
    cp   a, #0x3E
    jr   z, 1020$
    cp   a, #0x4B
    jr   z, 1020$
    push hl
    ld   e, c
    ld   d, b
    srl  d
    rr   e
    srl  d
    rr   e
    srl  d
    rr   e
    ld   hl, #_g_obj_taken
    add  hl, de
    ld   a, c
    and  a, #7
    ld   e, a
    ld   a, #1
    inc  e
1011$:
    dec  e
    jr   z, 1012$
    add  a, a
    jr   1011$
1012$:
    and  a, (hl)
    pop  hl
    jr   nz, 1080$
1020$:
    push hl
    ld   hl, #0xA800
    add  hl, bc
    ld   a, (hl)
    cp   a, #24
    jr   nc, 1070$
    ld   l, a
    ld   h, #0
    add  hl, hl
    add  hl, hl
    ld   e, l
    ld   d, h
    add  hl, hl
    add  hl, hl
    add  hl, de
    ld   de, #_g_objs
    add  hl, de
    ld   a, (hl)
    or   a, a
    jr   z, 1070$
    inc  hl
    inc  hl
    inc  hl
    ld   a, (hl+)
    cp   a, c
    jr   nz, 1070$
    ld   a, (hl)
    cp   a, b
    jr   nz, 1070$
    ld   de, #5
    add  hl, de
    pop  de
    push de
    ld   a, (de)
    cp   a, (hl)
    jr   nz, 1070$
    inc  de
    inc  hl
    ld   a, (de)
    cp   a, (hl)
    jr   nz, 1070$
    pop  hl
    jr   1080$
1070$:
    pop  hl
    ld   a, l
    ld   (_s_sc_p), a
    ld   a, h
    ld   (_s_sc_p + 1), a
    ld   a, c
    ld   (_s_sc_idx), a
    ld   a, b
    ld   (_s_sc_idx + 1), a
    ld   a, #1
    ret
1080$:
    ld   de, #6
    add  hl, de
    inc  bc
    jp   1001$
1090$:
    xor  a, a
    ret
    __endasm;
}

void hw_scan_spawn_objects(void) {
    uint8_t prev;
    const uint8_t *p;
    uint16_t idx = 0;
    int16_t min_x = g_cam_x - 96;
    int16_t max_x = g_cam_x + 464;
    uint8_t scan_tag = (uint8_t)((g_cur_act << 1) | (g_rev_mode & 1));
    int16_t dcam = g_cam_x - s_last_scan_cam_x;
    uint8_t prefix = 1;

    if (!s_scan_force && scan_tag == s_last_scan_tag && dcam > -8 && dcam < 8) return;
    if (s_scan_force || scan_tag != s_last_scan_tag || min_x < s_scan_skip_min) s_scan_skip = 0;
    s_scan_force = 0;
    s_last_scan_tag = scan_tag;
    s_last_scan_cam_x = g_cam_x;
    prev = CURRENT_BANK;
    if (g_rev_mode && (g_cur_act == 2 || g_cur_act == 9)) {
        SWITCH_ROM(SUPPLEMENT_REV00_BANK);
        p = (g_cur_act == 2) ? rev00_ghz3_objs : rev00_lz1_objs;
    } else {
        SWITCH_ROM(g_ram_act.data_bank);
        p = g_ram_act.objects;
    }
    idx = s_scan_skip;
    s_sc_p = p + idx * 6U;
    s_sc_idx = idx;
    s_sc_minb = (uint16_t)min_x ^ 0x8000U;
    s_sc_maxb = (uint16_t)max_x ^ 0x8000U;
    s_sc_prefix = prefix;
    while (scan_next()) {
        const uint8_t *q = s_sc_p;
        spawn_try_place_b127(s_sc_idx, (int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8)),
                             (int16_t)((uint16_t)q[2] | ((uint16_t)q[3] << 8)), q[4], q[5]);
        s_sc_p = q + 6;
        s_sc_idx++;
    }
    s_scan_skip_min = min_x;
    SWITCH_ROM(prev);
}

void main(void) {
    /* Activate 8.388 MHz Game Boy Color Double-Speed mode immediately! */
    if (_cpu == CGB_TYPE) {
        cpu_fast();
    }

    /* Enable 4-channel CGB APU Audio */
    NR52_REG = 0x80;
    NR50_REG = 0x77;
    NR51_REG = 0xFF;
    hw_install_bass_wave();

    /* Preload HUD/Font tiles (Bank 1 BG 80..127) + Common Sprites (Bank 0 OBJ 32..127) */
    reload_common_hud_and_sprites();

    /* Load Battery-Backed MBC5 SRAM Save State & keep MBC5 fast RAM (`$A200..$B9FF` caches) enabled! */
    sram_load_or_init();
    ENABLE_RAM;
    rRAMB = 1;

    /* Load authentic SEGA Boot Screen BEFORE enabling LCD so Frame 1 is IMMEDIATELY crisp multi-color SEGA! */
    g_game_mode = GM_SEGA;
    g_next_mode = GM_SEGA;
    g_mode_init_done = 0;
    hw_load_fullscreen_screen(0);

    /* Turn LCD ON ONCE with BG ($8800, $9800) + Window ($9C00) + Sprites ON — NEVER turned off again! */
    LCDC_REG = LCDCF_ON | LCDCF_BG8800 | LCDCF_BG9800 | LCDCF_BGON | LCDCF_WIN9C00 | LCDCF_WINON | LCDCF_OBJ8 | LCDCF_OBJON;

    /* Drive 15 kHz SEGA digital voice (`TIM_IFLAG`) + 60 Hz SMPS music/SFX (`VBL_IFLAG`) from hardware interrupts! */
    CRITICAL {
        add_VBL(hw_sound_isr);
        add_TIM(hw_sega_pcm_tim_isr);
    }
    set_interrupts(VBL_IFLAG | TIM_IFLAG);
    hw_start_sega_chant();

    while (1) {
        uint8_t latched = 0;
        if (g_game_mode == GM_LEVEL && !g_paused) {
            __critical {
                latched = s_vbl_joy_latch;
                s_vbl_joy_latch = 0;
            }
        } else {
            s_vbl_joy_latch = 0;
        }
        g_joy_prev = g_joy;
        g_joy = joypad() | latched;
        g_joy_pressed = (g_joy ^ g_joy_prev) & g_joy;

        if (g_next_mode != g_game_mode) {
            g_game_mode = g_next_mode;
            g_mode_init_done = 0;
        }
        if (!g_mode_init_done) {
            g_mode_init_done = 1;
            screens_init_mode(g_game_mode);
            g_joy = joypad();
            g_joy_prev = g_joy;
            g_joy_pressed = 0;
            g_frame_counter++;
            vsync();
            flush_vblank_hdma();
            continue;
        }

        screens_update_mode(g_game_mode);

        g_frame_counter++;
        vsync();
        s_ring_upload_ok = 1;   /* ring art follows the ring map flush below, as before (not on mode-init passes) */
        flush_vblank_hdma();
        s_ring_upload_ok = 0;
        if (g_game_mode == GM_LEVEL && !g_paused) hw_flush_bkg_rings();
        hw_animate_level_gfx();
#if SONIC_GAMEPLAY_30FPS
        /* Experimental: pace gameplay at 30 Hz; VBlank audio and timers remain at 60 Hz. */
        if (g_game_mode == GM_LEVEL) vsync();
#endif
    }
}
