#pragma bank 122
#define PR_WANT_PLATFORM
#define ZP_WANT_SBALL
#define PR_WANT_SMASH
#include "zp_common.h"
#include "zp_syz.h"
#include "zp_slz.h"

/* =============================================================================================== */
/* SPRING YARD ZONE                                                                                 */
/* =============================================================================================== */

/* 0x56 Floating blocks (`56 SYZ, SLZ Floating Blocks and LZ Doors.asm`): oscillating blocks (SYZ types 0..4). */
static void syz_fblock(ActiveObj *o) {
    static const uint8_t k_wh[8][2] = { {16,16}, {32,32}, {16,32}, {32,26}, {16,39}, {16,16}, {8,32}, {64,16} };
    uint8_t fr = (uint8_t)((o->subtype >> 4) & 3U), t = (uint8_t)(o->subtype & 0x0FU);
    uint8_t xflip = (uint8_t)((o->subtype & 0x40U) != 0);   /* converter ORs the object's X flip into bit 6 */
    int16_t old_x = o->x, v, w, h, sx, sy;
    uint8_t k;

    if (o->state == 0) {
        o->state = 1;
        o->hp = k_wh[fr][0];
        o->timer = k_wh[fr][1];
        o->vy = 0;
    }
    w = o->hp;
    h = o->timer;
    if (t == 1 || t == 2 || t == 3 || t == 4) {
        if (t & 1U) {
            v = (int16_t)OSC_HI(0x0A);                 /* small: +$A, range $20 */
            if (xflip) v = 0x40 - v;
        } else {
            v = (int16_t)OSC_HI(0x1E);                 /* large: +$1E, range $40 */
            if (xflip) v = 0x80 - v;
        }
        if (t <= 2) o->x = o->base_x - v;
        else o->y = o->base_y - v;
    }
    else if (t == 7) {                                   /* SYZ3 tunnel block: slides right once switch $F is pressed */
        if (o->vy == 0 && (g_button_state & 0x80U)) { o->vy = 1; o->vx = 0; }
        if (o->vy) {
            o->x++;
            if (++o->vx >= 0x380) { o->vy = 0; o->subtype = (uint8_t)(o->subtype & 0xF0U); }
        }
    }
    pr_solid(o, w + 11, h, w);
    if (g_stand_obj == o && o->x != old_x) pr_move_x(o->x - old_x);
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    if (fr == 1 || fr == 3) {
        pr_cells(sx, sy, zp_syz_blk1, ZP_SYZ_BLK1_N, 0);
    } else if (fr == 2) {
        pr_cells(sx, sy - 16, zp_syz_door, ZP_SYZ_DOOR_N, 0);
        pr_cells(sx, sy, zp_syz_door, ZP_SYZ_DOOR_N, 0);
    } else {
        pr_cells(sx, sy, zp_syz_blk0, ZP_SYZ_BLK0_N, 0);
    }
    (void)k;
}

/* 0x58 Big spiked ball (`58 SYZ Big Spiked Ball.asm`): stationary / left-right / up-down / circling (radius $50). */
static void syz_bball(ActiveObj *o, const PropCell *c, uint8_t n) {
    uint8_t t = (uint8_t)(o->subtype & 3U);
    int16_t v;
    if (o->state == 0) {
        o->state = 1;
        o->vx = 0;
    }
    if (t == 1) {
        v = (int16_t)OSC_HI(0x0E);
        if (o->subtype & 0x40U) v = 0x60 - v;
        o->x = o->base_x - v;
    } else if (t == 2) {
        v = (int16_t)OSC_HI(0x0E);
        if (o->subtype & 0x40U) v = 0x80 - v;
        o->y = o->base_y - v;
    } else if (t == 3) {
        int8_t sp = (int8_t)(o->subtype & 0xF0U);
        uint8_t a;
        o->vx += (int16_t)sp << 3;
        a = (uint8_t)((uint16_t)o->vx >> 8);
        o->x = o->base_x + (int16_t)(((int32_t)0x50 * hw_cos(a)) >> 8);
        o->y = o->base_y + (int16_t)(((int32_t)0x50 * hw_sin(a)) >> 8);
    }
    zp_hurt(o, 20, 20);
    pr_cells(ZP_SX(o), ZP_SY(o), c, n, 0);
}


