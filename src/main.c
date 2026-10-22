#include "engine.h"
#include <string.h>

/* Global Engine State in WRAM Bank 0 */
volatile uint8_t g_game_mode = GM_SEGA;
volatile uint8_t g_next_mode = GM_SEGA;
volatile uint8_t g_mode_init_done = 0;
volatile uint16_t g_frame_counter = 0;
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
static uint8_t s_underwater_pal_active = 0;

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
    /* OBP0: Sonic Royal Cobalt Blue + Peach Skin / White Glove */
    0x0000, RGB8(16, 16, 56),   RGB8(36, 72, 248),  RGB8(252, 228, 196),
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

void hw_sound_isr(void) {
    g_sound_isr_ticks++;
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

void hw_play_sega_chant_step(uint16_t pkt_idx) {
    uint8_t prev;
    uint8_t i;
    const uint8_t *p;
    if (pkt_idx >= PCM_SEGA_PACKETS) return;
    s_audio_lock = 1;
    prev = CURRENT_BANK;
    SWITCH_ROM(PCM_SEGA_BANK);
    p = &sega_wave_pcm[pkt_idx << 4];
    NR30_REG = 0x00;
    for (i = 0; i < 16; i++) {
        ((volatile uint8_t *)0xFF30)[i] = p[i];
    }
    NR30_REG = 0x80;
    NR32_REG = 0x20; /* 100% output level */
    /* Play 32 samples at ~1920 Hz (1 packet per 60Hz frame -> period = 2048 - 65536/60 = 956 -> seamless voice!) */
    NR33_REG = (uint8_t)(1994 & 0xFF);
    NR34_REG = 0x80 | ((1994 >> 8) & 0x07);
    g_sound_events++;
    SWITCH_ROM(prev);
    s_audio_lock = 0;
}

/* Convert ASCII character to Bank 1 HUD/Font tile index (80..127) */
static uint8_t ascii_to_font_tile(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(84 + (c - '0'));
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'X') return (uint8_t)(96 + (c - 'A'));
    if (c == 'Y') return 94;
    if (c == 'Z') return 95;
    if (c == ':' || c == '-' || c == '/') return 120;
    if (c == '*') return 121; /* Gold Ring icon */
    if (c == '@') return 122; /* Sonic Life L */
    if (c == '#') return 123; /* Sonic Life R */
    if (c == '$') return 124; /* Chaos Emerald */
    if (c == '>') return 125; /* Cursor '>' */
    if (c == '%') return 126; /* Checker border */
    if (c == '!') return 127; /* Stripe accent */
    return 80; /* Space (solid navy backdrop slot 0) */
}

void hw_fill_win_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t fill_tile) {
    uint8_t row_t[20];
    uint8_t row_a[20];
    uint8_t i, ry;
    if (w > 20) w = 20;
    for (i = 0; i < w; i++) {
        row_t[i] = fill_tile;
        row_a[i] = 0x08 | 7; /* VRAM Bank 1 + BGP7 */
    }
    for (ry = 0; ry < h; ry++) {
        VBK_REG = 1;
        set_win_tiles(x, y + ry, w, 1, row_a);
        VBK_REG = 0;
        set_win_tiles(x, y + ry, w, 1, row_t);
    }
}

void hw_print_win(uint8_t x, uint8_t y, const char *str) {
    uint8_t row_t[20];
    uint8_t row_a[20];
    uint8_t n = 0;
    while (str[n] != '\0' && (uint8_t)(x + n) < 20) {
        row_t[n] = ascii_to_font_tile(str[n]);
        row_a[n] = 0x08 | 7;
        n++;
    }
    if (n > 0) {
        VBK_REG = 1;
        set_win_tiles(x, y, n, 1, row_a);
        VBK_REG = 0;
        set_win_tiles(x, y, n, 1, row_t);
    }
}

void hw_clear_remaining_oam(void) {
    uint8_t i;
    for (i = g_oam_next; i < 40; i++) {
        move_sprite(i, 0, 0);
    }
}

void hw_put_sprite8(int16_t sx, int16_t sy, uint8_t tile, uint8_t attr) {
    if (g_oam_next >= 40) return;
    if (sx <= -8 || sx >= 160 || sy <= -8 || sy >= 128) return;
    set_sprite_tile(g_oam_next, tile);
    set_sprite_prop(g_oam_next, attr);
    move_sprite(g_oam_next, (uint8_t)(sx + 8), (uint8_t)(sy + 16));
    g_oam_next++;
}

void hw_put_sprite_cols2rows(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t ncols, uint8_t attr) {
    uint8_t cx, src_cx, t_top, t_bot;
    for (cx = 0; cx < ncols; cx++) {
        src_cx = (attr & 0x20) ? (uint8_t)(ncols - 1u - cx) : cx;
        t_top = (uint8_t)(base_tile + (uint8_t)(src_cx << 1));
        t_bot = (uint8_t)(t_top + 1u);
        if (attr & 0x40) {
            hw_put_sprite8((int16_t)(sx + (int16_t)(cx << 3)), sy, t_bot, attr);
            hw_put_sprite8((int16_t)(sx + (int16_t)(cx << 3)), (int16_t)(sy + 8), t_top, attr);
        } else {
            hw_put_sprite8((int16_t)(sx + (int16_t)(cx << 3)), sy, t_top, attr);
            hw_put_sprite8((int16_t)(sx + (int16_t)(cx << 3)), (int16_t)(sy + 8), t_bot, attr);
        }
    }
}

