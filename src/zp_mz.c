#pragma bank 123
#define ZP_WANT_COMMON
#include "zp_common.h"
#include "zp_mz.h"
#include "zp_syz.h"
#include "zp_slz.h"
#include "zp_lz.h"

/* =============================================================================================== */
/* MARBLE ZONE                                                                                      */
/* =============================================================================================== */

/* 0x46 Bricks: static / wobble (ceiling) / fall when Sonic is near / slow wobble on lava (`46 MZ Bricks.asm`). */
static void mz_brick(ActiveObj *o) {
    uint8_t t = (uint8_t)(o->subtype & 7U);
    int16_t v, dx;
    uint8_t ang;

    if (t == 2) {
        dx = g_sonic.x - o->x;
        if (dx < 0) dx = -dx;
        if (dx < 0x90) {
            o->subtype = 3;
            o->vy = 0;
            o->timer = 0;
        }
        t = 1;
    }
    if (t == 1) {
        v = (int16_t)OSC_HI(0x16);
        if (o->subtype & 8U) v = 0x10 - v;
        o->y = o->base_y - v;
    } else if (t == 3) {
        zp_fall(o, 0x18);
        if (o->vy >= 0 && hw_floor_distance(o->x, o->y + 15, &ang) < 0) {
            o->y += hw_floor_distance(o->x, o->y + 15, &ang);
            o->vy = 0;
            o->base_y = o->y;
            o->subtype = zp_is_lava(o->x, o->y + 16) ? 4 : 0;
        }
    } else if (t == 4) {
        o->y = o->base_y - (int16_t)(OSC_HI(0x12) >> 3);
    }
    pr_solid(o, 27, 16, 16);
    pr_cells(ZP_SX(o), ZP_SY(o), zp_mz_brick, ZP_MZ_BRICK_N, 0);
}

/* 0x2F Large grassy platforms (`2F MZ Large Grassy Platforms`): slope heightmaps, oscillating / burnable. */
static int16_t mz_grass_top(ActiveObj *o, uint8_t fr, int16_t d1) {
    int16_t d0 = g_sonic.x - o->x + d1;
    uint8_t idx, h;
    if (d0 < 0) d0 = 0;
    idx = (uint8_t)(d0 >> 1);
    if (fr == 0) {
        if (idx < 14) h = 0x20;
        else if (idx < 29) h = (uint8_t)(0x21 + (idx - 14));
        else if (idx < 47) h = 0x30;
        else if (idx < 62) h = (uint8_t)(0x2F - (idx - 47));
        else h = 0x20;
    } else if (fr == 1) {
        if (idx < 6) h = 0x20;
        else if (idx < 37) h = (uint8_t)(0x21 + (idx - 6));
        else if (idx < 55) h = 0x40;
        else if (idx < 70) h = (uint8_t)(0x3F - (idx - 55));
        else h = 0x30;
    } else {
        h = 0x30;
    }
    return o->y - (int16_t)h;
}