/* 0x47 Bumper (`47 SYZ Bumper.asm`): bounces Sonic away from the centre at $700. */
static void syz_bumper(ActiveObj *o) {
    int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y, ax, ay, d;
    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    if (o->timer) o->timer--;
    if (!g_sonic.dead && ax < 24 && ay < 24) {
        d = (ax > ay) ? (int16_t)(ax + (ay >> 1) - (ax >> 3)) : (int16_t)(ay + (ax >> 1) - (ay >> 3));
        if (d < 1) d = 1;
        g_sonic.xsp = (int16_t)(((int32_t)dx * 0x700) / d);
        g_sonic.ysp = (int16_t)(((int32_t)dy * 0x700) / d);
        g_sonic.gsp = g_sonic.xsp;
        g_sonic.on_ground = 0;
        g_sonic.jumping = 0;
        o->timer = 8;
        add_score_points(10);
        hw_play_sfx(SFX_BUMPER);
        g_stand_obj = 0;
    }
    pr_cells(ZP_SX(o), ZP_SY(o), o->timer ? zp_syz_bump1 : zp_syz_bump0, o->timer ? ZP_SYZ_BUMP1_N : ZP_SYZ_BUMP0_N, 0);
}

static uint8_t zp_spring(ActiveObj *o) {
    switch (o->id) {
    case 0x12: return 1;                                   /* search light: pure decoration (level tiles) */
    case 0x18: pr_platform(o, zp_syz_plat, ZP_SYZ_PLAT_N, 0, 0, 0); return 1;
    case 0x47: syz_bumper(o); return 1;
    case 0x56: syz_fblock(o); return 1;
    case 0x57: zp_sball(o, zp_syz_sball, ZP_SYZ_SBALL_N, 1); return 1;
    case 0x58: syz_bball(o, zp_syz_bball, ZP_SYZ_BBALL_N); return 1;
    default: return 0;
    }
}


/* =============================================================================================== */
/* STAR LIGHT ZONE                                                                                  */
/* =============================================================================================== */

/* 0x5A Circling platform (`5A SLZ Circling Platform.asm`): ellipse from oscillators +$22 / +$26, radius $50. */
static void zp_circ(ActiveObj *o, const PropCell *c, uint8_t n) {
    int16_t d1, d2, t, old_x = o->x;
    uint8_t was = (g_stand_obj == o);
    d1 = (int16_t)(int8_t)(OSC_HI(0x22) - 0x50);
    d2 = (int16_t)(int8_t)(OSC_HI(0x26) - 0x50);
    if (o->subtype & 1U) { d1 = -d1; d2 = -d2; }
    if (o->subtype & 2U) { d1 = -d1; t = d1; d1 = d2; d2 = t; }
    if (o->subtype & 4U) d1 = -d1;
    o->x = o->base_x + d1;
    o->y = o->base_y + d2;
    if (was && !pr_still_on(24, o->x)) { g_stand_obj = 0; was = 0; }
    if (!was) pr_try_land(o, 24, o->y - 8, o->x);
    else { pr_set_y(o->y - 9 - pr_h()); pr_move_x(o->x - old_x); }
    pr_cells(ZP_SX(o), ZP_SY(o), c, n, 0);
}

/* 0x5B Staircase (`5B SLZ Staircase.asm`): four 32 px blocks; vx/vy hold the 4 block offsets (u8 each),
   state 1 = idle, 2 = counting down (hp), 3 = moving. */
