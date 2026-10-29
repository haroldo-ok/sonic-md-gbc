#pragma bank 127
#include "engine.h"
#include <string.h>

/* ============================================================================================
 * Bank 123: Complete Engine Subsystems from `vendor/s1disasm/_inc/`:
 *   1. `_inc/DynamicLevelEvents.asm` (`DLE_GHZ1..DLE_FZ` dynamic arena locks & water events)
 *   2. `_inc/Oscillatory Routines.asm` (`OscillateNumInit` & `OscillateNumDo` 16-channel bank)
 *   3. `_incObj/sub AddPoints.asm` (`50,000`-point Extra Life + Live Top Score `v_topscore`)
 *   4. Battery-Backed MBC5 `32 KB` SRAM Save Persistence (`$A000..$A01F`)
 * ============================================================================================ */

typedef struct {
    uint32_t magic;         /* 'S1GB' = 0x42473153UL */
    uint32_t high_score;
    uint8_t best_emeralds;
    uint8_t unlocked_act;
    uint8_t rev_mode;
    uint8_t jp_credits;
    uint32_t checksum;
} SramSaveBlock;

static uint32_t calc_sram_checksum(const SramSaveBlock *b) {
    return b->magic ^ b->high_score ^
           ((uint32_t)b->best_emeralds << 24) ^
           ((uint32_t)b->unlocked_act << 16) ^
           ((uint32_t)b->rev_mode << 8) ^
           (uint32_t)b->jp_credits ^ 0xA55A5AA5UL;
}

void sram_load_or_init(void) BANKED {
    SramSaveBlock *sram = (SramSaveBlock *)0xA000;
    ENABLE_RAM;
    rRAMB = 0;
    if (sram->magic == 0x42473153UL && sram->checksum == calc_sram_checksum(sram)) {
        g_high_score = sram->high_score;
        g_unlocked_act = sram->unlocked_act;
        g_rev_mode = sram->rev_mode & 1;
        g_jp_credits = sram->jp_credits & 1;
    } else {
        /* Authentic Sonic 1 initial top score (`v_topscore = 50000` -> `500000` pts display) */
        g_high_score = 50000UL;
        g_unlocked_act = 0;
        g_rev_mode = 0;
        g_jp_credits = 0;
        sram->magic = 0x42473153UL;
        sram->high_score = g_high_score;
        sram->best_emeralds = 0;
        sram->unlocked_act = 0;
        sram->rev_mode = 0;
        sram->jp_credits = 0;
        sram->checksum = calc_sram_checksum(sram);
    }
    rRAMB = 1;
}

void sram_save_progress(void) BANKED {
    SramSaveBlock *sram = (SramSaveBlock *)0xA000;
    if (g_score > g_high_score) g_high_score = g_score;
    if (g_cur_act > g_unlocked_act && g_cur_act < NUM_PLAYABLE_ACTS) {
        g_unlocked_act = g_cur_act;
    }
    ENABLE_RAM;
    rRAMB = 0;
    sram->magic = 0x42473153UL;
    sram->high_score = g_high_score;
    if ((g_emeralds & 0x3F) > sram->best_emeralds) {
        sram->best_emeralds = g_emeralds & 0x3F;
    }
    if (g_unlocked_act > sram->unlocked_act) {
        sram->unlocked_act = g_unlocked_act;
    }
    sram->rev_mode = g_rev_mode & 1;
    sram->jp_credits = g_jp_credits & 1;
    sram->checksum = calc_sram_checksum(sram);
    rRAMB = 1;
}

/* Authentic `_incObj/sub AddPoints.asm`: Updates Score, High Score (`v_topscore`), & 50,000-pt 1UP! */
void add_score_points(uint16_t pts) BANKED {
    g_score += pts;
    if (g_score > 9999990UL) g_score = 9999990UL;
    if (g_score > g_high_score) {
        g_high_score = g_score;
    }
    if (!g_score_life_awarded && g_score >= 50000UL) {
        g_score_life_awarded = 1;
        g_lives++;
        hw_play_bgm(BGM_EXTRALIFE);
    }
    g_hud_dirty = 1;
}