void hw_put_sprite16(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t attr) {
    /* Full `0.5x` half-scale (`32x32 MD -> 16x16 GBC`) column-major `2x2` sprite renderer with seamless H/V-flip! */
    hw_put_sprite_cols2rows(sx - 8, sy - 8, base_tile, 2u, attr);
}

void hw_show_transition_card(const char *line1, const char *line2, uint8_t act_num) {
    char act_buf[8];
    uint8_t prev = CURRENT_BANK;
    g_oam_next = 0;
    hw_clear_remaining_oam();

    /* Ensure HUD & Title Card font/border tiles (`80..127` in `VBK=1`) are always 100% pristine! */
    SWITCH_ROM(4);
    VBK_REG = 1;
    set_bkg_data(80, 48, hud_font_tiles);
    SWITCH_ROM(prev);

    /* Paint iconic Sonic Navy + Crimson Ribbon + Gold Stripe Title Card in Window Layer ($9C00) */
    hw_fill_win_rect(0, 0, 20, 18, 80);  /* Rich Sonic Navy */
    hw_fill_win_rect(0, 0, 20, 1, 126);  /* Top Checker Border */
    hw_fill_win_rect(0, 17, 20, 1, 126); /* Bottom Checker Border */
    hw_fill_win_rect(1, 5, 18, 6, 81);   /* Sonic Crimson Ribbon */
    hw_fill_win_rect(1, 11, 18, 1, 82);  /* Ring Gold Underline */

    hw_print_win(1, 2, "SONIC THE HEDGEHOG");
    if (line1) hw_print_win(2, 6, line1);
    if (line2) hw_print_win(2, 8, line2);
    if (act_num > 0) {
        act_buf[0] = 'A'; act_buf[1] = 'C'; act_buf[2] = 'T'; act_buf[3] = ' ';
        act_buf[4] = (char)('0' + act_num); act_buf[5] = '\0';
        hw_print_win(12, 9, act_buf);
    }
    WX_REG = 7;
    WY_REG = 0;
}

static void write_u16_digits(char *dst, uint16_t val, uint8_t digits) {
    uint8_t i;
    for (i = digits; i > 0; i--) {
        dst[i - 1] = (char)('0' + (val % 10));
        val /= 10;
    }
}

void hw_update_hud(void) {
    char row0[21];
    char row1[21];
    uint16_t secs_total = g_level_frames / 60;
    uint8_t mins = (uint8_t)(secs_total / 60);
    uint8_t secs = (uint8_t)(secs_total % 60);
    uint16_t sc_disp = (uint16_t)(g_score / 10);

    /* Row 0: "SCR 000000  T 0:00  " */
    memcpy(row0, "SCR 0000000 T 0:00  ", 20);
    row0[20] = '\0';
    write_u16_digits(&row0[4], sc_disp, 6);
    row0[14] = (char)('0' + (mins % 10));
    row0[16] = (char)('0' + (secs / 10));
    row0[17] = (char)('0' + (secs % 10));

    /* Row 1: "*000  @#x03  EM:0/6 " */
    memcpy(row1, "*000  @#03   EM 0:6 ", 20);
    row1[20] = '\0';
    write_u16_digits(&row1[1], g_rings > 999 ? 999 : g_rings, 3);
    write_u16_digits(&row1[8], g_lives > 99 ? 99 : g_lives, 2);
    {
        uint8_t em_cnt = 0, b;
        for (b = 0; b < 6; b++) if (g_emeralds & (1 << b)) em_cnt++;
        row1[16] = (char)('0' + em_cnt);
    }

    hw_print_win(0, 0, row0);
    hw_print_win(0, 1, row1);
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
    SWITCH_ROM(prev);
}

uint8_t hw_get_raw_chunk_id(int16_t world_x, int16_t world_y) {
    uint8_t prev, cx, cy, chunk_id;
    if (world_x < 0 || world_y < 0) return 0;
    cx = (uint8_t)((uint16_t)world_x >> 8);
    cy = (uint8_t)((uint16_t)world_y >> 8);
    if (cx >= g_ram_act.fg_w || cy >= g_ram_act.fg_h) return 0;
    prev = CURRENT_BANK;
    SWITCH_ROM(g_ram_act.data_bank);
    chunk_id = g_ram_act.fg_layout[(uint16_t)cy * g_ram_act.fg_w + cx];
    SWITCH_ROM(prev);
    return chunk_id;
}