static void slz_stair(ActiveObj *o) {
    uint8_t off[4];
    uint8_t k, flipx = (uint8_t)((o->subtype & 0x40U) != 0), type = (uint8_t)(o->subtype & 3U);
    int16_t sx;
    ActiveObj blk;

    if (o->state == 0) {
        o->state = 1;
        o->vx = 0;
        o->vy = 0;
        o->hp = 0;
    }
    off[0] = (uint8_t)o->vx;
    off[1] = (uint8_t)((uint16_t)o->vx >> 8);
    off[2] = (uint8_t)o->vy;
    off[3] = (uint8_t)((uint16_t)o->vy >> 8);
    sx = ZP_SX(o);
    for (k = 0; k < 4; k++) {
        uint8_t kk = flipx ? (uint8_t)(3U - k) : k;
        int16_t bx = o->x + ((int16_t)k << 5) - 48;
        int16_t by = o->y + (int16_t)off[kk];
        uint8_t mine = (uint8_t)(g_stand_obj == o && g_sonic.x >= bx - 16 && g_sonic.x < bx + 16);
        if (type == 2 && o->state == 2 && (o->hp & 4U)) by++;
        blk.x = bx;
        blk.y = by;
        if (mine) g_stand_obj = &blk;
        pr_solid(&blk, 27, 16, 16);
        if (g_stand_obj == &blk) g_stand_obj = o;
        else if (mine) g_stand_obj = 0;
        pr_cells(sx + ((int16_t)k << 4) - 24, (by - g_cam_y) >> 1, zp_slz_stair, ZP_SLZ_STAIR_N, 0);
    }
    if (o->state == 1) {
        if (type == 0 && g_stand_obj == o) { o->state = 2; o->hp = 30; }
        else if (type == 2 && g_sonic.ysp < 0 && !g_sonic.on_ground) {
            int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y;
            if (dx > -64 && dx < 64 && dy > -8 && dy < 40) { o->state = 2; o->hp = 60; }
        }
    } else if (o->state == 2) {
        if (--o->hp == 0) o->state = 3;
    } else if (off[0] < 128) {
        uint8_t d = (uint8_t)(off[0] + 1U);
        off[0] = d;
        off[1] = (uint8_t)((d >> 1) + (d >> 2));
        off[2] = (uint8_t)(d >> 1);
        off[3] = (uint8_t)(d >> 2);
        o->vx = (int16_t)((uint16_t)off[0] | ((uint16_t)off[1] << 8));
        o->vy = (int16_t)((uint16_t)off[2] | ((uint16_t)off[3] << 8));
    }
}

/* 0x59 Elevator (`59 SLZ Elevators.asm`): vx = speed (8.8 px/frame), vy = distance (px), timer = fraction,
   hp = half distance, subtype = action type (1/3/5/7 wait for Sonic, 2 rise, 4 descend, 6 rise right, 8 descend left,
   9 riser of a spawner that restarts at its origin), state bit 0 = slowing down. */
