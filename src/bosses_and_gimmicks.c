#pragma bank 126
#include "engine.h"
#include <string.h>

/* ============================================================================================
 * Bank 122: Complete Zone Boss Battles (`GHZ`, `MZ`, `SYZ`, `LZ`, `SLZ`, & `Final Zone`!)
 *           + All Authentic Zone-Specific Interactive Gimmicks (`s1disasm/_incObj/`)
 *           + Live Projectile & Chain-Combo Score Popup Pools!
 * ============================================================================================ */

static uint8_t s_fz_phase = 0;       /* 0 = Hydraulic Crushing Pistons, 1 = 4-Orb Homing Plasma Volley */
static uint8_t s_fz_timer = 0;
static uint8_t s_fz_pair = 0;        /* Active pair of crushing hydraulic cylinders (`0..3`) */
static uint8_t s_fz_egg_slot = 0;    /* Which piston (`0..3`) currently houses Dr. Eggman! */
static int16_t s_fz_ext = 0;         /* Hydraulic cylinder extension depth (`0..64` world px) */

static uint8_t bg_overlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                          int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    int16_t dx = ax - bx;
    int16_t dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx < (aw + bw)) && (dy < (ah + bh));
}

static uint8_t bg_solid_top(ActiveObj *o, int16_t half_w, int16_t half_h) {
    int16_t dx = g_sonic.x - o->x;
    int16_t top_y = o->y - half_h;
    if (dx > -(half_w + 8) && dx < (half_w + 8)) {
        if (g_sonic.ysp >= 0 && g_sonic.y + 19 >= top_y - 6 && g_sonic.y + 19 <= top_y + 12) {
            g_sonic.y = top_y - 19;
            g_sonic.y_sub = (int32_t)g_sonic.y << 8;
            g_sonic.ysp = 0;
            g_sonic.on_ground = 1;
            g_sonic.angle = 0;
            g_sonic.air_combo = 0;
            return 1;
        }
    }
    return 0;
}

static void bg_solid_full_box(ActiveObj *o, int16_t half_w, int16_t half_h, uint8_t pushable) {
    if (bg_solid_top(o, half_w, half_h)) return;
    if (bg_overlap(g_sonic.x, g_sonic.y, 9, 15, o->x, o->y, half_w, half_h)) {
        if (g_sonic.x < o->x) {
            g_sonic.x = o->x - half_w - 9;
            if (pushable && (g_joy & J_RIGHT)) {
                o->x += 1;
                g_sonic.anim = ANI_PUSH;
                if ((g_frame_counter & 15) == 0) hw_play_sfx(SFX_PUSH);
            }
        } else {
            g_sonic.x = o->x + half_w + 9;
            if (pushable && (g_joy & J_LEFT)) {
                o->x -= 1;
                g_sonic.anim = ANI_PUSH;
                if ((g_frame_counter & 15) == 0) hw_play_sfx(SFX_PUSH);
            }
        }
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        g_sonic.xsp = 0;
        g_sonic.gsp = 0;
    }
}

void boss_and_gimmick_reset(void) BANKED {
    memset(g_projs, 0, sizeof(g_projs));
    memset(g_pops, 0, sizeof(g_pops));
    s_fz_phase = 0;
    s_fz_timer = 0;
    s_fz_pair = 0;
    s_fz_egg_slot = 1;
    s_fz_ext = 0;
    g_syz_blocks_alive = 0x03FF;
}

static void update_syz3_destructible_blocks(ActiveObj *o) {
    uint8_t b;
    int16_t base_bx = o->base_x - 126;
    int16_t blk_y = o->base_y + 52;

    /* At bottom of Eggman's spike dive (`frame & 127 == 107`), shatter the nearest intact floor block! */
    if ((g_frame_counter & 127) == 107 && g_syz_blocks_alive != 0) {
        int16_t best_dx = 9999;
        uint8_t best_b = 0xFF;
        for (b = 0; b < 10; b++) {
            if (g_syz_blocks_alive & ((uint16_t)1U << b)) {
                int16_t bx = base_bx + (int16_t)b * 28;
                int16_t dx = o->x - bx;
                if (dx < 0) dx = -dx;
                if (dx < best_dx) {
                    best_dx = dx;
                    best_b = b;
                }
            }
        }
        if (best_b < 10 && best_dx < 40) {
            int16_t sbx = base_bx + (int16_t)best_b * 28;
            g_syz_blocks_alive &= (uint16_t)~((uint16_t)1U << best_b);
            spawn_enemy_projectile(sbx, blk_y, -0x1A0, -0x340, PROJ_DEBRIS);
            spawn_enemy_projectile(sbx, blk_y,  0x1A0, -0x340, PROJ_DEBRIS);
            hw_play_sfx(SFX_WALLSMASH);
        }
    }

    /* Render & Solidify Intact `SYZ3` Arena Floor Blocks (`0x76`) */
    for (b = 0; b < 10; b++) {
        if (g_syz_blocks_alive & ((uint16_t)1U << b)) {
            int16_t bx = base_bx + (int16_t)b * 28;
            int16_t bsx = (bx - g_cam_x) >> 1;
            int16_t bsy = (blk_y - g_cam_y) >> 1;
            if (bsx >= -8 && bsx <= 168) {
                hw_put_sprite8(bsx - 4, bsy - 4, ZSPR_PROP, 0x08 | 6);
            }
            if (!g_sonic.dead && g_sonic.ysp >= 0 &&
                bg_overlap(g_sonic.x, g_sonic.y + 16, 10, 6, bx, blk_y - 6, 13, 6)) {
                g_sonic.y = blk_y - 22;
                g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                g_sonic.ysp = 0;
                g_sonic.on_ground = 1;
                g_sonic.angle = 0;
            }
        }
    }
}