static void mz_grass(ActiveObj *o) {
    static const uint8_t k_off[4] = { 2, 6, 10, 14 };
    static const uint8_t k_mid[4] = { 0x10, 0x18, 0x20, 0x30 };
    uint8_t fr = (uint8_t)((o->subtype >> 4) & 3U);
    uint8_t t = (uint8_t)(o->subtype & 7U);
    int16_t d1, v, sx, sy;
    const PropCell *c;
    uint8_t n;

    if (fr > 2) fr = 0;
    d1 = (fr == 2 ? 32 : 64) + 11;
    if (t >= 1 && t <= 4) {
        uint8_t oi = (uint8_t)(t - 1U);
        v = (int16_t)((uint8_t *)g_osc_val)[((k_off[oi] - 2) >> 1) + 1];
        if (o->subtype & 8U) v = ((int16_t)k_mid[oi] << 1) - v;
        o->y = o->base_y - v;
    } else if (t == 5) {
        uint8_t n8 = o->hp;
        if (g_stand_obj == o) {
            n8 = (uint8_t)(n8 + 4U);
            if (n8 >= 0x40) n8 = 0x40;
        } else {
            n8 = (n8 >= 2) ? (uint8_t)(n8 - 2U) : 0;
        }
        o->hp = n8;
        o->y = o->base_y + (hw_sin(n8) >> 4);
        if (n8 == 0x20) o->state = 1;      /* burning */
    }
    zp_stand_slope(o, d1, mz_grass_top(o, fr, d1));
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    c = fr == 0 ? zp_mz_grass0 : (fr == 1 ? zp_mz_grass1 : zp_mz_grass2);
    n = fr == 0 ? ZP_MZ_GRASS0_N : (fr == 1 ? ZP_MZ_GRASS1_N : ZP_MZ_GRASS2_N);
    pr_cells(sx, sy, c, n, 0);
    if (o->state == 1) {
        /* `35 Burning Grass`: flames licking over the platform surface */
        uint8_t k;
        for (k = 0; k < 4; k++) {
            int16_t fx = ((int16_t)k << 4) - 28;
            hw_put_sprite8(sx + fx, sy - 16 - (int16_t)((g_frame_counter >> 2) & 1U) * 2, zp_mz_fire0[0].tile + 0, zp_mz_fire0[0].attr);
        }
        if (g_stand_obj == o && !((g_frame_counter + o->spawn_slot) & 31U)) sonic_hurt_or_kill(0);
    }
}

/* 0x51 Smashable green block: roll onto it from above to break it (`51 MZ Smashable Green Block.asm`). */
static void mz_smash(ActiveObj *o) {
    static const int8_t k_vx[4] = { -2, 2, -1, 1 };
    static const int8_t k_vy[4] = { -2, -2, -1, -1 };
    int16_t sx = ZP_SX(o), sy = ZP_SY(o);
    uint8_t k;

    if (o->state == 0) {
        uint8_t was_roll = (uint8_t)(g_sonic.rolling || g_sonic.anim == ANI_ROLL);
        pr_solid(o, 27, 16, 16);
        if (g_stand_obj == o && was_roll) {
            g_stand_obj = 0;
            g_sonic.rolling = 1;
            g_sonic.anim = ANI_ROLL;
            g_sonic.ysp = -0x300;
            g_sonic.on_ground = 0;
            o->state = 1;
            o->timer = 0;
            pr_taken(o);
            add_score_points(100);
            hw_play_sfx(SFX_SMASH);
        } else {
            pr_cells(sx, sy, zp_mz_smab, ZP_MZ_SMAB_N, 0);
            return;
        }
    }
    {
        uint16_t t = o->timer;
        if (t > 60U) {
            o->active = 0;
            return;
        }
        o->timer++;
        for (k = 0; k < 4; k++) {
            int16_t xo = (int16_t)k_vx[k] * (int16_t)t;
            int16_t yo = (int16_t)k_vy[k] * (int16_t)t + (int16_t)((7U * t * t) >> 4);
            hw_put_sprite8(sx + zp_mz_smab[k].dx + (xo >> 1), sy + zp_mz_smab[k].dy + (yo >> 1),
                           zp_mz_smab[k].tile, zp_mz_smab[k].attr);
        }
    }
}


