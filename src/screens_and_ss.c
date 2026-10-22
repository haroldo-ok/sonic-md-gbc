#pragma bank 3
#include "engine.h"
#include <string.h>

/* Exposed for PyBoy E2E Verification of Live 360° Special Stage Rotation! */
volatile uint16_t g_ss_rot_angle = 0;

static uint16_t s_mode_timer = 0;
static uint8_t s_ls_sel = 0;
static uint8_t s_ls_top = 0;
static uint8_t s_ls_sound = 0x81;
static uint8_t s_ls_dirty = 1;
static uint8_t s_attract_idx = 0;
static uint8_t s_cheat_ls = 0;
static uint8_t s_cheat_dbg = 0;
static uint8_t s_cheat_jp = 0;
static uint8_t s_cred_idx = 0;

static const char *const s_cred_en_role[9] = {
    "SONIC THE HEDGEHOG  ",
    "EXECUTIVE PRODUCER  ",
    "PROJECT MANAGER     ",
    "PRODUCER            ",
    "PROGRAM             ",
    "CHARACTER DESIGN    ",
    "ZONE DESIGN         ",
    "SOUND PRODUCE       ",
    "PRESENTED BY SEGA   "
};
static const char *const s_cred_en_name[9] = {
    "STAFF CREDITS       ",
    "HAYAO NAKAYAMA      ",
    "HISASHI SUZUKI      ",
    "SHINOBU TOYODA      ",
    "YUJI NAKA           ",
    "NAOTO OHSHIMA       ",
    "HIROKAZU YASUHARA   ",
    "MASATO NAKAMURA     ",
    "THANK YOU FOR PLAY! "
};
static const char *const s_cred_jp_name[9] = {
    "JP SECRET CREDITS!  ",
    "HAYAO NAKAYAMA      ",
    "HISASHI SUZUKI      ",
    "SHINOBU TOYODA      ",
    "YU2  (YUJI NAKA)    ",
    "BIG ISLAND (OHSHIMA)",
    "CAROL YAS (YASUHARA)",
    "DREAMS COME TRUE +BO",
    "SONIC TEAM 1991 JP! "
};

/* Live 4KB RAM copy of active `64x64` Special Stage Maze (`sslayout/1..6.eni`) */
static uint8_t s_ss_ram[4096];
static int16_t s_ss_px = 0;
static int16_t s_ss_py = 0;
static int16_t s_ss_vx = 0;
static int16_t s_ss_vy = 0;
static int16_t s_ss_rot_spd = 56;

static const char *const s_ls_names[23] = {
    "GREEN HILL 1", "GREEN HILL 2", "GREEN HILL 3",
    "MARBLE ZONE 1","MARBLE ZONE 2","MARBLE ZONE 3",
    "SPRING YARD 1","SPRING YARD 2","SPRING YARD 3",
    "LABYRINTH 1  ","LABYRINTH 2  ","LABYRINTH 3  ",
    "STAR LIGHT 1 ","STAR LIGHT 2 ","STAR LIGHT 3 ",
    "SCRAP BRAIN 1","SCRAP BRAIN 2","SCRAP BRAIN 3",
    "FINAL ZONE   ","SPECIAL STAGE","SOUND TEST   ",
    "ENDING DEMO  ","TITLE SCREEN "
};

static const uint16_t s_ui_pal[8] = {
    RGB8(16, 32, 108), RGB8(228, 28, 24), RGB8(252, 216, 0),  RGB8(252, 252, 252),
    RGB8(16, 32, 108), RGB8(64, 176, 252),RGB8(252, 216, 0),  RGB8(252, 252, 252)
};

static void write_u32_win(uint8_t x_right, uint8_t y, uint32_t val, uint8_t width) {
    char buf[12];
    uint8_t i;
    buf[width] = '\0';
    for (i = 0; i < width; i++) {
        buf[width - 1 - i] = (char)('0' + (val % 10));
        val /= 10;
    }
    hw_print_win(x_right - width + 1, y, buf);
}

static void write_hex16(char *dst, uint16_t v) {
    static const char hx[] = "0123456789ABCDEF";
    dst[0] = hx[(v >> 12) & 0xF];
    dst[1] = hx[(v >> 8) & 0xF];
    dst[2] = hx[(v >> 4) & 0xF];
    dst[3] = hx[v & 0xF];
}

/* ============================================================================================
 * REAL-TIME 360° ROTATING CHAOS EMERALD SPECIAL STAGE RASTERIZER (`09 Sonic in Special Stage.asm`)
 * Uses incremental fixed-point row scanning (`u += cos_a, v -= sin_a` — ZERO inner multiplies!)
 * ============================================================================================ */
static uint8_t ss_is_solid(uint8_t c) {
    return (c >= 0x01 && c <= 0x28) || (c >= 0x29 && c <= 0x30) || (c >= 0x41 && c <= 0x4B);
}

