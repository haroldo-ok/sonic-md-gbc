#ifndef PROPS_COMMON_H
#define PROPS_COMMON_H
/* Shared Sonic-1 object helpers for the per-zone prop banks (`PlatformObject`, `ExitPlatform`, `MvSonicOnPtfm`, `SolidObject`).
   All static: each bank gets its own copy so nothing here costs a bank switch. */
#include "engine.h"

static int16_t pr_h(void) {
    return g_sonic.rolling ? 14 : 19;
}

static void pr_set_y(int16_t y) {
    g_sonic.y = y;
    g_sonic.y_sub = (int32_t)y << 8;
}

static void pr_move_x(int16_t d) {
    g_sonic.x += d;
    g_sonic.x_sub += (int32_t)d << 8;
}

static void pr_land(ActiveObj *o, int16_t y) {
    pr_set_y(y);
    physics_land_on_object();
    g_stand_obj = o;
}

static void pr_taken(ActiveObj *o) {
    g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1U << (o->spawn_slot & 7U));
}

/* `PlatformObject` / `SlopeObject` + `Plat_NoXCheck_AltY`: land on a surface at `top`; x range [ox-w, ox+w). */
static uint8_t pr_try_land(ActiveObj *o, int16_t w, int16_t top, int16_t ox) {
    int16_t dx, d0;
    if (g_sonic.ysp < 0 || g_sonic.dead) return 0;
    dx = g_sonic.x - ox + w;
    if (dx < 0 || dx >= (w << 1)) return 0;
    d0 = top - (g_sonic.y + pr_h() + 4);
    if (d0 > 0 || d0 < -16) return 0;
    pr_land(o, g_sonic.y + d0 + 3);
    return 1;
}

/* `ExitPlatform`: still on while grounded and inside [ox-w, ox+w). */
static uint8_t pr_still_on(int16_t w, int16_t ox) {
    int16_t dx = g_sonic.x - ox + w;
    return g_sonic.on_ground && dx >= 0 && dx < (w << 1);
}

static void pr_cells(int16_t sx, int16_t sy, const PropCell *c, uint8_t n, uint8_t flip) {
    while (n--) {
        if (flip) hw_put_sprite8(sx - 8 - c->dx, sy + c->dy, c->tile, c->attr ^ 0x20);
        else hw_put_sprite8(sx + c->dx, sy + c->dy, c->tile, c->attr);
        c++;
    }
}

/* `SolidObject` (`sub SolidObject.asm`): d1 = half width (incl. Sonic's solid width), d2 = half height,
   actw = `obActWid` (landing x range). Returns 1 when Sonic is pushed from the side while grounded. */
static uint8_t pr_solid(ActiveObj *o, int16_t d1, int16_t d2, int16_t actw) {
    int16_t h = pr_h();
    int16_t d0, d3, d4, d5, t;

    if (g_stand_obj == o) {
        d0 = g_sonic.x - o->x + d1;
        if (!g_sonic.on_ground || d0 < 0 || d0 >= (d1 << 1)) {
            g_stand_obj = 0;
            return 0;
        }
        pr_set_y(o->y - d2 - h);
        return 0;
    }
    if (g_sonic.dead) return 0;
    d0 = g_sonic.x - o->x + d1;
    if (d0 < 0 || d0 > (d1 << 1)) return 0;
    d2 += h;
    d3 = g_sonic.y - o->y + 4 + d2;
    d4 = d2 << 1;
    if (d3 < 0 || d3 >= d4) return 0;

    d5 = d0;
    if (d1 < d0) {
        d0 -= (d1 << 1);
        d5 = -d0;
    }
    t = d3;
    if (d2 < d3) {
        d3 -= 4;
        d3 -= d4;
        t = -d3;
    }
    if (d5 > t) {
        /* nearer the top / bottom than a side */
        if (d3 >= 0) {
            if (d3 < 0x10) {
                d3 -= 4;
                t = actw + g_sonic.x - o->x;
                if (t >= 0 && t < (actw << 1) && g_sonic.ysp >= 0) {
                    pr_land(o, g_sonic.y - d3 - 1);
                }
            }
        } else if (g_sonic.ysp < 0) {
            pr_set_y(g_sonic.y - d3);
            g_sonic.ysp = 0;
        }
        return 0;
    }
    /* side collision */
    if (t <= 4) return 0;
    if (d0 != 0) {
        if (d0 > 0 ? (g_sonic.xsp >= 0) : (g_sonic.xsp < 0)) {
            g_sonic.xsp = 0;
            g_sonic.gsp = 0;
        }
        pr_move_x(-d0);
    }
    if (!g_sonic.on_ground) return 0;
    if (g_joy & (d0 > 0 ? J_RIGHT : J_LEFT)) g_sonic.anim = ANI_PUSH;
    return 1;
}


