#pragma bank 2
#include "engine.h"
#include <string.h>

static uint8_t box_overlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                           int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    int16_t dx = ax - bx;
    int16_t dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx < (aw + bw)) && (dy < (ah + bh));
}

void objects_reset_for_act(void) BANKED {
    memset(g_objs, 0, sizeof(g_objs));
    memset(g_lost_rings, 0, sizeof(g_lost_rings));
    memset(g_obj_taken, 0, sizeof(g_obj_taken));
    g_signpost_passed = 0;
    g_act_clear_done = 0;
    g_boss_active = 0;
    g_boss_defeated = 0;
}

static uint8_t is_badnik_id(uint8_t id) {
    return (id == 0x1E || id == 0x1F || id == 0x22 || id == 0x2B || id == 0x2C ||
            id == 0x2D || id == 0x40 || id == 0x42 || id == 0x43 || id == 0x4D ||
            id == 0x4F || id == 0x50 || id == 0x55 || id == 0x5F || id == 0x60 ||
            id == 0x78);
}

static uint8_t badnik_sprite_and_pal(uint8_t id, uint8_t *out_pal) {
    uint8_t v = g_ram_act.vset_idx;
    /* Exact 1-to-1 VisualSet sprite bank mapping (`vset_idx` 0..8 matching loaded VRAM Bank 1!) */
    if (v == 0 || v == 8) { /* GHZ / Ending */
        if (id == 0x40) { *out_pal = 3; return ZSPR_ENEMY1; } /* Moto Bug (Red/White/Black) */
        if (id == 0x1F) { *out_pal = 3; return ZSPR_ENEMY2; } /* Crabmeat */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY3; } /* Buzz Bomber (Cobalt Blue) */
        if (id == 0x2B) { *out_pal = 3; return ZSPR_ENEMY4; } /* Chopper */
        *out_pal = 7; return ZSPR_ENEMY5;                     /* Newtron (Vivid Emerald Green!) */
    }
    if (v == 1 || v == 7) { /* LZ / SBZ Act 3 */
        if (id == 0x2D) { *out_pal = 4; return ZSPR_ENEMY1; } /* Burrobot */
        if (id == 0x2C) { *out_pal = 3; return ZSPR_ENEMY2; } /* Jaws */
        if (id == 0x60) { *out_pal = 5; return ZSPR_ENEMY3; } /* Orbinaut */
        if (id == 0x62) { *out_pal = 5; return ZSPR_ENEMY4; } /* Gargoyle */
        *out_pal = 7; return ZSPR_ENEMY5;
    }
    if (v == 2) { /* MZ */
        if (id == 0x78) { *out_pal = 3; return ZSPR_ENEMY1; } /* Caterkiller */
        if (id == 0x50) { *out_pal = 3; return ZSPR_ENEMY2; } /* Yadrin */
        if (id == 0x55) { *out_pal = 5; return ZSPR_ENEMY3; } /* Basaran */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 3; return ZSPR_ENEMY5;                     /* Splats */
    }
    if (v == 3) { /* SLZ */
        if (id == 0x5F) { *out_pal = 5; return ZSPR_ENEMY1; } /* Bomb */
        if (id == 0x60) { *out_pal = 3; return ZSPR_ENEMY2; } /* SLZ Red Orbinaut */
        if (id == 0x5D) { *out_pal = 5; return ZSPR_ENEMY3; } /* Fan */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 3; return ZSPR_ENEMY5;                     /* Crabmeat */
    }
    if (v == 4) { /* SYZ */
        if (id == 0x43) { *out_pal = 4; return ZSPR_ENEMY1; } /* Roller */
        if (id == 0x1F) { *out_pal = 3; return ZSPR_ENEMY2; } /* Crabmeat */
        if (id == 0x50) { *out_pal = 3; return ZSPR_ENEMY3; } /* Yadrin */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 7; return ZSPR_ENEMY5;
    }
    /* SBZ1 / SBZ2 / FZ */
    if (id == 0x1E) { *out_pal = 2; return ZSPR_ENEMY1; }     /* Ball Hog (Yellow/Gold) */
    if (id == 0x78) { *out_pal = 3; return ZSPR_ENEMY2; }     /* Caterkiller */
    if (id == 0x5F) { *out_pal = 5; return ZSPR_ENEMY3; }     /* Bomb */
    if (id == 0x60) { *out_pal = 5; return ZSPR_ENEMY4; }     /* Orbinaut */
    *out_pal = 4; return ZSPR_ENEMY5;                         /* Burrobot */
}

