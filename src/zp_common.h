#ifndef ZP_COMMON_H
#define ZP_COMMON_H
/* Helpers shared by the zone-prop banks (`src/zp_*.c`): `ZP_WANT_xxx` selects which of the larger ones a bank needs. */
#include "engine.h"
#include "props_common.h"

#define ZP_SX(o) (((o)->x - g_cam_x) >> 1)
#define ZP_SY(o) (((o)->y - g_cam_y) >> 1)

static uint8_t zp_near(ActiveObj *o, int16_t hw, int16_t hh) {
    int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (uint8_t)(dx < hw + 8 && dy < hh + 12);
}

static void zp_hurt(ActiveObj *o, int16_t hw, int16_t hh) {
    if (!g_sonic.dead && zp_near(o, hw, hh)) sonic_hurt_or_kill(0);
}

/* Free-fall step with 8.8 velocity (`SpeedToPos` + gravity): sub-pixel remainder in `timer`. */
static void zp_fall(ActiveObj *o, int16_t grav) {
    uint16_t s = (uint16_t)o->timer + (uint8_t)o->vy;
    o->y += (o->vy >> 8) + (int16_t)(s >> 8);
    o->timer = (uint8_t)s;
    o->vy += grav;
}

/* `ObjFloorDist`: distance from the object's feet (y + h) to the floor (negative = embedded). */
static int16_t zp_floor(ActiveObj *o, int16_t h) {
    uint8_t ang;
    return hw_floor_distance(o->x, o->y + h, &ang);
}

static uint8_t zp_is_lava(int16_t x, int16_t y) {
    return (uint8_t)((hw_get_chunk_word(x, y, 0) & 0x3FFU) >= 0x16AU);
}

/* ---- sloped / flat platform helper: stand on a heightmap surface at `top` (x range +-w) ------------------------ */
static void zp_stand_slope(ActiveObj *o, int16_t w, int16_t top) {
    if (g_stand_obj == o) {
        if (!pr_still_on(w, o->x)) g_stand_obj = 0;
        else pr_set_y(top - pr_h());
    } else {
        pr_try_land(o, w, top, o->x);
    }
}





#ifdef ZP_WANT_COMMON
/* 0x32 Button (`32 Button.asm`): pressed while Sonic stands on it; sets `g_button_state` bit (subtype & 7). */
static void zp_button(ActiveObj *o, const PropCell *c0, const PropCell *c1, uint8_t n) {
    uint8_t bit = (uint8_t)(1U << (o->subtype & 7U));
    uint8_t pressed;
    if (o->state == 0) {
        o->state = 1;
        o->y += 3;
    }
    pr_solid(o, 27, 5, 16);
    pressed = (uint8_t)(g_stand_obj == o);
    if ((o->subtype & 0x80U) && g_ram_act.vset_idx == 2 && !pressed) {   /* `But_MZBlock`: a pushable block pressing the button */
        uint8_t j;
        for (j = 0; j < MAX_ACTIVE_OBJS; j++) {
            ActiveObj *b = &g_objs[j];
            int16_t dx = b->x - o->x, dy = o->y - b->y;
            if (b->active && b->id == 0x33 && dx > -32 && dx < 32 && dy > 8 && dy < 32) pressed = 1;
        }
    }
    if (pressed) {
        if (!(g_button_state & bit)) hw_play_sfx(SFX_SWITCH);
        g_button_state |= bit;
    } else {
        g_button_state &= (uint8_t)~bit;
    }
    pr_cells(ZP_SX(o), ZP_SY(o), pressed ? c1 : c0, n, 0);
}

/* 0x13 Lava ball maker + ball (`13, 14 MZ, SLZ Fire Balls and Maker.asm`): one live ball per maker.
   hp = interval, timer = countdown, state: 0 init, 1 idle, 2 ball flying; vx = ball velocity, vy = ball offset. */