#ifdef PR_WANT_PLATFORM
/* ------------------------------------------------------------------------------------------------ */
/* `18 Platforms.asm` (GHZ / SYZ / SLZ). `pc`/`pn`: normal platform cells, `cc`/`cn`: tall GHZ2 column (type $A).    */
/* State: vx = raw Y, vy = fall speed, hp = nudge, timer = delay, state 1 = solid, 2 = detached; base_y = sub-pixel.  */
/* ------------------------------------------------------------------------------------------------ */
static void pr_platform(ActiveObj *o, const PropCell *pc, uint8_t pn, const PropCell *cc, uint8_t cn, uint8_t slz) {
    uint8_t type;
    uint8_t was;
    int16_t old_x, ang;

    if (o->state == 0) {
        o->state = 1;
        o->vx = o->y;
        o->vy = 0;
        o->hp = 0;
        o->timer = 0;
        if (slz) o->subtype = 3;
    }
    type = (uint8_t)(o->subtype & 0x0FU);
    ang = (int16_t)OSC_HI(0x1A);
    old_x = o->x;
    was = (g_stand_obj == o);

    if (o->state == 1) {
        if (was) {
            if (o->hp < 0x40) o->hp += 4;
            if (!pr_still_on(32, o->x)) {
                g_stand_obj = 0;
                was = 0;
            }
        } else {
            if (o->hp) o->hp -= 4;
            pr_try_land(o, 32, o->y - 8, o->x);
        }
    }

    switch (type) {
    case 1: o->x = o->base_x + (ang - 0x40); break;
    case 5: o->x = o->base_x + (0x40 - ang); break;
    case 2: o->vx = o->base_y + (ang - 0x40); break;
    case 6: o->vx = o->base_y + (0x40 - ang); break;
    case 0x0A: o->vx = o->base_y + ((ang - 0x40) >> 1); break;
    case 0x0B: o->vx = o->base_y + ((int16_t)OSC_HI(0x0E) - 0x30); break;
    case 0x0C: o->vx = o->base_y + (0x30 - (int16_t)OSC_HI(0x0E)); break;
    case 3:
        if (o->timer) {
            if (--o->timer == 0) {
                o->timer = 32;
                o->subtype = (uint8_t)((o->subtype & 0xF0U) | 4U);
                o->vy = 0;
                o->base_y = 0;     /* sub-pixel accumulator while falling */
            }
        } else if (g_stand_obj == o) {
            o->timer = 30;
        }
        break;
    case 4:
        if (o->timer && --o->timer == 0) {
            if (g_stand_obj == o) {
                g_sonic.on_ground = 0;
                g_stand_obj = 0;
                g_sonic.ysp = o->vy;
            }
            o->state = 2;
            was = 0;
        }
        {
            uint16_t s = (uint16_t)(uint8_t)o->base_y + (uint8_t)o->vy;
            o->vx += (o->vy >> 8) + (int16_t)(s >> 8);
            o->base_y = (int16_t)(uint8_t)s;
            o->vy += 0x38;
        }
        if (o->vx > g_dyn_bottom_bound + 224) {
            pr_taken(o);
            o->active = 0;
            return;
        }
        break;
    case 7:      /* `Plat_RiseOnSwitch`: waits for switch (upper digit), 1 s, then rises 2 px/frame */
        if (o->timer) {
            if (--o->timer == 0) o->subtype++;
        } else if (g_button_state & (uint8_t)(1U << (o->subtype >> 4))) {
            o->timer = 60;
        }
        break;
    case 8:
        o->vx -= 2;
        if (o->vx == o->base_y - 0x200) o->subtype = 0;
        break;
    default:
        break;
    }
    o->y = o->vx + (hw_sin(o->hp) >> 6);

    if (was && g_stand_obj == o) {
        pr_set_y(o->y - 9 - pr_h());
        pr_move_x(o->x - old_x);
    }

    pr_cells((o->x - g_cam_x) >> 1, (o->y - g_cam_y) >> 1,
             (type == 0x0A && cc) ? cc : pc, (type == 0x0A && cc) ? cn : pn, 0);
}
#endif