void spawn_enemy_projectile(int16_t x, int16_t y, int16_t vx, int16_t vy, uint8_t kind) BANKED {
    uint8_t i;
    for (i = 0; i < MAX_PROJS; i++) {
        if (!g_projs[i].active) {
            g_projs[i].active = 1;
            g_projs[i].kind = kind;
            g_projs[i].x = x;
            g_projs[i].y = y;
            g_projs[i].vx = vx;
            g_projs[i].vy = vy;
            g_projs[i].timer = (kind == PROJ_DEBRIS) ? 32 : 120;
            return;
        }
    }
}

void spawn_score_popup(int16_t x, int16_t y, uint8_t combo_step) BANKED {
    uint8_t i;
    static const uint16_t bonus_table[4] = { 100, 200, 500, 1000 };
    uint8_t st = combo_step > 3 ? 3 : combo_step;
    add_score_points(bonus_table[st]);
    for (i = 0; i < MAX_SCORE_POPS; i++) {
        if (!g_pops[i].active) {
            g_pops[i].active = 1;
            g_pops[i].step = st;
            g_pops[i].x = x;
            g_pops[i].y = y;
            g_pops[i].timer = 28;
            return;
        }
    }
}

void projectiles_and_popups_update(void) BANKED {
    uint8_t i;

    /* 1. Update & Draw Active Enemy / Boss Projectiles & Brick Debris */
    for (i = 0; i < MAX_PROJS; i++) {
        ProjObj *p = &g_projs[i];
        int16_t sx, sy;
        if (!p->active) continue;
        if (p->timer == 0) {
            p->active = 0;
            continue;
        }
        p->timer--;

        if (p->kind == PROJ_SPLASH) {
            if (p->timer > 18) p->timer = 18;
        } else if (p->kind == PROJ_ANIMAL_HOP) {
            /* `28 Animals.asm`: Freed animal bounces playfully off real terrain floor & flies free! */
            uint8_t ang = 0;
            int16_t d;
            p->vy += 24;
            d = hw_floor_distance(p->x, p->y + 6, &ang);
            if (d <= 0 && p->vy > 0) {
                p->y += d;
                p->vy = -0x360;
            }
        } else if (p->kind == PROJ_BOMB || p->kind == PROJ_DEBRIS) {
            p->vy += 28; /* Gravity arc */
            if (p->kind == PROJ_BOMB) {
                uint8_t ang = 0;
                int16_t d = hw_floor_distance(p->x, p->y + 6, &ang);
                if (d <= 0 && p->vy > 0) {
                    p->y += d;
                    p->vy = -0x2C0; /* Classic Ball Hog / SLZ Seesaw bomb bounce! */
                }
            }
        } else if (p->kind == PROJ_FIREBALL) {
            uint8_t ang = 0;
            int16_t d = hw_floor_distance(p->x, p->y + 6, &ang);
            if (d <= 0 && p->vy > 0) {
                /* MZ Boss lava drop spreads horizontally along marble platform! */
                p->y += d;
                p->vy = 0;
                if (p->vx == 0) p->vx = (p->x < g_sonic.x) ? 0x180 : -0x180;
            } else if (p->vy != 0) {
                p->vy += 20;
            }
        }

        p->x += (p->vx >> 8);
        p->y += (p->vy >> 8);
        sx = (p->x - g_cam_x) >> 1;
        sy = (p->y - g_cam_y) >> 1;

        if (sx < -24 || sx > 184 || sy < -24 || sy > 152) {
            p->active = 0;
            continue;
        }

        if (p->kind != PROJ_DEBRIS && p->kind != PROJ_SPLASH && p->kind != PROJ_ANIMAL_HOP &&
            !g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 7, 13, p->x, p->y, 6, 6)) {
            sonic_hurt_or_kill(0);
            p->active = 0;
            continue;
        }

        if (p->kind == PROJ_ANIMAL_HOP) {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_ANIMAL + ((g_frame_counter >> 2) & 3),
                           4 | (p->vx > 0 ? 0x20 : 0));
        } else if (p->kind == PROJ_SPLASH) {
            hw_put_sprite16(sx, sy - 4, SPR_TILE_BUBBLE_BIG, 4);
        } else if (p->kind == PROJ_PLASMA) {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_SHOT_PLASMA + ((g_frame_counter >> 1) & 1), 4);
        } else if (p->kind == PROJ_FIREBALL) {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_SHOT_FIRE + ((g_frame_counter >> 1) & 1),
                           1 | (p->vy < 0 ? 0x40 : 0) | (p->vx > 0 ? 0x20 : 0));
        } else if (p->kind == PROJ_BOMB) {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_SHOT_BOMB, 5);
        } else if (p->kind == PROJ_DEBRIS) {
            hw_put_sprite8(sx - 4, sy - 4, ZSPR_PROP + 24, 0x08 | 6);
        } else {
            /* `PROJ_ENERGY` (Buzz Bomber, Crabmeat, Newtron, LZ3 Boss, Bomb Shrapnel) */
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_SHOT_ENERGY + ((g_frame_counter >> 1) & 1), 2);
        }
    }

    /* 2. Update & Draw Rising Score Popups (`100` / `200` / `500` / `1000`) */
    for (i = 0; i < MAX_SCORE_POPS; i++) {
        ScorePop *sp = &g_pops[i];
        if (!sp->active) continue;
        if (sp->timer == 0) {
            sp->active = 0;
            continue;
        }
        sp->timer--;
        sp->y -= 1;
        hw_put_sprite8(((sp->x - g_cam_x) >> 1) - 4, ((sp->y - g_cam_y) >> 1) - 4,
                       SPR_TILE_POINTS + (sp->step & 3), 2);
    }

    /* 3. Underwater Drowning Countdown Numbers `5..0` above Sonic (`0A LZ Drowning Countdown.asm`) */
    if (g_sonic.underwater && g_sonic.air_frames < 720 && (g_frame_counter & 4) == 0) {
        uint8_t sec_idx = (uint8_t)(g_sonic.air_frames / 120); /* 5, 4, 3, 2, 1, 0 */
        int16_t sx = (g_sonic.x - g_cam_x) >> 1;
        int16_t sy = ((g_sonic.y - g_cam_y) >> 1) - 14;
        hw_put_sprite8(sx - 4, sy, SPR_TILE_BUBBLE + (sec_idx & 3), 4);
    }
}

