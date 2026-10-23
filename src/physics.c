#pragma bank 1
#include "engine.h"
#include <string.h>

/* Authentic Sonic 1 8.8 Fixed-Point Physics Constants (`01 Sonic.asm`) */
#define SONIC_ACC       12      /* 0.046875 px/f^2 */
#define SONIC_DEC       128     /* 0.5 px/f^2 skid deceleration */
#define SONIC_FRC       12      /* friction */
#define SONIC_TOP       0x0600  /* 6.0 px/f normal top speed */
#define SONIC_TOP_SHOES 0x0C00  /* 12.0 px/f with Speed Shoes */
#define SONIC_GRV       56      /* 0.21875 px/f^2 gravity */
#define SONIC_UW_GRV    16      /* underwater gravity */
#define SONIC_JMP       0x0680  /* 6.5 px/f jump impulse */
#define SONIC_UW_JMP    0x0380  /* 3.5 px/f underwater jump */

static void compute_camera_target(int16_t *out_cx, int16_t *out_cy) {
    int16_t look = 0;
    int16_t tx, ty;
    int16_t max_x = g_dyn_right_bound;
    int16_t max_y = g_dyn_bottom_bound;

    if (g_sonic.gsp > 0x200) look = 24;
    else if (g_sonic.gsp < -0x200) look = -24;

    tx = g_sonic.x + look - 152;
    ty = g_sonic.y - 136;

    if (tx < g_dyn_left_bound) tx = g_dyn_left_bound;
    if (tx > max_x) tx = max_x;
    if (ty < g_dyn_top_bound) ty = g_dyn_top_bound;
    if (ty > max_y) ty = max_y;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;

    *out_cx = tx;
    *out_cy = ty;
}

static void sonic_enter_roll(uint8_t force_tunnel) {
    if (!g_sonic.rolling) {
        g_sonic.rolling = 1;
        g_sonic.anim = ANI_ROLL;
        g_sonic.y += 5;
        g_sonic.y_sub += (5L << 8);
        hw_play_sfx(SFX_ROLL);
    }
    if (force_tunnel && g_sonic.gsp == 0) {
        g_sonic.gsp = 0x0200;
    }
}

static void sonic_exit_roll(void) {
    if (g_sonic.rolling) {
        /* Never unroll inside low ceiling tunnels (`< 19px` headroom above center) so Sonic stays in spinball! */
        if (hw_ceil_distance(g_sonic.x, g_sonic.y - 19) < 0) {
            return;
        }
        g_sonic.rolling = 0;
        g_sonic.y -= 5;
        g_sonic.y_sub -= (5L << 8);
    }
}

void physics_init_sonic(int16_t sx, int16_t sy) BANKED {
    memset(&g_sonic, 0, sizeof(g_sonic));
    g_sonic.x = sx;
    g_sonic.y = sy;
    g_sonic.x_sub = (int32_t)sx << 8;
    g_sonic.y_sub = (int32_t)sy << 8;
    g_sonic.on_ground = 1;
    g_sonic.anim = ANI_WAIT;
    g_sonic.prev_anim = 0xFF;
    g_sonic.frame = 1;
    g_sonic.air_frames = 1800;

    {
        uint8_t ang = 0;
        int16_t d = hw_floor_distance(g_sonic.x, g_sonic.y + 19, &ang);
        if (d > -20 && d < 24) {
            g_sonic.y += d;
            g_sonic.y_sub = (int32_t)g_sonic.y << 8;
            g_sonic.angle = ang;
        }
    }

    compute_camera_target(&g_cam_x, &g_cam_y);
}

void sonic_hurt_or_kill(uint8_t instant_kill) BANKED {
    if (g_sonic.dead || g_debug_active || g_signpost_passed) return;
    if (!instant_kill && (g_sonic.invuln > 0 || g_sonic.invinc > 0)) return;

    if (!instant_kill && g_sonic.shield) {
        sonic_exit_roll();
        g_sonic.shield = 0;
        g_sonic.invuln = 120;
        g_sonic.ysp = -0x400;
        g_sonic.xsp = g_sonic.facing_left ? 0x200 : -0x200;
        g_sonic.on_ground = 0;
        g_sonic.angle = 0;
        g_sonic.anim = ANI_HURT;
        hw_play_sfx(SFX_DEATH);
        return;
    }

    if (!instant_kill && g_rings > 0) {
        uint8_t count = g_rings > MAX_LOST_RINGS ? MAX_LOST_RINGS : (uint8_t)g_rings;
        uint8_t i;
        sonic_exit_roll();
        for (i = 0; i < count; i++) {
            uint8_t ang = (uint8_t)(0x48 + i * 22);
            g_lost_rings[i].active = 1;
            g_lost_rings[i].x = g_sonic.x;
            g_lost_rings[i].y = g_sonic.y;
            g_lost_rings[i].vx = hw_cos(ang) * 2;
            g_lost_rings[i].vy = -0x380 - ((i & 1) << 7);
            g_lost_rings[i].timer = 180;
        }
        g_rings = 0;
        g_hud_dirty = 1;
        g_sonic.invuln = 120;
        g_sonic.ysp = -0x400;
        g_sonic.xsp = g_sonic.facing_left ? 0x200 : -0x200;
        g_sonic.on_ground = 0;
        g_sonic.angle = 0;
        g_sonic.anim = ANI_HURT;
        hw_play_sfx(SFX_RINGLOSS);
        return;
    }

    g_sonic.dead = 1;
    g_sonic.on_ground = 0;
    g_sonic.rolling = 0;
    g_sonic.angle = 0;
    g_sonic.xsp = 0;
    g_sonic.ysp = -0x700;
    g_sonic.anim = ANI_DEATH;
    hw_play_sfx(SFX_DEATH);
}