static uint8_t ss_cell_tile(uint8_t c) {
    if (c == 0) return SS_TILE_BG;
    if (c >= 0x01 && c <= 0x24) return (uint8_t)(SS_TILE_WALL + ((c - 1) & 3));
    if (c == 0x25) return SS_TILE_RING;      /* Pinball Bumper */
    if (c == 0x27) return SS_TILE_GOAL;      /* GOAL Sphere */
    if (c == 0x28) return SS_TILE_EMERALD;   /* 1UP Sphere */
    if (c == 0x29 || c == 0x2A) return SS_TILE_UPDOWN;
    if (c == 0x2B) return SS_TILE_RBLOCK;
    if (c >= 0x2D && c <= 0x30) return SS_TILE_GLASS;
    if (c == 0x3A) return SS_TILE_RING;
    if (c >= 0x3B && c <= 0x40) return SS_TILE_EMERALD;
    return SS_TILE_WALL;
}

static uint8_t ss_cell_pal(uint8_t c) {
    if (c == 0) return 0;
    if (c >= 0x3B && c <= 0x40) return 6; /* Prismatic Chaos Emerald */
    if (c == 0x3A || c == 0x25) return 3; /* Gold Ring / Bumper */
    if (c == 0x27 || c == 0x29 || c == 0x2A) return 4; /* Crimson GOAL / UP-DOWN */
    if (c == 0x2B || (c >= 0x2D && c <= 0x30)) return 5; /* Cyan Crystal Glass / R */
    return (c & 1) ? 1 : 2; /* Alternating Pink / Emerald Maze Walls! */
}

static void ss_render_rotating_viewport(void) {
    uint8_t rot_b = (uint8_t)(g_ss_rot_angle >> 8);
    int16_t cos_a = hw_cos(rot_b);
    int16_t sin_a = hw_sin(rot_b);
    /* `s_ss_px, s_ss_py` are `.4` cell units (`16` px per cell -> `cell << 8 = px << 4`)! */
    int16_t center_u = s_ss_px << 4;
    int16_t center_v = s_ss_py << 4;
    uint8_t rx, ry;
    static uint8_t row_t[20];
    static uint8_t row_a[20];

    for (ry = 0; ry < 16; ry++) {
        int8_t dy = (int8_t)ry - 8;
        int16_t u = center_u - (cos_a * 10) + (sin_a * dy);
        int16_t v = center_v + (sin_a * 10) + (cos_a * dy);

        for (rx = 0; rx < 20; rx++) {
            uint8_t c = s_ss_ram[(((uint16_t)((uint8_t)(v >> 8) & 63)) << 6) | ((uint8_t)(u >> 8) & 63)];
            row_t[rx] = ss_cell_tile(c);
            row_a[rx] = ss_cell_pal(c);
            u += cos_a;
            v -= sin_a;
        }
        VBK_REG = 1;
        set_bkg_tiles(0, ry, 20, 1, row_a);
        VBK_REG = 0;
        set_bkg_tiles(0, ry, 20, 1, row_t);
    }
}

/* ============================================================================================
 * MODE INITIALIZER (`screens_init_mode`)
 * ============================================================================================ */
