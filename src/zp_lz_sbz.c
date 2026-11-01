#pragma bank 124
#define ZP_WANT_SBALL
#include "zp_common.h"
#include "zp_lz.h"

/* =============================================================================================== */
/* LABYRINTH ZONE                                                                                   */
/* =============================================================================================== */

/* 0x61 Labyrinth blocks (`61 LZ Blocks.asm`): 0 stationary, 1/3 wait for Sonic then sink (2) / rise (4), 5 sinks when
   touched from the side, 6 sink, 7 cork that floats on the water. state: bit 0 init, bit 1 untouched, bit 2 pushed from the
   side last frame; hp = nudge, timer = wait, vy = Y speed (8.8), vx = sub-pixel. */
static void lz_block(ActiveObj *o) {
    static const uint8_t k_w[4] = { 16, 32, 16, 16 };
    static const uint8_t k_h[4] = { 16, 12, 16, 16 };
    uint8_t fr = (uint8_t)((o->subtype >> 4) & 3U), t = (uint8_t)(o->subtype & 0x0FU), ang;
    int16_t h = k_h[fr], w = k_w[fr], d, sx, sy;
    uint8_t push;

    if (o->state == 0) {
        o->state = 1 | ((t != 0 && t != 7) ? 2 : 0);
        o->hp = 0;
        o->timer = 0;
        o->vy = 0;
        o->vx = 0;
    }
    switch (t) {
    case 1: case 3:
        if (o->timer) {
            if (--o->timer == 0) { o->subtype++; o->state &= (uint8_t)~2U; }
        } else if (g_stand_obj == o) {
            o->timer = 30;
        }
        break;
    case 2: case 6:
        zp_fall(o, 8);
        d = zp_floor(o, h);
        if (d < 0) { o->y += d + 1; o->vy = 0; o->subtype = 0; }
        break;
    case 4: {
        uint16_t s = (uint16_t)o->timer + (uint8_t)o->vy;
        o->y += (o->vy >> 8) + (int16_t)(s >> 8);
        o->timer = (uint8_t)s;
        o->vy -= 8;
        d = hw_ceil_surface_distance(o->x, o->y - h, &ang);
        if (d < 0) { o->y -= d; o->vy = 0; o->subtype = 0; }
        break;
    }
    case 5:
        if (o->state & 4U) { o->subtype++; o->state &= (uint8_t)~2U; }
        break;
    case 7:
        d = g_water_y - o->y;
        if (d != 0) {
            if (d < 0) {
                if (d < -2) d = -2;
                o->y += d;
                if (hw_ceil_surface_distance(o->x, o->y - h, &ang) < 0) o->y -= hw_ceil_surface_distance(o->x, o->y - h, &ang);
            } else {
                if (d > 2) d = 2;
                o->y += d;
                if (zp_floor(o, h) < 0) o->y += zp_floor(o, h) + 1;
            }
        }
        break;
    default:
        break;
    }
    push = pr_solid(o, w + 11, h, w);
    if (push) o->state |= 4U; else o->state &= (uint8_t)~4U;
    if (o->state & 2U) {                        /* LBlk_Nudge */
        if (g_stand_obj == o) {
            if (o->hp != 0x40) { o->hp += 4; o->y = o->base_y + (hw_sin(o->hp) >> 6); }
        } else if (o->hp) {
            o->hp -= 4;
            o->y = o->base_y + (hw_sin(o->hp) >> 6);
        }
    }
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    if (fr == 1) pr_cells(sx, sy, zp_lz_lblk1, ZP_LZ_LBLK1_N, 0);
    else if (fr == 2) pr_cells(sx, sy, zp_lz_lblk2, ZP_LZ_LBLK2_N, 0);
    else pr_cells(sx, sy, zp_lz_lblk0, ZP_LZ_LBLK0_N, 0);
}

/* 0x56 LZ doors (`56 ... LZ Doors.asm`): a vertical (16x64) or horizontal (128x32) door that opens while its switch is
   pressed. hp = switch number, vx = distance still to open, state 1 closed / 2 opening / 3 open. */