/* Authentic `DebugMode.asm` 16-Object Placement Table */
static void update_debug_placement_mode(uint8_t pad, uint8_t pressed, int16_t old_cx, int16_t old_cy) {
    static const uint8_t dbg_ids[16]  = {
        0x25, 0x26, 0x26, 0x26, 0x26, 0x41, 0x40, 0x4B,
        0x55, 0x4F, 0x19, 0x45, 0x79, 0x36, 0x22, 0x78
    };
    static const uint8_t dbg_subs[16] = {
        0x00, 0x06, 0x04, 0x05, 0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
    };

    if (pressed & J_SELECT) {
        g_debug_active = 0;
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
        g_sonic.xsp = 0;
        g_sonic.ysp = 0;
        g_sonic.on_ground = 0;
        return;
    }

    if (pad & J_LEFT)  g_sonic.x -= 6;
    if (pad & J_RIGHT) g_sonic.x += 6;
    if (pad & J_UP)    g_sonic.y -= 6;
    if (pad & J_DOWN)  g_sonic.y += 6;
    if (g_sonic.x < 16) g_sonic.x = 16;
    if (g_sonic.y < 16) g_sonic.y = 16;
    g_sonic.x_sub = (int32_t)g_sonic.x << 8;
    g_sonic.y_sub = (int32_t)g_sonic.y << 8;

    if (pressed & J_B) {
        g_debug_item = (g_debug_item + 1) & 15;
        hw_play_sfx(SFX_SWITCH);
    }
    if (pressed & J_A) {
        uint8_t s;
        for (s = 0; s < MAX_ACTIVE_OBJS; s++) {
            if (!g_objs[s].active) {
                g_objs[s].active = 1;
                g_objs[s].id = dbg_ids[g_debug_item];
                g_objs[s].subtype = dbg_subs[g_debug_item];
                g_objs[s].spawn_slot = 0xFE;
                g_objs[s].x = g_sonic.x;
                g_objs[s].y = g_sonic.y;
                g_objs[s].base_x = g_sonic.x;
                g_objs[s].base_y = g_sonic.y;
                g_objs[s].vx = -0x100;
                g_objs[s].vy = 0;
                g_objs[s].state = 0;
                g_objs[s].timer = 0;
                g_objs[s].hp = 1;
                hw_play_sfx(SFX_RING);
                break;
            }
        }
    }

    {
        int16_t target_cx, target_cy;
        compute_camera_target(&target_cx, &target_cy);
        if (target_cx > g_cam_x + 12) g_cam_x += 12;
        else if (target_cx < g_cam_x - 12) g_cam_x -= 12;
        else g_cam_x = target_cx;

        if (target_cy > g_cam_y + 12) g_cam_y += 12;
        else if (target_cy < g_cam_y - 12) g_cam_y -= 12;
        else g_cam_y = target_cy;
    }
    hw_stream_viewport_delta(old_cx, old_cy);
}

/* Exact quadrant classifier from `Sonic AnglePos.asm` lines 25-44 (`0x00, 0x40, 0x80, 0xC0`) */
static uint8_t classify_surface_quadrant(uint8_t ang) {
    int8_t s = (int8_t)(ang + 0x20);
    uint8_t d0 = ang;
    if (s >= 0) {
        if ((int8_t)d0 < 0) d0++;
        d0 += 0x1F;
    } else {
        if ((int8_t)d0 < 0) d0--;
        d0 += 0x20;
    }
    return d0 & 0xC0;
}

/* Exact `Sonic_Angle` (`Sonic AnglePos.asm` lines 197-216): nearest sensor + `btst #0` 90-degree snap! */
static int16_t select_sonic_angle(int16_t dr, uint8_t ang_r, int16_t dl, uint8_t ang_l) {
    int16_t best;
    uint8_t chosen;
    if (dl <= dr) {
        best = dl;
        chosen = ang_l;
    } else {
        best = dr;
        chosen = ang_r;
    }
    if (chosen & 1) {
        g_sonic.angle = (uint8_t)((g_sonic.angle + 0x20) & 0xC0);
    } else {
        g_sonic.angle = chosen;
    }
    return best;
}