static void slz_elev(ActiveObj *o) {
    static const uint8_t k_var[15][2] = {
        { 0x10, 1 }, { 0x20, 1 }, { 0x34, 1 }, { 0x10, 3 }, { 0x20, 3 }, { 0x34, 3 }, { 0x14, 1 }, { 0x24, 1 },
        { 0x2C, 1 }, { 0x14, 3 }, { 0x24, 3 }, { 0x2C, 3 }, { 0x20, 5 }, { 0x20, 7 }, { 0x30, 9 }
    };
    uint8_t t, was;
    int16_t old_x = o->x;

    if (o->state == 0) {
        uint8_t sub = o->subtype;
        uint8_t i = (sub & 0x80U) ? 14U : (uint8_t)(sub > 14 ? 0 : sub);
        o->hp = (uint8_t)(k_var[i][0] << 2);
        o->subtype = k_var[i][1];
        o->state = 2;
        o->vx = 0;
        o->vy = 0;
        o->timer = 0;
    }
    t = o->subtype;
    was = (g_stand_obj == o);
    if (was && !pr_still_on(40, o->x)) { g_stand_obj = 0; was = 0; }
    if (!was) pr_try_land(o, 40, o->y - 8, o->x);

    if ((t & 1U) && t != 9) {
        if (g_stand_obj == o) o->subtype++;                  /* NextOnTouch */
    } else if (t >= 2 && t <= 9) {
        uint16_t s;
        int16_t dist;
        if (!(o->state & 1U)) {
            if (o->vx < 0x800) o->vx += 0x10;
        } else if (o->vx > 0) {
            o->vx -= 0x10;
        }
        s = (uint16_t)o->timer + (uint16_t)o->vx;
        o->vy += (int16_t)(s >> 8);
        o->timer = (uint8_t)s;
        if (o->vy > (int16_t)o->hp) o->state |= 1U;
        if (o->vy >= (int16_t)((uint16_t)o->hp << 1)) {
            o->subtype = 0;
            if (t == 9) {                                    /* spawner platform: vanish and restart from the origin */
                if (g_stand_obj == o) { g_stand_obj = 0; g_sonic.on_ground = 0; }
                o->subtype = 9;
                o->vy = 0;
                o->vx = 0;
                o->timer = 0;
                o->state &= (uint8_t)~1U;
            }
        }
        dist = o->vy;
        switch (t) {
        case 2: case 9: o->y = o->base_y - dist; break;
        case 4: o->y = o->base_y + dist; break;
        case 6: o->y = o->base_y - (dist >> 1); o->x = o->base_x + dist; break;
        case 8: o->y = o->base_y + (dist >> 1); o->x = o->base_x - dist; break;
        default: break;
        }
    }
    if (g_stand_obj == o) {
        pr_set_y(o->y - 9 - pr_h());
        pr_move_x(o->x - old_x);
    }
    pr_cells(ZP_SX(o), ZP_SY(o), zp_slz_elev, ZP_SLZ_ELEV_N, 0);
}

/* 0x5D Fan (`5D SLZ Fan.asm`): blows Sonic away horizontally while switched on (2 s off / 3 s on unless subtype & 2). */
static void slz_fan(ActiveObj *o) {
    uint8_t xf = (uint8_t)((o->subtype & 0x40U) != 0);
    int16_t d0, d1;
    if (o->state == 0) {
        o->state = 1;
        o->timer = 0;
        o->vx = 0;
    }
    if (!(o->subtype & 2U)) {
        if (o->timer) {
            o->timer--;
        } else if (o->vx == 0) {
            o->vx = 1;
            o->timer = 120;
        } else {
            o->vx = 0;
            o->timer = 180;
        }
    }
    if (o->vx == 0 || (o->subtype & 2U)) {                   /* fan is on while `vx` is 0 */
        d0 = g_sonic.x - o->x;
        if (!xf) d0 = -d0;
        d0 += 0x50;
        d1 = (g_sonic.y + 0x60) - o->y;
        if (!g_sonic.dead && (uint16_t)d0 < 0xF0U && d1 >= 0 && d1 < 0x70) {
            d0 -= 0x50;
            if (d0 < 0) d0 = (int16_t)(~d0) << 1;
            d0 += 0x60;
            if (!xf) d0 = -d0;
            d0 = (int16_t)((d0 & 0xFF00) | (uint8_t)(-(int8_t)(d0 & 0xFF)));
            d0 >>= 4;
            if (o->subtype & 1U) d0 = -d0;
            pr_move_x(d0);
        }
    }
    {
        uint8_t odd = (uint8_t)((g_frame_counter >> 1) & 1U);
        pr_cells(ZP_SX(o), ZP_SY(o), odd ? zp_slz_fan1 : zp_slz_fan0, ZP_SLZ_FAN0_N, xf);
    }
}

/* 0x5E Seesaw (`5E SLZ Seesaw.asm`) with its spike ball. state bits 0-1 = tilt (0 right side down, 1 flat, 2 left side down),
   bits 2-3 = ball side (0 right, 2 left), bit 4 = ball in the air, bits 5-6 = launch speed index.
   vx = Sonic's landing speed, vy = ball Y speed, timer = ball dy (px, relative), hp = ball y sub-pixel, base_y = ball dx (8.8). */