/* Authentic `_inc/Oscillatory Routines.asm` (`OscillateNumInit` & `OscillateNumDo` — 2x delta every 2nd frame) */

void osc_reset(void) BANKED {
    g_osc_dir = 0;
    hw_init_osc_from_rom(g_osc_val);
}

/* Live VBlank Zone Color Cycling matching `_inc/PaletteCycle.asm` — cycles ONLY true water/lava/light sub-palettes,
   NEVER modifying brown/stone/steel foreground wall sub-palettes! */
void hw_step_palette_cycle(void) BANKED {
    static const uint8_t vset_slot[9] = { 1, 5, 1, 5, 4, 5, 5, 5, 1 };
    static const uint16_t vset_shimmer[9][4] = {
        { RGB8(168,224,252), RGB8(216,244,252), RGB8(248,252,252), RGB8(140,200,248) },
        { RGB8( 96,220,240), RGB8(156,240,252), RGB8(236,252,252), RGB8( 64,188,220) },
        { RGB8(252,136,  0), RGB8(252,192, 24), RGB8(252,236, 88), RGB8(244, 88,  0) },
        { RGB8(196,224,252), RGB8(252,244,196), RGB8(252,252,252), RGB8(164,204,248) },
        { RGB8(252,228, 64), RGB8(252,112,188), RGB8(112,244,220), RGB8(252,172, 56) },
        { RGB8(252,220, 64), RGB8(252,132, 56), RGB8(180,236,252), RGB8(224,188, 48) },
        { RGB8(252,220, 64), RGB8(252,132, 56), RGB8(180,236,252), RGB8(224,188, 48) },
        { RGB8( 96,220,240), RGB8(156,240,252), RGB8(236,252,252), RGB8( 64,188,220) },
        { RGB8(168,224,252), RGB8(216,244,252), RGB8(248,252,252), RGB8(140,200,248) }
    };
    uint16_t cyc_pal[4];
    const uint16_t *src_pal = g_sonic.underwater ? g_ram_vset.uw_pal : g_ram_vset.bg_pal;
    uint8_t vset = g_ram_act.vset_idx < 9 ? g_ram_act.vset_idx : 0;
    uint8_t slot;

    if ((g_frame_counter & 7) != 0) return;
    slot = vset_slot[vset];
    cyc_pal[0] = src_pal[(slot << 2) + 0];
    cyc_pal[1] = src_pal[(slot << 2) + 1];
    cyc_pal[2] = src_pal[(slot << 2) + 2];
    cyc_pal[3] = vset_shimmer[vset][(g_frame_counter >> 3) & 3];
    set_bkg_palette(slot, 1, cyc_pal);
    g_pal_cycles++;
}

/* One oscillator step (`OscillateNumDo`): bounce oscillator IDX between 0 and LIM by STEP; direction bit MASK of D.
   Values never exceed 0x90 (init 0x80, limits <= 0x70, steps <= 16) and the high byte is always 0, so only the low
   byte is touched and all arithmetic is 8-bit. IDX/STEP/LIM are literals, so every access is a direct address. */
#define OSC_N(IDX, STEP, LIM, D, MASK)                                   \
    do {                                                                 \
        uint8_t v = ((uint8_t *)g_osc_val)[(IDX) * 2U];                  \
        if (!(D & (MASK))) {                                             \
            v += (STEP);                                                 \
            if (v >= (LIM)) D |= (MASK);                                 \
        } else if (v > (STEP)) {                                         \
            v -= (STEP);                                                 \
        } else {                                                         \
            v = 0;                                                       \
            D &= (uint8_t)~(MASK);                                       \
        }                                                                \
        ((uint8_t *)g_osc_val)[(IDX) * 2U] = v;                          \
    } while (0)

/* Per-oscillator step sizes {4,4,4,4,8,16,16,8,4,4,4,6,10,14,4,4} and limits
   {0x10,0x18,0x20,0x30,0x20,0x08,0x40,0x40,0x50,0x50,0x20,0x30,0x50,0x70,0x10,0x10} unrolled below. */