/* Exact `Sonic_CalcRoomAhead` (`Sonic Collision.asm` lines 13-62) */
static int16_t sonic_calc_room_ahead(uint8_t move_ang) {
    int16_t d3 = (int16_t)((g_sonic.x_sub + g_sonic.xsp) >> 8);
    int16_t d2 = (int16_t)((g_sonic.y_sub + g_sonic.ysp) >> 8);
    uint8_t q = classify_surface_quadrant(move_ang);
    uint8_t dummy = move_ang;

    if (q == 0x00) {
        return hw_find_floor(d2 + 10, d3, 14, 0, 16, &dummy);
    }
    if (q == 0x80) {
        return hw_find_floor((d2 - 10) ^ 0x0F, d3, 14, 0x1000, -16, &dummy);
    }
    if ((move_ang & 0x38) == 0) {
        d2 += g_sonic.rolling ? 3 : 8;
    }
    if (q == 0x40) {
        return hw_find_wall(d2, (d3 - 10) ^ 0x0F, 14, 0x0800, -16, &dummy);
    }
    return hw_find_wall(d2, d3 + 10, 14, 0, 16, &dummy);
}

/* Exact `Sonic_AnglePos` (`Sonic AnglePos.asm` lines 10-431): full 4-quadrant ground tracking! */
static void sonic_ground_sensors(uint8_t pad) {
    int16_t h_rad = g_sonic.rolling ? 14 : 19;
    int16_t w_rad = g_sonic.rolling ? 7 : 9;
    uint8_t q = classify_surface_quadrant(g_sonic.angle);
    uint8_t ang_r = 3, ang_l = 3;
    int16_t dr, dl, d;

    if (q == 0xC0) {
        /* `Sonic_WalkVertR`: Climbing Right Vertical Wall (`0xC0`) */
        dr = hw_find_wall(g_sonic.y - w_rad, g_sonic.x + h_rad, 13, 0, 16, &ang_r);
        dl = hw_find_wall(g_sonic.y + w_rad, g_sonic.x + h_rad, 13, 0, 16, &ang_l);
        d = select_sonic_angle(dr, ang_r, dl, ang_l);
        if (d < -14) return;
        if (d > 14) {
            g_sonic.on_ground = 0;
            return;
        }
        g_sonic.x += d;
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        return;
    }

    if (q == 0x80) {
        /* `Sonic_WalkCeiling`: Running Upside-Down Across 360-Degree Loop Crown (`0x80`) */
        dr = hw_find_floor((g_sonic.y - h_rad) ^ 0x0F, g_sonic.x + w_rad, 13, 0x1000, -16, &ang_r);
        dl = hw_find_floor((g_sonic.y - h_rad) ^ 0x0F, g_sonic.x - w_rad, 13, 0x1000, -16, &ang_l);
        d = select_sonic_angle(dr, ang_r, dl, ang_l);
        if (d < -14) return;
        if (d > 14) {
            g_sonic.on_ground = 0;
            return;
        }
        g_sonic.y -= d;
        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
        return;
    }

    if (q == 0x40) {
        /* `Sonic_WalkVertL`: Descending Left Vertical Wall (`0x40`) */
        dr = hw_find_wall(g_sonic.y - w_rad, (g_sonic.x - h_rad) ^ 0x0F, 13, 0x0800, -16, &ang_r);
        dl = hw_find_wall(g_sonic.y + w_rad, (g_sonic.x - h_rad) ^ 0x0F, 13, 0x0800, -16, &ang_l);
        d = select_sonic_angle(dr, ang_r, dl, ang_l);
        if (d < -14) return;
        if (d > 14) {
            g_sonic.on_ground = 0;
            return;
        }
        g_sonic.x -= d;
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        return;
    }

    /* `Sonic_AnglePos` Default Quadrant `0x00`: Floor Mode */
    dr = hw_find_floor(g_sonic.y + h_rad, g_sonic.x + w_rad, 13, 0, 16, &ang_r);
    dl = hw_find_floor(g_sonic.y + h_rad, g_sonic.x - w_rad, 13, 0, 16, &ang_l);
    d = select_sonic_angle(dr, ang_r, dl, ang_l);
    if (d < -14) return;
    if (d > 14) {
        g_sonic.on_ground = 0;
        return;
    }
    g_sonic.y += d;
    g_sonic.y_sub = (int32_t)g_sonic.y << 8;
    if (g_sonic.gsp == 0 && !(pad & (J_DOWN | J_UP)) &&
        ((dl > 12 && dr <= 4) || (dr > 12 && dl <= 4))) {
        g_sonic.anim = ANI_BALANCE;
    }
}

/* Exact 4-Direction Airborne `Sonic_Floor` (`01 Sonic.asm` lines 1530-1805):
   Converts vertical fall speed directly into high-speed curve inertia inside S-Tunnels & Ramps! */