/* Look up 16-bit chunk block word at `(world_x, world_y)` matching `FindNearestTile` (`sub FindNearestTile.asm`) */
uint16_t hw_get_chunk_word(int16_t world_x, int16_t world_y, uint8_t loop_alt) {
    uint8_t prev, cx, cy, chunk_id, page_bank, bx, by;
    const uint8_t *cptr;
    uint16_t bw;

    if (world_x < 0 || world_y < 0) return 0;
    cx = (uint8_t)((uint16_t)world_x >> 8);
    cy = (uint8_t)((uint16_t)world_y >> 8);
    if (cx >= g_ram_act.fg_w || cy >= g_ram_act.fg_h) return 0;

    prev = CURRENT_BANK;
    SWITCH_ROM(g_ram_act.data_bank);
    chunk_id = g_ram_act.fg_layout[(uint16_t)cy * g_ram_act.fg_w + cx];
    if (chunk_id == 0) {
        SWITCH_ROM(prev);
        return 0;
    }
    /* Exact `.specialtile` logic from `sub FindNearestTile.asm`: loop chunks (`& 0x80`) swap to `cid + 1` (and `$29 -> $51`)! */
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
    chunk_id--; /* 0-based index into map256 */
    page_bank = g_ram_vset.chunk_base_bank + (chunk_id >> 4);
    SWITCH_ROM(8);
    cptr = g_chunk_bank_ptrs[page_bank];
    SWITCH_ROM(page_bank);
    bx = ((uint8_t)world_x >> 4) & 0x0F;
    by = ((uint8_t)world_y >> 4) & 0x0F;
    bw = ((const uint16_t *)cptr)[((uint16_t)(chunk_id & 0x0F) << 8) + ((uint16_t)by << 4) + bx];
    SWITCH_ROM(prev);
    return bw;
}

/* Resolve half-scale `(tile_byte, cgb_attr)` at GBC 8x8 tile `(wx8, wy8)` (= MD 16x16 block `(wx8*16, wy8*16)`) */
static uint8_t s_tc_tag = 0xFF;
static uint8_t s_tc_fcx = 0xFF, s_tc_fcy = 0xFF, s_tc_fcid = 0, s_tc_fpbank = 0;
static const uint8_t *s_tc_fcptr = 0;
static uint8_t s_tc_bcx = 0xFF, s_tc_bcy = 0xFF, s_tc_bcid = 0, s_tc_bpbank = 0;
static const uint8_t *s_tc_bcptr = 0;

static uint16_t resolve_world_block8(int16_t wx8, int16_t wy8) {
    uint16_t bw = 0;

    if (wx8 >= 0 && wy8 >= 0) {
        uint8_t cx = (uint8_t)((uint16_t)wx8 >> 4);
        uint8_t cy = (uint8_t)((uint16_t)wy8 >> 4);
        if (cx < g_ram_act.fg_w && cy < g_ram_act.fg_h) {
            uint8_t cid;
            if (cx == s_tc_fcx && cy == s_tc_fcy) {
                cid = s_tc_fcid;
            } else {
                s_tc_fcx = cx;
                s_tc_fcy = cy;
                SWITCH_ROM(g_ram_act.data_bank);
                cid = g_ram_act.fg_layout[(uint16_t)cy * g_ram_act.fg_w + cx] & 0x7F;
                s_tc_fcid = cid;
                if (cid > 0 && cid <= g_ram_vset.num_chunks) {
                    s_tc_fpbank = g_ram_vset.chunk_base_bank + ((cid - 1) >> 4);
                    SWITCH_ROM(8);
                    s_tc_fcptr = g_chunk_bank_ptrs[s_tc_fpbank];
                }
            }
            if (cid > 0 && cid <= g_ram_vset.num_chunks) {
                SWITCH_ROM(s_tc_fpbank);
                bw = ((const uint16_t *)s_tc_fcptr)[((uint16_t)((cid - 1) & 0x0F) << 8)
                    + (uint16_t)(wy8 & 0x0F) * 16 + (wx8 & 0x0F)];
                if (bw & 0x03FF) return bw;
            }
        }
    }

    if (g_ram_act.bg_w > 0 && g_ram_act.bg_h > 0) {
        uint8_t vset = g_ram_act.vset_idx;
        uint16_t raw_x = (uint16_t)(wx8 >= 0 ? wx8 : 0);
        uint16_t raw_y = (uint16_t)(wy8 >= 0 ? wy8 : 0);
        uint16_t bg_tx, bg_ty;
        if (vset == 0 || vset == 8) {
            bg_tx = (raw_x * 3U) >> 3;
            bg_ty = raw_y >> 2;
        } else if (vset == 2 || vset == 3) {
            bg_tx = raw_x >> 2;
            bg_ty = raw_y >> 1;
        } else if (vset == 5 || vset == 6) {
            bg_tx = (raw_x * 3U) >> 3;
            bg_ty = raw_y >> 1;
        } else {
            bg_tx = raw_x >> 1;
            bg_ty = raw_y >> 1;
        }
        {
            uint8_t raw_bcx = (uint8_t)(bg_tx >> 4);
            uint8_t bcy = (uint8_t)(bg_ty >> 4);
            uint8_t bcid;
            if (bcy >= g_ram_act.bg_h) bcy = g_ram_act.bg_h - 1;
            if (raw_bcx == s_tc_bcx && bcy == s_tc_bcy) {
                bcid = s_tc_bcid;
            } else {
                uint8_t bcx = (uint8_t)(raw_bcx % g_ram_act.bg_w);
                s_tc_bcx = raw_bcx;
                s_tc_bcy = bcy;
                SWITCH_ROM(g_ram_act.data_bank);
                bcid = g_ram_act.bg_layout[(uint16_t)bcy * g_ram_act.bg_w + bcx] & 0x7F;
                s_tc_bcid = bcid;
                if (bcid > 0 && bcid <= g_ram_vset.num_chunks) {
                    s_tc_bpbank = g_ram_vset.chunk_base_bank + ((bcid - 1) >> 4);
                    SWITCH_ROM(8);
                    s_tc_bcptr = g_chunk_bank_ptrs[s_tc_bpbank];
                }
            }
            if (bcid > 0 && bcid <= g_ram_vset.num_chunks) {
                SWITCH_ROM(s_tc_bpbank);
                return ((const uint16_t *)s_tc_bcptr)[((uint16_t)((bcid - 1) & 0x0F) << 8)
                    + (uint16_t)(bg_ty & 0x0F) * 16 + (bg_tx & 0x0F)];
            }
        }
    }
    return 0;
}

