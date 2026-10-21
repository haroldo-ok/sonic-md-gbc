#pragma bank 123
#include "engine.h"

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
    DISABLE_RAM;
}

void sram_save_progress(void) BANKED {
    SramSaveBlock *sram = (SramSaveBlock *)0xA000;
    if (g_score > g_high_score) g_high_score = g_score;
    if (g_cur_act > g_unlocked_act && g_cur_act < NUM_PLAYABLE_ACTS) {
        g_unlocked_act = g_cur_act;
    }
    ENABLE_RAM;
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
    DISABLE_RAM;
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

/* Authentic `_inc/Oscillatory Routines.asm` (`OscillateNumInit` & `OscillateNumDo`) */
static const uint8_t s_osc_freq[16]  = { 2, 2, 2, 2, 4, 8, 8, 4, 2, 2, 2, 3, 5, 7, 2, 2 };
static const uint16_t s_osc_limit[16] = {
    0x10, 0x18, 0x20, 0x30, 0x20, 0x08, 0x40, 0x40,
    0x50, 0x50, 0x20, 0x30, 0x50, 0x70, 0x10, 0x10
};

void osc_reset(void) BANKED {
    g_osc_dir = 0;
    hw_init_osc_from_rom(g_osc_val);
}

void osc_step_all(void) BANKED {
    uint8_t i;
    if (g_paused) return;
    for (i = 0; i < 16; i++) {
        uint8_t step = s_osc_freq[i];
        uint16_t lim = s_osc_limit[i];
        uint16_t bit = (uint16_t)1U << i;
        if (!(g_osc_dir & bit)) {
            g_osc_val[i] += step;
            if (g_osc_val[i] >= lim) g_osc_dir |= bit;
        } else {
            if (g_osc_val[i] > step) g_osc_val[i] -= step;
            else {
                g_osc_val[i] = 0;
                g_osc_dir &= (uint16_t)~bit;
            }
        }
    }
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
    } else if (g_cur_act == 17 && g_cam_x >= 0x0D00 && g_sonic.y < 0x0040) {
        g_checkpoint_active = 0;
        g_cur_act = 18; /* Authentic `DLE_SBZ3`: Direct seamless entry into Final Zone! */
        g_next_mode = GM_LEVEL;
        return;
    }

    /* 4. Complete Authentic `DynamicLevelEvents.asm` Dynamic Bottom Camera Boundaries across ALL 20 Acts! */
    if (g_cur_act == 0) {
        /* `DLE_GHZ1` */
        g_dyn_bottom_bound = (g_cam_x >= 0x1700 || g_sonic.x >= 0x1700) ? 0x0400 : 0x0300;
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