static void sonic_air_sensors(void) {
    int16_t h_rad = g_sonic.rolling ? 14 : 19;
    int16_t w_rad = g_sonic.rolling ? 7 : 9;
    int16_t ax = (g_sonic.xsp < 0) ? -g_sonic.xsp : g_sonic.xsp;
    int16_t ay = (g_sonic.ysp < 0) ? -g_sonic.ysp : g_sonic.ysp;
    int16_t d_wl, d_wr, dr, dl, best;
    uint8_t ang_r = 3, ang_l = 3, land_ang;

    /* 1. Side wall graze checks (`Sonic_FindWallLeft_Quick` & `Sonic_FindWallRight_Quick`) */
    if (g_sonic.xsp <= 0) {
        d_wl = hw_wall_distance(g_sonic.x - 10, g_sonic.y, -1);
        if (d_wl < 0) {
            g_sonic.x -= d_wl;
            g_sonic.x_sub = (int32_t)g_sonic.x << 8;
            g_sonic.xsp = 0;
            if (ax > ay && g_sonic.ysp != 0) g_sonic.gsp = g_sonic.ysp;
        }
    }
    if (g_sonic.xsp >= 0) {
        d_wr = hw_wall_distance(g_sonic.x + 10, g_sonic.y, 1);
        if (d_wr < 0) {
            g_sonic.x += d_wr;
            g_sonic.x_sub = (int32_t)g_sonic.x << 8;
            g_sonic.xsp = 0;
            if (ax > ay && g_sonic.ysp != 0) g_sonic.gsp = g_sonic.ysp;
        }
    }

    /* 2. Upward ceiling & angled-arch catch (`Sonic_FloorUp`) */
    if (g_sonic.ysp < 0) {
        ang_r = 3; ang_l = 3;
        dr = hw_find_floor((g_sonic.y - h_rad) ^ 0x0F, g_sonic.x + w_rad, 14, 0x1000, -16, &ang_r);
        dl = hw_find_floor((g_sonic.y - h_rad) ^ 0x0F, g_sonic.x - w_rad, 14, 0x1000, -16, &ang_l);
        best = (dl <= dr) ? dl : dr;
        land_ang = (dl <= dr) ? ang_l : ang_r;
        if (land_ang & 1) land_ang = 0x80;
        if (best < 0) {
            g_sonic.y -= best;
            g_sonic.y_sub = (int32_t)g_sonic.y << 8;
            if (((land_ang + 0x20) & 0x40) != 0) {
                /* Landed onto steep upper curve of loop / S-tunnel! */
                sonic_exit_roll();
                g_sonic.angle = land_ang;
                g_sonic.on_ground = 1;
                g_sonic.gsp = (land_ang >= 0x80) ? -g_sonic.ysp : g_sonic.ysp;
            } else {
                g_sonic.ysp = 0;
            }
        }
        return;
    }

    /* 3. Downward floor & S-tunnel bottom-elbow catch (`Sonic_FloorDown / Left / Right`) */
    ang_r = 3; ang_l = 3;
    dr = hw_find_floor(g_sonic.y + h_rad, g_sonic.x + w_rad, 13, 0, 16, &ang_r);
    dl = hw_find_floor(g_sonic.y + h_rad, g_sonic.x - w_rad, 13, 0, 16, &ang_l);
    best = (dl <= dr) ? dl : dr;
    land_ang = (dl <= dr) ? ang_l : ang_r;
    if (land_ang & 1) land_ang = 0;

    if (best < 0 && best >= -((g_sonic.ysp >> 8) + 14)) {
        uint8_t in_roll_tunnel = 0;
        uint8_t rcid;
        g_sonic.y += best;
        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
        rcid = hw_get_raw_chunk_id(g_sonic.x, g_sonic.y);
        if (g_ram_act.vset_idx == 0 && (rcid == 0x1F || rcid == 0x20)) {
            in_roll_tunnel = 1;
        }
        if (!in_roll_tunnel) {
            sonic_exit_roll();
        }
        g_sonic.angle = land_ang;
        g_sonic.on_ground = 1;
        g_sonic.air_combo = 0;

        if (((land_ang + 0x20) & 0x40) != 0) {
            /* Steep slope (`>= 45°`): Convert FULL vertical fall velocity into curve speed! */
            int16_t cap_y = (g_sonic.ysp > 0x0FC0) ? 0x0FC0 : g_sonic.ysp;
            g_sonic.xsp = 0;
            g_sonic.gsp = (land_ang >= 0x80) ? -cap_y : cap_y;
        } else if (((land_ang + 0x10) & 0x20) != 0) {
            /* Medium slope (`22.5°..45°`): Half vertical speed transfer */
            int16_t half_y = g_sonic.ysp >> 1;
            if ((land_ang >= 0x80 ? -half_y : half_y) > g_sonic.xsp || g_sonic.xsp == 0) {
                g_sonic.gsp = (land_ang >= 0x80) ? -half_y : half_y;
            } else {
                g_sonic.gsp = g_sonic.xsp;
            }
        } else {
            g_sonic.gsp = g_sonic.xsp;
        }
        g_sonic.ysp = 0;
    }
}

/* Exact `Sonic_Loops` (`01 Sonic.asm` lines 1960-2035):
   Handles both `GHZ` S-Tunnels (`0x1F, 0x20` -> `Sonic_ChkRoll`) AND `GHZ/SLZ` 360-degree Loops! */