static void blocks_to_tiles_batch(const uint16_t *bw_buf, uint8_t count, uint8_t *out_t, uint8_t *out_a) {
    uint8_t i;
    uint16_t nm16 = g_ram_vset.num_m16;
    const uint8_t *map16 = g_ram_vset.map16;
    SWITCH_ROM(g_ram_vset.meta_bank);
    for (i = 0; i < count; i++) {
        uint16_t bw = bw_buf[i];
        uint16_t bidx = bw & 0x03FF;
        if (bidx == 0 || bidx >= nm16) {
            out_t[i] = 0;
            out_a[i] = 0;
        } else {
            const uint8_t *mptr = &map16[bidx << 1];
            uint8_t flip = ((bw & 0x0800) ? 0x20 : 0) | ((bw & 0x1000) ? 0x40 : 0);
            out_t[i] = mptr[0];
            out_a[i] = (mptr[1] & 0x7F) ^ flip;
        }
    }
}

static void draw_world_col(int16_t wx8, int16_t top_wy8) {
    uint8_t prev = CURRENT_BANK;
    uint8_t tag = (uint8_t)((g_cur_act << 1) | (g_rev_mode & 1));
    uint16_t bw_buf[20];
    uint8_t col_t[20];
    uint8_t col_a[20];
    uint8_t i, vx, vy0, n1, n2;
    if (tag != s_tc_tag) { s_tc_tag = tag; s_tc_fcx = 0xFF; s_tc_bcx = 0xFF; }
    vx = (uint8_t)wx8 & 31;
    vy0 = (uint8_t)top_wy8 & 31;
    n1 = (vy0 + 20 <= 32) ? 20 : (uint8_t)(32 - vy0);
    n2 = (uint8_t)(20 - n1);
    for (i = 0; i < 20; i++) {
        bw_buf[i] = resolve_world_block8(wx8, top_wy8 + i);
    }
    blocks_to_tiles_batch(bw_buf, 20, col_t, col_a);
    SWITCH_ROM(prev);
    VBK_REG = 1;
    set_bkg_tiles(vx, vy0, 1, n1, col_a);
    if (n2) set_bkg_tiles(vx, 0, 1, n2, col_a + n1);
    VBK_REG = 0;
    set_bkg_tiles(vx, vy0, 1, n1, col_t);
    if (n2) set_bkg_tiles(vx, 0, 1, n2, col_t + n1);
}

static void draw_world_row(int16_t left_wx8, int16_t wy8) {
    uint8_t prev = CURRENT_BANK;
    uint8_t tag = (uint8_t)((g_cur_act << 1) | (g_rev_mode & 1));
    uint16_t bw_buf[23];
    uint8_t row_t[23];
    uint8_t row_a[23];
    uint8_t i, vx0, vy, n1, n2;
    if (tag != s_tc_tag) { s_tc_tag = tag; s_tc_fcx = 0xFF; s_tc_bcx = 0xFF; }
    vx0 = (uint8_t)left_wx8 & 31;
    vy = (uint8_t)wy8 & 31;
    n1 = (vx0 + 23 <= 32) ? 23 : (uint8_t)(32 - vx0);
    n2 = (uint8_t)(23 - n1);
    for (i = 0; i < 23; i++) {
        bw_buf[i] = resolve_world_block8(left_wx8 + i, wy8);
    }
    blocks_to_tiles_batch(bw_buf, 23, row_t, row_a);
    SWITCH_ROM(prev);
    VBK_REG = 1;
    set_bkg_tiles(vx0, vy, n1, 1, row_a);
    if (n2) set_bkg_tiles(0, vy, n2, 1, row_a + n1);
    VBK_REG = 0;
    set_bkg_tiles(vx0, vy, n1, 1, row_t);
    if (n2) set_bkg_tiles(0, vy, n2, 1, row_t + n1);
}

void hw_refresh_full_viewport(void) {
    int16_t wx0 = (g_cam_x >> 4) - 1;
    int16_t wy0 = (g_cam_y >> 4) - 1;
    uint8_t r;
    for (r = 0; r < 20; r++) {
        draw_world_row(wx0, wy0 + r);
        if ((r & 3) == 3) vsync();
    }
    SCX_REG = (uint8_t)(g_cam_x >> 1);
    SCY_REG = (uint8_t)(g_cam_y >> 1);
}