void osc_step_all(void) BANKED {
    uint8_t lo, hi;
    if (g_paused) return;
    lo = (uint8_t)g_osc_dir;
    hi = (uint8_t)(g_osc_dir >> 8);
    OSC_N( 0,  4, 0x10, lo, 0x01);
    OSC_N( 1,  4, 0x18, lo, 0x02);
    OSC_N( 2,  4, 0x20, lo, 0x04);
    OSC_N( 3,  4, 0x30, lo, 0x08);
    OSC_N( 4,  8, 0x20, lo, 0x10);
    OSC_N( 5, 16, 0x08, lo, 0x20);
    OSC_N( 6, 16, 0x40, lo, 0x40);
    OSC_N( 7,  8, 0x40, lo, 0x80);
    OSC_N( 8,  4, 0x50, hi, 0x01);
    OSC_N( 9,  4, 0x50, hi, 0x02);
    OSC_N(10,  4, 0x20, hi, 0x04);
    OSC_N(11,  6, 0x30, hi, 0x08);
    OSC_N(12, 10, 0x50, hi, 0x10);
    OSC_N(13, 14, 0x70, hi, 0x20);
    OSC_N(14,  4, 0x10, hi, 0x40);
    OSC_N(15,  4, 0x10, hi, 0x80);
    g_osc_dir = (uint16_t)(((uint16_t)hi << 8) | lo);
}

/* Authentic `_inc/DynamicLevelEvents.asm`: Dynamic Camera Boundary Locks & Zone Events */
void dle_init_for_act(void) BANKED {
    g_dyn_left_bound = (int16_t)g_ram_act.left_bound;
    g_dyn_right_bound = (int16_t)g_ram_act.right_bound;
    g_dyn_top_bound = g_ram_act.top_bound;
    g_dyn_bottom_bound = g_ram_act.bottom_bound;
}