/* 0x31 Chained stompers (`31 MZ Chained Stompers.asm`): vx = extension (8.8), vy = speed, hp = rising, timer = delay. */
static void mz_stomper(ActiveObj *o) {
    static const uint16_t k_len[7] = { 0x7000, 0xA000, 0x5000, 0x7800, 0x3800, 0x5800, 0xB800 };
    uint8_t t, big;
    uint16_t cur, mx;
    int16_t by, sx, sy;
    uint8_t k;

    if (o->state == 0) {
        o->state = 1;
        o->hp = 0;
        o->timer = 0;
        o->vy = 0;
        o->vx = ((o->subtype & 0x0FU) == 0 || (o->subtype & 0x80U)) ? (int16_t)k_len[0] : 0;
    }
    t = (uint8_t)(o->subtype & 0x0FU);
    big = (uint8_t)(((o->subtype >> 4) & 7U) != 2U);
    mx = k_len[t > 6 ? 0 : t];
    cur = (uint16_t)o->vx;

    if (o->subtype & 0x80U) {
        /* switch controlled: rise while the switch is pressed, else fall */
        if (g_button_state & 1U) {
            if (cur) {
                cur = (cur > 0x80U) ? (uint16_t)(cur - 0x80U) : 0;
                if (!(g_frame_counter & 15U)) hw_play_sfx(SFX_CHAIN);
            }
            o->vy = 0;
        } else if (cur != mx) {
            uint16_t spd = (uint16_t)o->vy;
            o->vy += 0x70;
            cur += spd;
            if (cur >= mx || cur < spd) {
                cur = mx;
                o->vy = 0;
                hw_play_sfx(SFX_SMASH);
            }
        }
    } else {
        if ((t == 3 || t == 5) && o->hp == 0 && o->vy == 0 && cur == 0) {
            int16_t dx = g_sonic.x - o->x;
            if (dx < 0) dx = -dx;
            if (dx < 0x90) o->subtype = (uint8_t)((o->subtype & 0xF0U) | (uint8_t)(t + 1U));
        } else if (t != 3 && t != 5) {
            if (o->hp) {
                if (o->timer) {
                    o->timer--;
                } else {
                    cur = (cur > 0x80U) ? (uint16_t)(cur - 0x80U) : 0;
                    if (cur == 0) o->hp = 0;
                    else if (!(g_frame_counter & 15U)) hw_play_sfx(SFX_CHAIN);
                }
            } else {
                uint16_t spd = (uint16_t)o->vy;
                o->vy += 0x70;
                cur += spd;
                if (cur >= mx) {
                    cur = mx;
                    o->vy = 0;
                    o->hp = 1;
                    o->timer = 60;
                    hw_play_sfx(SFX_SMASH);
                }
            }
        }
    }
    o->vx = (int16_t)cur;
    by = o->base_y + (int16_t)(cur >> 8);
    {
        int16_t sy0 = o->y;
        o->y = by;
        pr_solid(o, (big ? 56 : 16) + 11, 12, big ? 56 : 16);
        if (g_stand_obj == o && (cur >> 8) < 0x10) sonic_hurt_or_kill(1);
        if (big) zp_hurt(o, 56, 12);
        o->y = sy0;
    }
    sx = ZP_SX(o);
    sy = ((by + (o->y - o->base_y) * 0) - g_cam_y) >> 1;
    /* chain from the ceiling to the block */
    for (k = 0; k < 6; k++) {
        int16_t cy = (int16_t)((o->base_y - 36) + ((int16_t)k << 5));
        if (cy + 32 > by - 20) break;
        pr_cells(sx, (cy - g_cam_y) >> 1, zp_mz_stchn, ZP_MZ_STCHN_N, 0);
    }
    pr_cells(sx, sy, zp_mz_stblk, ZP_MZ_STBLK_N, 0);
    if (big) pr_cells(sx, sy + 14, zp_mz_stspk, ZP_MZ_STSPK_N, 0);
}


/* 0x33 Pushable block (`33 MZ Pushable Blocks.asm`): pushed 1 px/frame, falls off ledges, sinks in lava. */
static void mz_push(ActiveObj *o) {
    uint8_t ang;
    int16_t d, dir;
    if (o->state == 0) {
        o->state = 1;
        o->vy = 0;
        o->timer = 0;
        o->vx = 0;
    }
    if (o->state == 2) {                       /* sinking in lava */
        o->y++;
        if (++o->vx > 160) {
            o->active = 0;
            pr_taken(o);
            if (g_stand_obj == o) g_stand_obj = 0;
            return;
        }
    } else {
        d = zp_floor(o, 16);
        if (d > 0 || o->vy) {
            zp_fall(o, 0x18);
            d = zp_floor(o, 16);
            if (d < 0) {
                o->y += d;
                o->vy = 0;
                o->timer = 0;
                if (zp_is_lava(o->x, o->y + 16)) o->state = 2;
            }
        }
    }
    {
        uint8_t push = pr_solid(o, (o->subtype ? 64 : 16) + 11, 16, o->subtype ? 64 : 16);
        if (push && o->state == 1 && o->vy == 0) {
            dir = (g_sonic.x < o->x) ? 1 : -1;
            d = hw_rwall_distance(o->x + (dir > 0 ? (o->subtype ? 64 : 16) : -(o->subtype ? 64 : 16) - 1), o->y, &ang);
            if (dir < 0) d = hw_rwall_distance(o->x - (o->subtype ? 64 : 16) - 1, o->y, &ang);
            if (d >= 0 || (dir > 0 && 0)) {
                o->x += dir;
                if (!(g_frame_counter & 15U)) hw_play_sfx(SFX_PUSH);
            }
        }
    }
    {
        int16_t sx = ZP_SX(o), sy = ZP_SY(o);
        uint8_t k, nb = (uint8_t)(o->subtype ? 4 : 1);
        for (k = 0; k < nb; k++)
            pr_cells(sx + ((int16_t)k << 4) - ((int16_t)(nb - 1U) << 3), sy, zp_mz_push, ZP_MZ_PUSH_N, 0);
    }
}


