#pragma bank 127
#include "engine.h"
#include "ghz_props.h"

/* ============================================================================================
 * Green Hill Zone interactive props, ported from `s1disasm/_incObj/`:
 *   0x11 GHZ bridge        (`11 GHZ Bridge.asm`: 12 logs, nudge/sine sag, `Bri_Data_Align`)
 *   0x18 platforms         (`18 Platforms.asm`: oscillator-driven slide/bob, fall-after-stand, GHZ2 column)
 *   0x1A collapsing ledge  (`1A, 53 Collapsing Ledges and Floors.asm`: slope top, 7-frame delay, 25 pieces)
 *   0x1C bridge stump      (`1C GHZ, SYZ Scenery.asm`)
 *   0x3B purple rock       (`3B GHZ Purple Rock.asm`: `SolidObject`)
 *   0x3C smashable wall    (`3C GHZ, SLZ Smashable Wall.asm`: roll at >= $480 to break into 8 pieces)
 * Art is the real Mega Drive art at half scale (`tools/ghz_props.py` -> generated/ghz_props.h, zone sprite tiles 40..67).
 * Sonic stands on these like in the original: `g_stand_obj` is set, the ground sensors are skipped and the object
 * glues Sonic to its surface every frame (no more "snap + fall + snap" jitter).
 * ============================================================================================ */

ActiveObj *g_stand_obj;
static uint16_t s_osc_v;     /* `v_oscillate+$1A` value (hi byte = obAngle source), freq 8 / amplitude $40 */
static int16_t  s_osc_r;     /* its rate */
static uint8_t  s_osc_dn;    /* direction */
static uint8_t  s_osc_f;     /* last frame the oscillator was stepped */

void ghz_objects_reset(void) BANKED {
    g_stand_obj = 0;
    s_osc_v = 0x0080;
    s_osc_r = 0;
    s_osc_dn = 0;
    s_osc_f = (uint8_t)(g_frame_counter + 1U);
}

/* `OscillateNumDo` for the platform channel (exactly one step per game frame). */
static void gh_osc_tick(void) {
    if (s_osc_f == (uint8_t)g_frame_counter) return;
    s_osc_f = (uint8_t)g_frame_counter;
    if (!s_osc_dn) {
        s_osc_r += 8;
        s_osc_v += (uint16_t)s_osc_r;
        if ((uint8_t)(s_osc_v >> 8) >= 0x40) s_osc_dn = 1;
    } else {
        s_osc_r -= 8;
        s_osc_v += (uint16_t)s_osc_r;
        if ((uint8_t)(s_osc_v >> 8) < 0x40) s_osc_dn = 0;
    }
}

static int16_t gh_h(void) {
    return g_sonic.rolling ? 14 : 19;
}

static void gh_set_y(int16_t y) {
    g_sonic.y = y;
    g_sonic.y_sub = (int32_t)y << 8;
}

static void gh_move_x(int16_t d) {
    g_sonic.x += d;
    g_sonic.x_sub += (int32_t)d << 8;
}

static void gh_land(ActiveObj *o, int16_t y) {
    gh_set_y(y);
    physics_land_on_object();
    g_stand_obj = o;
}

static void gh_taken(ActiveObj *o) {
    g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1U << (o->spawn_slot & 7U));
}

/* `PlatformObject` / `SlopeObject` + `Plat_NoXCheck_AltY`: land on a surface at `top`; x range [ox-w, ox+w). */
static uint8_t gh_try_land(ActiveObj *o, int16_t w, int16_t top, int16_t ox) {
    int16_t dx, d0;
    if (g_sonic.ysp < 0 || g_sonic.dead) return 0;
    dx = g_sonic.x - ox + w;
    if (dx < 0 || dx >= (w << 1)) return 0;
    d0 = top - (g_sonic.y + gh_h() + 4);
    if (d0 > 0 || d0 < -16) return 0;
    gh_land(o, g_sonic.y + d0 + 3);
    return 1;
}

/* `ExitPlatform`: still on while grounded and inside [ox-w, ox+w). */
static uint8_t gh_still_on(int16_t w, int16_t ox) {
    int16_t dx = g_sonic.x - ox + w;
    return g_sonic.on_ground && dx >= 0 && dx < (w << 1);
}