/* Spawn Act 3 / Final Zone Boss & Run Distinct Per-Zone Boss AI (`GHZ`, `MZ`, `SYZ`, `LZ`, `SLZ`, `FZ`)! */
void boss_check_and_update(uint8_t sonic_spinning) BANKED {
    int16_t arena_x;
    uint8_t s;
    (void)sonic_spinning;

    if (!g_boss_active && !g_boss_defeated) {
        if (g_cur_act != 2 && g_cur_act != 5 && g_cur_act != 8 &&
            g_cur_act != 11 && g_cur_act != 14 && g_cur_act != 18) {
            return;
        }
        arena_x = (int16_t)g_ram_act.right_bound + 48;
        if (g_cur_act == 18) arena_x = (int16_t)g_ram_act.start_x + 36;
        if (g_sonic.x < arena_x) return;

        g_boss_active = 1;
        s_fz_phase = 0;
        s_fz_timer = 0;
        s_fz_pair = 0;
        s_fz_egg_slot = 1;
        s_fz_ext = 0;
        hw_play_bgm(g_cur_act == 18 ? BGM_FZ : BGM_BOSS);
        for (s = 0; s < MAX_ACTIVE_OBJS; s++) {
            if (!g_objs[s].active) {
                g_objs[s].active = 1;
                g_objs[s].id = 0x3D;
                g_objs[s].x = g_sonic.x + 112;
                g_objs[s].y = g_sonic.y - 52;
                g_objs[s].base_x = g_sonic.x + 80;
                g_objs[s].base_y = g_sonic.y - 52;
                g_objs[s].vx = -0x120;
                g_objs[s].vy = 0;
                g_objs[s].hp = 8; /* Authentic 8 hits to defeat Dr. Eggman! */
                g_objs[s].timer = 0;
                g_objs[s].state = 0;
                break;
            }
        }
    }
}