void screens_init_mode(uint8_t mode) BANKED {
    s_mode_timer = 0;
    g_oam_next = 0;
    hw_clear_remaining_oam();

    if (mode == GM_SEGA) {
        if (g_frame_counter > 0) {
            hw_load_fullscreen_screen(0);
        }
    } else if (mode == GM_TITLE) {
        s_cheat_ls = 0;
        s_cheat_dbg = 0;
        s_cheat_jp = 0;
        hw_load_fullscreen_screen(1);
        hw_play_bgm(BGM_TITLE);
    } else if (mode == GM_LEVEL_SELECT) {
        s_ls_sel = g_cur_act < 19 ? g_cur_act : 0;
        s_ls_top = 0;
        s_ls_dirty = 2; /* Single clean initial paint */
        set_bkg_palette(7, 1, s_ui_pal);
        hw_fill_win_rect(0, 0, 20, 18, 80);
        hw_fill_win_rect(0, 0, 20, 1, 126);
        hw_fill_win_rect(1, 2, 18, 1, 82);
        hw_fill_win_rect(0, 17, 20, 1, 126);
        WX_REG = 7;
        WY_REG = 0;
        hw_play_bgm(BGM_TITLE);
    } else if (mode == GM_LEVEL) {
        g_demo_pos = 0;
        g_demo_btn = 0;
        g_demo_btn_timer = 0;
        hw_load_act(g_cur_act);
    } else if (mode == GM_ACT_CLEAR) {
        uint16_t secs = g_level_frames / 60;
        if (secs < 30) g_time_bonus = 50000U;
        else if (secs < 45) g_time_bonus = 10000U;
        else if (secs < 60) g_time_bonus = 5000U;
        else if (secs < 90) g_time_bonus = 4000U;
        else if (secs < 120) g_time_bonus = 3000U;
        else if (secs < 180) g_time_bonus = 2000U;
        else g_time_bonus = 1000U;
        g_ring_bonus = g_rings * 100U;

        set_bkg_palette(7, 1, s_ui_pal);
        hw_show_transition_card("SONIC GOT THRU", s_ls_names[g_cur_act < 19 ? g_cur_act : 0], 0);
        hw_print_win(2, 13, "SCORE");
        hw_print_win(2, 14, "TIME BONUS");
        hw_print_win(2, 15, "RING BONUS");
        hw_play_bgm(BGM_ACTCLEAR);
    } else if (mode == GM_SPECIAL_STAGE) {
        hw_load_special_stage_vram(g_cur_ss, &s_ss_px, &s_ss_py);
        hw_copy_ss_maze(g_cur_ss, s_ss_ram);
        g_rings = 0;
        g_ss_got_emerald = 0;
        g_ss_rot_angle = 0;
        s_ss_rot_spd = 56;
        s_ss_vx = 0;
        s_ss_vy = 0;
        hw_play_bgm(BGM_SS);
        ss_render_rotating_viewport();
    } else if (mode == GM_SS_RESULTS) {
        uint8_t e, cnt = 0;
        char em_buf[21];
        for (e = 0; e < 6; e++) if (g_emeralds & (1 << e)) cnt++;
        g_ring_bonus = g_rings * 100U;
        set_bkg_palette(7, 1, s_ui_pal);
        hw_show_transition_card(
            (cnt >= 6) ? "GOT THEM ALL!" : (g_ss_got_emerald ? "CHAOS EMERALD!" : "SPECIAL STAGE"),
            g_ss_got_emerald ? "SONIC GOT ONE" : "RESULTS",
            0
        );
        memcpy(em_buf, "EMERALDS  $$$$$$    ", 20);
        em_buf[20] = '\0';
        for (e = 0; e < 6; e++) {
            em_buf[10 + e] = (g_emeralds & (1 << e)) ? '$' : '-';
        }
        hw_print_win(1, 13, em_buf);
        hw_print_win(2, 14, "SCORE");
        hw_print_win(2, 15, "RING BONUS");
        hw_play_bgm(BGM_ACTCLEAR);
        sram_save_progress();
    } else if (mode == GM_CONTINUE) {
        set_bkg_palette(7, 1, s_ui_pal);
        hw_show_transition_card("CONTINUE", "PRESS START", 0);
        hw_play_bgm(BGM_CONTINUE);
    } else if (mode == GM_GAME_OVER) {
        sram_save_progress();
        set_bkg_palette(7, 1, s_ui_pal);
        hw_show_transition_card("GAME OVER", "TRY AGAIN", 0);
        hw_play_bgm(BGM_GAMEOVER);
    } else if (mode == GM_ENDING) {
        sram_save_progress();
        g_cur_act = 19; /* Authentic Green Hill Blooming Flowers Ending Zone! */
        hw_load_act(19);
        hw_play_bgm(BGM_ENDING);
        if ((g_emeralds & 0x3F) == 0x3F) {
            hw_print_win(0, 0, "ALL 6 EMERALDS OK!  ");
            hw_print_win(0, 1, "THANK YOU FOR PLAY! ");
        } else {
            hw_print_win(0, 0, "SONIC THE HEDGEHOG  ");
            hw_print_win(0, 1, "EGGMAN: TRY AGAIN!  ");
        }
    } else if (mode == GM_CREDITS) {
        s_cred_idx = 0;
        sram_save_progress();
        hw_load_fullscreen_screen(2); /* Real converted Sonic 1 Ending/Credits Montage! */
        set_bkg_palette(7, 1, s_ui_pal);
        hw_print_win(0, 0, s_cred_en_role[0]);
        hw_print_win(0, 1, g_jp_credits ? s_cred_jp_name[0] : s_cred_en_name[0]);
        WX_REG = 7;
        WY_REG = 128;
        hw_play_bgm(BGM_CREDITS);
    } else if (mode == GM_TRY_AGAIN) {
        set_bkg_palette(7, 1, s_ui_pal);
        if ((g_emeralds & 0x3F) == 0x3F) {
            hw_show_transition_card("SONIC THE HEDGEHOG", "END", 0);
        } else {
            hw_show_transition_card("TRY AGAIN", "COLLECT 6 EMERALDS", 0);
        }
    }
}

/* ============================================================================================
 * PER-FRAME MODE STEP (`screens_update_mode`)
 * ============================================================================================ */