static int16_t slz_see_top(ActiveObj *o, uint8_t tilt) {
    int16_t d0 = g_sonic.x - o->x + 48;
    uint8_t idx, h;
    if (d0 < 0) d0 = 0;
    if (d0 > 95) d0 = 95;
    if (tilt == 1) return o->y - 0x15;
    if (tilt == 2) d0 = 95 - d0;
    idx = (uint8_t)(d0 >> 1);
    if (idx < 2) h = 0x24;
    else if (idx < 6) h = (uint8_t)(0x24 + ((idx - 1U) << 1));
    else if (idx < 10) h = (uint8_t)(0x2C - ((idx - 5U) << 1));
    else if (idx < 43) h = (uint8_t)(0x23 - (idx - 10U));
    else h = 2;
    return o->y - (int16_t)h;
}

static void slz_seesaw(ActiveObj *o) {
    static const int8_t k_yoff[5] = { -8, -28, -47, -28, -8 };
    static const int16_t k_vxs[3] = { 0x114, 0xCC, 0xA0 };
    uint8_t tilt, bside, air, was;
    int16_t sx, sy, bdx, bdy, dx;

    if (o->state == 0) {
        o->state = 1;                                         /* flat, ball on the right */
        o->vx = 0;
        o->vy = 0;
        o->timer = 0;
        o->hp = 0;
        o->base_y = 40 << 8;
    }
    tilt = (uint8_t)(o->state & 3U);
    bside = (uint8_t)((o->state >> 2) & 3U);
    air = (uint8_t)((o->state >> 4) & 1U);
    was = (g_stand_obj == o);

    if (was) {
        if (!pr_still_on(48, o->x)) {
            g_stand_obj = 0;
            was = 0;
        } else {
            dx = o->x - g_sonic.x;
            tilt = (uint8_t)(dx >= 0 ? (dx < 8 ? 1 : 2) : (-dx < 8 ? 1 : 0));
        }
    } else if (g_sonic.ysp >= 0) {
        o->vx = g_sonic.ysp;
    }
    if (!was && pr_try_land(o, 48, slz_see_top(o, tilt), o->x)) was = 1;
    if (was && g_stand_obj == o) pr_set_y(slz_see_top(o, tilt) - pr_h());

    if (!air) {
        if (bside != tilt) {
            uint8_t dd = (uint8_t)(bside > tilt ? bside - tilt : tilt - bside);
            int16_t vy = -0x818;
            uint8_t vi = 0;
            if (dd != 1) {
                vy = -0xAF0;
                vi = 1;
                if (o->vx >= 0xA00) { vy = -0xE00; vi = 2; }
            }
            o->vy = vy;
            o->hp = 0;
            air = 1;
            o->state = (uint8_t)((o->state & 0x8FU) | 0x10U | (uint8_t)(vi << 5));
        } else {
            o->base_y = (int16_t)((bside ? -40 : 40) << 8);
            o->timer = (uint8_t)k_yoff[tilt + (bside ? 2 : 0)];
        }
    }
    if (air) {
        uint8_t vi = (uint8_t)((o->state >> 5) & 3U);
        uint16_t sacc;
        int16_t vxs = (bside == 0) ? -k_vxs[vi] : k_vxs[vi];
        o->base_y += vxs;
        sacc = (uint16_t)o->hp + (uint8_t)o->vy;
        o->timer = (uint8_t)((int8_t)o->timer + (int8_t)((o->vy >> 8) + (int16_t)(sacc >> 8)));
        o->hp = (uint8_t)sacc;
        o->vy += 0x38;
        if (o->vy >= 0) {
            int16_t bx = o->base_y >> 8;
            uint8_t side = (uint8_t)(bx < 0 ? 2 : 0);
            int16_t ty = k_yoff[tilt + (side ? 2 : 0)];
            if ((int8_t)o->timer >= ty) {
                uint8_t nt = (uint8_t)(vxs < 0 ? 2 : 0);          /* flying left lands on the left side */
                if (nt != tilt && g_stand_obj == o) {
                    g_sonic.ysp = -o->vy;
                    g_sonic.on_ground = 0;
                    g_sonic.jumping = 0;
                    g_stand_obj = 0;
                    hw_play_sfx(SFX_SPRING);
                }
                o->vy = 0;
                tilt = nt;
                bside = nt;
                air = 0;
            }
        }
    }
    o->state = (uint8_t)((o->state & 0x70U) | tilt | (uint8_t)(bside << 2));
    if (!air) o->state &= (uint8_t)~0x10U;
    bdx = (int16_t)(o->base_y >> 8);
    bdy = (int8_t)o->timer;
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    if (!g_sonic.dead && ((g_sonic.x - (o->x + bdx)) < 20 && (g_sonic.x - (o->x + bdx)) > -20) &&
        ((g_sonic.y - (o->y + bdy)) < 24 && (g_sonic.y - (o->y + bdy)) > -24)) sonic_hurt_or_kill(0);
    if (tilt == 1) pr_cells(sx, sy, zp_slz_see1, ZP_SLZ_SEE1_N, 0);
    else pr_cells(sx, sy, zp_slz_see0, ZP_SLZ_SEE0_N, (uint8_t)(tilt == 2));
    pr_cells(sx + (bdx >> 1), sy + (bdy >> 1), zp_slz_ball, ZP_SLZ_BALL_N, 0);
}