/* Returns `1` if `o->id` was handled as a Boss or Zone Gimmick, `0` otherwise */
uint8_t gimmick_handle_object(ActiveObj *o, int16_t sx, int16_t sy, uint8_t sonic_spinning) BANKED {
    uint8_t id = o->id;

    /* ========================================================================================
     * 1. ALL 6 DISTINCT ZONE BOSS FIGHTS (`id == 0x3D` — dispatches on `g_cur_act`)
     * ======================================================================================== */
    if (id == 0x3D) {
        uint8_t ph = (uint8_t)(g_frame_counter << 1);
        if (o->timer > 0) o->timer--;

        /* --- FINAL ZONE (`Act 18` — `85,84,86 Boss - FZ Main, Cylinders, and Plasma Balls.asm`)! --- */
        if (g_cur_act == 18) {
            uint8_t c;
            int16_t base_arena_x = (int16_t)g_ram_act.start_x + 112;
            int16_t ceil_y = o->base_y - 16; /* Fixed Final Zone ceiling anchor! */
            s_fz_timer++;

            if (s_fz_phase == 0) {
                /* Phase 0: Two Giant Hydraulic Pistons Crush Inward (one houses Dr. Eggman)! */
                if (s_fz_timer < 40) s_fz_ext += 2;
                else if (s_fz_timer == 40) hw_play_sfx(SFX_SMASH);
                else if (s_fz_timer > 68 && s_fz_ext > 0) s_fz_ext -= 2;

                for (c = 0; c < 4; c++) {
                    uint8_t active_cyl = (c == (s_fz_pair & 3)) || (c == ((s_fz_pair + 2) & 3));
                    if (!active_cyl) continue;
                    {
                        int16_t cx = base_arena_x + (int16_t)c * 52;
                        int16_t cy = (c & 1) ? (ceil_y + s_fz_ext) : (ceil_y + 92 - s_fz_ext);
                        int16_t csx = (cx - g_cam_x) >> 1;
                        int16_t csy = (cy - g_cam_y) >> 1;

                        /* Draw Hydraulic Cylinder Column + Eggman inside `s_fz_egg_slot`! */
                        hw_put_sprite16(csx, csy - 8, ZSPR_PROP, 0x08 | 6);
                        hw_put_sprite16(csx, csy, ZSPR_BOSS_WPN + 8, 0x08 | 5);
                        if (c == s_fz_egg_slot) {
                            o->x = cx;
                            o->y = cy;
                            if (!(o->timer > 0 && (g_frame_counter & 1))) {
                                hw_put_sprite_cols2rows(csx - 16, csy - 8, ZSPR_BOSS, 4u, 0x08 | 1);
                            }
                            if (o->hp > 0 && o->timer == 0 && !g_sonic.dead &&
                                bg_overlap(g_sonic.x, g_sonic.y, 12, 16, cx, cy, 18, 20)) {
                                if (sonic_spinning) {
                                    o->hp--;
                                    o->timer = 28;
                                    g_sonic.xsp = (g_sonic.x < cx) ? -0x280 : 0x280;
                                    g_sonic.ysp = -0x300;
                                    hw_play_sfx(SFX_HITBOSS);
                                    if (o->hp == 0) {
                                        g_boss_defeated = 1;
                                        g_score += 1000;
                                        g_hud_dirty = 1;
                                        hw_play_sfx(SFX_BREAK);
                                        g_next_mode = GM_ENDING;
                                        return 1;
                                    }
                                } else {
                                    sonic_hurt_or_kill(0);
                                }
                            }
                        } else if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 8, 14, cx, cy, 12, 16)) {
                            sonic_hurt_or_kill(0);
                        }
                    }
                }
                if (s_fz_timer >= 104) {
                    s_fz_phase = 1;
                    s_fz_timer = 0;
                    s_fz_ext = 0;
                }
            } else {
                /* Phase 1: Overhead Plasma Generator Launches 4 Homing Energy Orbs toward Sonic! */
                if (s_fz_timer == 16 || s_fz_timer == 32 || s_fz_timer == 48 || s_fz_timer == 64) {
                    int16_t lx = base_arena_x + 156;
                    int16_t aim_vx = (g_sonic.x < lx) ? -0x220 : 0x180;
                    spawn_enemy_projectile(lx, ceil_y + 12, aim_vx, 0x1A0, PROJ_PLASMA);
                    hw_play_sfx(SFX_SPIKES);
                }
                if (s_fz_timer >= 96) {
                    s_fz_phase = 0;
                    s_fz_timer = 0;
                    s_fz_pair = (s_fz_pair + 1) & 3;
                    s_fz_egg_slot = s_fz_pair;
                }
            }
            return 1;
        }

        /* --- Zone Act 3 Boss Movement Patterns (`GHZ3`, `MZ3`, `SYZ3`, `LZ3`, `SLZ3`) --- */
        if (g_cur_act == 8) {
            /* SYZ3 (`Obj75`): Track Sonic horizontally, then dive straight down with floor spike! */
            if ((g_frame_counter & 127) < 88) {
                if (o->x < g_sonic.x - 4) o->x += 2;
                else if (o->x > g_sonic.x + 4) o->x -= 2;
                o->y = o->base_y + (hw_sin((uint8_t)(ph << 1)) >> 5);
            } else if ((g_frame_counter & 127) < 108) {
                o->y += 2;
            } else if (o->y > o->base_y) {
                o->y -= 3;
            }
        } else if (g_cur_act == 11) {
            /* LZ3 (`Obj77`): Weave vertically and fire darting energy charges! */
            o->x = o->base_x + (hw_cos(ph) >> 2);
            o->y = o->base_y + (hw_sin(ph) >> 3);
            if ((g_frame_counter & 63) == 0) {
                spawn_enemy_projectile(o->x - 12, o->y + 8, -0x240, 0x80, PROJ_ENERGY);
                hw_play_sfx(SFX_FIREBALL);
            }
        } else {
            /* GHZ3 / MZ3 / SLZ3 Horizontal Figure-8 Sweep across Arena */
            o->x = o->base_x + (hw_cos(ph) >> 2);
            o->y = o->base_y + (hw_sin((uint8_t)(ph << 1)) >> 4);
        }

        sx = (o->x - g_cam_x) >> 1;
        sy = (o->y - g_cam_y) >> 1;

        /* Zone-Specific Boss Weapon Attack! */
        if (g_cur_act == 2) {
            /* GHZ3 (`Obj3D/48`): Pendulum Chain Link + Swinging Checkered Wrecking Ball (`ZSPR_BOSS_WPN`)! */
            int16_t swing = hw_sin(ph) >> 2;
            int16_t wx = o->x + swing;
            int16_t wy = o->y + 42;
            hw_put_sprite8(((o->x + (swing >> 1) - g_cam_x) >> 1) - 4, ((o->y + 21 - g_cam_y) >> 1) - 4, ZSPR_BOSS_WPN + 12, 0x08 | 5);
            if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 8, 14, wx, wy, 10, 10)) {
                sonic_hurt_or_kill(0);
            }
            hw_put_sprite16((wx - g_cam_x) >> 1, (wy - g_cam_y) >> 1, ZSPR_BOSS_WPN, 0x08 | 6);
        } else if (g_cur_act == 5) {
            /* MZ3 (`Obj73/74`): Drop Incendiary Lava Fireballs onto Marble Arena! */
            if ((g_frame_counter & 63) == 16) {
                spawn_enemy_projectile(o->x, o->y + 12, 0, 0x180, PROJ_FIREBALL);
                hw_play_sfx(SFX_FIREBALL);
            }
        } else if (g_cur_act == 8) {
            /* SYZ3 (`Obj75/76`): Protruding ventral spike (`ZSPR_BOSS_WPN + 4`) + 10-Block Destructible Arena Floor! */
            update_syz3_destructible_blocks(o);
            hw_put_sprite16(sx, sy + 9, ZSPR_BOSS_WPN + 4, 0x08 | 5);
            if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 7, 12, o->x, o->y + 18, 6, 8)) {
                sonic_hurt_or_kill(0);
            }
        } else if (g_cur_act == 14) {
            /* SLZ3 (`Obj7A/7B`): Drop Bouncing Spiked Bombs onto Star Light Seesaws! */
            if ((g_frame_counter & 63) == 24) {
                spawn_enemy_projectile(o->x, o->y + 14, (o->x > g_sonic.x) ? -0x120 : 0x120, 0x100, PROJ_BOMB);
                hw_play_sfx(SFX_BUMPER);
            }
        }

        /* Eggman Hovercraft Hit Detection (`8` hits -> Prison Capsule `0x3E`!) */
        if (o->hp > 0 && o->timer == 0 && !g_sonic.dead &&
            bg_overlap(g_sonic.x, g_sonic.y, 12, 16, o->x, o->y, 20, 18)) {
            if (sonic_spinning) {
                o->hp--;
                o->timer = 30;
                g_sonic.ysp = -g_sonic.ysp;
                if (g_sonic.ysp > -0x300) g_sonic.ysp = -0x380;
                hw_play_sfx(SFX_HITBOSS);
                if (o->hp == 0) {
                    g_boss_defeated = 1;
                    g_score += 1000;
                    g_hud_dirty = 1;
                    hw_play_bgm(g_ram_act.music_id);
                    o->id = 0x3E; /* Spawn Prison Capsule directly in arena! */
                    o->y = g_sonic.y;
                    o->state = 0;
                    return 1;
                }
            } else {
                sonic_hurt_or_kill(0);
            }
        }

        if (!(o->timer > 0 && (g_frame_counter & 1))) {
            uint8_t boss_base = (o->timer > 0) ? (ZSPR_BOSS + 8) : ZSPR_BOSS;
            uint8_t boss_flip = (g_sonic.x > o->x) ? 0x20 : 0;
            hw_put_sprite_cols2rows(sx - 16, sy - 8, boss_base, 4u, 0x08 | 1 | boss_flip);
        }
        return 1;
    }

    /* ========================================================================================
     * 2. ZONE-SPECIFIC INTERACTIVE GIMMICKS (`s1disasm/_incObj/`)
     * ======================================================================================== */

    /* --- SYZ PINBALL BUMPER (`0x47` — `47 Bumper.asm`: `-$700` bounce without `jumping` clamp!) --- */
    if (id == 0x47) {
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 10, 16, o->x, o->y, 12, 12)) {
            g_sonic.xsp = (g_sonic.x < o->x) ? -0x0700 : 0x0700;
            g_sonic.ysp = (g_sonic.y < o->y) ? -0x0700 : 0x0500;
            g_sonic.on_ground = 0;
            g_sonic.jumping = 0;
            spawn_score_popup(o->x, o->y - 12, 0);
            hw_play_sfx(SFX_BUMPER);
        }
        hw_put_sprite16(sx, sy, ZSPR_PROP, 0x08 | 3);
        return 1;
    }

    /* --- PUSH BUTTON / SWITCH (`0x32` — `32 Button.asm`, slot `ZSPR_PROP + 12`) --- */
    if (id == 0x32) {
        if (bg_overlap(g_sonic.x, g_sonic.y, 10, 20, o->x, o->y, 12, 10)) {
            if (!o->state) {
                o->state = 1;
                g_button_state |= (uint8_t)(1 << (o->subtype & 7));
                hw_play_sfx(SFX_SWITCH);
            }
        } else {
            o->state = 0;
        }
        hw_put_sprite16(sx, sy + (o->state ? 2 : 0), ZSPR_PROP + 12, 0x08 | (o->state ? 2 : 5));
        return 1;
    }

    /* --- PUSHABLE MARBLE / LABYRINTH BLOCK (`0x33`) & SOLID ROCK / WALL (`0x3B, 0x44, 0x71`) --- */
    if (id == 0x33 || id == 0x3B || id == 0x44 || id == 0x71) {
        if (!(id == 0x44 && (o->subtype & 0x10))) {
            bg_solid_full_box(o, (id == 0x44) ? 10 : 16, (id == 0x44) ? 32 : 16, (id == 0x33) ? 1 : 0);
        }
        if (id == 0x33) {
            uint8_t ang = 0;
            int16_t d = hw_floor_distance(o->x, o->y + 16, &ang);
            if (d > 0) o->y += 2;
        }
        if (id != 0x71) {
            uint8_t blk_tile = (id == 0x33) ? (ZSPR_PROP + 8) : (ZSPR_PROP + 4);
            uint8_t blk_pal = (id == 0x33 && g_ram_act.vset_idx == 2) ? 7 : 6;
            hw_put_sprite16(sx, sy, blk_tile, 0x08 | blk_pal);
        }
        return 1;
    }

    /* --- SMASHABLE WALLS & GREEN BLOCKS (`0x3C, 0x51` — `SmashObject.asm`, slot `ZSPR_PROP + 24`) --- */
    if (id == 0x3C || id == 0x51) {
        if (sonic_spinning && bg_overlap(g_sonic.x, g_sonic.y, 12, 16, o->x, o->y, 16, 20)) {
            spawn_enemy_projectile(o->x - 6, o->y - 8, -0x220, -0x380, PROJ_DEBRIS);
            spawn_enemy_projectile(o->x + 6, o->y - 8,  0x220, -0x380, PROJ_DEBRIS);
            spawn_enemy_projectile(o->x - 6, o->y + 8, -0x180, -0x200, PROJ_DEBRIS);
            spawn_enemy_projectile(o->x + 6, o->y + 8,  0x180, -0x200, PROJ_DEBRIS);
            spawn_score_popup(o->x, o->y, 0);
            hw_play_sfx(SFX_SMASH);
            g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
            o->active = 0;
            return 1;
        }
        bg_solid_full_box(o, 16, 20, 0);
        hw_put_sprite16(sx, sy - 6, ZSPR_PROP + 24, 0x08 | ((id == 0x51) ? 7 : 6));
        hw_put_sprite16(sx, sy + 6, ZSPR_PROP + 24, 0x08 | ((id == 0x51) ? 7 : 6));
        return 1;
    }

    /* --- COLLAPSING LEDGES & FLOORS (`0x1A, 0x53` — `1A, 53 Collapsing Ledges and Floors.asm`) --- */
    if (id == 0x1A || id == 0x53) {
        if (o->state == 0) {
            if (bg_solid_top(o, 24, 10)) {
                o->state = 1;
                o->timer = 22;
            }
        } else if (o->state == 1) {
            bg_solid_top(o, 24, 10);
            if (--o->timer == 0) {
                o->state = 2;
                hw_play_sfx(SFX_COLLAPSE);
            }
        } else {
            o->y += 4;
            if (sy > 136) { o->active = 0; return 1; }
        }
        hw_put_sprite16(sx - 6, sy, ZSPR_PROP, 0x08 | 6);
        hw_put_sprite16(sx + 6, sy, ZSPR_PROP, 0x08 | 6);
        return 1;
    }

    /* --- SLZ WIND FAN (`0x5D` — `5D SLZ Fan.asm`) --- */
    if (id == 0x5D) {
        uint8_t blowing = ((g_frame_counter + o->spawn_slot * 16) & 191) < 128;
        if (blowing) {
            int16_t dx = g_sonic.x - o->x;
            int16_t dy = g_sonic.y - o->y;
            if (dy > -48 && dy < 48 && dx > -128 && dx < 128) {
                int16_t push = (o->subtype & 0x40) ? -2 : 2;
                g_sonic.x += push;
                g_sonic.x_sub = (int32_t)g_sonic.x << 8;
            }
        }
        hw_put_sprite16(sx, sy, ZSPR_ENEMY3 + (blowing ? (((g_frame_counter >> 1) & 1) << 2) : 0), 0x08 | 4);
        return 1;
    }

    /* --- SLZ SEESAW (`0x5E` — `5E SLZ Seesaw.asm`) --- */
    if (id == 0x5E) {
        if (!g_sonic.dead && g_sonic.ysp > 0x180 &&
            bg_overlap(g_sonic.x, g_sonic.y + 16, 16, 10, o->x, o->y, 24, 10)) {
            /* Classic Seesaw High-Altitude Catapult Launch! */
            g_sonic.ysp = -0x0A80;
            g_sonic.on_ground = 0;
            g_sonic.jumping = 0;
            g_sonic.rolling = 0;
            g_sonic.anim = ANI_SPRING;
            hw_play_sfx(SFX_SPRING);
        } else {
            bg_solid_top(o, 22, 8);
        }
        hw_put_sprite16(sx - 6, sy, ZSPR_PROP, 0x08 | 6);
        hw_put_sprite16(sx + 6, sy, ZSPR_PROP, 0x08 | 6);
        return 1;
    }

    /* --- SBZ CONVEYOR BELT (`0x63, 0x68`), ROTATING JUNCTION (`0x66`), TELEPORTER (`0x72`), & SBZ2 CUTSCENE (`0x82, 0x83`) --- */
    if (id == 0x63 || id == 0x68) {
        if (bg_solid_top(o, 32, 8)) {
            int16_t belt = (o->subtype & 0x0F) ? -2 : 2;
            g_sonic.x += belt;
            g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        }
        return 1;
    }
    if (id == 0x66 || id == 0x72) {
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 10, 14, o->x, o->y, 14, 14)) {
            g_sonic.ysp = (id == 0x72) ? -0x0900 : -0x0600;
            g_sonic.xsp = 0x0500;
            g_sonic.on_ground = 0;
            g_sonic.jumping = 0;
            g_sonic.rolling = 1;
            hw_play_sfx(SFX_ROLL);
        }
        hw_put_sprite16(sx, sy, ZSPR_PROP, 0x08 | 5);
        return 1;
    }
    if (id >= 0x82 && id <= 0x86) {
        /* In Scrap Brain Act 2 (`g_cur_act == 16`), `0x82/0x83` runs the Eggman Collapsing Floor drop into SBZ3!
           In Final Zone (`g_cur_act == 18`), `0x84..0x86` layout markers defer to our live `0x3D` FZ Boss controller! */
        if (g_cur_act == 16 && (id == 0x82 || id == 0x83)) {
            hw_put_sprite16(sx, sy, ZSPR_BOSS, 0x08 | 5);
            if (bg_overlap(g_sonic.x, g_sonic.y, 48, 48, o->x, o->y, 48, 48)) {
                hw_play_sfx(SFX_COLLAPSE);
                hw_load_act(17);
            }
        }
        return 1;
    }

    /* --- BRIDGES, SWINGING PENDULUMS, ESCALATORS, STAIRCASES, DOORS & PLATFORMS
           (`0x0B, 0x0C, 0x11, 0x15, 0x18, 0x2A, 0x2F, 0x30, 0x31, 0x52, 0x56, 0x59, 0x5A, 0x5B, 0x61, 0x67, 0x69, 0x6B, 0x6C, 0x6F, 0x70`) --- */
    if (id == 0x0B || id == 0x0C || id == 0x11 || id == 0x15 || id == 0x18 ||
        id == 0x2A || id == 0x2F || id == 0x30 || id == 0x31 || id == 0x52 ||
        id == 0x56 || id == 0x59 || id == 0x5A || id == 0x5B || id == 0x61 ||
        id == 0x67 || id == 0x69 || id == 0x6B || id == 0x6C || id == 0x6F || id == 0x70) {
        /* Vanishing SBZ Platform (`0x6C`) toggles solid/invisible on 64-frame phase! */
        if (id == 0x6C && (((g_frame_counter + o->subtype * 16) & 127) >= 72)) {
            return 1;
        }
        if (id == 0x15 || id == 0x5A) {
            /* True Pendulum / Circular Orbit Platform (`hw_cos` / `hw_sin`)! */
            uint8_t ph = (uint8_t)(g_frame_counter << 1);
            o->x = o->base_x + (hw_cos(ph) >> 3);
            o->y = o->base_y + ((id == 0x5A) ? (hw_sin(ph) >> 3) : 28);
            sx = (o->x - g_cam_x) >> 1;
            sy = (o->y - g_cam_y) >> 1;
            if (id == 0x15) {
                hw_put_sprite8(((o->base_x - g_cam_x) >> 1) - 2, ((o->base_y + 14 - g_cam_y) >> 1) - 2, SPR_TILE_STARS, 6);
            }
        } else if (id == 0x18 || id == 0x52 || id == 0x56 || id == 0x59 || id == 0x31 || id == 0x6B) {
            uint8_t ph = (uint8_t)(g_frame_counter << 1);
            if (o->subtype & 1) o->x = o->base_x + (hw_sin(ph) >> 3);
            else o->y = o->base_y + (hw_sin(ph) >> 4);
            sx = (o->x - g_cam_x) >> 1;
            sy = (o->y - g_cam_y) >> 1;
        }

        if (id == 0x11) {
            /* Parabolic Sagging Log Bridge under Sonic! */
            int16_t sag = bg_overlap(g_sonic.x, g_sonic.y, 24, 28, o->x, o->y, 24, 16) ? 2 : 0;
            bg_solid_top(o, 40, 8);
            hw_put_sprite16(sx - 12, sy, ZSPR_PROP, 0x08 | 6);
            hw_put_sprite16(sx,      sy + sag, ZSPR_PROP, 0x08 | 6);
            hw_put_sprite16(sx + 12, sy, ZSPR_PROP, 0x08 | 6);
        } else {
            uint8_t pl_tile = (id == 0x30 || id == 0x6B) ? (ZSPR_PROP + 4) :
                              ((id == 0x15 || id == 0x5A || id == 0x6C) ? (ZSPR_PROP + 8) :
                              ((id == 0x2A) ? (ZSPR_PROP + 24) : ZSPR_PROP));
            uint8_t pl_pal = (id == 0x30) ? 7 : 6;
            bg_solid_top(o, 20, 10);
            hw_put_sprite16(sx, sy, pl_tile, 0x08 | pl_pal);
        }
        return 1;
    }

    /* --- LZ GARGOYLE (`0x62` — Spits Horizontal Fireballs using `ZSPR_ENEMY4`!) --- */
    if (id == 0x62) {
        if (((g_frame_counter + o->spawn_slot * 8) & 127) == 0) {
            spawn_enemy_projectile(o->x, o->y, (o->subtype & 0x40) ? 0x1C0 : -0x1C0, 0, PROJ_FIREBALL);
            hw_play_sfx(SFX_FIREBALL);
        }
        hw_put_sprite16(sx, sy, ZSPR_ENEMY4, 0x08 | 5 | ((o->subtype & 0x40) ? 0x20 : 0));
        return 1;
    }

    /* --- LZ AIR BUBBLES (`0x64` — Big Breathable Air Bubble `SPR_TILE_BUBBLE_BIG` + Small Bubbles!) --- */
    if (id == 0x64) {
        uint8_t phase64 = (uint8_t)((g_frame_counter + o->spawn_slot * 17) & 63);
        o->y = o->base_y - phase64;
        sy = (o->y - g_cam_y) >> 1;
        if (g_sonic.underwater && phase64 > 24 && bg_overlap(g_sonic.x, g_sonic.y, 10, 16, o->x, o->y, 10, 10)) {
            g_sonic.air_frames = 1800;
            g_sonic.anim = ANI_GETAIR;
            hw_play_sfx(SFX_BUBBLE);
            if (g_cur_bgm == BGM_DROWNING) hw_play_bgm(g_ram_act.music_id);
        }
        if (phase64 > 28) {
            hw_put_sprite16(sx, sy, SPR_TILE_BUBBLE_BIG, 4);
        } else {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_BUBBLE + ((phase64 >> 3) & 3), 4);
        }
        return 1;
    }

    /* --- CUT / UNUSED `s1disasm` OBJECTS (`0x10`, `0x19`, `0x1D`, `0x24`, `0x45`, `0x4A`)! --- */
    if (id == 0x19) {
        /* `19 Unused - GHZ Ball.asm`: Solid Rolling Checkered Boulder pushed down Green Hill slopes! */
        uint8_t ang = 0;
        int16_t d;
        if (bg_overlap(g_sonic.x, g_sonic.y, 12, 16, o->x, o->y, 16, 16)) {
            o->vx = (g_sonic.x < o->x) ? 0x1C0 : -0x1C0;
        }
        o->x += (o->vx >> 8);
        o->vx -= (o->vx >> 6);
        d = hw_floor_distance(o->x, o->y + 14, &ang);
        if (d >= -12 && d <= 12) o->y += d;
        sx = (o->x - g_cam_x) >> 1;
        sy = (o->y - g_cam_y) >> 1;
        hw_put_sprite16(sx, sy, ZSPR_BOSS_WPN, 0x08 | 6);
        return 1;
    }
    if (id == 0x45) {
        int16_t ext = (int16_t)(g_osc_val[3] >> 1);
        o->x = o->base_x - ext;
        sx = (o->x - g_cam_x) >> 1;
        bg_solid_top(o, 18, 12);
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 8, 12, o->x - 12, o->y, 6, 10)) {
            sonic_hurt_or_kill(0);
        }
        hw_put_sprite16(sx, sy, ZSPR_PROP + 16, 0x08 | 5);
        return 1;
    }
    if (id == 0x4A) {
        if (bg_overlap(g_sonic.x, g_sonic.y, 12, 18, o->x, o->y, 16, 16)) {
            hw_play_sfx(SFX_ENTERSPEC);
            g_next_mode = GM_SPECIAL_STAGE;
            o->active = 0;
            return 1;
        }
        hw_put_sprite16(sx, sy, ZSPR_GIANTRING, 0x08 | 2);
        return 1;
    }
    if (id == 0x10 || id == 0x1D || id == 0x24) {
        if (id == 0x1D && bg_overlap(g_sonic.x, g_sonic.y, 12, 16, o->x, o->y, 12, 10)) {
            g_button_state ^= 1;
            hw_play_sfx(SFX_SWITCH);
        }
        hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_STARS + ((g_frame_counter >> 2) & 3), 2);
        return 1;
    }

    /* --- ADVANCING MARBLE WALL OF LAVA (`0x4E`) & BURNING GRASS (`0x35`) --- */
    if (id == 0x4E) {
        if (g_sonic.x > o->base_x - 96 && o->x < o->base_x + 320) {
            o->x += 1;
        }
        sx = (o->x - g_cam_x) >> 1;
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 10, 18, o->x, o->y, 18, 32)) {
            sonic_hurt_or_kill(0);
        }
        hw_put_sprite16(sx, sy - 8, ZSPR_PROP + 20, 0x08 | 1);
        hw_put_sprite16(sx, sy + 8, ZSPR_PROP + 20, 0x08 | 1);
        return 1;
    }
    if (id == 0x35) {
        int16_t flame_x = o->x - 24 + (int16_t)((g_frame_counter >> 1) & 47);
        bg_solid_top(o, 32, 10);
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 8, 12, flame_x, o->y - 12, 8, 8)) {
            sonic_hurt_or_kill(0);
        }
        hw_put_sprite16(sx - 8, sy, ZSPR_PROP, 0x08 | 7);
        hw_put_sprite16(sx + 8, sy, ZSPR_PROP, 0x08 | 7);
        hw_put_sprite8(((flame_x - g_cam_x) >> 1) - 4, sy - 10, SPR_TILE_SHOT_FIRE + ((g_frame_counter >> 1) & 1), 1);
        return 1;
    }

    /* --- SCENERY, PARALLAX PYLONS, WATER SURFACE, SEARCHLIGHTS & AMBIENT SFX (`0x12,0x1B,0x1C,0x46,0x49,0x54,0x5C,0x65`) --- */
    if (id == 0x5C) {
        int16_t px = ((o->x - g_cam_x) * 3) >> 2;
        hw_put_sprite16(px, sy - 8, ZSPR_PROP + 20, 0x08 | 5);
        hw_put_sprite16(px, sy + 8, ZSPR_PROP + 20, 0x08 | 5);
        return 1;
    }
    if (id == 0x46) {
        o->y = o->base_y + ((o->subtype & 1) ? (int16_t)(g_osc_val[1] >> 1) : 0);
        sy = (o->y - g_cam_y) >> 1;
        bg_solid_top(o, 16, 16);
        hw_put_sprite16(sx, sy, ZSPR_PROP + 24, 0x08 | 6);
        return 1;
    }
    if (id == 0x49) {
        if ((g_frame_counter & 63) == 0 && sx >= -16 && sx <= 176) hw_play_sfx(SFX_SPLASH);
        return 1;
    }
    if (id == 0x1B || id == 0x65) {
        int16_t wy = (id == 0x1B) ? ((g_water_y - g_cam_y) >> 1) : sy;
        hw_put_sprite8(sx - 4, wy - 4, SPR_TILE_BUBBLE + ((g_frame_counter >> 3) & 3), 4);
        return 1;
    }
    if (id == 0x12 || id == 0x1C) {
        hw_put_sprite8(sx - 4, sy - 4, (id == 0x12) ? (SPR_TILE_STARS + ((g_frame_counter >> 3) & 3)) : (ZSPR_PROP + 4),
                       (id == 0x12) ? 2 : (0x08 | 6));
        return 1;
    }

    /* --- DYNAMIC HAZARDS (`0x13/0x14` Fireballs, `0x16` Harpoon, `0x17` Helix, `0x4C/0x54` Lava, `0x57/0x58` Spikeballs, `0x6A` Saw, `0x6D` Flame, `0x6E` Electro) --- */
    if (id == 0x13 || id == 0x14 || id == 0x16 || id == 0x17 || id == 0x4C ||
        id == 0x54 || id == 0x57 || id == 0x58 || id == 0x6A || id == 0x6D || id == 0x6E) {
        if ((id == 0x16 || id == 0x6D || id == 0x6E) && ((g_frame_counter & 95) >= 48)) {
            return 1;
        }
        if (id == 0x57) {
            uint8_t ang = (uint8_t)(g_frame_counter << 1);
            o->x = o->base_x + ((hw_cos(ang) * 28) >> 8);
            o->y = o->base_y + ((hw_sin(ang) * 28) >> 8);
        } else {
            uint8_t ph = (uint8_t)(g_frame_counter << 2);
            o->y = o->base_y - ((hw_sin(ph) > 0) ? (hw_sin(ph) >> 3) : 0);
        }
        sy = (o->y - g_cam_y) >> 1;
        sx = (o->x - g_cam_x) >> 1;
        if (!g_sonic.dead && bg_overlap(g_sonic.x, g_sonic.y, 8, 14, o->x, o->y, 10, 12)) {
            sonic_hurt_or_kill(0);
        }
        if (id == 0x13 || id == 0x14 || id == 0x6D) {
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_SHOT_FIRE + ((g_frame_counter >> 1) & 1), 1);
        } else {
            uint8_t hz_tile;
            uint8_t hz_pal = (id == 0x4C || id == 0x54) ? 1 : ((id == 0x6E) ? 4 : 5);
            if (g_cur_act >= 6 && g_cur_act <= 8) {
                /* SYZ IDs 0x57/0x58 are the distinct small and giant spikeballs, not the roller prop. */
                hz_tile = (id == 0x58) ? (ZSPR_PROP + 4) : (ZSPR_PROP + 16);
            } else {
                hz_tile = (id == 0x16 || id == 0x6A) ? (ZSPR_PROP + 16) : (ZSPR_PROP + 20);
            }
            hw_put_sprite16(sx, sy, hz_tile, 0x08 | hz_pal);
        }
        return 1;
    }

    return 0;
}