void hw_stream_viewport_delta(int16_t old_cam_x, int16_t old_cam_y) {
    int16_t old_tx = old_cam_x >> 4;
    int16_t new_tx = g_cam_x >> 4;
    int16_t old_ty = old_cam_y >> 4;
    int16_t new_ty = g_cam_y >> 4;

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

void hw_set_underwater_palette(uint8_t underwater) {
    if (underwater != s_underwater_pal_active) {
        uint16_t pal_ram[28];
        s_underwater_pal_active = underwater;
        memcpy(pal_ram, underwater ? g_ram_vset.uw_pal : g_ram_vset.bg_pal, sizeof(pal_ram));
        set_bkg_palette(0, 7, pal_ram);
    }
}

/* Live VBlank Zone Color Cycling matching `_inc/PaletteCycle.asm` — cycles ONLY true water/lava/light sub-palettes,
   NEVER modifying brown/stone/steel foreground wall sub-palettes! */
void hw_step_palette_cycle(void) {
    static const uint8_t vset_slot[9] = { 1, 5, 1, 5, 4, 5, 5, 5, 1 };
    static const uint16_t vset_shimmer[9][4] = {
        { RGB8(168,224,252), RGB8(216,244,252), RGB8(248,252,252), RGB8(140,200,248) }, /* 0: GHZ BGP1 water */
        { RGB8( 96,220,240), RGB8(156,240,252), RGB8(236,252,252), RGB8( 64,188,220) }, /* 1: LZ  BGP5 water */
        { RGB8(252,136,  0), RGB8(252,192, 24), RGB8(252,236, 88), RGB8(244, 88,  0) }, /* 2: MZ  BGP1 lava  */
        { RGB8(196,224,252), RGB8(252,244,196), RGB8(252,252,252), RGB8(164,204,248) }, /* 3: SLZ BGP5 stars */
        { RGB8(252,228, 64), RGB8(252,112,188), RGB8(112,244,220), RGB8(252,172, 56) }, /* 4: SYZ BGP4 neon  */
        { RGB8(252,220, 64), RGB8(252,132, 56), RGB8(180,236,252), RGB8(224,188, 48) }, /* 5: SBZ1 BGP5 elec */
        { RGB8(252,220, 64), RGB8(252,132, 56), RGB8(180,236,252), RGB8(224,188, 48) }, /* 6: SBZ2 BGP5 elec */
        { RGB8( 96,220,240), RGB8(156,240,252), RGB8(236,252,252), RGB8( 64,188,220) }, /* 7: SBZ3 BGP5 water*/
        { RGB8(168,224,252), RGB8(216,244,252), RGB8(248,252,252), RGB8(140,200,248) }  /* 8: END BGP1 water */
    };
    uint16_t cyc_pal[4];
    const uint16_t *src_pal = s_underwater_pal_active ? g_ram_vset.uw_pal : g_ram_vset.bg_pal;
    uint8_t vset = g_ram_act.vset_idx < 9 ? g_ram_act.vset_idx : 0;
    uint8_t slot;

    if ((g_frame_counter & 7) != 0) return;
    slot = vset_slot[vset];
    memcpy(cyc_pal, &src_pal[slot << 2], sizeof(cyc_pal));
    cyc_pal[3] = vset_shimmer[vset][(g_frame_counter >> 3) & 3];
    set_bkg_palette(slot, 1, cyc_pal);
    g_pal_cycles++;
}

static void reload_common_hud_and_sprites(void) {
    SWITCH_ROM(4);
    VBK_REG = 1;
    set_bkg_data(80, 48, hud_font_tiles);
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

    if (act_idx >= NUM_PLAYABLE_ACTS) act_idx = 0;
    g_cur_act = act_idx;

    SWITCH_ROM(8);
    g_ram_act = g_acts[act_idx];
    g_ram_vset = g_visual_sets[g_ram_act.vset_idx];

    /* Display crisp colorful Sonic Title Card Window during seamless VRAM streaming */
    set_bkg_palette(7, 1, &g_ram_vset.bg_pal[28]);
    hw_show_transition_card(g_ram_act.label, "ZONE", (uint8_t)((act_idx % 3) + 1));

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
    }
    g_rings = 0;
    g_next_ring_life = 1;
    g_signpost_passed = 0;
    g_act_clear_done = 0;
    g_boss_active = 0;
    g_boss_defeated = 0;
    g_water_y = g_ram_act.water_y;
    s_underwater_pal_active = 0;
    s_loaded_sonic_frame = 0xFF;

    g_debug_active = 0;
    g_button_state = 0;
    dle_init_for_act();
    osc_reset();
    physics_init_sonic(sx, sy);
    objects_reset_for_act();
    boss_and_gimmick_reset();
    hw_scan_spawn_objects();

    memcpy(pal_ram, g_ram_vset.bg_pal, sizeof(pal_ram));
    set_bkg_palette(0, 8, pal_ram);
    set_sprite_palette(0, 8, g_default_obj_pals);
    hw_play_bgm(g_ram_act.music_id);
    hw_refresh_full_viewport();

    /* Hold Title Card briefly so player reads Zone & Act, then reveal HUD bar at WY=128! */
    for (hold = 0; hold < 4; hold++) {
        vsync();
    }
    hw_update_hud();
    WX_REG = 7;
    WY_REG = 128;
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
    SCX_REG = 0;
    SCY_REG = 0;
    hw_update_hud();
    WX_REG = 7;
    WY_REG = 128;
    SWITCH_ROM(prev);
}

void hw_copy_ss_maze(uint8_t ss_idx, uint8_t *dst4096) {
    uint8_t prev = CURRENT_BANK;
    SpecialStageDesc d;
    SWITCH_ROM(8);
    d = g_special_stages[ss_idx % NUM_SPECIAL_STAGES];
    SWITCH_ROM(d.bank);
    memcpy(dst4096, d.maze64x64, 4096);
    SWITCH_ROM(prev);
}