static void update_single_badnik(ActiveObj *o, uint8_t id, uint8_t sonic_spinning) {
    uint8_t pal = 3;
    uint8_t spr = badnik_sprite_and_pal(id, &pal);
    uint8_t spike_top = (id == 0x50 || id == 0x5F || id == 0x60);
    uint8_t anim_off = ((g_frame_counter >> 3) & 1) ? 4 : 0;
    int16_t sx, sy;

    if (id == 0x2B) {
        /* Chopper: Leap vertically out of GHZ river under bridges! */
        o->y += (o->vy >> 8);
        o->vy += 22;
        if (o->y >= o->base_y + 24) {
            o->y = o->base_y + 24;
            o->vy = -0x0680;
        }
    } else if (id == 0x22) {
        /* Buzz Bomber: Sweep -> Pause & Fire Diagonal Energy Bolt! */
        uint8_t cyc = (uint8_t)((g_frame_counter + o->spawn_slot * 19) & 127);
        if (cyc < 80) {
            o->x += (o->vx >> 8);
            if (o->x < o->base_x - 56 || o->x > o->base_x + 56) o->vx = -o->vx;
        } else if (cyc == 96) {
            int16_t dir_x = (g_sonic.x < o->x) ? -0x200 : 0x200;
            spawn_enemy_projectile(o->x, o->y + 8, dir_x, 0x200, PROJ_ENERGY);
            hw_play_sfx(SFX_FIREBALL);
        }
    } else if (id == 0x1F) {
        /* Crabmeat: Scuttle -> Halt & Fire Twin Parabolic Energy Shots! */
        uint8_t cyc = (uint8_t)((g_frame_counter + o->spawn_slot * 23) & 127);
        if (cyc < 84) {
            o->x += (o->vx >> 8);
            if (o->x < o->base_x - 36 || o->x > o->base_x + 36) o->vx = -o->vx;
        } else if (cyc == 100) {
            spawn_enemy_projectile(o->x - 10, o->y - 4, -0x180, -0x280, PROJ_ENERGY);
            spawn_enemy_projectile(o->x + 10, o->y - 4,  0x180, -0x280, PROJ_ENERGY);
        }
    } else if (id == 0x42) {
        /* Newtron: Materialize when Sonic approaches -> Fire Horizontal Shot! */
        int16_t dx = g_sonic.x - o->x;
        if (dx > -128 && dx < 128) {
            if (o->state == 0) {
                o->state = 1;
                o->timer = 24;
            } else if (o->state == 1 && --o->timer == 0) {
                o->state = 2;
                spawn_enemy_projectile(o->x, o->y, (dx < 0) ? -0x280 : 0x280, 0, PROJ_ENERGY);
            }
        }
    } else if (id == 0x2D) {
        /* Burrobot: Ambush leap straight out of ground when Sonic nears! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0 && dx > -72 && dx < 72) {
            o->state = 1;
            o->vy = -0x0580;
        } else if (o->state == 1) {
            uint8_t ang = 0;
            int16_t d;
            o->vy += 28;
            o->y += (o->vy >> 8);
            d = hw_floor_distance(o->x, o->y + 14, &ang);
            if (o->vy > 0 && d <= 0) {
                o->y += d;
                o->vy = 0;
                o->state = 2;
            }
        } else if (o->state == 2) {
            o->x += (g_sonic.x < o->x) ? -1 : 1;
        }
    } else if (id == 0x1E) {
        /* Ball Hog: Launch bouncing black time-bombs every 96 frames! */
        if (((g_frame_counter + o->spawn_slot * 13) & 95) == 0) {
            spawn_enemy_projectile(o->x, o->y + 6, (o->subtype & 0x10) ? 0x140 : -0x140, -0x100, PROJ_BOMB);
        }
    } else if (id == 0x5F) {
        /* Bomb: Proximity Fuse Detonates into 4 Diagonal Shrapnel Bolts! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0 && dx > -64 && dx < 64) {
            o->state = 1;
            o->timer = 45;
        } else if (o->state == 1) {
            if (--o->timer == 0) {
                spawn_enemy_projectile(o->x, o->y, -0x1E0, -0x280, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y,  0x1E0, -0x280, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y, -0x140, -0x380, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y,  0x140, -0x380, PROJ_ENERGY);
                hw_play_sfx(SFX_BREAK);
                o->active = 0;
                return;
            }
        }
    } else if (id == 0x55) {
        /* `55 Badnik - Basaran.asm`: Hangs motionless upside-down on ceiling until Sonic approaches, then swoops! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0) {
            anim_off = 0;
            if (dx > -96 && dx < 96) {
                o->state = 1;
                o->vx = (dx < 0) ? -0x1C0 : 0x1C0;
                o->vy = 0x200;
            }
        } else {
            o->x += (o->vx >> 8);
            if (o->y < g_sonic.y - 6) o->y += 2;
            else o->y += (hw_sin((uint8_t)(g_frame_counter << 3)) >> 7);
        }
    } else if (id == 0x4F) {
        /* `4F Unused - Splats.asm`: Cut pogo-hopping rabbit Badnik bouncing across terrain! */
        uint8_t ang = 0;
        int16_t d;
        o->vy += 28;
        o->x += (o->vx >> 8);
        o->y += (o->vy >> 8);
        d = hw_floor_distance(o->x, o->y + 14, &ang);
        if (o->vy > 0 && d <= 0) {
            o->y += d;
            o->vy = -0x420;
        }
        if (o->x < o->base_x - 64 || o->x > o->base_x + 64) o->vx = -o->vx;
    } else if (id == 0x60) {
        /* Orbinaut: 4 orbiting spikeballs composited directly in frames 0 & 1 (`ZSPR_ENEMY`)! */
        o->x += (o->vx >> 8);
        if (o->x < o->base_x - 48 || o->x > o->base_x + 48) o->vx = -o->vx;
    } else {
        int16_t step = (id == 0x43) ? ((o->vx >> 8) * 3) : (o->vx >> 8);
        o->x += step;
        if (o->x < o->base_x - 56 || o->x > o->base_x + 56) {
            o->vx = -o->vx;
        }
        if (id == 0x40 || id == 0x50 || id == 0x78) {
            uint8_t ang = 0;
            int16_t d = hw_floor_distance(o->x, o->y + 14, &ang);
            if (d >= -10 && d <= 10) o->y += d;
            else o->vx = -o->vx;
        }
    }

    sx = (o->x - g_cam_x) >> 1;
    sy = (o->y - g_cam_y) >> 1;

    if (!g_sonic.dead && box_overlap(g_sonic.x, g_sonic.y, 11, 16, o->x, o->y, 13, 13)) {
        uint8_t protected_spike = (id == 0x5F) ||
                                  (spike_top && g_sonic.invinc == 0 && !g_sonic.on_ground && g_sonic.y < o->y - 6);
        if (sonic_spinning && !protected_spike) {
            if (!g_sonic.on_ground && g_sonic.ysp > 0) {
                g_sonic.ysp = -g_sonic.ysp;
            } else if (g_sonic.ysp < 0) {
                g_sonic.ysp += 0x100;
            }
            spawn_score_popup(o->x, o->y - 8, g_sonic.air_combo);
            spawn_enemy_projectile(o->x, o->y - 4, (o->spawn_slot & 1) ? 0x160 : -0x160, -0x380, PROJ_ANIMAL_HOP);
            if (g_sonic.air_combo < 3) g_sonic.air_combo++;
            hw_play_sfx(SFX_BREAK);
            g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
            o->state = 99;
            o->timer = 24;
            return;
        } else {
            sonic_hurt_or_kill(0);
        }
    }

    hw_put_sprite16(sx, sy, spr + anim_off, 0x08 | pal | (o->vx > 0 ? 0x20 : 0));
}