static void sonic_loops_and_tunnels_update(void) {
    uint8_t vset = g_ram_act.vset_idx;
    uint8_t raw_cid;
    uint8_t lx;

    if (vset != 0 && vset != 3) {
        g_sonic.loop_plane = 0;
        return;
    }

    raw_cid = hw_get_raw_chunk_id(g_sonic.x, g_sonic.y);

    /* `GHZ` Roll Tunnels (`v_256roll1 = 0x1F`, `v_256roll2 = 0x20`): Force compact ball roll! */
    if (vset == 0 && (raw_cid == 0x1F || raw_cid == 0x20)) {
        sonic_enter_roll(1);
        g_sonic.loop_plane = 0;
        return;
    }

    /* Check `LoopChunkNums`: `GHZ` (`0xB5`), `SLZ` (`0xAA`, `0xB4`) */
    if ((vset == 0 && raw_cid == 0xB5) ||
        (vset == 3 && (raw_cid == 0xAA || (raw_cid == 0xB4 && g_sonic.on_ground)))) {
        lx = (uint8_t)g_sonic.x;
        if (lx < 44) {
            g_sonic.loop_plane = 0;
            return;
        }
        if (lx >= 224) {
            g_sonic.loop_plane = 1;
            return;
        }
        if (!g_sonic.loop_plane) {
            if (g_sonic.angle != 0 && g_sonic.angle <= 0x80) {
                g_sonic.loop_plane = 1;
            }
        } else {
            if (g_sonic.angle > 0x80) {
                g_sonic.loop_plane = 0;
            }
        }
        return;
    }

    g_sonic.loop_plane = 0;
}