/* 0x4C Lava geyser maker: a column of lava shoots up, then falls back (`4C, 4D MZ Lava Geyser and Maker.asm`).
   vx = height (px), vy = velocity (8.8), timer = cooldown. */
static void mz_geyser(ActiveObj *o) {
    int16_t sx, sy, h, k;
    if (o->state == 0) {
        o->state = 1;
        o->timer = 60;
        o->vx = 0;
        o->vy = 0;
    }
    if (o->state == 1) {
        if (--o->timer == 0) {
            o->state = 2;
            o->vy = -0x500;
            hw_play_sfx(SFX_BURN);
        }
    } else {
        int16_t dh = -(int16_t)(o->vy >> 8);
        o->vx += dh;
        o->vy += 0x18;
        if (o->vx < 0 || (o->vy > 0 && o->vx <= 0)) {
            o->vx = 0;
            o->state = 1;
            o->timer = 90;
        }
    }
    h = o->vx;
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    if (h > 0) {
        for (k = 0; k < (h >> 4); k++)
            pr_cells(sx - 8, sy - 8 - (k << 3), zp_mz_gycol, ZP_MZ_GYCOL_N, 0);
        pr_cells(sx, sy - (h >> 1) - 8, zp_mz_gyhead, ZP_MZ_GYHEAD_N, 0);
        if (!g_sonic.dead && g_sonic.x > o->x - 20 && g_sonic.x < o->x + 20 &&
            g_sonic.y < o->y + 8 && g_sonic.y > o->y - h - 14) sonic_hurt_or_kill(0);
    }
}

/* 0x54 Invisible lava tag: a hurt box sized by the subtype (`54 MZ Invisible Lava Tag.asm`). */
static void mz_lavatag(ActiveObj *o) {
    static const uint8_t k_w[3] = { 32, 64, 128 };
    uint8_t s = (uint8_t)(o->subtype & 0x0FU);
    if (s > 2) s = 0;
    zp_hurt(o, k_w[s], 32);
}


/* 0x4E Lava wall (`4E MZ Wall of Lava.asm`): starts creeping at 1.5 px/frame when Sonic is near, hurts on touch. */
static void mz_lavawall(ActiveObj *o) {
    int16_t sx, sy;
    uint8_t r, c;
    if (o->state == 0) {
        o->state = 1;
        o->vx = 0;
        o->timer = 0;
    }
    if (o->state == 1) {
        int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 192 && dy < 96) o->state = 2;
    } else if (o->x < 0x6A0) {
        uint16_t s = (uint16_t)o->timer + 0x80U;
        o->x += 1 + (int16_t)(s >> 8);
        o->timer = (uint8_t)s;
    }
    zp_hurt(o, 40, 48);
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    for (r = 0; r < 6; r++)
        for (c = 0; c < 4; c++)
            pr_cells(sx - 32 + ((int16_t)c << 4), sy - 24 + ((int16_t)r << 3), zp_mz_gycol, ZP_MZ_GYCOL_N, 0);
}