static void zp_fireball(ActiveObj *o, const PropCell *vert, uint8_t vn, const PropCell *horz, uint8_t hn) {
    static const int16_t k_spd[9] = { -0x400, -0x500, -0x600, -0x700, -0x200, 0x200, -0x200, 0x200, 0 };
    uint8_t st = (uint8_t)(o->subtype & 0x0FU);
    int16_t sx, sy;
    uint8_t ang;

    if (o->state == 0) {
        static const uint8_t k_rate[6] = { 30, 60, 90, 120, 150, 180 };
        uint8_t r = (uint8_t)((o->subtype >> 4) & 0x0FU);
        o->hp = k_rate[r > 5 ? 5 : r];
        o->timer = o->hp;
        o->subtype = st;
        o->state = 1;
        o->base_y = 0;
        return;
    }
    if (o->state == 1) {
        if (--o->timer == 0) {
            o->timer = o->hp;
            if (o->x > g_cam_x - 16 && o->x < g_cam_x + 336 && o->y > g_cam_y - 16 && o->y < g_cam_y + 304) {
                o->state = 2;
                o->vx = k_spd[st > 8 ? 8 : st];
                o->vy = 0;
                o->base_y = 0;
                hw_play_sfx(SFX_FIREBALL);
            }
        }
        return;
    }
    /* flying */
    if (st >= 6) {
        int16_t s = (int16_t)((int32_t)o->vx >> 8);
        o->vy += s;
        sx = ((o->x + o->vy) - g_cam_x) >> 1;
        sy = ZP_SY(o);
        if (hw_rwall_distance(o->x + o->vy + (s < 0 ? -8 : 8), o->y, &ang) < 0 && (s > 0)) o->state = 1;
        if (o->vy > 320 || o->vy < -320) o->state = 1;
        if (!g_sonic.dead && ((g_sonic.x - (o->x + o->vy)) < 16 && (g_sonic.x - (o->x + o->vy)) > -16) &&
            ((g_sonic.y - o->y) < 22 && (g_sonic.y - o->y) > -22)) sonic_hurt_or_kill(0);
        pr_cells(sx, sy, horz, hn, (uint8_t)(s < 0));
        return;
    }
    {
        uint16_t sacc = (uint16_t)(uint8_t)o->base_y + (uint8_t)o->vx;
        int16_t dy = (o->vx >> 8) + (int16_t)(sacc >> 8);
        o->base_y = (int16_t)(uint8_t)sacc;
        if (st <= 3) {
            o->vx += 0x18;
            o->vy += dy;
            if (o->vy > 0) {
                o->state = 1;
                return;
            }
        } else {
            o->vy += dy;
            if (st == 4 ? (hw_ceil_surface_distance(o->x, o->y + o->vy - 8, &ang) < 0)
                        : (hw_floor_distance(o->x, o->y + o->vy + 8, &ang) < 0)) {
                o->state = 1;
                return;
            }
        }
        sx = ZP_SX(o);
        sy = ((o->y + o->vy) - g_cam_y) >> 1;
        if (!g_sonic.dead && ((g_sonic.x - o->x) < 16 && (g_sonic.x - o->x) > -16) &&
            ((g_sonic.y - (o->y + o->vy)) < 22 && (g_sonic.y - (o->y + o->vy)) > -22)) sonic_hurt_or_kill(0);
        if ((st <= 3 && o->vx < 0) || st == 4) {
            int8_t k;
            for (k = 0; k < vn; k++)
                hw_put_sprite8(sx + vert[k].dx, sy - 8 - vert[k].dy - 8 + 8 * 0, vert[k].tile, vert[k].attr ^ 0x40);
        } else {
            pr_cells(sx, sy, vert, vn, 0);
        }
    }
}

#endif

#if defined(ZP_WANT_COMMON) || defined(ZP_WANT_MBLK)
/* 0x52 Moving blocks (`52 Moving Blocks.asm`), MZ single/triple block flavours. */
static void zp_mblock(ActiveObj *o, const PropCell *c, uint8_t n, int16_t ph) {
    static const uint8_t k_w[5] = { 16, 32, 32, 64, 48 };
    uint8_t t, was, k, nb;
    int16_t old_x, d, w, v;
    uint8_t ang;

    if (o->state == 0) {
        uint8_t wi = (uint8_t)((o->subtype >> 4) & 7U);
        if (wi > 4) wi &= 3U;                         /* converter-ORed X flip (bit 6) */
        o->hp = k_w[wi > 4 ? 0 : wi];
        o->subtype &= 0x0FU;
        o->state = 1;
        o->vy = 0;
        o->vx = 0;
        o->timer = 0;
    }
    w = o->hp;
    old_x = o->x;
    was = (g_stand_obj == o);
    t = (uint8_t)(o->subtype & 0x0FU);
    if (was && !pr_still_on(w, o->x)) {
        g_stand_obj = 0;
        was = 0;
    }
    switch (t) {
    case 1:
        v = (int16_t)OSC_HI(0x0E);
        o->x = o->base_x - v;
        break;
    case 2: case 4: case 9:
        if (was) o->subtype++;
        break;
    case 3: case 5:
        if (hw_rwall_distance(o->x + w, o->y, &ang) < 0) {
            o->subtype = (t == 3) ? 0 : 6;
        } else {
            o->x++;
        }
        break;
    case 6:
        zp_fall(o, 0x18);
        d = zp_floor(o, ph);
        if (d < 0) {
            o->y += d;
            o->vy = 0;
            o->subtype = 0;
        }
        break;
    case 8:
        o->y = o->base_y - (int16_t)OSC_HI(0x1E);
        break;
    case 10: {      /* SBZ sliding floor: 8 px/frame out, wait 5 s, back */
        int16_t dist = w << 1;
        if (o->hp == 0) break;
        if (o->vy) {
            if (o->x == o->base_x) {
                o->vy = 0;
                o->subtype--;
            } else {
                o->x -= 8;
            }
        } else if (o->x - o->base_x == dist) {
            if (--o->vx == 0) o->vy = 1;
        } else {
            o->x += 8;
            o->vx = 300;
        }
        break;
    }
    default:
        break;
    }
    if (!was) {
        pr_try_land(o, w, o->y - 8, o->x);
    } else if (g_stand_obj == o) {
        pr_set_y(o->y - 9 - pr_h());
        pr_move_x(o->x - old_x);
    }
    nb = (uint8_t)(w >> 4);
    if (nb == 0) nb = 1;
    for (k = 0; k < nb; k++) {
        pr_cells(((o->x - g_cam_x) >> 1) + ((int16_t)k << 4) - ((int16_t)(nb - 1U) << 3), ZP_SY(o), c, n, 0);
    }
}