void screens_update_mode(uint8_t mode) BANKED {
    static const uint8_t s_attract_acts[4] = { 0, 3, 6, 12 }; /* GHZ1 -> MZ1 -> SYZ1 -> SLZ1 */
    static const uint8_t s_ls_code[4]  = { J_UP, J_DOWN, J_LEFT, J_RIGHT };
    static const uint8_t s_dbg_code[8] = { J_UP, J_B, J_DOWN, J_B, J_LEFT, J_B, J_RIGHT, J_B };
    static const uint8_t s_jp_code[5]  = { J_DOWN, J_DOWN, J_DOWN, J_LEFT, J_RIGHT };

    /* 1. SEGA Boot Screen + Real Wave-RAM Voice Chant -> Opens Directly into Level Select at Start of Game! */
    if (mode == GM_SEGA) {
        if (s_mode_timer < PCM_SEGA_PACKETS) {
            hw_play_sega_chant_step(s_mode_timer);
        }
        s_mode_timer++;
        if ((g_joy_pressed & (J_START | J_A | J_SELECT)) || s_mode_timer >= (PCM_SEGA_PACKETS + 24)) {
            g_cur_act = 0;
            g_next_mode = GM_LEVEL_SELECT;
        }
        return;
    }

    /* 2. Animated Title Screen + Cheat Codes + 4-Zone Attract Mode Cycle (`START`/`A`/`SELECT` -> Level Select) */
    if (mode == GM_TITLE) {
        s_mode_timer++;
        if (g_joy_pressed) {
            if (g_joy_pressed == s_ls_code[s_cheat_ls]) {
                if (++s_cheat_ls == 4) {
                    s_cheat_ls = 0;
                    hw_play_sfx(SFX_RING);
                    g_next_mode = GM_LEVEL_SELECT;
                    return;
                }
            } else if (!(g_joy_pressed & (J_START | J_A))) {
                s_cheat_ls = 0;
            }
            if (g_joy_pressed == s_dbg_code[s_cheat_dbg]) {
                if (++s_cheat_dbg == 8) {
                    s_cheat_dbg = 0;
                    g_debug_unlocked = 1;
                    hw_play_sfx(SFX_GIANTRING);
                }
            } else if (!(g_joy_pressed & J_START)) {
                s_cheat_dbg = 0;
            }
            /* Authentic Sonic 1 Japanese Credits Code (`DOWN, DOWN, DOWN, LEFT, RIGHT` -> `8C` JP pseudonyms!) */
            if (g_joy_pressed == s_jp_code[s_cheat_jp]) {
                if (++s_cheat_jp == 5) {
                    s_cheat_jp = 0;
                    g_jp_credits = !g_jp_credits;
                    sram_save_progress();
                    hw_play_sfx(SFX_ENTERSPEC);
                }
            } else if (!(g_joy_pressed & (J_START | J_A))) {
                s_cheat_jp = 0;
            }
        }
        if (g_joy_pressed & (J_START | J_A | J_SELECT)) {
            if ((g_joy & J_B) && (g_joy & J_A)) g_debug_unlocked = 1;
            hw_play_sfx(SFX_RING);
            g_next_mode = GM_LEVEL_SELECT;
            return;
        }
        if (s_mode_timer >= 420) {
            g_demo_mode = 1;
            g_demo_timer = 540;
            g_cur_act = s_attract_acts[s_attract_idx & 3];
            s_attract_idx = (s_attract_idx + 1) & 3;
            g_checkpoint_active = 0;
            g_next_mode = GM_LEVEL;
        }
        return;
    }

    /* 3. Start-of-Game Interactive Level Select + Special Stage Picker + 68-Track Sound Test + Debug Toggle */
    if (mode == GM_LEVEL_SELECT) {
        uint8_t prev_sel = s_ls_sel;
        if (g_joy_pressed & J_UP) {
            s_ls_sel = (s_ls_sel == 0) ? 22 : (s_ls_sel - 1);
            hw_play_sfx(SFX_SWITCH);
            if (!s_ls_dirty) s_ls_dirty = 1;
        } else if (g_joy_pressed & J_DOWN) {
            s_ls_sel = (s_ls_sel + 1) % 23;
            hw_play_sfx(SFX_SWITCH);
            if (!s_ls_dirty) s_ls_dirty = 1;
        } else if (s_ls_sel < 19 && (g_joy_pressed & (J_LEFT | J_RIGHT))) {
            if (g_joy_pressed & J_RIGHT) {
                s_ls_sel = (uint8_t)((s_ls_sel + 3 <= 18) ? (s_ls_sel + 3) : 0);
            } else {
                s_ls_sel = (uint8_t)((s_ls_sel >= 3) ? (s_ls_sel - 3) : 18);
            }
            hw_play_sfx(SFX_SWITCH);
            if (!s_ls_dirty) s_ls_dirty = 1;
        } else if (s_ls_sel == 19 && (g_joy_pressed & (J_LEFT | J_RIGHT))) {
            if (g_joy_pressed & J_RIGHT) g_cur_ss = (g_cur_ss + 1) % 6;
            else g_cur_ss = (g_cur_ss == 0) ? 5 : (g_cur_ss - 1);
            hw_play_sfx(SFX_SWITCH);
            s_ls_dirty = 2;
        } else if (g_joy_pressed & J_SELECT) {
            g_debug_unlocked = !g_debug_unlocked;
            hw_play_sfx(SFX_GIANTRING);
            s_ls_dirty = 2;
        } else if (s_ls_sel != 20 && (g_joy_pressed & J_B)) {
            g_rev_mode = !g_rev_mode;
            sram_save_progress();
            hw_play_sfx(SFX_SWITCH);
            s_ls_dirty = 2;
        } else if (s_ls_sel == 20 && (g_joy_pressed & (J_LEFT | J_RIGHT | J_A | J_B))) {
            if (g_joy_pressed & J_LEFT) s_ls_sound = (s_ls_sound <= 0x81) ? 0xD0 : (s_ls_sound - 1);
            if (g_joy_pressed & J_RIGHT) s_ls_sound = (s_ls_sound >= 0xD0) ? 0x81 : (s_ls_sound + 1);
            if (g_joy_pressed & (J_A | J_B)) {
                if (s_ls_sound <= 0x93) hw_play_bgm(s_ls_sound);
                else hw_play_sfx(s_ls_sound);
            }
            s_ls_dirty = 2;
        } else if (g_joy_pressed & (J_START | J_A)) {
            g_demo_mode = 0;
            g_checkpoint_active = 0;
            g_score = 0;
            g_rings = 0;
            g_lives = 3;
            hw_play_sfx(SFX_RING);
            if (s_ls_sel < 19) {
                g_cur_act = s_ls_sel;
                g_next_mode = GM_LEVEL;
                return;
            } else if (s_ls_sel == 19) {
                g_next_mode = GM_SPECIAL_STAGE;
                return;
            } else if (s_ls_sel == 21) {
                g_next_mode = GM_ENDING;
                return;
            } else if (s_ls_sel == 22) {
                g_next_mode = GM_TITLE;
                return;
            }
        }

        if (s_ls_dirty) {
            uint8_t old_top = s_ls_top;
            uint8_t full = (s_ls_dirty >= 2);
            s_ls_dirty = 0;
            if (s_ls_sel < s_ls_top) s_ls_top = s_ls_sel;
            if (s_ls_sel >= s_ls_top + 12) s_ls_top = s_ls_sel - 11;
            if (s_ls_top != old_top || prev_sel == 20 || s_ls_sel == 20) full = 1;

            if (!full) {
                /* Ultra-fast 0.04ms single-frame cursor update so rapid D-pad taps NEVER drop! */
                hw_print_win(1, (uint8_t)(3 + (prev_sel - s_ls_top)), " ");
                hw_print_win(1, (uint8_t)(3 + (s_ls_sel - s_ls_top)), ">");
            } else {
                uint8_t r;
                char sline[21];
                if (g_debug_unlocked) {
                    hw_print_win(1, 1, g_rev_mode ? "LEVEL SELECT DBG R0" : "LEVEL SELECT DBG ON");
                } else if (g_rev_mode) {
                    hw_print_win(1, 1, "LEVEL SELECT REV00 ");
                } else {
                    hw_print_win(1, 1, "SONIC LEVEL SELECT ");
                }
                for (r = 0; r < 12; r++) {
                    uint8_t idx = s_ls_top + r;
                    hw_print_win(1, 3 + r, (idx == s_ls_sel) ? ">" : " ");
                    if (idx == 19) {
                        char ss_buf[16];
                        memcpy(ss_buf, "SPECIAL STG 1", 13);
                        ss_buf[12] = (char)('1' + (g_cur_ss % 6));
                        ss_buf[13] = '\0';
                        hw_print_win(3, 3 + r, ss_buf);
                    } else {
                        hw_print_win(3, 3 + r, s_ls_names[idx]);
                    }
                }
                if (s_ls_sel == 20) {
                    memcpy(sline, "SOUND 81  A:PLAY   ", 20);
                    sline[19] = '\0';
                    sline[6] = "0123456789ABCDEF"[(s_ls_sound >> 4) & 0xF];
                    sline[7] = "0123456789ABCDEF"[s_ls_sound & 0xF];
                    hw_print_win(1, 16, sline);
                } else {
                    hw_print_win(1, 16, "UD:LVL LR:ZN A:PLAY");
                }
            }
        }
        return;
    }

    /* 4. Live Half-Scale Level Gameplay (`GM_LEVEL`) */
    if (mode == GM_LEVEL) {
        g_oam_next = 0;
        physics_update_level();
        if (g_next_mode != GM_LEVEL) {
            hw_clear_remaining_oam();
            return;
        }
        SCX_REG = (uint8_t)(g_cam_x >> 1);
        SCY_REG = (uint8_t)(g_cam_y >> 1);
        objects_update_and_draw();
        hw_clear_remaining_oam();

        if (g_debug_active) {
            char dbg_row[21];
            memcpy(dbg_row, "DBG X0000 Y0000 ITM0", 20);
            dbg_row[20] = '\0';
            write_hex16(&dbg_row[5], (uint16_t)g_sonic.x);
            write_hex16(&dbg_row[11], (uint16_t)g_sonic.y);
            dbg_row[19] = (char)('0' + (g_debug_item & 7));
            hw_print_win(0, 0, dbg_row);
        } else if (g_hud_dirty && !g_paused) {
            g_hud_dirty = 0;
            hw_update_hud();
        }
        return;
    }

    /* 5. Act Clear Bonus Tally (`GM_ACT_CLEAR`) */
    if (mode == GM_ACT_CLEAR) {
        if (g_time_bonus > 0 || g_ring_bonus > 0) {
            if (g_time_bonus >= 1000) { g_time_bonus -= 500; add_score_points(500); }
            else if (g_time_bonus >= 100) { g_time_bonus -= 100; add_score_points(100); }
            else if (g_time_bonus > 0) { add_score_points(g_time_bonus); g_time_bonus = 0; }
            if (g_ring_bonus >= 1000) { g_ring_bonus -= 500; add_score_points(500); }
            else if (g_ring_bonus >= 100) { g_ring_bonus -= 100; add_score_points(100); }
            else if (g_ring_bonus > 0) { add_score_points(g_ring_bonus); g_ring_bonus = 0; }
            if (g_joy_pressed & (J_START | J_A)) {
                add_score_points(g_time_bonus + g_ring_bonus);
                g_time_bonus = 0;
                g_ring_bonus = 0;
            }
            if (g_time_bonus == 0 && g_ring_bonus == 0) {
                hw_play_sfx(SFX_CASH);
                sram_save_progress();
            }
            write_u32_win(17, 13, g_score, 7);
            write_u32_win(17, 14, g_time_bonus, 5);
            write_u32_win(17, 15, g_ring_bonus, 5);
        } else {
            if (s_mode_timer == 0) {
                write_u32_win(17, 13, g_score, 7);
                write_u32_win(17, 14, 0, 5);
                write_u32_win(17, 15, 0, 5);
            }
            s_mode_timer++;
            if (s_mode_timer >= 72 || (g_joy_pressed & (J_START | J_A))) {
                g_checkpoint_active = 0;
                if (g_cur_act < 18) {
                    g_cur_act++;
                    g_next_mode = GM_LEVEL;
                } else {
                    g_next_mode = GM_ENDING;
                }
                return;
            }
        }
        return;
    }

    /* 6. Real-Time 360° Rotating Special Stage (`GM_SPECIAL_STAGE`) */
    if (mode == GM_SPECIAL_STAGE) {
        uint8_t rot_b;
        int16_t grav_x, grav_y, steer_x, steer_y;
        uint8_t cx, cy, hit;

        if (g_joy_pressed & J_SELECT) {
            g_next_mode = GM_LEVEL_SELECT;
            return;
        }

        /* Step Continuous 360° Maze Rotation! */
        g_ss_rot_angle = (uint16_t)((int32_t)g_ss_rot_angle + s_ss_rot_spd);
        rot_b = (uint8_t)(g_ss_rot_angle >> 8);
        grav_x  = (hw_sin(rot_b) * 18) >> 8;
        grav_y  = (hw_cos(rot_b) * 18) >> 8;
        steer_x = (hw_cos(rot_b) * 24) >> 8;
        steer_y = (-hw_sin(rot_b) * 24) >> 8;

        if (g_joy & J_LEFT)  { s_ss_vx -= steer_x; s_ss_vy -= steer_y; }
        if (g_joy & J_RIGHT) { s_ss_vx += steer_x; s_ss_vy += steer_y; }
        if (g_joy_pressed & (J_A | J_B)) {
            s_ss_vx -= grav_x * 22;
            s_ss_vy -= grav_y * 22;
            hw_play_sfx(SFX_JUMP);
        }

        s_ss_vx += grav_x;
        s_ss_vy += grav_y;
        if (s_ss_vx > 0x0380) s_ss_vx = 0x0380;
        if (s_ss_vx < -0x0380) s_ss_vx = -0x0380;
        if (s_ss_vy > 0x0380) s_ss_vy = 0x0380;
        if (s_ss_vy < -0x0380) s_ss_vy = -0x0380;

        /* X-axis collision against `64x64` Special Stage Grid (`16` sub-units per cell) */
        {
            int16_t nx = s_ss_px + (s_ss_vx >> 8);
            cx = (uint8_t)((nx >> 4) & 63);
            cy = (uint8_t)((s_ss_py >> 4) & 63);
            hit = s_ss_ram[((uint16_t)cy << 6) | cx];
            if (ss_is_solid(hit)) {
                if (hit == 0x25) { s_ss_vx = (s_ss_vx < 0) ? 0x0480 : -0x0480; hw_play_sfx(SFX_BUMPER); }
                else if (hit == 0x27) { hw_play_sfx(SFX_GOAL); g_next_mode = GM_SS_RESULTS; return; }
                else if (hit == 0x28) { s_ss_ram[((uint16_t)cy << 6) | cx] = 0; g_lives++; g_hud_dirty = 1; hw_play_bgm(BGM_EXTRALIFE); }
                else if (hit == 0x29) { s_ss_rot_spd = (s_ss_rot_spd >= 0) ? 96 : -96; s_ss_vx = -s_ss_vx; hw_play_sfx(SFX_SWITCH); }
                else if (hit == 0x2A) { s_ss_rot_spd = (s_ss_rot_spd >= 0) ? 32 : -32; s_ss_vx = -s_ss_vx; hw_play_sfx(SFX_SWITCH); }
                else if (hit == 0x2B) { s_ss_rot_spd = -s_ss_rot_spd; s_ss_vx = -s_ss_vx; hw_play_sfx(SFX_SPRING); }
                else if (hit >= 0x2D && hit <= 0x30) {
                    s_ss_ram[((uint16_t)cy << 6) | cx] = (hit == 0x30) ? 0 : (hit + 1);
                    s_ss_vx = -s_ss_vx;
                    hw_play_sfx(SFX_BREAK);
                } else {
                    s_ss_vx = -(s_ss_vx >> 1);
                }
            } else {
                s_ss_px = nx;
            }
        }
        /* Y-axis collision against `64x64` Special Stage Grid */
        {
            int16_t ny = s_ss_py + (s_ss_vy >> 8);
            cx = (uint8_t)((s_ss_px >> 4) & 63);
            cy = (uint8_t)((ny >> 4) & 63);
            hit = s_ss_ram[((uint16_t)cy << 6) | cx];
            if (ss_is_solid(hit)) {
                if (hit == 0x25) { s_ss_vy = (s_ss_vy < 0) ? 0x0480 : -0x0480; hw_play_sfx(SFX_BUMPER); }
                else if (hit == 0x27) { hw_play_sfx(SFX_GOAL); g_next_mode = GM_SS_RESULTS; return; }
                else if (hit == 0x2B) { s_ss_rot_spd = -s_ss_rot_spd; s_ss_vy = -s_ss_vy; hw_play_sfx(SFX_SPRING); }
                else if (hit >= 0x2D && hit <= 0x30) {
                    s_ss_ram[((uint16_t)cy << 6) | cx] = (hit == 0x30) ? 0 : (hit + 1);
                    s_ss_vy = -s_ss_vy;
                    hw_play_sfx(SFX_BREAK);
                } else {
                    s_ss_vy = -(s_ss_vy >> 1);
                }
            } else {
                s_ss_py = ny;
            }
        }

        /* Collect Special Stage Rings & Chaos Emeralds */
        cx = (uint8_t)((s_ss_px >> 4) & 63);
        cy = (uint8_t)((s_ss_py >> 4) & 63);
        hit = s_ss_ram[((uint16_t)cy << 6) | cx];
        if (hit == 0x3A) {
            s_ss_ram[((uint16_t)cy << 6) | cx] = 0;
            g_rings++;
            g_score += 10;
            g_hud_dirty = 1;
            if (g_rings == 50) {
                g_continues++;
                hw_play_bgm(BGM_CONTINUE);
            } else {
                hw_play_sfx(SFX_RING);
            }
        } else if (hit >= 0x3B && hit <= 0x40) {
            s_ss_ram[((uint16_t)cy << 6) | cx] = 0;
            g_emeralds |= (uint8_t)(1 << (g_cur_ss % 6));
            g_ss_got_emerald = 1;
            g_cur_ss = (g_cur_ss + 1) % 6;
            hw_play_bgm(BGM_EMERALD);
            g_next_mode = GM_SS_RESULTS;
            return;
        }

        ss_render_rotating_viewport();

        /* Render Spinning Half-Scale Sonic at Playfield Center `(80, 64)`! */
        g_oam_next = 0;
        g_sonic.x = 160;
        g_sonic.y = 128;
        g_cam_x = 0;
        g_cam_y = 0;
        g_sonic.anim = ANI_ROLL;
        hw_step_sonic_anim();
        hw_draw_sonic_sprite();
        hw_clear_remaining_oam();

        if (g_hud_dirty) {
            g_hud_dirty = 0;
            hw_update_hud();
        }
        return;
    }

    /* 7. Special Stage Results Screen (`GM_SS_RESULTS` — `7E Special Stage Results.asm`) */
    if (mode == GM_SS_RESULTS) {
        if (g_ring_bonus > 0) {
            if (g_ring_bonus >= 100) { g_ring_bonus -= 100; g_score += 100; }
            else { g_score += g_ring_bonus; g_ring_bonus = 0; }
            if (g_joy_pressed & (J_START | J_A)) {
                g_score += g_ring_bonus;
                g_ring_bonus = 0;
            }
            if (g_ring_bonus == 0) hw_play_sfx(SFX_CASH);
        } else {
            s_mode_timer++;
            if (s_mode_timer >= 90 || (g_joy_pressed & (J_START | J_A))) {
                g_checkpoint_active = 0;
                if (g_cur_act < 18) g_cur_act++;
                g_next_mode = GM_LEVEL;
                return;
            }
        }
        write_u32_win(17, 14, g_score, 7);
        write_u32_win(17, 15, g_ring_bonus, 5);
        return;
    }

    /* 8. Continue / Game Over / Ending Sequence / Staff Credits */
    if (mode == GM_CONTINUE) {
        char cbuf[2];
        s_mode_timer++;
        cbuf[0] = (char)('9' - ((s_mode_timer / 60) % 10));
        cbuf[1] = '\0';
        hw_print_win(9, 13, cbuf);
        if (g_joy_pressed & (J_START | J_A)) g_next_mode = GM_LEVEL;
        else if (s_mode_timer >= 600) g_next_mode = GM_GAME_OVER;
        return;
    }

    if (mode == GM_GAME_OVER) {
        s_mode_timer++;
        if (s_mode_timer >= 180 || (g_joy_pressed & (J_START | J_A))) {
            g_next_mode = GM_TITLE;
        }
        return;
    }

    if (mode == GM_ENDING) {
        int16_t old_cx = g_cam_x;
        s_mode_timer++;
        g_oam_next = 0;
        if (s_mode_timer < 180) {
            g_sonic.x += 2;
            g_cam_x += 2;
            hw_stream_viewport_delta(old_cx, g_cam_y);
            SCX_REG = (uint8_t)(g_cam_x >> 1);
            g_sonic.anim = ANI_RUN;
        } else {
            g_sonic.anim = ANI_WAIT;
            if ((g_emeralds & 0x3F) != 0x3F) {
                /* Eggman juggling uncollected Chaos Emeralds (`8B Try Again & End Eggman.asm`)! */
                hw_put_sprite_cols2rows(110, 60 - ((s_mode_timer >> 2) & 3), ZSPR_BOSS, 4u, 0x08 | 1);
            }
        }
        hw_step_palette_cycle();
        hw_step_sonic_anim();
        hw_draw_sonic_sprite();
        hw_clear_remaining_oam();
        if (s_mode_timer >= 260 || (g_joy_pressed & (J_START | J_A))) {
            g_next_mode = GM_CREDITS;
        }
        return;
    }

    if (mode == GM_CREDITS) {
        s_mode_timer++;
        if (s_mode_timer >= 44 || (g_joy_pressed & J_B)) {
            s_mode_timer = 0;
            s_cred_idx++;
            if (s_cred_idx >= 9) {
                g_next_mode = GM_TRY_AGAIN;
                return;
            }
            hw_print_win(0, 0, s_cred_en_role[s_cred_idx]);
            hw_print_win(0, 1, g_jp_credits ? s_cred_jp_name[s_cred_idx] : s_cred_en_name[s_cred_idx]);
        }
        if (g_joy_pressed & (J_START | J_A)) {
            g_next_mode = GM_TRY_AGAIN;
        }
        return;
    }

    /* 9. Authentic Post-Credits `"END"` vs `"TRY AGAIN"` Screen (`8B Try Again & End Eggman.asm`) */
    if (mode == GM_TRY_AGAIN) {
        s_mode_timer++;
        g_oam_next = 0;
        if ((g_emeralds & 0x3F) != 0x3F) {
            /* Dr. Eggman juggling missing Chaos Emeralds! */
            uint8_t ang = (uint8_t)(s_mode_timer << 2);
            hw_put_sprite_cols2rows(64, 88, ZSPR_BOSS, 4u, 0x08 | 1);
            hw_put_sprite8(76 + ((hw_cos(ang) * 14) >> 8), 80 - ((hw_sin(ang) > 0) ? (hw_sin(ang) >> 4) : 0),
                           SPR_TILE_STARS, 2);
            hw_put_sprite8(76 - ((hw_cos(ang) * 14) >> 8), 80 - ((hw_sin((uint8_t)(ang + 128)) > 0) ? (hw_sin((uint8_t)(ang + 128)) >> 4) : 0),
                           SPR_TILE_STARS + 1, 1);
        } else {
            /* Celebratory Chaos Emerald Halo around `"SONIC THE HEDGEHOG — END"`! */
            uint8_t e;
            for (e = 0; e < 6; e++) {
                uint8_t ang = (uint8_t)((s_mode_timer << 1) + e * 42);
                hw_put_sprite8(76 + ((hw_cos(ang) * 20) >> 8), 92 + ((hw_sin(ang) * 10) >> 8),
                               SPR_TILE_STARS + (e & 3), (e & 3) + 1);
            }
        }
        hw_clear_remaining_oam();
        if (s_mode_timer >= 180 || (g_joy_pressed & (J_START | J_A))) {
            g_next_mode = GM_TITLE;
        }
    }
}