#ifdef PR_WANT_SWING
/* ------------------------------------------------------------------------------------------------ */
/* `15 Swinging Platforms.asm`: platform of 48x16 on a chain of `n` links + anchor. Oscillator +$1A, xflip = subtype bit 6. */
/* ------------------------------------------------------------------------------------------------ */
static void pr_swing(ActiveObj *o, const PropCell *pl, uint8_t pln, const PropCell *chain, const PropCell *anchor) {
    uint8_t n = (uint8_t)(o->subtype & 0x0FU);
    uint8_t was, k, a;
    int16_t old_x = o->x, sn, cs, r, cx = g_cam_x, cy = g_cam_y;

    if (n == 0 || n > 8) n = (n == 0) ? 1 : 8;
    was = (g_stand_obj == o);
    if (was && !pr_still_on(24, o->x)) {
        g_stand_obj = 0;
        was = 0;
    }
    if (!was) pr_try_land(o, 24, o->y - 8, o->x);

    a = OSC_HI(0x1A);
    if (o->subtype & 0x40U) a = (uint8_t)(0x80U - a);
    sn = hw_sin(a);
    cs = hw_cos(a);
    r = ((int16_t)n << 4) + 8;
    o->x = o->base_x + (int16_t)(((int32_t)r * cs) >> 8);
    o->y = o->base_y + (int16_t)(((int32_t)r * sn) >> 8);
    if (was && g_stand_obj == o) {
        pr_set_y(o->y - 9 - pr_h());
        pr_move_x(o->x - old_x);
    }
    for (k = 0; k <= n; k++) {
        int16_t lr = (int16_t)k << 4;
        int16_t lx = o->base_x + (int16_t)(((int32_t)lr * cs) >> 8);
        int16_t ly = o->base_y + (int16_t)(((int32_t)lr * sn) >> 8);
        pr_cells((lx - cx) >> 1, (ly - cy) >> 1, k ? chain : anchor, 1, 0);
    }
    pr_cells((o->x - cx) >> 1, (o->y - cy) >> 1, pl, pln, 0);
}
#endif

#ifdef PR_WANT_SMASH
/* `3C GHZ, SLZ Smashable Wall`: roll into it at >= $480 to break it into 8 flying pieces (cells `c[8]`). */
/* `Smash_FragSpd1/2` (8.8 -> whole pixels: every entry is a multiple of $100). */
static const int8_t k_smash_v1[16] = { 4, -5, 6, -1, 6, 1, 4, 5, 6, -6, 8, -2, 8, 2, 6, 6 };
static const int8_t k_smash_v2[16] = { -6, -6, -8, -2, -8, 2, -6, 6, -4, -5, -6, -1, -6, 1, -4, 5 };

static void pr_smash_wall(ActiveObj *o, int16_t sx, int16_t sy, const PropCell *c) {
    uint8_t k;

    if (o->state == 0) {
        int16_t spd = g_sonic.on_ground ? g_sonic.gsp : g_sonic.xsp;
        uint8_t push = pr_solid(o, 27, 32, 16);
        int16_t a = spd < 0 ? -spd : spd;
        if (push && g_sonic.rolling && a >= 0x0480) {
            g_sonic.xsp = spd;
            g_sonic.gsp = spd;
            pr_move_x(spd < 0 ? -4 : 4);
            o->state = 1;
            o->timer = 0;
            o->hp = (uint8_t)(spd < 0);
            pr_taken(o);
            hw_play_sfx(SFX_WALLSMASH);
        } else {
            pr_cells(sx, sy, c, 8, 0);
            return;
        }
    }
    {
        const int8_t *v = o->hp ? k_smash_v2 : k_smash_v1;
        uint16_t t = o->timer;
        if (t > 70U) {
            o->active = 0;
            return;
        }
        o->timer++;
        for (k = 0; k < 8; k++) {
            int16_t xo = (int16_t)v[k << 1] * (int16_t)t;
            int16_t yo = (int16_t)v[(k << 1) + 1] * (int16_t)t + (int16_t)((7U * t * (t ? t - 1U : 0U)) >> 5);
            hw_put_sprite8(sx + c[k].dx + (xo >> 1), sy + c[k].dy + (yo >> 1), c[k].tile, c[k].attr);
        }
    }
}
#endif


#endif