void dle_update_act_boundaries(void) BANKED {
    /* 1. Lock camera left boundary inside Boss Arena (`DLE_GHZ3`, `MZ3`, `SYZ3`, `LZ3`, `SLZ3`, `FZ`) */
    if (g_boss_active && !g_boss_defeated) {
        if (g_cam_x > g_dyn_left_bound) {
            g_dyn_left_bound = g_cam_x;
        }
    }

    /* 2. Smooth `+4 px/frame` Right Boundary Expansion after Boss Defeat so Sonic reaches the Prison Capsule! */
    if (g_boss_defeated) {
        int16_t max_r = (int16_t)g_ram_act.fg_w * 256 - 320;
        int16_t target_r = (int16_t)g_ram_act.right_bound + 384;
        if (target_r > max_r) target_r = max_r;
        if (g_dyn_right_bound < target_r) {
            g_dyn_right_bound += 4;
            if (g_dyn_right_bound > target_r) g_dyn_right_bound = target_r;
        }
    }

    /* 3. `DLE_LZ3` & `DLE_SBZ3`: Dynamic Water Level Rise + SBZ3 Top-of-Shaft Transition to Final Zone! */
    if (g_cur_act == 11 && g_sonic.x > 0x1CA0 && g_water_y > 0x0200) {
        g_water_y -= 1;
    } else if (g_cur_act == 17 && g_sonic.x >= 0x2080 && g_sonic.y < 0x0180) {
        g_checkpoint_active = 0;
        g_cur_act = 18; /* Authentic `DLE_SBZ3`: Direct seamless entry into Final Zone! */
        g_mode_init_done = 0;
        g_next_mode = GM_LEVEL;
        return;
    }

    /* 4. Complete Authentic `DynamicLevelEvents.asm` Dynamic Bottom Camera Boundaries across ALL 20 Acts! */
    if (g_cur_act == 0) {
        /* `DLE_GHZ1`: Lower S-tunnels (`X >= 0x1400`, `Y >= 1050`) + Goal Post valley (`Y = 1196`)! */
        g_dyn_bottom_bound = (g_cam_x >= 0x1400 || g_sonic.x >= 0x1400) ? 0x0500 : 0x0300;
    } else if (g_cur_act == 1) {
        /* `DLE_GHZ2` */
        if (g_cam_x < 0x0ED0) g_dyn_bottom_bound = 0x0300;
        else if (g_cam_x < 0x1600) g_dyn_bottom_bound = 0x0200;
        else if (g_cam_x < 0x1D60) g_dyn_bottom_bound = 0x0400;
        else g_dyn_bottom_bound = 0x0300;
    } else if (g_cur_act == 2 && !g_boss_active) {
        /* `DLE_GHZ3` */
        if (g_cam_x < 0x0380) g_dyn_bottom_bound = 0x0300;
        else if (g_cam_x < 0x0960) g_dyn_bottom_bound = 0x0310;
        else if (g_cam_x < 0x1380) g_dyn_bottom_bound = 0x04C0;
        else if (g_cam_x < 0x1700) g_dyn_bottom_bound = 0x0400;
        else g_dyn_bottom_bound = 0x0300;
    } else if (g_cur_act == 3) {
        /* `DLE_MZ1`: Surface -> Underground Lava Dungeons (`$500`) -> Final Signpost (`$210`) */
        if (g_cam_x < 0x0700) g_dyn_bottom_bound = 0x01D0;
        else if (g_cam_x >= 0x1430 && g_sonic.y < 0x0340) g_dyn_bottom_bound = 0x0210;
        else g_dyn_bottom_bound = 0x0500;
    } else if (g_cur_act == 4) {
        /* `DLE_MZ2` */
        g_dyn_bottom_bound = (g_cam_x < 0x1700) ? 0x0520 : 0x0200;
    } else if (g_cur_act == 5) {
        /* `DLE_MZ3` */
        g_dyn_bottom_bound = (g_cam_x < 0x1560) ? 0x0720 : 0x0210;
    } else if (g_cur_act == 7) {
        /* `DLE_SYZ2` */
        g_dyn_bottom_bound = (g_cam_x >= 0x25A0 && g_sonic.y < 0x04D0) ? 0x0420 : 0x0520;
    } else if (g_cur_act == 8) {
        /* `DLE_SYZ3` */
        g_dyn_bottom_bound = (g_cam_x < 0x2AC0) ? 0x0620 : 0x04C0;
    } else if (g_cur_act == 14) {
        /* `DLE_SLZ3` */
        g_dyn_bottom_bound = (g_cam_x < 0x1E70) ? 0x06C0 : 0x0210;
    } else if (g_cur_act == 15) {
        /* `DLE_SBZ1` */
        if (g_cam_x < 0x1880) g_dyn_bottom_bound = 0x0720;
        else if (g_cam_x < 0x2000) g_dyn_bottom_bound = 0x0620;
        else g_dyn_bottom_bound = 0x02A0;
    } else if (g_cur_act == 16) {
        /* `DLE_SBZ2` */
        g_dyn_bottom_bound = (g_cam_x < 0x1800) ? 0x0800 : 0x0510;
    }
}

void hw_fill_win_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t fill_tile) BANKED {
    uint8_t row_t[20];
    uint8_t row_a[20];
    uint8_t i, ry;
    if (w > 20) w = 20;
    for (i = 0; i < w; i++) {
        row_t[i] = fill_tile;
        row_a[i] = 0x08 | 7;
    }
    for (ry = 0; ry < h; ry++) {
        VBK_REG = 1;
        set_win_tiles(x, y + ry, w, 1, row_a);
        VBK_REG = 0;
        set_win_tiles(x, y + ry, w, 1, row_t);
    }
}

static uint8_t s_underwater_pal_banked = 0xFF;

void hw_set_underwater_palette(uint8_t underwater) BANKED {
    if (underwater == 0xFF) {
        s_underwater_pal_banked = 0;
        return;
    }
    if (underwater != s_underwater_pal_banked) {
        uint16_t pal_ram[28];
        s_underwater_pal_banked = underwater;
        memcpy(pal_ram, underwater ? g_ram_vset.uw_pal : g_ram_vset.bg_pal, sizeof(pal_ram));
        set_bkg_palette(0, 7, pal_ram);
    }
}

