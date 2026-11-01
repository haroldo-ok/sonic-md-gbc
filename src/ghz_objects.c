#pragma bank 127
#include "engine.h"
#define PR_WANT_PLATFORM
#define PR_WANT_SWING
#define PR_WANT_SMASH
#include "props_common.h"
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
void ghz_objects_reset(void) BANKED {
    g_stand_obj = 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* 0x11 Bridge                                                                                       */
/* ------------------------------------------------------------------------------------------------ */
static void pr_bridge(ActiveObj *o) {
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
    if (was && !pr_still_on(span, ex)) {
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
        if (pr_try_land(o, span, o->y - 8, ex)) {
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
        pr_set_y(o->y + (o->hp ? (int16_t)(t >> 8) : 0) - 8 - pr_h());
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
/* 0x1A Collapsing ledge                                                                             */
/* ------------------------------------------------------------------------------------------------ */
static const uint8_t k_ledge_delay[25] = {
    0x1C, 0x18, 0x14, 0x10, 0x1A, 0x16, 0x12, 0x0E, 0x0A, 0x06, 0x18, 0x14, 0x10,
    0x0C, 0x08, 0x04, 0x16, 0x12, 0x0E, 0x0A, 0x06, 0x02, 0x14, 0x10, 0x0C
};

/* `Ledge_SlopeData`: 8x $20, $21..$2F (x2), 10x $30 — indexed by (x offset from left edge) / 2. */
static int16_t pr_ledge_top(ActiveObj *o, uint8_t flip) {
    int16_t d0 = g_sonic.x - o->x + 48;
    uint8_t idx, hgt;
    if (d0 < 0) d0 = 0;
    if (d0 > 95) d0 = 95;
    if (flip) d0 = 95 - d0;
    idx = (uint8_t)(d0 >> 1);
    hgt = (idx < 8) ? 0x20 : ((idx < 38) ? (uint8_t)(0x21 + ((idx - 8) >> 1)) : 0x30);
    return o->y - hgt;
}

static void pr_ledge_break(ActiveObj *o) {
    o->state = 2;
    o->hp = 0;
    pr_taken(o);
    hw_play_sfx(SFX_COLLAPSE);
}

static void pr_ledge(ActiveObj *o, int16_t sx, int16_t sy) {
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
                    pr_ledge_break(o);
                } else {
                    o->timer--;
                }
            }
            if (o->state == 1) pr_try_land(o, 48, pr_ledge_top(o, flip), o->x);
        } else {
            if (o->timer == 0) {
                pr_ledge_break(o);
            } else {
                o->vx = 1;
                o->timer--;
            }
            if (!pr_still_on(48, o->x)) g_stand_obj = 0;
            else pr_set_y(pr_ledge_top(o, flip) - pr_h());
        }
    }
    if (o->state == 2) {
        t = o->hp;
        if (t < 200) o->hp = (uint8_t)(t + 1U);
        if (g_stand_obj == o) {
            if (t >= 2 || !pr_still_on(48, o->x)) g_stand_obj = 0;
            else pr_set_y(pr_ledge_top(o, flip) - pr_h());
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
static void pr_smash(ActiveObj *o, int16_t sx, int16_t sy) {
    uint8_t fr = (uint8_t)(o->subtype & 3U);
    pr_smash_wall(o, sx, sy, (fr == 0) ? ghzc_smash0 : ((fr == 1) ? ghzc_smash1 : ghzc_smash2));
}

/* ------------------------------------------------------------------------------------------------ */
void ghz_object(ActiveObj *o, int16_t sx, int16_t sy) BANKED {
    switch (o->id) {
    case 0x11:
        pr_bridge(o);
        break;
    case 0x15:
        pr_swing(o, ghzc_swing, GHZC_SWING_N, ghzc_chain, ghzc_anchor);
        break;
    case 0x18:
        pr_platform(o, ghzc_plat, GHZC_PLAT_N, ghzc_col, GHZC_COL_N, 0);
        break;
    case 0x1A:
        pr_ledge(o, sx, sy);
        break;
    case 0x1C:
        pr_cells(sx, sy, ghzc_stump, GHZC_STUMP_N, (uint8_t)((o->subtype & 0x40U) != 0));
        break;
    case 0x3B:
        pr_solid(o, 27, 16, 19);
        pr_cells(sx, sy, ghzc_rock, GHZC_ROCK_N, 0);
        break;
    case 0x3C:
        pr_smash(o, sx, sy);
        break;
    default:
        break;
    }
}