static void lz_door(ActiveObj *o) {
    uint8_t fr = (uint8_t)((o->subtype >> 4) & 7U), horiz = (uint8_t)(fr == 7);
    int16_t w = horiz ? 64 : 8, h = horiz ? 16 : 32, x = o->base_x, y = o->base_y;
    if (o->state == 0) {
        o->state = 1;
        o->hp = (uint8_t)(o->subtype & 0x0FU);
        o->vx = horiz ? 128 : 64;
    }
    if (o->state == 1 && (g_button_state & (uint8_t)(1U << (o->hp & 7U)))) o->state = 2;
    if (o->state == 2) {
        o->vx -= 2;
        if (o->vx <= 0) { o->vx = 0; o->state = 3; }
    }
    if (horiz) o->x = x + o->vx; else o->y = y + o->vx;
    pr_solid(o, w + 11, h, w);
    if (horiz) pr_cells(ZP_SX(o), ZP_SY(o), zp_lz_door7, ZP_LZ_DOOR7_N, 0);
    else pr_cells(ZP_SX(o), ZP_SY(o), zp_lz_door6, ZP_LZ_DOOR6_N, 0);
}

/* 0x16 Harpoon (`16 LZ Harpoon.asm`): extends / retracts every second; a spear that hurts while it is out. */
static void lz_harpoon(ActiveObj *o) {
    uint8_t ph = (uint8_t)((g_frame_counter + o->spawn_slot * 9U) % 120U);
    uint8_t ext = (uint8_t)(ph >= 60U), step, vert = (uint8_t)((o->subtype & 2U) != 0), xf = (uint8_t)((o->subtype & 0x40U) != 0);
    int16_t sx = ZP_SX(o), sy = ZP_SY(o), len;
    uint8_t k, n;
    const PropCell *c;
    step = ext ? (uint8_t)(ph - 60U) : (uint8_t)(ph);
    n = (uint8_t)(ph < 6 ? 1 + (ph >> 1) / 1 : (ph >= 60 && ph < 66 ? 3 : (ext ? 3 : 1)));
    if (ph >= 60U && ph < 66U) n = (uint8_t)(1 + (step >> 1)); else if (ph < 6U) n = (uint8_t)(3 - (step >> 1)); else n = ext ? 3 : 1;
    if (n > 3) n = 3;
    len = (int16_t)(n * 16);
    if (!g_sonic.dead) {
        int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y;
        if (vert) {
            if (dx > -16 && dx < 16 && dy > -(len + 8) - 12 && dy < 12) sonic_hurt_or_kill(0);
        } else {
            if (xf) dx = -dx;
            if (dx > -8 - 8 && dx < len + 8 && dy > -12 && dy < 12) sonic_hurt_or_kill(0);
        }
    }
    c = vert ? zp_lz_harpv : zp_lz_harph;
    for (k = 0; k < n; k++) {
        if (vert) hw_put_sprite8(sx + c[0].dx, sy - 4 - ((int16_t)k << 3) + c[0].dy + 20, c[k].tile, c[k].attr);
        else hw_put_sprite8(xf ? sx - 8 - (c[k].dx + 0) : sx + c[k].dx, sy + c[k].dy, c[k].tile, xf ? (uint8_t)(c[k].attr ^ 0x20) : c[k].attr);
    }
}

/* ------------------------------------------------------------------------------------------------ */
uint8_t zp_lz_sbz_object(ActiveObj *o) BANKED {
    switch (g_ram_act.vset_idx) {
    case 1:
        switch (o->id) {
        case 0x16: lz_harpoon(o); return 1;
        case 0x56: if (o->subtype & 0x80U) { lz_door(o); return 1; } return 0;
        case 0x57: zp_sball(o, zp_lz_link, ZP_LZ_LINK_N, 0); return 1;
        case 0x61: lz_block(o); return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}