void hw_fade_step(uint8_t step_idx) BANKED {
    uint16_t faded[4];
    uint8_t s = step_idx & 3;
    faded[0] = RGB8(20 + (s << 2), 36 + (s << 3), 112 + (s << 4));
    faded[1] = RGB8(28 + (s << 3), 56 + (s << 3), 140 + (s << 3));
    faded[2] = RGB8(80 + (s << 4), 120 + (s << 4), 196 + (s << 2));
    faded[3] = RGB8(180 + (s << 4), 196 + (s << 3), 220 + (s << 2));
    set_bkg_palette(7, 1, faded);
}

static void write_u16_digits(char *dst, uint16_t val, uint8_t digits) {
    uint8_t i;
    for (i = digits; i > 0; i--) {
        dst[i - 1] = (char)('0' + (val % 10));
        val /= 10;
    }
}

void hw_update_hud(void) BANKED {
    char row0[21];
    char row1[21];
    uint16_t secs_total = g_level_frames / 60;
    uint8_t mins = (uint8_t)(secs_total / 60);
    uint8_t secs = (uint8_t)(secs_total % 60);
    uint16_t sc_disp = (uint16_t)(g_score / 10);

    memcpy(row0, "SCR 0000000 T 0:00  ", 20);
    row0[20] = '\0';
    write_u16_digits(&row0[4], sc_disp, 6);
    row0[14] = (char)('0' + (mins % 10));
    row0[16] = (char)('0' + (secs / 10));
    row0[17] = (char)('0' + (secs % 10));

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

BANKREF(draw_transition_card_win_b127)
void draw_transition_card_win_b127(const char *line1, const char *line2, uint8_t act_num) BANKED {
    char act_buf[8];
    hw_fill_win_rect(0, 0, 20, 1, 126);
    hw_fill_win_rect(0, 1, 20, 16, 80);
    hw_fill_win_rect(0, 17, 20, 1, 126);
    hw_fill_win_rect(1, 5, 18, 6, 81);
    hw_fill_win_rect(1, 11, 18, 1, 82);

    hw_print_win(1, 2, "SONIC THE HEDGEHOG");
    if (line1[0]) hw_print_win(2, 6, line1);
    if (line2[0]) hw_print_win(2, 8, line2);
    if (act_num > 0) {
        act_buf[0] = 'A'; act_buf[1] = 'C'; act_buf[2] = 'T'; act_buf[3] = ' ';
        act_buf[4] = (char)('0' + act_num); act_buf[5] = '\0';
        hw_print_win(12, 9, act_buf);
    }
    WX_REG = 7;
    WY_REG = 0;
}

BANKREF(spawn_try_place_b127)
void spawn_try_place_b127(uint16_t idx, int16_t ox, int16_t oy, uint8_t oid, uint8_t osub) BANKED {
    uint8_t already = 0, s, slot_out = 0xFF;
    if (idx < 512U && oid != 0x0D && oid != 0x3E && oid != 0x4B &&
        (g_obj_taken[idx >> 3] & (uint8_t)(1U << (idx & 7)))) return;
    for (s = 0; s < MAX_ACTIVE_OBJS; s++) {
        if (g_objs[s].active) {
            if (g_objs[s].spawn_slot == idx && g_objs[s].base_x == ox) {
                already = 1;
                break;
            }
        } else if (slot_out == 0xFF) {
            slot_out = s;
        }
    }
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
        g_objs[slot_out].x = ox;
        g_objs[slot_out].y = oy;
        g_objs[slot_out].base_x = ox;
        g_objs[slot_out].base_y = oy;
        g_objs[slot_out].vx = -0x80;
        g_objs[slot_out].vy = 0;
        g_objs[slot_out].state = 0;
        g_objs[slot_out].timer = 0;
        g_objs[slot_out].hp = 1;
    }
}