/* 0x56 SLZ rotating stair blocks (types 8..B): a 32 px block moving round a square (oscillators +$2A..+$36). */
static void slz_fblock(ActiveObj *o) {
    static const uint8_t k_off[4] = { 0x2A, 0x2E, 0x32, 0x36 };
    static const uint8_t k_rng[4] = { 0x10, 0x30, 0x50, 0x70 };
    uint8_t t = (uint8_t)((o->subtype & 0x0FU) - 8U);
    int16_t old_x = o->x, d0, d1;
    if (o->state == 0) {
        o->state = 1;
        o->hp = 0;
    }
    if (t < 4) {
        uint8_t off = k_off[t];
        d1 = k_rng[t];
        d0 = (int16_t)OSC_HI(off);
        if (t == 0) d0 >>= 1;
        if (OSC_RATE(off) == 0) o->hp = (uint8_t)((o->hp + 1U) & 3U);
        switch (o->hp) {
        case 0: o->x = o->base_x + d0 - d1; o->y = o->base_y - d1; break;
        case 1: o->y = o->base_y - (d0 - (d1 - 1)); o->x = o->base_x + d1; break;
        case 2: o->x = o->base_x - (d0 - (d1 - 1)); o->y = o->base_y + d1; break;
        default: o->y = o->base_y + d0 - d1; o->x = o->base_x - d1; break;
        }
    }
    pr_solid(o, 16 + 11, 16, 16);
    if (g_stand_obj == o) pr_move_x(o->x - old_x);
    pr_cells(ZP_SX(o), ZP_SY(o), zp_slz_stair, ZP_SLZ_STAIR_N, 0);
}

static uint8_t zp_star(ActiveObj *o) {
    switch (o->id) {
    case 0x18: pr_platform(o, zp_slz_plat, ZP_SLZ_PLAT_N, 0, 0, 1); return 1;
    case 0x1C: case 0x5C: return 1;
    case 0x3C: pr_smash_wall(o, ZP_SX(o), ZP_SY(o), zp_slz_smash); return 1;
    case 0x56: slz_fblock(o); return 1;
    case 0x59: slz_elev(o); return 1;
    case 0x5A: zp_circ(o, zp_slz_circ, ZP_SLZ_CIRC_N); return 1;
    case 0x5B: slz_stair(o); return 1;
    case 0x5D: slz_fan(o); return 1;
    case 0x5E: slz_seesaw(o); return 1;
    default: return 0;
    }
}


uint8_t zp_syz_slz_object(ActiveObj *o) BANKED {
    switch (g_ram_act.vset_idx) {
    case 3: return zp_star(o);
    case 4: return zp_spring(o);
    default: return 0;
    }
}