/* 0x71 Invisible solid barrier (`71 Invisible Solid Barriers.asm`): width/height from the subtype nibbles. */
static void zp_barrier(ActiveObj *o) {
    int16_t w = (int16_t)(((o->subtype & 0x0FU) << 3) + 8);
    int16_t h = (int16_t)(((o->subtype >> 4) << 3) + 8);
    pr_solid(o, w + 11, h, w);
}

/* 0x53 Collapsing floor (`1A, 53 Collapsing Ledges and Floors.asm`): flat 64 px floor that crumbles shortly after Sonic
   steps on it. `c`/`n` = the piece cells of the floor art. */
static void zp_cflo(ActiveObj *o, const PropCell *c, uint8_t n) {
    int16_t sx = ZP_SX(o), sy = ZP_SY(o);
    uint8_t k;
    if (o->state == 0) {
        o->state = 1;
        o->timer = 7;
        o->vx = 0;
    }
    if (o->state == 1) {
        if (g_stand_obj != o) {
            if (o->vx) {
                if (o->timer == 0) { o->state = 2; o->hp = 0; pr_taken(o); hw_play_sfx(SFX_COLLAPSE); }
                else o->timer--;
            }
            if (o->state == 1) pr_try_land(o, 32, o->y - 8, o->x);
        } else {
            if (o->timer == 0) { o->state = 2; o->hp = 0; pr_taken(o); hw_play_sfx(SFX_COLLAPSE); }
            else { o->vx = 1; o->timer--; }
            if (!pr_still_on(32, o->x)) g_stand_obj = 0;
            else pr_set_y(o->y - 8 - pr_h());
        }
    }
    if (o->state == 2) {
        uint8_t t = o->hp;
        if (t < 200) o->hp = (uint8_t)(t + 1U);
        if (g_stand_obj == o) g_stand_obj = 0;
        if (t > 80) { o->active = 0; return; }
        for (k = 0; k < n; k++) {
            uint8_t d = (uint8_t)((k & 3U) * 5U + (k >> 2));
            int16_t yo = 0;
            if (t > d) {
                uint16_t dt = (uint16_t)(t - d);
                yo = (int16_t)((7U * dt * dt) >> 5);
            }
            hw_put_sprite8(sx + c[k].dx, sy + c[k].dy + yo, c[k].tile, c[k].attr);
        }
        return;
    }
    pr_cells(sx, sy, c, n, 0);
}

#endif

#ifdef ZP_WANT_SBALL
/* 0x57 Spiked ball and chain (`57 SYZ, LZ Spiked Ball and Chain.asm`): a chain of `n` links twirling around the origin. */
static void zp_sball(ActiveObj *o, const PropCell *c, uint8_t cn, uint8_t harm) {
    uint8_t n = (uint8_t)(o->subtype & 7U), k, a;
    int16_t sn, cs, lx, ly;
    if (o->state == 0) {
        o->state = 1;
        o->vx = 0;
    }
    o->vx += (int16_t)((int8_t)(o->subtype & 0xF0U)) << 3;
    a = (uint8_t)((uint16_t)o->vx >> 8);
    sn = hw_sin(a);
    cs = hw_cos(a);
    for (k = 1; k <= n; k++) {
        int16_t r = (int16_t)k << 4;
        lx = o->base_x + (int16_t)(((int32_t)r * cs) >> 8);
        ly = o->base_y + (int16_t)(((int32_t)r * sn) >> 8);
        if (k == n) {
            o->x = lx;
            o->y = ly;
        }
        pr_cells((lx - g_cam_x) >> 1, (ly - g_cam_y) >> 1, c, cn, 0);
        if (harm && !g_sonic.dead && ((g_sonic.x - lx) < 16 && (g_sonic.x - lx) > -16) &&
            ((g_sonic.y - ly) < 22 && (g_sonic.y - ly) > -22)) sonic_hurt_or_kill(0);
    }
    pr_cells((o->base_x - g_cam_x) >> 1, (o->base_y - g_cam_y) >> 1, c, cn, 0);
}
#endif

#endif