BANKREF(sonic_anim_oct_b127)
uint8_t sonic_anim_oct_b127(uint8_t *out_fx, uint8_t *out_fy, uint8_t cmd, uint8_t do_oct) BANKED {
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
        *out_fx = fx;
        *out_fy = fy;
        if (do_oct) {
            uint8_t rd0 = g_sonic.facing_left ? g_sonic.angle : (uint8_t)(~g_sonic.angle);
            uint8_t oct = (uint8_t)(((uint8_t)(rd0 + 0x10) >> 5) & 3);
            cmd = (uint8_t)(cmd + oct * (g_sonic.anim == ANI_WALK ? 6 : 4));
        }
    } else {
        *out_fx = g_sonic.facing_left ? 1 : 0;
        *out_fy = 0;
    }
    return cmd;
}

static uint8_t b127_ascii_tile(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(84 + (c - '0'));
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'X') return (uint8_t)(96 + (c - 'A'));
    if (c == 'Y') return 94;
    if (c == 'Z') return 95;
    if (c == ':' || c == '-' || c == '/') return 120;
    if (c == '*') return 121;
    if (c == '@') return 122;
    if (c == '#') return 123;
    if (c == '$') return 124;
    if (c == '>') return 125;
    if (c == '%') return 126;
    if (c == '!') return 127;
    return 80;
}

BANKREF(print_win_ram_b127)
void print_win_ram_b127(uint8_t x, uint8_t y, const char *ram_str) BANKED {
    uint8_t row_t[20], row_a[20];
    uint8_t i, n = 0;
    while (ram_str[n] != '\0' && (uint8_t)(x + n) < 20U) {
        row_t[n] = b127_ascii_tile(ram_str[n]);
        row_a[n] = 0x08U | 7U;
        n++;
    }
    if (n > 0U) {
        volatile uint8_t *vdst = (volatile uint8_t *)(0x9C00U + ((uint16_t)y << 5) + x);
        VBK_REG = 1;
        for (i = 0; i < n; i++) { while (STAT_REG & STATF_BUSY) {} vdst[i] = row_a[i]; }
        VBK_REG = 0;
        for (i = 0; i < n; i++) { while (STAT_REG & STATF_BUSY) {} vdst[i] = row_t[i]; }
    }
}

typedef struct {
    uint8_t is_col;
    uint8_t vx;
    uint8_t vy;
    uint8_t t[23];
    uint8_t a[23];
} VScrollSpanB127;

static void write_vram_span_b127(uint8_t vx, uint8_t vy, uint8_t is_col, const uint8_t *ps) {
    volatile uint8_t *vd = (volatile uint8_t *)(0x9800U + ((uint16_t)vy << 5) + vx);
    uint8_t n;
    if (is_col) {
        for (n = 20U; n != 0U; n--) {
            if (LY_REG < 144U || LY_REG >= 153U) { while (STAT_REG & STATF_BUSY) {} }
            *vd = *ps++;
            vd = ((uint16_t)vd < 0x9BE0U) ? (vd + 32) : (vd - 0x03E0U);
        }
    } else {
        for (n = 23U; n != 0U; n--) {
            if (LY_REG < 144U || LY_REG >= 153U) { while (STAT_REG & STATF_BUSY) {} }
            *vd = *ps++;
            vd = (((uint8_t)(uint16_t)vd & 31U) != 31U) ? (vd + 1) : (vd - 31);
        }
    }
}

BANKREF(flush_vscroll_b127)
void flush_vscroll_b127(uint8_t cnt) BANKED {
    const VScrollSpanB127 *q = (const VScrollSpanB127 *)0xBC00U;
    uint8_t i;
    rRAMB = 1;
    for (i = 0; i < cnt; i++) {
        VBK_REG = 1;
        write_vram_span_b127(q[i].vx, q[i].vy, q[i].is_col, q[i].a);
        VBK_REG = 0;
        write_vram_span_b127(q[i].vx, q[i].vy, q[i].is_col, q[i].t);
    }
}