void objects_update_and_draw(void) BANKED {
    uint8_t i;
    uint8_t sonic_spinning = (g_sonic.rolling || g_sonic.anim == ANI_ROLL || g_sonic.invinc > 0);

    /* 1. Draw Sonic Sprite + Shield/Invincibility Halo (`hw_draw_sonic_sprite` in Bank 0) */
    hw_draw_sonic_sprite();

    /* 1b. Guarantee CGB 10-sprites/scanline hardware priority for end-of-act Goal Post (`0x0D`)
     *     by placing it first in `g_objs[0]` whenever active (`<=4` Sonic + `<=3` Signpost = `<=7 <= 10`!) */
    for (i = 1; i < MAX_ACTIVE_OBJS; i++) {
        if (g_objs[i].active && g_objs[i].id == 0x0D && g_objs[0].id != 0x0D) {
            ActiveObj tmp = g_objs[0];
            g_objs[0] = g_objs[i];
            g_objs[i] = tmp;
            break;
        }
    }

    /* 2. Check & Spawn Act 3 / Final Zone Boss (`GHZ3`, `MZ3`, `SYZ3`, `LZ3`, `SLZ3`, `FZ`) */
    boss_check_and_update(sonic_spinning);

    /* 3. Update & Draw Scattered Lost Rings (`37 Rings.asm`) */
    for (i = 0; i < MAX_LOST_RINGS; i++) {
        LostRing *r = &g_lost_rings[i];
        int16_t sx, sy;
        if (!r->active) continue;
        if (r->timer == 0) {
            r->active = 0;
            continue;
        }
        r->timer--;
        r->vy += 24;
        r->x += (r->vx >> 8);
        r->y += (r->vy >> 8);
        if (r->vy > 0) {
            uint8_t ang = 0;
            int16_t d = hw_floor_distance(r->x, r->y + 8, &ang);
            if (d <= 0) {
                r->y += d;
                r->vy = -(r->vy - (r->vy >> 2));
            }
        }
        if (!g_sonic.dead && r->timer < 156 && box_overlap(g_sonic.x, g_sonic.y, 10, 16, r->x, r->y, 8, 8)) {
            r->active = 0;
            g_rings++;
            g_hud_dirty = 1;
            hw_play_sfx(SFX_RING);
            continue;
        }
        sx = (r->x - g_cam_x) >> 1;
        sy = (r->y - g_cam_y) >> 1;
        if (r->timer > 40 || (g_frame_counter & 1)) {
            hw_put_sprite8(sx - 2, sy - 2, SPR_TILE_RING + ((g_frame_counter >> 2) & 3), 2);
        }
    }

    /* 4. Update & Draw Active Level Objects */
    for (i = 0; i < MAX_ACTIVE_OBJS; i++) {
        ActiveObj *o = &g_objs[i];
        int16_t sx, sy;
        uint8_t id;
        if (!o->active) continue;

        /* Despawn far-away non-critical objects so slots stay available across wide levels */
        if (o->id != 0x3D && o->id != 0x3E && o->id != 0x0D && o->id != 0x7C) {
            if (o->x < g_cam_x - 160 || o->x > g_cam_x + 480) {
                o->active = 0;
                continue;
            }
        }

        id = o->id;
        sx = (o->x - g_cam_x) >> 1;
        sy = (o->y - g_cam_y) >> 1;

        /* Explosion / Animal Release State (state == 99) */
        if (o->state == 99) {
            if (o->timer > 12) {
                uint8_t ex_frame = (o->timer > 18) ? SPR_TILE_EXPLODE : (SPR_TILE_EXPLODE + 4);
                hw_put_sprite16(sx, sy, ex_frame, (o->timer > 18) ? 1 : 2);
            } else {
                o->y -= 3;
                o->x += (o->vx >= 0) ? 2 : -2;
                hw_put_sprite8(((o->x - g_cam_x) >> 1) - 4, ((o->y - g_cam_y) >> 1) - 4,
                               SPR_TILE_ANIMAL + ((g_frame_counter >> 2) & 3), 4 | (o->vx >= 0 ? 0x20 : 0));
            }
            if (--o->timer == 0) o->active = 0;
            continue;
        }

        /* --- GOLDEN RING (`0x25`) --- */
        if (id == 0x25) {
            if (!g_sonic.dead && box_overlap(g_sonic.x, g_sonic.y, 10, 16, o->x, o->y, 8, 8)) {
                g_rings++;
                g_score += 10;
                g_hud_dirty = 1;
                hw_play_sfx(SFX_RING);
                g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                o->active = 0;
                continue;
            }
            hw_put_sprite8(sx - 4, sy - 4, SPR_TILE_RING + ((g_frame_counter >> 3) & 3), 2);
            continue;
        }

        /* --- ITEM MONITOR (`0x26` — `26 Monitor.asm` with true icon per subtype!) --- */
        if (id == 0x26) {
            uint8_t sub = o->subtype & 0x0F;
            uint8_t mon_tile, mon_pal;
            if (sonic_spinning && box_overlap(g_sonic.x, g_sonic.y, 12, 18, o->x, o->y, 14, 14)) {
                if (g_sonic.ysp > 0) g_sonic.ysp = -g_sonic.ysp;
                if (sub == 0x01) {
                    sonic_hurt_or_kill(0);
                } else if (sub == 0x02) {
                    g_lives++;
                    g_hud_dirty = 1;
                    hw_play_bgm(BGM_EXTRALIFE);
                } else if (sub == 0x03) {
                    g_sonic.shoes = 1200;
                    hw_play_sfx(SFX_SPRING);
                } else if (sub == 0x04) {
                    g_sonic.shield = 1;
                    hw_play_sfx(SFX_SHIELD);
                } else if (sub == 0x05) {
                    g_sonic.invinc = 1200;
                    hw_play_bgm(BGM_INVINC);
                } else if (sub == 0x07) {
                    g_rings += 50;
                    g_sonic.shield = 1;
                    g_sonic.shoes = 1200;
                    g_sonic.invinc = 1200;
                    g_hud_dirty = 1;
                    hw_play_bgm(BGM_INVINC);
                } else {
                    g_rings += 10;
                    g_score += 100;
                    g_hud_dirty = 1;
                    hw_play_sfx(SFX_RING);
                }
                hw_play_sfx(SFX_BREAK);
                g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                o->state = 99;
                o->timer = 22;
                continue;
            }
            if (!sonic_spinning && g_sonic.ysp >= 0 &&
                box_overlap(g_sonic.x, g_sonic.y + 16, 10, 6, o->x, o->y - 12, 14, 6)) {
                g_sonic.y = o->y - 30;
                g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                g_sonic.ysp = 0;
                g_sonic.on_ground = 1;
            }
            if (sub == 0x03)      { mon_tile = SPR_TILE_MONITOR + 4;  mon_pal = 1; } /* Speed Shoes (Red) */
            else if (sub == 0x04) { mon_tile = SPR_TILE_MONITOR + 8;  mon_pal = 4; } /* Shield (Cyan-Blue) */
            else if (sub == 0x05 || sub == 0x07) { mon_tile = SPR_TILE_MONITOR + 12; mon_pal = 2; } /* Invincibility (Gold) */
            else if (sub == 0x01 || sub == 0x02) { mon_tile = SPR_TILE_MONITOR + 16; mon_pal = (sub == 0x01) ? 1 : 0; } /* 1UP Sonic / Eggman */
            else                  { mon_tile = SPR_TILE_MONITOR;      mon_pal = 2; } /* Super 10-Ring (Gold) */
            hw_put_sprite16(sx, sy, mon_tile, mon_pal);
            continue;
        }

        /* --- BOUNCE SPRING (`0x41` — Exact `s1disasm/_incObj/41 Springs.asm` Up, LR, & Down in Red/Yellow!) --- */
        if (id == 0x41) {
            uint8_t raw_sub = o->subtype & 0x3F;              /* Mask out `objpos` HV-flip bits (`0x40`/`0x80`)! */
            uint8_t xflip = (o->subtype & 0x40) ? 1 : 0;      /* `status_xflip_bit`: 1 = faces Left, 0 = faces Right */
            uint8_t horiz = (raw_sub & 0x10) ? 1 : 0;         /* `btst #4, d0` -> `Spring_LR` */
            uint8_t upside_down = (!horiz && ((raw_sub & 0x20) || (o->subtype & 0x80))) ? 1 : 0; /* `Spring_Down` */
            uint8_t sp_pal = (raw_sub & 0x02) ? 2 : 1;        /* `btst #1, d0`: Yellow (`OBP2`) vs Red (`OBP1`) */
            int16_t power = (raw_sub & 0x02) ? 0x0A00 : 0x1000; /* Exact `Spring_Powers`: Red `-$1000`, Yellow `-$0A00` */
            if (o->timer > 0) o->timer--;

            if (!g_sonic.dead) {
                if (horiz) {
                    /* `Spring_LR` (`41 Springs.asm:129-165`): `d1 = 19, d2 = 14`, launch `-power` if `xflip` else `+power`,
                     * push clear of wall, copy to `gsp`, `locktime = 15`, flip facing, `id_Walk` if unrolled! */
                    if (o->timer == 0 && box_overlap(g_sonic.x, g_sonic.y, 10, 14, o->x, o->y, 10, 14)) {
                        if (xflip) {
                            g_sonic.xsp = -power;
                            g_sonic.x = o->x - 16;
                            g_sonic.facing_left = 1;
                        } else {
                            g_sonic.xsp = power;
                            g_sonic.x = o->x + 16;
                            g_sonic.facing_left = 0;
                        }
                        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
                        g_sonic.gsp = g_sonic.xsp;
                        g_sonic.lock_ctrl = 15;
                        if (!g_sonic.rolling) g_sonic.anim = ANI_WALK;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                } else if (upside_down) {
                    /* `Spring_Down` (`41 Springs.asm:179-215`): ceiling spring launches downward at `+power`! */
                    if (o->timer == 0 && g_sonic.y >= o->y - 4 &&
                        box_overlap(g_sonic.x, g_sonic.y, 11, 19, o->x, o->y, 16, 10)) {
                        g_sonic.y = o->y + 16;
                        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                        g_sonic.ysp = power;
                        g_sonic.on_ground = 0;
                        g_sonic.jumping = 0;
                        g_sonic.rolling = 0;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                } else {
                    /* `Spring_Up` (`41 Springs.asm:88-115`): `d1 = 27, d2 = 8`; launch upward at `-power` (`jumping = 0`!) */
                    if (g_sonic.ysp >= -0x100 && g_sonic.y <= o->y + 10 &&
                        box_overlap(g_sonic.x, g_sonic.y, 11, 19, o->x, o->y, 16, 10)) {
                        g_sonic.y = o->y - 8;
                        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                        g_sonic.ysp = -power;
                        g_sonic.on_ground = 0;
                        g_sonic.jumping = 0;
                        g_sonic.rolling = 0;
                        g_sonic.angle = 0;
                        g_sonic.anim = ANI_SPRING;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                }
            }
            if (horiz) {
                hw_put_sprite16(sx + (o->timer ? (xflip ? -2 : 2) : 0), sy,
                                SPR_TILE_SPRING + 4, sp_pal | (xflip ? 0x20 : 0));
            } else {
                hw_put_sprite16(sx, sy + (o->timer ? (upside_down ? 2 : -2) : 0),
                                SPR_TILE_SPRING, sp_pal | (upside_down ? 0x40 : 0));
            }
            continue;
        }

        /* --- SPIKES (`0x36` — `36 Spikes.asm` including moving retractable & ceiling spikes!) --- */
        if (id == 0x36) {
            int16_t off_y = ((o->subtype & 0x0F) == 1 && ((g_frame_counter & 63) >= 32)) ? 16 : 0;
            if (!g_sonic.dead && box_overlap(g_sonic.x, g_sonic.y, 10, 16, o->x, o->y + off_y, 14, 14)) {
                sonic_hurt_or_kill(0);
            }
            hw_put_sprite16(sx, sy + (off_y >> 1), SPR_TILE_SPIKES, 5);
            continue;
        }

        /* --- LAMPPOST / CHECKPOINT (`0x79`) --- */
        if (id == 0x79) {
            if (!o->state && box_overlap(g_sonic.x, g_sonic.y, 12, 24, o->x, o->y, 10, 24)) {
                o->state = 1;
                g_checkpoint_active = 1;
                g_checkpoint_act = g_cur_act;
                g_checkpoint_x = o->x;
                g_checkpoint_y = o->y;
                hw_play_sfx(SFX_LAMPPOST);
            }
            hw_put_sprite16(sx, sy, SPR_TILE_LAMP, o->state ? 1 : 4);
            continue;
        }

        /* --- END-OF-ACT SIGNPOST (`0x0D`) & HIDDEN BONUS POINTS (`0x7D`) --- */
        if (id == 0x7D) {
            if (g_signpost_passed && !o->state && box_overlap(g_sonic.x, g_sonic.y, 16, 20, o->x, o->y, 16, 16)) {
                o->state = 1;
                spawn_score_popup(o->x, o->y, 3); /* 1,000 Hidden Bonus! */
                hw_play_sfx(SFX_CASH);
                o->active = 0;
            }
            continue;
        }
        if (id == 0x0D) {
            uint8_t sp_frame;
            /* Lock camera horizontally onto end-of-act Signpost clearing just like `0D Signpost.asm`! */
            if (o->x >= g_dyn_right_bound && g_cam_x >= g_dyn_right_bound - 16) {
                g_dyn_left_bound = g_dyn_right_bound;
            }
            if (!o->state && g_sonic.x >= o->x) {
                o->state = 1;
                o->timer = 104;
                g_signpost_passed = 1;
                hw_play_sfx(SFX_SIGNPOST);
            }
            if (o->state == 1) {
                static const uint8_t s_spin_seq[8] = { 0, 1, 2, 3, 4, 1, 2, 3 };
                sp_frame = s_spin_seq[(o->timer >> 1) & 7];
                /* Twinkling golden ring glints popping around the spinning sign (`0D Signpost.asm`) */
                {
                    int8_t gx = (int8_t)(((o->timer * 7) & 15) - 8);
                    int8_t gy = (int8_t)(((o->timer * 5) & 11) - 6);
                    hw_put_sprite8(sx + gx - 4, sy + gy - 4, SPR_TILE_STARS + ((o->timer >> 1) & 3), 2);
                }
                if (--o->timer == 0) {
                    o->state = 2;
                    o->timer = 30;
                    sp_frame = 4;
                    hw_play_sfx(SFX_RING);
                }
            } else if (o->state == 2) {
                sp_frame = 4; /* Hold Sonic's triumphant smiling face clearly on the Goal Post! */
                if (o->timer > 0) {
                    if (--o->timer == 0) {
                        g_act_clear_done = 1;
                    }
                }
            } else {
                sp_frame = 0; /* Dr. Eggman 24x16 oval sign before Sonic passes! */
            }

            /* Render full half-scale `24x24` Goal Post (`24x16` sign plate + `8x8` steel pole at `sy + 8`) */
            if (sp_frame == 0) {
                /* Eggman 24x16 (`OBP1` crimson/white/black; right cap = H-flip of left cap) */
                hw_put_sprite8(sx - 12, sy - 8, SPR_TILE_SIGNPOST + 0, 1);
                hw_put_sprite8(sx - 12, sy,     SPR_TILE_SIGNPOST + 1, 1);
                hw_put_sprite8(sx - 4,  sy - 8, SPR_TILE_SIGNPOST + 2, 1);
                hw_put_sprite8(sx - 4,  sy,     SPR_TILE_SIGNPOST + 3, 1);
                hw_put_sprite8(sx + 4,  sy - 8, SPR_TILE_SIGNPOST + 0, 1 | 0x20);
                hw_put_sprite8(sx + 4,  sy,     SPR_TILE_SIGNPOST + 1, 1 | 0x20);
            } else if (sp_frame == 1) {
                /* Spin1 16x16 (`45 deg`) */
                hw_put_sprite8(sx - 8, sy - 8, SPR_TILE_SIGNPOST + 4, 1);
                hw_put_sprite8(sx - 8, sy,     SPR_TILE_SIGNPOST + 5, 1);
                hw_put_sprite8(sx,     sy - 8, SPR_TILE_SIGNPOST + 6, 1);
                hw_put_sprite8(sx,     sy,     SPR_TILE_SIGNPOST + 7, 1);
            } else if (sp_frame == 2) {
                /* Spin2 8x16 (`90 deg` edge-on rim) */
                hw_put_sprite8(sx - 4, sy - 8, SPR_TILE_SIGNPOST + 8, 2);
                hw_put_sprite8(sx - 4, sy,     SPR_TILE_SIGNPOST + 9, 2);
            } else if (sp_frame == 3) {
                /* Spin3 16x16 (`135 deg` H-flip of Spin1) */
                hw_put_sprite8(sx - 8, sy - 8, SPR_TILE_SIGNPOST + 6, 0 | 0x20);
                hw_put_sprite8(sx - 8, sy,     SPR_TILE_SIGNPOST + 7, 0 | 0x20);
                hw_put_sprite8(sx,     sy - 8, SPR_TILE_SIGNPOST + 4, 0 | 0x20);
                hw_put_sprite8(sx,     sy,     SPR_TILE_SIGNPOST + 5, 0 | 0x20);
            } else {
                /* Sonic 24x16 (`OBP0` royal Sonic blue + white eyes + quills) */
                hw_put_sprite8(sx - 12, sy - 8, SPR_TILE_SIGNPOST + 10, 0);
                hw_put_sprite8(sx - 12, sy,     SPR_TILE_SIGNPOST + 11, 0);
                hw_put_sprite8(sx - 4,  sy - 8, SPR_TILE_SIGNPOST + 12, 0);
                hw_put_sprite8(sx - 4,  sy,     SPR_TILE_SIGNPOST + 13, 0);
                hw_put_sprite8(sx + 4,  sy - 8, SPR_TILE_SIGNPOST + 14, 0);
                hw_put_sprite8(sx + 4,  sy,     SPR_TILE_SIGNPOST + 15, 0);
            }
            /* Vertical steel Goal Post pole (`$38..$39`) anchored right onto the ground at `sy + 8..16`! */
            hw_put_sprite8(sx - 4, sy + 8, SPR_TILE_SIGNPOST + 16, 3);
            continue;
        }

        /* --- GIANT SPECIAL STAGE RING (`0x4B`) & STARBURST FLASH (`0x7C` — `7C Giant Ring Flash.asm`) --- */
        if (id == 0x7C) {
            uint8_t ang = (uint8_t)(o->timer << 4);
            hw_put_sprite8(sx - ((hw_cos(ang) * 4) >> 8) - 4, sy - 4, SPR_TILE_STARS + (o->timer & 3), 2);
            hw_put_sprite8(sx + ((hw_cos(ang) * 4) >> 8) - 4, sy - 4, SPR_TILE_STARS + ((o->timer + 2) & 3), 1);
            if (--o->timer == 0) {
                o->active = 0;
                g_next_mode = GM_SPECIAL_STAGE;
                return;
            }
            continue;
        }
        if (id == 0x4B) {
            if (g_rings >= 50) {
                if (box_overlap(g_sonic.x, g_sonic.y, 12, 18, o->x, o->y, 16, 16)) {
                    g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                    add_score_points(1000);
                    hw_play_sfx(SFX_GIANTRING);
                    g_signpost_passed = 1;
                    g_act_clear_done = 1;
                    o->id = 0x7C;
                    o->timer = 14;
                    continue;
                }
                hw_put_sprite16(sx, sy, ZSPR_GIANTRING + (((g_frame_counter >> 2) & 3) << 2), 0x08 | 2);
            }
            continue;
        }

        /* --- EGG PRISON CAPSULE (`0x3E` — Golden 24x16 Capsule + Separate Red Release Button `122`!) --- */
        if (id == 0x3E) {
            if (!o->state && g_boss_defeated &&
                box_overlap(g_sonic.x, g_sonic.y, 16, 24, o->x, o->y, 24, 28)) {
                o->state = 1;
                o->timer = 90;
                if (g_sonic.ysp > 0) g_sonic.ysp = -0x380;
                hw_play_sfx(SFX_BREAK);
            }
            if (o->state == 1) {
                if ((o->timer & 7) == 0) {
                    spawn_enemy_projectile(o->x, o->y - 8, (o->timer & 8) ? 0x1C0 : -0x1C0, -0x380, PROJ_ANIMAL_HOP);
                }
                if (--o->timer == 0) {
                    g_act_clear_done = 1;
                    g_next_mode = GM_ACT_CLEAR;
                    return;
                }
            }
            if (o->subtype != 0) {
                hw_put_sprite8(sx - 8, sy - 4 + (o->state ? 2 : 0), ZSPR_CAPSULE + 6, 0x08 | 1);
                hw_put_sprite8(sx,     sy - 4 + (o->state ? 2 : 0), ZSPR_CAPSULE + 7, 0x08 | 1);
            } else {
                hw_put_sprite8(sx - 8, sy - 14 + (o->state ? 3 : 0), ZSPR_CAPSULE + 6, 0x08 | 1);
                hw_put_sprite8(sx,     sy - 14 + (o->state ? 3 : 0), ZSPR_CAPSULE + 7, 0x08 | 1);
                if (o->state) {
                    hw_put_sprite16(sx, sy, ZSPR_CAPSULE + 8, 0x08 | 2);
                } else {
                    hw_put_sprite_cols2rows(sx - 12, sy - 8, ZSPR_CAPSULE, 3u, 0x08 | 2);
                }
            }
            continue;
        }

        /* --- DELEGATE ALL BOSSES & INTERACTIVE ZONE GIMMICKS TO BANK 122 --- */
        if (gimmick_handle_object(o, sx, sy, sonic_spinning)) {
            continue;
        }

        /* --- ALL 14 DISTINCT SONIC 1 BADNIK AIs (`s1disasm/_incObj/`) --- */
        if (is_badnik_id(id)) {
            update_single_badnik(o, id, sonic_spinning);
            continue;
        }
    }

    /* 5. Update & Draw Active Projectiles, Brick Debris, Score Popups & Drowning Numbers */
    projectiles_and_popups_update();
}