/* 0x30 Glass pillar (`30 MZ Large Green Glass Blocks.asm`): a pillar of 64 px width. Types: 0 static, 1/2 wobble (+$12),
   3 pushed down by Sonic landing, 4 moved by switch. Height 112 (short) / 144 (tall); top cap + repeated body rows. */
static void mz_glass(ActiveObj *o) {
    uint8_t t = (uint8_t)(o->subtype & 7U);
    uint8_t tall = (uint8_t)(!(o->subtype & 0x08U) && ((o->subtype >> 4) & 1U) == 0);
    int16_t sx, sy, off, h;
    uint8_t k, rows;

    (void)tall;
    h = 112;
    if (t == 1 || t == 2) {
        int16_t v = (int16_t)OSC_HI(0x12);
        if (t == 2) v = 0x40 - v;
        o->y = o->base_y + (v >> 1);
    } else if (t == 4) {
        if (g_button_state & 1U) {
            if (o->y < o->base_y + 0x80) o->y += 2;
        }
    }
    off = 0;
    pr_solid(o, 32 + 11, 28, 32);
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    rows = (uint8_t)((h - 32) >> 4);
    pr_cells(sx, sy - 28, zp_mz_glcap, ZP_MZ_GLCAP_N, 0);
    for (k = 0; k < rows && k < 3; k++) pr_cells(sx, sy - 20 + (int16_t)(k << 3) + off, zp_mz_glmid, ZP_MZ_GLMID_N, 0);
}

/* ------------------------------------------------------------------------------------------------ */
static uint8_t zp_marble(ActiveObj *o) {
    switch (o->id) {
    case 0x2F: mz_grass(o); return 1;
    case 0x30: mz_glass(o); return 1;
    case 0x31: mz_stomper(o); return 1;
    case 0x33: mz_push(o); return 1;
    case 0x46: mz_brick(o); return 1;
    case 0x4C: mz_geyser(o); return 1;
    case 0x4E: mz_lavawall(o); return 1;
    case 0x51: mz_smash(o); return 1;
    case 0x52: zp_mblock(o, zp_mz_mblk, ZP_MZ_MBLK_N, 0); return 1;
    case 0x54: mz_lavatag(o); return 1;
    default: return 0;
    }
}



extern uint8_t zp_syz_slz_object(ActiveObj *o) BANKED;
extern uint8_t zp_lz_sbz_object(ActiveObj *o) BANKED;

/* Entry point called by `objects.c` for the non-GHZ zones (bank 123): returns 1 when the object was handled. */
uint8_t zp_object(ActiveObj *o) BANKED {
    uint8_t v = g_ram_act.vset_idx;
    switch (o->id) {                                         /* objects shared by several zones live here */
    case 0x13:
        if (v == 3) zp_fireball(o, zp_slz_fire0, ZP_SLZ_FIRE0_N, zp_slz_fireh, ZP_SLZ_FIREH_N);
        else zp_fireball(o, zp_mz_fire0, ZP_MZ_FIRE0_N, zp_mz_fireh, ZP_MZ_FIREH_N);
        return 1;
    case 0x32:
        if (v == 4) zp_button(o, zp_syz_but0, zp_syz_but1, ZP_SYZ_BUT0_N);
        else if (v == 1) zp_button(o, zp_lz_but0, zp_lz_but1, ZP_LZ_BUT0_N);
        else zp_button(o, zp_mz_but0, zp_mz_but1, ZP_MZ_BUT0_N);
        return 1;
    case 0x71:
        zp_barrier(o);
        return 1;
    case 0x53:
        if (v == 3) zp_cflo(o, zp_slz_cflo, ZP_SLZ_CFLO_N);
        else return 0;
        return 1;
    case 0x52:
        if (v == 1) {
            if ((o->subtype & 0x0FU) == 7 && !(g_button_state & 4U)) return 1;
            zp_mblock(o, zp_lz_raft, ZP_LZ_RAFT_N, 7);
            return 1;
        }
        break;
    default:
        break;
    }
    if (v == 2) return zp_marble(o);
    if (v == 1 || v >= 5) return zp_lz_sbz_object(o);
    return zp_syz_slz_object(o);
}