int16_t hw_sin(uint8_t angle) {
    uint8_t prev = CURRENT_BANK;
    int16_t v;
    SWITCH_ROM(5);
    v = s1_sine_table[angle];
    SWITCH_ROM(prev);
    return v;
}

int16_t hw_cos(uint8_t angle) {
    uint8_t prev = CURRENT_BANK;
    int16_t v;
    SWITCH_ROM(5);
    v = s1_sine_table[(uint16_t)angle + 64];
    SWITCH_ROM(prev);
    return v;
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
 * Preserves signed negative arch heights (`-1..-16`), full blocks (`16`), `H-flip` (`0x0800`),
 * `V-flip` (`0x1000`), ceiling `d6=0x1000`, left-wall `d6=0x0800`, and loop swap `loop_plane`!
 * ============================================================================================ */
static uint8_t lookup_col_id(uint16_t d4) {
    uint16_t bidx = d4 & 0x07FF;
    if (bidx == 0) return 0;
    if (bidx < 640) return g_ram_vset.col1[bidx];
    if (bidx < 1280) return g_ram_vset.col2[bidx - 640];
    return 0;
}

static uint8_t sample_col_height(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6,
                                 uint8_t loop_f, uint8_t rotated, uint8_t *a4, int16_t *out_d0) {
    uint16_t d4 = hw_get_chunk_word(d3, d2, loop_f);
    uint8_t prev, cid;
    int16_t idx_coord;

    if ((d4 & 0x07FF) == 0 || (d4 & ((uint16_t)1U << solid_bit)) == 0) return 0;
    prev = CURRENT_BANK;
    SWITCH_ROM(g_ram_vset.meta_bank);
    cid = lookup_col_id(d4);
    if (cid == 0) {
        SWITCH_ROM(prev);
        return 0;
    }
    SWITCH_ROM(5);
    if (a4) {
        uint8_t ang = s1_angle_map[cid];
        if (!rotated) {
            if (d4 & 0x0800) ang = (uint8_t)(0 - ang);
            if (d4 & 0x1000) ang = (uint8_t)((uint8_t)(0 - (uint8_t)(ang + 0x40)) - 0x40);
        } else {
            if (d4 & 0x1000) ang = (uint8_t)((uint8_t)(0 - (uint8_t)(ang + 0x40)) - 0x40);
            if (d4 & 0x0800) ang = (uint8_t)(0 - ang);
        }
        *a4 = ang;
    }
    if (!rotated) {
        idx_coord = (d4 & 0x0800) ? ~d3 : d3;
        *out_d0 = (int16_t)s1_coll_normal[((uint16_t)cid << 4) + (idx_coord & 0x0F)];
        if ((d4 ^ d6) & 0x1000) *out_d0 = -(*out_d0);
    } else {
        idx_coord = (d4 & 0x1000) ? ~d2 : d2;
        *out_d0 = (int16_t)s1_coll_rotated[((uint16_t)cid << 4) + (idx_coord & 0x0F)];
        if ((d4 ^ d6) & 0x0800) *out_d0 = -(*out_d0);
    }
    SWITCH_ROM(prev);
    return 1;
}

static int16_t find_floor2_s1(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, uint8_t loop_f, uint8_t *a4) {
    int16_t d0 = 0, d1;
    if (!sample_col_height(d2, d3, solid_bit, d6, loop_f, 0, a4, &d0) || d0 == 0) {
        return (int16_t)(0x0F - (d2 & 0x0F));
    }
    if (d0 > 0) return (int16_t)(0x0F - ((d2 & 0x0F) + d0));
    d1 = (d2 & 0x0F) + d0;
    return (d1 >= 0) ? (int16_t)(0x0F - (d2 & 0x0F)) : ~d1;
}

int16_t hw_find_floor(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, int16_t a3, uint8_t *a4) {
    uint8_t loop_f = g_sonic.loop_plane;
    int16_t d0 = 0;
    if (!sample_col_height(d2, d3, solid_bit, d6, loop_f, 0, a4, &d0) || d0 == 0) {
        return find_floor2_s1(d2 + a3, d3, solid_bit, d6, loop_f, a4) + 16;
    }
    if (d0 < 0) {
        if (((d2 & 0x0F) + d0) >= 0) return find_floor2_s1(d2 + a3, d3, solid_bit, d6, loop_f, a4) + 16;
        return find_floor2_s1(d2 - a3, d3, solid_bit, d6, loop_f, a4) - 16;
    }
    if (d0 == 16) return find_floor2_s1(d2 - a3, d3, solid_bit, d6, loop_f, a4) - 16;
    return (int16_t)(0x0F - ((d2 & 0x0F) + d0));
}

static int16_t find_wall2_s1(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, uint8_t loop_f, uint8_t *a4) {
    int16_t d0 = 0, d1;
    if (!sample_col_height(d2, d3, solid_bit, d6, loop_f, 1, a4, &d0) || d0 == 0) {
        return (int16_t)(0x0F - (d3 & 0x0F));
    }
    if (d0 > 0) return (int16_t)(0x0F - ((d3 & 0x0F) + d0));
    d1 = (d3 & 0x0F) + d0;
    return (d1 >= 0) ? (int16_t)(0x0F - (d3 & 0x0F)) : ~d1;
}

int16_t hw_find_wall(int16_t d2, int16_t d3, uint8_t solid_bit, uint16_t d6, int16_t a3, uint8_t *a4) {
    uint8_t loop_f = g_sonic.loop_plane;
    int16_t d0 = 0;
    if (!sample_col_height(d2, d3, solid_bit, d6, loop_f, 1, a4, &d0) || d0 == 0) {
        return find_wall2_s1(d2, d3 + a3, solid_bit, d6, loop_f, a4) + 16;
    }
    if (d0 < 0) {
        if (((d3 & 0x0F) + d0) >= 0) return find_wall2_s1(d2, d3 + a3, solid_bit, d6, loop_f, a4) + 16;
        return find_wall2_s1(d2, d3 - a3, solid_bit, d6, loop_f, a4) - 16;
    }
    if (d0 == 16) return find_wall2_s1(d2, d3 - a3, solid_bit, d6, loop_f, a4) - 16;
    return (int16_t)(0x0F - ((d3 & 0x0F) + d0));
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

static uint8_t s_dplc_buf[512];

/* Stream dynamic Sonic DPLC tiles into VRAM Bank 0 slots `0..31` whenever `g_sonic.frame` changes! */
static void ensure_sonic_dplc_loaded(uint8_t frame) {
    uint8_t prev, cnt, i;
    uint16_t off, st;
    if (frame >= 89) frame = 1;
    if (frame == s_loaded_sonic_frame) return;
    s_loaded_sonic_frame = frame;

    prev = CURRENT_BANK;
    SWITCH_ROM(9);
    off = sonic_dplc_offsets[frame];
    cnt = sonic_dplc_blob[off];
    if (cnt > 32) cnt = 32;
    for (i = 0; i < cnt; i++) {
        SWITCH_ROM(9);
        st = ((uint16_t)sonic_dplc_blob[off + 2 + i * 2] << 8) | sonic_dplc_blob[off + 1 + i * 2];
        if (st < 768) {
            SWITCH_ROM(6);
            memcpy(&s_dplc_buf[(uint16_t)i << 4], &sonic_tiles_part0[st << 4], 16);
        } else {
            SWITCH_ROM(7);
            memcpy(&s_dplc_buf[(uint16_t)i << 4], &sonic_tiles_part1[(st - 768) << 4], 16);
        }
    }
    VBK_REG = 0;
    set_sprite_data(0, cnt, s_dplc_buf);
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

    /* Update 360-degree 8-octant sprite flip/rotation flags every frame (`SAnim_WalkRun`, `01 Sonic.asm:2230`) */
    if (!g_sonic.rolling && g_sonic.on_ground && (g_sonic.anim == ANI_WALK || g_sonic.anim == ANI_RUN)) {
        uint8_t d0 = g_sonic.angle;
        uint8_t fx = g_sonic.facing_left ? 1 : 0;
        uint8_t fy = 0;
        if (!fx) d0 = (uint8_t)(~d0);
        d0 = (uint8_t)(d0 + 0x10);
        if (d0 & 0x80) {
            fx ^= 1;
            fy ^= 1;
        }
        s_sonic_rot_fx = fx;
        s_sonic_rot_fy = fy;
    } else {
        s_sonic_rot_fx = g_sonic.facing_left ? 1 : 0;
        s_sonic_rot_fy = 0;
    }

    if (g_sonic.anim_timer > 0) {
        g_sonic.anim_timer--;
        return;
    }

    SWITCH_ROM(9);
    base = sonic_ani_offsets[g_sonic.anim];
    spd = sonic_ani_blob[base];
    /* Speed-proportional walk/run/roll cadence matching Sonic 1 `Sonic_Animate`! */
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
    if (cmd > 0 && cmd < 89) {
        /* Apply authentic 8-octant diagonal/vertical/upside-down frame offset (`+oct*6` Walk, `+oct*4` Run)! */
        if (!g_sonic.rolling && g_sonic.on_ground && (g_sonic.anim == ANI_WALK || g_sonic.anim == ANI_RUN)) {
            uint8_t d0 = g_sonic.facing_left ? g_sonic.angle : (uint8_t)(~g_sonic.angle);
            uint8_t oct = (uint8_t)(((uint8_t)(d0 + 0x10) >> 5) & 3);
            cmd = (uint8_t)(cmd + oct * (g_sonic.anim == ANI_WALK ? 6 : 4));
        }
        g_sonic.frame = cmd;
        g_sonic.anim_idx++;
    }
    g_sonic.anim_timer = spd;
    SWITCH_ROM(prev);
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
    for (i = 0; i < cnt && g_oam_next < 40; i++) {
        uint16_t p = off + 1 + ((uint16_t)i << 2);
        int8_t dy = (int8_t)sonic_spr_blob[p];
        int8_t dx = (int8_t)sonic_spr_blob[p + 1];
        uint8_t lt = sonic_spr_blob[p + 2];
        uint8_t fl = sonic_spr_blob[p + 3];
        int16_t fx = s_sonic_rot_fx ? (scr_x - dx - 8) : (scr_x + dx);
        int16_t fy = s_sonic_rot_fy ? (scr_y - dy - 8) : (scr_y + dy);
        uint8_t attr = fl ^ (s_sonic_rot_fx ? 0x20 : 0) ^ (s_sonic_rot_fy ? 0x40 : 0);
        if (g_sonic.loop_plane) attr |= 0x80;
        hw_put_sprite8(fx, fy, lt, attr);
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

/* `_inc/Palette Fading.asm`: Non-Black / Non-White Smooth CGB Sky-Tinted Fade Step */
void hw_fade_step(uint8_t step_idx) {
    uint16_t faded[4];
    uint8_t s = step_idx & 3;
    faded[0] = RGB8(20 + (s << 2), 36 + (s << 3), 112 + (s << 4));
    faded[1] = RGB8(28 + (s << 3), 56 + (s << 3), 140 + (s << 3));
    faded[2] = RGB8(80 + (s << 4), 120 + (s << 4), 196 + (s << 2));
    faded[3] = RGB8(180 + (s << 4), 196 + (s << 3), 220 + (s << 2));
    set_bkg_palette(7, 1, faded);
}

/* Scan sorted Act object layout (`ram_act.objects` or `REV00` bank 125) across full 320px `0.5x` world viewport */
void hw_scan_spawn_objects(void) {
    uint8_t prev = CURRENT_BANK;
    const uint8_t *p;
    uint16_t idx = 0;
    int16_t min_x = g_cam_x - 64;
    int16_t max_x = g_cam_x + 384;

    if (g_rev_mode && (g_cur_act == 2 || g_cur_act == 9)) {
        SWITCH_ROM(SUPPLEMENT_REV00_BANK);
        p = (g_cur_act == 2) ? rev00_ghz3_objs : rev00_lz1_objs;
    } else {
        SWITCH_ROM(g_ram_act.data_bank);
        p = g_ram_act.objects;
    }
    while (idx < 512) {
        uint16_t ox = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if (ox == 0xFFFF) break;
        if ((int16_t)ox >= min_x && (int16_t)ox <= max_x) {
            if (!(g_obj_taken[idx >> 3] & (uint8_t)(1U << (idx & 7)))) {
                uint8_t already = 0, s, slot_out = 0xFF;
                int16_t oy = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
                uint8_t oid = p[4];
                uint8_t osub = p[5];
                for (s = 0; s < MAX_ACTIVE_OBJS; s++) {
                    if (g_objs[s].active) {
                        if (g_objs[s].spawn_slot == idx && g_objs[s].base_x == (int16_t)ox) {
                            already = 1;
                            break;
                        }
                    } else if (slot_out == 0xFF) {
                        slot_out = s;
                    }
                }
                /* Guarantee critical Goal Post (`0x0D`), Prison Capsule (`0x3E`), & Giant Ring (`0x4B`) ALWAYS spawn! */
                if (!already && slot_out == 0xFF && (oid == 0x0D || oid == 0x3E || oid == 0x4B)) {
                    for (s = 0; s < MAX_ACTIVE_OBJS; s++) {
                        uint8_t eid = g_objs[s].id;
                        if (eid != 0x0D && eid != 0x3E && eid != 0x4B && eid != 0x3D) {
                            slot_out = s;
                            break;
                        }
                    }
                }
                if (!already && slot_out != 0xFF) {
                    g_objs[slot_out].active = 1;
                    g_objs[slot_out].id = oid;
                    g_objs[slot_out].subtype = osub;
                    g_objs[slot_out].spawn_slot = idx;
                    g_objs[slot_out].x = (int16_t)ox;
                    g_objs[slot_out].y = oy;
                    g_objs[slot_out].base_x = (int16_t)ox;
                    g_objs[slot_out].base_y = oy;
                    g_objs[slot_out].vx = -0x80;
                    g_objs[slot_out].vy = 0;
                    g_objs[slot_out].state = 0;
                    g_objs[slot_out].timer = 0;
                    g_objs[slot_out].hp = 1;
                }
            }
        } else if ((int16_t)ox > max_x) {
            break; /* Sorted ascending by X -> early exit in microseconds! */
        }
        p += 6;
        idx++;
    }
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

    /* Load Battery-Backed MBC5 SRAM Save State (`high_score`, `unlocked_act`, `rev_mode`, `jp_credits`) */
    sram_load_or_init();

    /* Load authentic SEGA Boot Screen BEFORE enabling LCD so Frame 1 is IMMEDIATELY crisp multi-color SEGA! */
    g_game_mode = GM_SEGA;
    g_next_mode = GM_SEGA;
    g_mode_init_done = 0;
    hw_load_fullscreen_screen(0);

    /* Turn LCD ON ONCE with BG ($8800, $9800) + Window ($9C00) + Sprites ON — NEVER turned off again! */
    LCDC_REG = LCDCF_ON | LCDCF_BG8800 | LCDCF_BG9800 | LCDCF_BGON | LCDCF_WIN9C00 | LCDCF_WINON | LCDCF_OBJ8 | LCDCF_OBJON;

    /* Drive all SMPS music, Wave-RAM drum packets, & SFX directly from the SM83 hardware VBlank interrupt (`0x0040`)! */
    CRITICAL {
        add_VBL(hw_sound_isr);
    }
    set_interrupts(VBL_IFLAG);

    while (1) {
        g_joy_prev = g_joy;
        g_joy = joypad();
        g_joy_pressed = (g_joy ^ g_joy_prev) & g_joy;

        if (g_next_mode != g_game_mode) {
            g_game_mode = g_next_mode;
            g_mode_init_done = 0;
        }
        if (!g_mode_init_done) {
            g_mode_init_done = 1;
            screens_init_mode(g_game_mode);
        }

        screens_update_mode(g_game_mode);

        g_frame_counter++;
        vsync();
        hw_animate_level_gfx();
    }
}