static void gh_cells(int16_t sx, int16_t sy, const GhzCell *c, uint8_t n, uint8_t flip) {
    while (n--) {
        if (flip) hw_put_sprite8(sx - 8 - c->dx, sy + c->dy, c->tile, c->attr ^ 0x20);
        else hw_put_sprite8(sx + c->dx, sy + c->dy, c->tile, c->attr);
        c++;
    }
}

/* `SolidObject` (`sub SolidObject.asm`): d1 = half width (incl. Sonic's solid width), d2 = half height,
   actw = `obActWid` (landing x range). Returns 1 when Sonic is pushed from the side while grounded. */
static uint8_t gh_solid(ActiveObj *o, int16_t d1, int16_t d2, int16_t actw) {
    int16_t h = gh_h();
    int16_t d0, d3, d4, d5, t;

    if (g_stand_obj == o) {
        d0 = g_sonic.x - o->x + d1;
        if (!g_sonic.on_ground || d0 < 0 || d0 >= (d1 << 1)) {
            g_stand_obj = 0;
            return 0;
        }
        gh_set_y(o->y - d2 - h);
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
                    gh_land(o, g_sonic.y - d3 - 1);
                }
            }
        } else if (g_sonic.ysp < 0) {
            gh_set_y(g_sonic.y - d3);
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
        gh_move_x(-d0);
    }
    if (!g_sonic.on_ground) return 0;
    if (g_joy & (d0 > 0 ? J_RIGHT : J_LEFT)) g_sonic.anim = ANI_PUSH;
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x11 Bridge                                                                                       */
/* ------------------------------------------------------------------------------------------------ */
static void gh_bridge(ActiveObj *o) {
    uint8_t n = (uint8_t)(o->subtype & 0x0FU);
    int16_t span, ex, sine, cam_x = g_cam_x, cam_y = g_cam_y, lx;
    uint8_t cur, was, i, k, start, ymax;
    uint16_t t;

    if (n == 0 || n > 16) n = 12;
    if (o->state == 0) {
        o->state = 1;
        o->hp = 0;
        o->timer = 0;
    }
    span = (int16_t)n << 3;
    ex = o->x - 8;                           /* walk range is [ox-8n-8, ox+8n-8) */
    cur = o->timer;
    was = (g_stand_obj == o);
    if (was && !gh_still_on(span, ex)) {
        g_stand_obj = 0;
        was = 0;
    }
    if (was) {
        int16_t d = (g_sonic.x - ex + span) >> 4;
        cur = (uint8_t)(d < 0 ? 0 : (d >= (int16_t)n ? n - 1 : d));
        o->timer = cur;
        if (o->hp != 0x40) o->hp += 4;
    } else {
        if (o->hp) o->hp -= 4;
        if (gh_try_land(o, span, o->y - 8, ex)) {
            int16_t d = (g_sonic.x - ex + span) >> 4;
            cur = (uint8_t)(d < 0 ? 0 : (d >= (int16_t)n ? n - 1 : d));
            o->timer = cur;
        }
    }

    /* `Bri_Bend`: log y = origY + ((align + 1) * ymax(cur) * sine(nudge)) >> 16 */
    sine = hw_sin(o->hp);
    {
        uint8_t a = (uint8_t)(cur + 1U), b = (uint8_t)(n - cur);
        ymax = (uint8_t)((a < b ? a : b) << 1);
    }
    t = (uint16_t)ymax * (uint16_t)sine;      /* <= 16 * 256 */
    lx = o->x - ((int16_t)(n >> 1) << 4);

    if (was) {
        /* `Bri_MoveSonic`: stand on the current log (its align factor is $FF -> y offset = ymax * sine >> 8) */
        gh_set_y(o->y + (o->hp ? (int16_t)(t >> 8) : 0) - 8 - gh_h());
    }

    start = (n > 8 && (g_frame_counter & 1U)) ? (uint8_t)(n >> 1) : 0;
    for (k = 0; k < n; k++) {
        int16_t ly = o->y;
        i = (uint8_t)(start + k);
        if (i >= n) i -= n;
        if (o->hp) {
            uint8_t a8;
            if (i <= cur) {
                a8 = ghz_bri_align[((uint8_t)(cur * (cur + 1U)) >> 1) + i];
            } else {
                uint8_t nr = (uint8_t)(n - 1U - cur);
                a8 = ghz_bri_align[((uint8_t)(nr * (nr + 1U)) >> 1) + (uint8_t)(nr - (i - cur))];
            }
            ly += (int16_t)(((t >> 5) * ((uint16_t)a8 + 1U)) >> 11);
        }
        hw_put_sprite8(((lx + ((int16_t)i << 4) - cam_x) >> 1) + ghzc_log[0].dx,
                       ((ly - cam_y) >> 1) + ghzc_log[0].dy, ghzc_log[0].tile, ghzc_log[0].attr);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x18 Platforms                                                                                    */
/* ------------------------------------------------------------------------------------------------ */
static void gh_platform(ActiveObj *o) {
    uint8_t type = (uint8_t)(o->subtype & 0x0FU);
    uint8_t was;
    int16_t old_x, ang;

    if (o->state == 0) {
        o->state = 1;
        o->vx = o->y;          /* plat_rawY */
        o->vy = 0;
        o->hp = 0;             /* plat_nudge */
        o->timer = 0;          /* plat_delay */
    }
    gh_osc_tick();
    ang = (int16_t)(s_osc_v >> 8);
    old_x = o->x;
    was = (g_stand_obj == o);

    if (o->state == 1) {
        if (was) {
            if (o->hp < 0x40) o->hp += 4;
            if (!gh_still_on(32, o->x)) {
                g_stand_obj = 0;
                was = 0;
            }
        } else {
            if (o->hp) o->hp -= 4;
            gh_try_land(o, 32, o->y - 8, o->x);
        }
    }

    switch (type) {
    case 1: o->x = o->base_x + (ang - 0x40); break;
    case 5: o->x = o->base_x + (0x40 - ang); break;
    case 2: o->vx = o->base_y + (ang - 0x40); break;
    case 6: o->vx = o->base_y + (0x40 - ang); break;
    case 0x0A: o->vx = o->base_y + ((ang - 0x40) >> 1); break;
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
            gh_taken(o);
            o->active = 0;
            return;
        }
        break;
    default:
        break;
    }
    o->y = o->vx + (hw_sin(o->hp) >> 6);

    if (was && g_stand_obj == o) {
        gh_set_y(o->y - 9 - gh_h());
        gh_move_x(o->x - old_x);
    }

    gh_cells((o->x - g_cam_x) >> 1, (o->y - g_cam_y) >> 1,
             type == 0x0A ? ghzc_col : ghzc_plat, type == 0x0A ? GHZC_COL_N : GHZC_PLAT_N, 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x1A Collapsing ledge                                                                             */
/* ------------------------------------------------------------------------------------------------ */
static const uint8_t k_ledge_delay[25] = {
    0x1C, 0x18, 0x14, 0x10, 0x1A, 0x16, 0x12, 0x0E, 0x0A, 0x06, 0x18, 0x14, 0x10,
    0x0C, 0x08, 0x04, 0x16, 0x12, 0x0E, 0x0A, 0x06, 0x02, 0x14, 0x10, 0x0C
};

/* `Ledge_SlopeData`: 8x $20, $21..$2F (x2), 10x $30 — indexed by (x offset from left edge) / 2. */
static int16_t gh_ledge_top(ActiveObj *o, uint8_t flip) {
    int16_t d0 = g_sonic.x - o->x + 48;
    uint8_t idx, hgt;
    if (d0 < 0) d0 = 0;
    if (d0 > 95) d0 = 95;
    if (flip) d0 = 95 - d0;
    idx = (uint8_t)(d0 >> 1);
    hgt = (idx < 8) ? 0x20 : ((idx < 38) ? (uint8_t)(0x21 + ((idx - 8) >> 1)) : 0x30);
    return o->y - hgt;
}

static void gh_ledge_break(ActiveObj *o) {
    o->state = 2;
    o->hp = 0;
    gh_taken(o);
    hw_play_sfx(SFX_COLLAPSE);
}

static void gh_ledge(ActiveObj *o, int16_t sx, int16_t sy) {
    uint8_t flip = (uint8_t)((o->subtype & 0x40U) != 0);
    uint8_t alt = (uint8_t)(o->subtype & 1U);
    const GhzCell *c = alt ? ghzc_ledge1 : ghzc_ledge0;
    const uint8_t *pc = alt ? ghzp_ledge1 : ghzp_ledge0;
    uint8_t n = alt ? GHZC_LEDGE1_N : GHZC_LEDGE0_N;
    uint8_t k, t = 0;

    if (o->state == 0) {
        o->state = 1;
        o->timer = 7;          /* collapsible_timedelay */
        o->vx = 0;             /* collapsible_flag */
    }

    if (o->state == 1) {
        if (g_stand_obj != o) {
            if (o->vx) {
                if (o->timer == 0) {
                    gh_ledge_break(o);
                } else {
                    o->timer--;
                }
            }
            if (o->state == 1) gh_try_land(o, 48, gh_ledge_top(o, flip), o->x);
        } else {
            if (o->timer == 0) {
                gh_ledge_break(o);
            } else {
                o->vx = 1;
                o->timer--;
            }
            if (!gh_still_on(48, o->x)) g_stand_obj = 0;
            else gh_set_y(gh_ledge_top(o, flip) - gh_h());
        }
    }
    if (o->state == 2) {
        t = o->hp;
        if (t < 200) o->hp = (uint8_t)(t + 1U);
        if (g_stand_obj == o) {
            if (t >= 2 || !gh_still_on(48, o->x)) g_stand_obj = 0;
            else gh_set_y(gh_ledge_top(o, flip) - gh_h());
        }
        if (t > 90) {
            o->active = 0;
            return;
        }
    }

    for (k = 0; k < n; k++, c++) {
        int16_t yo = 0;
        if (o->state == 2) {
            uint8_t d = k_ledge_delay[pc[k]];
            if (t > d + 1U) {
                uint16_t dt = (uint16_t)(t - d - 1U);
                if (dt > 80U) continue;
                yo = (int16_t)((7U * dt * (dt - 1U)) >> 7);
            }
        }
        if (flip) hw_put_sprite8(sx - 8 - c->dx, sy + c->dy + yo, c->tile, c->attr ^ 0x20);
        else hw_put_sprite8(sx + c->dx, sy + c->dy + yo, c->tile, c->attr);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x3C Smashable wall                                                                               */
/* ------------------------------------------------------------------------------------------------ */
/* `Smash_FragSpd1/2` (8.8 -> whole pixels: every entry is a multiple of $100). */
static const int8_t k_smash_v1[16] = { 4, -5, 6, -1, 6, 1, 4, 5, 6, -6, 8, -2, 8, 2, 6, 6 };
static const int8_t k_smash_v2[16] = { -6, -6, -8, -2, -8, 2, -6, 6, -4, -5, -6, -1, -6, 1, -4, 5 };

static void gh_smash(ActiveObj *o, int16_t sx, int16_t sy) {
    uint8_t fr = (uint8_t)(o->subtype & 3U);
    const GhzCell *c = (fr == 0) ? ghzc_smash0 : ((fr == 1) ? ghzc_smash1 : ghzc_smash2);
    uint8_t k;

    if (o->state == 0) {
        int16_t spd = g_sonic.on_ground ? g_sonic.gsp : g_sonic.xsp;
        uint8_t push = gh_solid(o, 27, 32, 16);
        int16_t a = spd < 0 ? -spd : spd;
        if (push && g_sonic.rolling && a >= 0x0480) {
            g_sonic.xsp = spd;
            g_sonic.gsp = spd;
            gh_move_x(spd < 0 ? -4 : 4);
            o->state = 1;
            o->timer = 0;
            o->hp = (uint8_t)(spd < 0);
            gh_taken(o);
            hw_play_sfx(SFX_WALLSMASH);
        } else {
            gh_cells(sx, sy, c, 8, 0);
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

/* ------------------------------------------------------------------------------------------------ */
void ghz_object(ActiveObj *o, int16_t sx, int16_t sy) BANKED {
    switch (o->id) {
    case 0x11:
        gh_bridge(o);
        break;
    case 0x18:
        gh_platform(o);
        break;
    case 0x1A:
        gh_ledge(o, sx, sy);
        break;
    case 0x1C:
        gh_cells(sx, sy, ghzc_stump, GHZC_STUMP_N, (uint8_t)((o->subtype & 0x40U) != 0));
        break;
    case 0x3B:
        gh_solid(o, 27, 16, 19);
        gh_cells(sx, sy, ghzc_rock, GHZC_ROCK_N, 0);
        break;
    case 0x3C:
        gh_smash(o, sx, sy);
        break;
    default:
        break;
    }
}