void physics_update_level(void) BANKED {
    uint8_t pad = g_joy;
    uint8_t pressed = g_joy_pressed;
    int16_t top_spd = (g_sonic.shoes > 0) ? SONIC_TOP_SHOES : SONIC_TOP;
    int16_t acc = SONIC_ACC;
    int16_t dec = SONIC_DEC;
    int16_t frc = SONIC_FRC;
    int16_t grv = SONIC_GRV;
    int16_t old_cx = g_cam_x;
    int16_t old_cy = g_cam_y;

    if (g_demo_mode) {
        if (pressed & (J_START | J_A | J_SELECT)) {
            g_demo_mode = 0;
            g_next_mode = GM_TITLE;
            return;
        }
        pad = hw_read_demo_input(g_cur_act % 4);
        pressed = pad & ~g_joy_prev;
        if (g_demo_timer > 0) g_demo_timer--;
        if (g_demo_timer == 0) {
            g_demo_mode = 0;
            g_next_mode = GM_TITLE;
            return;
        }
    } else {
        if (pressed & J_START) {
            g_paused = !g_paused;
            if (g_paused) {
                hw_print_win(0, 0, "PAUSED  SEL:LVSEL   ");
            } else {
                g_hud_dirty = 1;
            }
        }
        if (g_paused) {
            if (pressed & J_SELECT) {
                g_paused = 0;
                g_next_mode = GM_LEVEL_SELECT;
            } else if ((pressed & J_B) && g_debug_unlocked) {
                g_paused = 0;
                g_debug_active = !g_debug_active;
                g_hud_dirty = 1;
            }
            return;
        }
        if (g_debug_unlocked && (pressed & J_SELECT)) {
            g_debug_active = !g_debug_active;
            if (g_debug_active) {
                hw_play_sfx(SFX_RING);
                return;
            }
        }
        if (g_debug_active) {
            update_debug_placement_mode(pad, pressed, old_cx, old_cy);
            hw_step_palette_cycle();
            return;
        }
    }

    if (g_signpost_passed) {
        pad = J_RIGHT;
        pressed = 0;
        if (g_act_clear_done) {
            g_next_mode = GM_ACT_CLEAR;
            return;
        }
    }

    if (g_sonic.dead) {
        g_sonic.ysp += 56;
        g_sonic.y_sub += g_sonic.ysp;
        g_sonic.y = (int16_t)(g_sonic.y_sub >> 8);
        hw_step_sonic_anim();
        if (g_sonic.y > g_cam_y + 300) {
            if (g_lives > 1) {
                g_lives--;
                hw_load_act(g_cur_act);
            } else if (g_continues > 0) {
                g_continues--;
                g_lives = 3;
                g_next_mode = GM_CONTINUE;
            } else {
                g_next_mode = GM_GAME_OVER;
            }
        }
        return;
    }

    if (g_sonic.invuln > 0) g_sonic.invuln--;
    if (g_sonic.invinc > 0) {
        g_sonic.invinc--;
        if (g_sonic.invinc == 0) hw_play_bgm(g_ram_act.music_id);
    }
    if (g_sonic.shoes > 0) g_sonic.shoes--;
    if (g_sonic.lock_ctrl > 0) g_sonic.lock_ctrl--;

    if (g_cur_act >= 9 && g_cur_act <= 11 && g_ram_act.water_y < 0x7000 && !(g_cur_act == 11 && g_sonic.x > 0x1CA0)) {
        g_water_y = g_ram_act.water_y + (hw_sin((uint8_t)(g_frame_counter >> 1)) >> 5);
    }
    if (g_water_y < 0x7000 && g_sonic.y > g_water_y) {
        if (!g_sonic.underwater) {
            g_sonic.underwater = 1;
            g_sonic.xsp >>= 1;
            g_sonic.ysp >>= 2;
            spawn_enemy_projectile(g_sonic.x, g_water_y, 0, 0, PROJ_SPLASH);
            hw_play_sfx(SFX_SPLASH);
        }
        top_spd >>= 1;
        acc >>= 1;
        dec >>= 1;
        frc >>= 1;
        grv = SONIC_UW_GRV;
        if (g_sonic.air_frames > 0) g_sonic.air_frames--;
        if (g_sonic.air_frames == 720) hw_play_bgm(BGM_DROWNING);
        if (g_sonic.air_frames == 0) {
            sonic_hurt_or_kill(1);
            return;
        }
    } else if (g_sonic.underwater) {
        g_sonic.underwater = 0;
        g_sonic.ysp <<= 1;
        g_sonic.air_frames = 1800;
        spawn_enemy_projectile(g_sonic.x, g_water_y, 0, 0, PROJ_SPLASH);
        hw_play_sfx(SFX_SPLASH);
        if (g_cur_bgm == BGM_DROWNING) hw_play_bgm(g_ram_act.music_id);
    }
    hw_set_underwater_palette(g_sonic.underwater);

    /* Ground vs Airborne Movement & Full 360° Slope Physics (`01 Sonic.asm`) */
    if (g_sonic.on_ground) {
        int16_t sin_a = hw_sin(g_sonic.angle);
        g_sonic.jumping = 0;
        if (!g_sonic.rolling) {
            g_sonic.gsp += (sin_a * 32) >> 8;
        } else {
            /* Authentic `Sonic_RollRepel`: `80` (`$50`) downhill acceleration vs `20` (`$14`) uphill resistance! */
            uint8_t same_dir = (sin_a >= 0 && g_sonic.gsp >= 0) || (sin_a <= 0 && g_sonic.gsp <= 0);
            g_sonic.gsp += (sin_a * (same_dir ? 80 : 20)) >> 8;
        }
        if (g_sonic.gsp > 0x1000) g_sonic.gsp = 0x1000;
        if (g_sonic.gsp < -0x1000) g_sonic.gsp = -0x1000;

        /* `Sonic_SlopeRepel`: Detach from steep wall / ceiling only if speed drops below `$280` */
        if (((g_sonic.angle + 0x20) & 0xC0) != 0 && g_sonic.lock_ctrl == 0) {
            int16_t abs_gsp = (g_sonic.gsp < 0) ? -g_sonic.gsp : g_sonic.gsp;
            if (abs_gsp < 0x0280) {
                g_sonic.gsp = 0;
                g_sonic.on_ground = 0;
                g_sonic.lock_ctrl = 30;
            }
        }

        if (g_sonic.spindash) {
            g_sonic.anim = ANI_ROLL;
            if (pad & J_DOWN) {
                if (pressed & (J_A | J_B)) {
                    if (g_sonic.spindash_rev < 0x0600) g_sonic.spindash_rev += 0x0180;
                    hw_play_sfx(SFX_ROLL);
                }
                g_sonic.spindash_rev -= (g_sonic.spindash_rev >> 5);
            } else {
                int16_t launch = 0x0800 + (g_sonic.spindash_rev >> 1);
                g_sonic.spindash = 0;
                sonic_enter_roll(0);
                g_sonic.gsp = g_sonic.facing_left ? -launch : launch;
                hw_play_sfx(SFX_SPRING);
            }
        } else if ((pad & J_DOWN) && (g_sonic.gsp > -0x100 && g_sonic.gsp < 0x100)) {
            g_sonic.gsp = 0;
            g_sonic.anim = ANI_DUCK;
            if (pressed & (J_A | J_B)) {
                g_sonic.spindash = 1;
                g_sonic.spindash_rev = 0x0180;
                sonic_enter_roll(0);
            }
        } else if (g_sonic.on_ground) {
            if (!g_sonic.rolling && g_sonic.lock_ctrl == 0) {
                if (pad & J_LEFT) {
                    if (g_sonic.gsp > 0) {
                        g_sonic.gsp -= dec;
                        if (g_sonic.gsp >= 0x200) {
                            g_sonic.anim = ANI_STOP;
                            if ((g_frame_counter & 7) == 0) hw_play_sfx(SFX_SKID);
                        }
                    } else {
                        g_sonic.facing_left = 1;
                        if (g_sonic.gsp > -top_spd) g_sonic.gsp -= acc;
                    }
                } else if (pad & J_RIGHT) {
                    if (g_sonic.gsp < 0) {
                        g_sonic.gsp += dec;
                        if (g_sonic.gsp <= -0x200) {
                            g_sonic.anim = ANI_STOP;
                            if ((g_frame_counter & 7) == 0) hw_play_sfx(SFX_SKID);
                        }
                    } else {
                        g_sonic.facing_left = 0;
                        if (g_sonic.gsp < top_spd) g_sonic.gsp += acc;
                    }
                } else {
                    if (g_sonic.gsp > frc) g_sonic.gsp -= frc;
                    else if (g_sonic.gsp < -frc) g_sonic.gsp += frc;
                    else g_sonic.gsp = 0;
                }
                if ((pad & J_DOWN) && (g_sonic.gsp >= 0x100 || g_sonic.gsp <= -0x100)) {
                    sonic_enter_roll(0);
                }
            } else if (g_sonic.rolling) {
                int16_t rfrc = frc >> 1;
                if ((pad & J_LEFT) && g_sonic.gsp > 0) g_sonic.gsp -= (dec >> 2);
                if ((pad & J_RIGHT) && g_sonic.gsp < 0) g_sonic.gsp += (dec >> 2);
                if (g_sonic.gsp > rfrc) g_sonic.gsp -= rfrc;
                else if (g_sonic.gsp < -rfrc) g_sonic.gsp += rfrc;
                else {
                    g_sonic.gsp = 0;
                    sonic_exit_roll();
                }
            }

            /* 360-Degree Rotated Push Wall Sensor (`Sonic_CalcRoomAhead`) */
            if (g_sonic.gsp != 0) {
                g_sonic.xsp = (int16_t)(((int32_t)g_sonic.gsp * hw_cos(g_sonic.angle)) >> 8);
                g_sonic.ysp = (int16_t)(((int32_t)g_sonic.gsp * hw_sin(g_sonic.angle)) >> 8);
                if (g_sonic.gsp > 0) {
                    int16_t wd = sonic_calc_room_ahead((uint8_t)(g_sonic.angle - 0x40));
                    if (wd < 0) {
                        g_sonic.gsp = 0;
                        g_sonic.xsp = 0;
                        g_sonic.ysp = 0;
                        if (pad & J_RIGHT) g_sonic.anim = ANI_PUSH;
                    }
                } else {
                    int16_t wd = sonic_calc_room_ahead((uint8_t)(g_sonic.angle + 0x40));
                    if (wd < 0) {
                        g_sonic.gsp = 0;
                        g_sonic.xsp = 0;
                        g_sonic.ysp = 0;
                        if (pad & J_LEFT) g_sonic.anim = ANI_PUSH;
                    }
                }
            } else {
                g_sonic.xsp = 0;
                g_sonic.ysp = 0;
            }

            /* Jump Perpendicular to Current Slope Angle (`Sonic_Jump`) */
            if (pressed & (J_A | J_B)) {
                int16_t j = g_sonic.underwater ? SONIC_UW_JMP : SONIC_JMP;
                g_sonic.xsp = (int16_t)(((int32_t)g_sonic.gsp * hw_cos(g_sonic.angle) - (int32_t)j * hw_sin(g_sonic.angle)) >> 8);
                g_sonic.ysp = (int16_t)(((int32_t)g_sonic.gsp * hw_sin(g_sonic.angle) - (int32_t)j * hw_cos(g_sonic.angle)) >> 8);
                g_sonic.on_ground = 0;
                g_sonic.jumping = 1;
                sonic_enter_roll(0);
                g_sonic.angle = 0;
                hw_play_sfx(SFX_JUMP);
            }
        }
    } else {
        /* Airborne Control (`Sonic_ChgJumpDir`) + Exact `Sonic_JumpHeight` (`01 Sonic.asm:1273-1303`) + Air Drag + Gravity */
        if (g_sonic.lock_ctrl == 0) {
            if (pad & J_LEFT) {
                g_sonic.facing_left = 1;
                if (g_sonic.xsp > -top_spd) g_sonic.xsp -= (acc << 1);
            } else if (pad & J_RIGHT) {
                g_sonic.facing_left = 0;
                if (g_sonic.xsp < top_spd) g_sonic.xsp += (acc << 1);
            }
        }
        if (g_sonic.jumping) {
            int16_t min_jmp = g_sonic.underwater ? -0x200 : -0x400;
            if (!(pad & (J_A | J_B)) && g_sonic.ysp < min_jmp) {
                g_sonic.ysp = min_jmp;
            }
        } else if (g_sonic.ysp < -0x1000) {
            g_sonic.ysp = -0x1000;
        }
        if (g_sonic.ysp < 0 && g_sonic.ysp > -0x400) {
            g_sonic.xsp -= (g_sonic.xsp >> 5);
        }
    }

    /* Integrate Fixed-Point 24.8 World Position FIRST, then apply `addi.w #$38, obVelY(a0)` (`ObjectFall`, `01 Sonic.asm:1264-1267`)! */
    if (g_sonic.xsp != 0) {
        g_sonic.x_sub += g_sonic.xsp;
        g_sonic.x = (int16_t)(g_sonic.x_sub >> 8);
    }
    if (g_sonic.ysp != 0) {
        g_sonic.y_sub += g_sonic.ysp;
        g_sonic.y = (int16_t)(g_sonic.y_sub >> 8);
    }
    if (!g_sonic.on_ground) {
        g_sonic.ysp += grv;
        if (g_sonic.ysp > 0x1000) g_sonic.ysp = 0x1000;
        /* `Spring_ResetAni`: switch from `id_Spring` to `id_Walk` once `obVelY >= 0` at apex! */
        if (g_sonic.anim == ANI_SPRING && g_sonic.ysp >= 0) {
            g_sonic.anim = ANI_WALK;
        }
    }

    /* Horizontal Level Boundaries */
    if (g_sonic.x < g_dyn_left_bound + 12) {
        g_sonic.x = g_dyn_left_bound + 12;
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        if (g_sonic.xsp < 0) g_sonic.xsp = 0;
        if (g_sonic.gsp < 0) g_sonic.gsp = 0;
    }
    /* Run Authentic `DynamicLevelEvents.asm` BEFORE `Sonic_LevelBound` (`01 Sonic.asm` exact order)! */
    if (((g_frame_counter & 3) == 0) || g_boss_active || g_sonic.y > g_dyn_bottom_bound) {
        dle_update_act_boundaries();
        if (g_next_mode != GM_LEVEL) return;
    }

    /* Horizontal Level Boundaries */
    if (!g_signpost_passed) {
        if (g_sonic.x > g_dyn_right_bound + 296) {
            g_sonic.x = g_dyn_right_bound + 296;
            g_sonic.x_sub = (int32_t)g_sonic.x << 8;
            if (g_sonic.xsp > 0) g_sonic.xsp = 0;
            if (g_sonic.gsp > 0) g_sonic.gsp = 0;
        }
    } else if (g_sonic.x > g_dyn_right_bound + 344) {
        /* Authentic `Sonic_LevelBound` post-Signpost hold safely off right edge of screen! */
        g_sonic.x = g_dyn_right_bound + 344;
        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
        g_sonic.xsp = 0;
        g_sonic.ysp = 0;
        g_sonic.gsp = 0;
        g_sonic.on_ground = 1;
    }

    /* Full 4-Quadrant Ground & Air Collision + Authentic `Sonic_Loops` S-Tunnel & 360-Loop Engine! */
    if (g_sonic.on_ground) {
        sonic_ground_sensors(pad);
    } else {
        sonic_air_sensors();
    }
    sonic_loops_and_tunnels_update();

    /* Global 16-Channel Oscillators (`OscillateNumDo` — 2x-stepped every 2nd frame for identical platform speed at 50% bank cost) */
    if ((g_frame_counter & 1) == 0) {
        osc_step_all();
    }

    /* Bottom Pit Check using Dynamic Boundary (`g_dyn_bottom_bound + 256`) so S-Tunnels NEVER kill Sonic! */
    if (g_sonic.y > g_dyn_bottom_bound + 256) {
        if (g_cur_act == 16) {
            hw_load_act(17);
            return;
        }
        sonic_hurt_or_kill(1);
        return;
    }

    /* Select Sonic Animation */
    if (g_sonic.rolling || g_sonic.spindash) {
        g_sonic.anim = ANI_ROLL;
    } else if (g_sonic.anim != ANI_STOP && g_sonic.anim != ANI_PUSH &&
        g_sonic.anim != ANI_DUCK && g_sonic.anim != ANI_BALANCE) {
        if (!g_sonic.on_ground) {
            if (g_sonic.anim != ANI_SPRING && g_sonic.anim != ANI_HURT) g_sonic.anim = ANI_WALK;
        } else {
            int16_t aspd = g_sonic.gsp < 0 ? -g_sonic.gsp : g_sonic.gsp;
            if (aspd >= 0x0580) g_sonic.anim = ANI_RUN;
            else if (aspd > 0) g_sonic.anim = ANI_WALK;
            else if (pad & J_UP) g_sonic.anim = ANI_LOOKUP;
            else g_sonic.anim = ANI_WAIT;
        }
    } else if (g_sonic.on_ground && g_sonic.gsp == 0 && !(pad & J_DOWN) && g_sonic.anim != ANI_BALANCE) {
        g_sonic.anim = (pad & J_UP) ? ANI_LOOKUP : ANI_WAIT;
    }
    hw_step_sonic_anim();

    if (!g_signpost_passed) {
        g_level_frames++;
        if ((g_level_frames % 60) == 0) g_hud_dirty = 1;
        if (g_level_frames >= 36000) {
            sonic_hurt_or_kill(1);
            return;
        }
    }

    {
        int16_t target_cx, target_cy;
        compute_camera_target(&target_cx, &target_cy);
        if (target_cx > g_cam_x + 16) g_cam_x += 16;
        else if (target_cx < g_cam_x - 16) g_cam_x -= 16;
        else g_cam_x = target_cx;

        if (target_cy > g_cam_y + 16) g_cam_y += 16;
        else if (target_cy < g_cam_y - 16) g_cam_y -= 16;
        else g_cam_y = target_cy;
    }

    hw_stream_viewport_delta(old_cx, old_cy);
    hw_step_palette_cycle();
    if ((g_frame_counter & 3) == 0) {
        hw_scan_spawn_objects();
    }
}
