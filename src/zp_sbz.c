#pragma bank 121
#define ZP_WANT_MBLK
#include "zp_common.h"
#include "zp_sbz.h"

/* =============================================================================================== */
/* SCRAP BRAIN ZONE (vsets 5 / 6, and the shared pieces of SBZ3 = vset 7)                           */
/* Behaviour follows the S1 disassembly (`_incObj/68..70`, `2A`); art is reduced to fit 8x8 sprites. */
/* =============================================================================================== */

/* Cells with both flips (`pr_cells` only knows X flip). */
static void sbz_cells(int16_t sx, int16_t sy, const PropCell *c, uint8_t n, uint8_t xf, uint8_t yf) {
    if ((uint16_t)(sx + 72) >= 304U || (uint16_t)(sy + 72) >= 272U) return;
    if (!xf && !yf) {
        pr_cells_fast(sx, sy, c, n);
        return;
    }
    while (n--) {
        int16_t x = xf ? (sx - 8 - c->dx) : (sx + c->dx);
        int16_t y = yf ? (sy - 8 - c->dy) : (sy + c->dy);
        hw_put_sprite8(x, y, c->tile, (uint8_t)(c->attr ^ (xf ? 0x20 : 0) ^ (yf ? 0x40 : 0)));
        c++;
    }
}

/* A row/rect of one repeated pattern: `cols` columns, `step` screen px apart, starting at `x0`. */
static void sbz_repeat(int16_t sx, int16_t sy, const PropCell *c, uint8_t n, int8_t x0, uint8_t cols, uint8_t step) {
    uint8_t k;
    if ((uint16_t)(sx + 72) >= 304U || (uint16_t)(sy + 72) >= 272U) return;
    for (k = 0; k < cols; k++) {
        uint8_t i;
        for (i = 0; i < n; i++)
            hw_put_sprite8(sx + x0 + (int16_t)k * step + (c[i].dx - c[0].dx), sy + c[i].dy, c[i].tile, c[i].attr);
    }
}

static uint8_t sbz_onscreen(ActiveObj *o) {
    int16_t dx = o->x - g_cam_x, dy = o->y - g_cam_y;
    return (uint8_t)(dx > -64 && dx < 384 && dy > -64 && dy < 336);
}

/* 0x6C Vanishing platform: visible 0x80 frames, hidden ((n+1)*0x80) frames; solid while the frame is 0/1. */
static void sbz_vanish(ActiveObj *o) {
    uint16_t lo = (uint16_t)((o->subtype & 0x0FU) + 1U) << 7;
    uint16_t per = lo + 0x80U;
    uint16_t off = (uint16_t)(((uint32_t)(o->subtype & 0x30U) * (lo + 0x80U)) >> 8);
    uint16_t t = (uint16_t)((g_level_frames + per * 2U - off) % per);
    uint16_t vis = lo;                       /* hidden until `lo`, then appears */
    uint8_t f, solid;
    if (t < 32U) f = (uint8_t)(t >> 3);                         /* vanishing */
    else if (t < vis) f = 3;
    else if (t < vis + 32U) f = (uint8_t)(3U - ((t - vis) >> 3)); /* appearing */
    else f = 0;
    solid = (uint8_t)(f < 2);
    if (solid) {
        if (g_stand_obj == o) {
            if (!pr_still_on(16, o->x)) g_stand_obj = 0;
            else pr_set_y(o->y - 8 - pr_h());
        } else {
            pr_try_land(o, 16, o->y - 8, o->x);
        }
    } else if (g_stand_obj == o) {
        g_stand_obj = 0;
    }
    if (f == 0) pr_cells(ZP_SX(o), ZP_SY(o), zp_sbz_vanw, ZP_SBZ_VANW_N, 0);
    else if (f == 1 || (f == 2 && (g_level_frames & 2U))) pr_cells(ZP_SX(o), ZP_SY(o), zp_sbz_vanh, ZP_SBZ_VANH_N, 0);
}

/* 0x69 Trapdoor (subtype < 0x80, opens/closes every N seconds) / spinning platform (>= 0x80).
   vx = timer, vy = period, hp = spin pending, timer = spin tick (0 = idle / flat). */
static void sbz_spin(ActiveObj *o) {
    uint8_t spinner = (uint8_t)(o->subtype & 0x80U);
    if (o->state == 0) {
        uint8_t n = (uint8_t)(o->subtype & 0x0FU);
        o->state = 1;
        o->hp = 0;
        o->timer = 0;
        if (spinner) { o->vx = o->vy = (int16_t)(n * 6U); }
        else { o->vx = 0; o->vy = (int16_t)(n * 60U); o->base_y = 0; }   /* base_y: 0 = closed, else open progress */
    }
    if (spinner) {
        uint16_t mask = (uint16_t)((((o->subtype & 0x70U) + 0x10U) << 2) - 1U);
        if (o->timer) {
            if (++o->timer > 34U) o->timer = 0;
        } else {
            if ((g_level_frames & mask) == 0) o->hp = 1;
            if (o->hp && --o->vx < 0) {
                o->vx = o->vy;
                o->hp = 0;
                o->timer = 1;
            }
        }
        if (o->timer == 0) {
            pr_solid(o, 27, 7, 16);
            pr_cells(ZP_SX(o), ZP_SY(o), zp_sbz_spinf, ZP_SBZ_SPINF_N, 0);
        } else {
            uint8_t k = (uint8_t)(o->timer >> 1), lvl = (uint8_t)(k & 15U);
            if (lvl > 8) lvl = (uint8_t)(16U - lvl);
            if (g_stand_obj == o) g_stand_obj = 0;
            if (lvl > 4) lvl = (uint8_t)(8U - lvl);
            if (lvl < 3) pr_cells(ZP_SX(o), ZP_SY(o), zp_sbz_spinf, ZP_SBZ_SPINF_N, 0);
            else pr_cells(ZP_SX(o), ZP_SY(o), zp_sbz_spine, ZP_SBZ_SPINE_N, 0);
        }
        return;
    }
    /* trapdoor */
    if (--o->vx < 0) {
        o->vx = o->vy;
        o->base_y = o->base_y ? 0 : 1;               /* toggle target: 1 = opening, 0 = closing */
        if (sbz_onscreen(o)) hw_play_sfx(SFX_SWITCH);
        o->hp = (uint8_t)(o->base_y ? 0 : 1);
    }
    /* timer = open progress 0..12 (frame = timer / 4) */
    if (o->base_y) { if (o->timer < 12) o->timer++; }
    else if (o->timer) o->timer--;
    {
        int16_t sx = ZP_SX(o), sy = ZP_SY(o);
        uint8_t f = (uint8_t)(o->timer >> 2);
        if (f == 0) {
            pr_solid(o, 75, 12, 64);
            sbz_repeat(sx, sy, zp_sbz_trap, ZP_SBZ_TRAP_N, -32, 4, 16);
        } else {
            if (g_stand_obj == o) g_stand_obj = 0;
            if (f == 1) sbz_repeat(sx, sy, zp_sbz_trap, 2, -32, 4, 16);   /* top row only */
        }
    }
}

/* 0x6D Flamethrower. subtype: bits 4-6 flame time (*2*16 frames), bits 0-3 pause (*32), bit 7 = flipped (valve, flames up).
   vx = countdown, vy = flame time, base_y = pause time, hp = phase (0 flaming / 1 retracting), timer = anim tick. */
static void sbz_flame(ActiveObj *o) {
    uint8_t yf = (uint8_t)(o->subtype & 0x80U), e, hurt;
    int16_t sx = ZP_SX(o), sy = ZP_SY(o), cy;
    if (o->state == 0) {
        o->state = 1;
        o->vy = (int16_t)((o->subtype & 0x70U) << 1);
        o->vx = o->vy;
        o->base_y = (int16_t)((o->subtype & 0x0FU) << 5);
        o->hp = 0;
        o->timer = 0;
    }
    if (--o->vx < 0) {
        o->timer = 0;
        if (o->hp == 0) { o->hp = 1; o->vx = o->base_y; }
        else {
            o->hp = 0;
            o->vx = o->vy;
            if (sbz_onscreen(o)) hw_play_sfx(SFX_FLAMETHROW);
        }
    }
    if (o->timer < 255) o->timer++;
    e = (uint8_t)(o->timer >> 2);
    if (e > 10) e = 10;
    if (o->hp) e = (uint8_t)(10 - e);
    hurt = (uint8_t)(e >= 8);
    if (hurt && !g_sonic.dead) {
        int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - (o->y + (yf ? -28 : 28));
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 20 && dy < 36) sonic_hurt_or_kill(0);
    }
    if (e >= 1) sbz_cells(sx, sy, zp_sbz_fhead, ZP_SBZ_FHEAD_N, 0, yf != 0);
    if (e >= 5) sbz_cells(sx, sy, zp_sbz_fbody, 1, 0, yf != 0);
    if (e >= 7) sbz_cells(sx, sy, zp_sbz_fbody + 1, 1, 0, yf != 0);
    if (e >= 9) sbz_cells(sx, sy, zp_sbz_ftip, ZP_SBZ_FTIP_N, 0, yf != 0);
    (void)cy;
}

/* 0x6E Electrocuter: zaps every `(subtype & 0x7F) * 16` frames (power-of-two), harmful on frame 4 of the discharge (~144 px wide). */
static void sbz_electro(ActiveObj *o) {
    uint16_t freq = (uint16_t)(((uint16_t)(o->subtype & 0x7FU) << 4) - 1U);
    int16_t sx = ZP_SX(o), sy = ZP_SY(o);
    uint8_t t = o->timer, hot;
    if (o->state == 0) { o->state = 1; t = 0; o->timer = 0; }
    if ((g_level_frames & freq) == 0) {
        t = 1;
        if (sbz_onscreen(o)) hw_play_sfx(SFX_ELECTRIC);
    } else if (t) {
        if (++t > 14U) t = 0;
    }
    o->timer = t;
    hot = (uint8_t)(t >= 7 && t <= 9);
    if (hot && !g_sonic.dead) {
        int16_t dx = g_sonic.x - o->x, dy = g_sonic.y - o->y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 80 && dy < 22) sonic_hurt_or_kill(0);
    }
    pr_cells(sx - 16, sy, zp_sbz_orb, ZP_SBZ_ORB_N, 0);
    pr_cells(sx + 16, sy, zp_sbz_orb, ZP_SBZ_ORB_N, 1);
    if (t >= 3 && t <= 11) {
        uint8_t k;
        pr_cells(sx - 12, sy - 1, zp_sbz_spark, ZP_SBZ_SPARK_N, 0);
        if (hot) for (k = 0; k < 4; k++) pr_cells(sx - 10 + (int16_t)k * 6, sy + (((k + t) & 1U) ? 4 : -4), zp_sbz_bolt, 1, (uint8_t)(k & 1U));
    }
}

/* 0x6B Stomper / sliding platform / ancient lift. vx = offset, vy = delay, hp = active, timer = action type,
   base_y = origin Y. */
static void sbz_stomp(ActiveObj *o) {
    static const uint8_t k_w[5] = { 64, 28, 28, 28, 128 };
    static const uint8_t k_h[5] = { 12, 32, 32, 32, 64 };
    static const uint8_t k_max[5] = { 128, 56, 64, 96, 0 };
    uint8_t v7 = (uint8_t)(g_ram_act.vset_idx == 7);
    uint8_t idx = v7 ? (uint8_t)((o->subtype >> 4) & 7U) : (uint8_t)((o->subtype >> 4) & 3U);
    uint8_t xf = (uint8_t)(!v7 && (o->subtype & 0x40U));
    int16_t old_x = o->x, off, w, h, mx;
    uint8_t bit;
    if (idx > 4) idx = 0;
    w = k_w[idx];
    h = k_h[idx];
    mx = k_max[idx];
    if (o->state == 0) {
        o->state = 1;
        o->base_y = o->y;
        o->vx = 0;
        o->vy = 0;
        o->hp = 0;
        o->timer = (uint8_t)((o->subtype & 0x80U) ? (idx == 4 ? 5 : 1) : (o->subtype & 0x0FU));
    }
    bit = (uint8_t)(1U << (o->subtype & 7U));
    off = o->vx;
    switch (o->timer) {
    case 1: case 2:       /* sliding platform: extend on switch, wait 3 s, retract */
        if (o->timer == 1) {
            if (!o->hp && (g_button_state & bit)) o->hp = 1;
            if (o->hp) {
                if (off == mx) { o->timer = 2; o->vy = 180; o->hp = 0; }
                else off += 2;
            }
        } else {
            if (!o->hp) { if (--o->vy == 0) o->hp = 1; }
            if (o->hp) {
                if (off == 0) { o->timer = 1; o->hp = 0; }
                else off -= 2;
            }
        }
        o->vx = off;
        o->x = o->base_x - (xf ? (off - 128) : off);
        break;
    case 3:               /* stomps down, creeps back up */
        if (o->hp) {
            off += 8;
            if (off == mx) o->hp = 0;
        } else if (off) {
            off--;
        } else if (--o->vy < 0) {
            o->vy = 60;
            o->hp = 1;
        }
        o->vx = off;
        o->y = o->base_y + (xf ? (56 - off) : off);
        break;
    case 4:               /* stomps up and down */
        if (o->hp) {
            if (off == mx) { if (--o->vy < 0) { o->vy = 60; o->hp = 0; } }
            else off += 8;
        } else if (off) {
            off -= 8;
        } else if (--o->vy < 0) {
            o->vy = 60;
            o->hp = 1;
        }
        o->vx = off;
        o->y = o->base_y + (xf ? (56 - off) : off);
        break;
    case 5:               /* SBZ3 ancient lift: drifts left/down once its switch is pressed */
        if (!o->hp && (g_button_state & bit)) o->hp = 1;
        if (o->hp) {
            if ((o->vx & 1) == 0) o->y++;
            o->vx++;
            o->x--;
            if (o->x <= 0x980) o->timer = 0;
        }
        break;
    default:
        break;
    }
    if (g_stand_obj == o && o->x != old_x) pr_move_x(o->x - old_x);
    pr_solid(o, w + 11, h, w);
    {
        int16_t sx = ZP_SX(o), sy = ZP_SY(o);
        if (idx == 0) {
            sbz_repeat(sx, sy, zp_sbz_sdoor, ZP_SBZ_SDOOR_N, -32, 8, 8);
        } else {
            uint8_t cx;
            for (cx = 0; cx < 3; cx++) {
                int16_t px = sx - 12 + (int16_t)cx * 8;
                hw_put_sprite8(px, sy - 16, zp_sbz_stmb[0].tile, zp_sbz_stmb[0].attr);
                hw_put_sprite8(px, sy - 8, zp_sbz_stmb[0].tile, zp_sbz_stmb[0].attr);
                hw_put_sprite8(px, sy, zp_sbz_stmb[0].tile, zp_sbz_stmb[0].attr);
                hw_put_sprite8(px, sy + 8, zp_sbz_stma[0].tile, zp_sbz_stma[0].attr);
            }
        }
    }
}

/* 0x70 Girder block: right 96 / down 48 / left+up 96 / up 24, with a 7-frame pause at each corner. 12 cells wide on the
   Mega Drive; here the top row is drawn with 10 sprites (one scanline budget), solidity keeps the full width. */
static void sbz_girder(ActiveObj *o) {
    int16_t old_x = o->x;
    uint8_t k;
    if (o->state == 0) {
        o->state = 1;
        o->hp = 0;
        o->timer = 96;
        o->vy = 7;
    }
    if (o->vy > 0) {
        o->vy--;
    }
    if (o->vy <= 0) {
        switch (o->hp & 3U) {
        case 0: o->x++; break;
        case 1: o->y++; break;
        case 2: o->x--; if ((o->timer & 3U) == 0) o->y--; break;
        default: o->y--; break;
        }
        if (--o->timer == 0) {
            static const uint8_t k_len[4] = { 96, 48, 96, 24 };
            o->hp = (uint8_t)((o->hp + 1U) & 3U);
            o->timer = k_len[o->hp];
            o->vy = 7;
        }
    }
    if (g_stand_obj == o && o->x != old_x) pr_move_x(o->x - old_x);
    pr_solid(o, 107, 24, 96);
    for (k = 0; k < 10; k++)
        hw_put_sprite8(ZP_SX(o) - 40 + (int16_t)k * 8, ZP_SY(o) - 8, (k & 1U) ? zp_sbz_gird[0].tile : zp_sbz_gird[1].tile, zp_sbz_gird[0].attr);
    for (k = 0; k < 10; k++)
        hw_put_sprite8(ZP_SX(o) - 40 + (int16_t)k * 8, ZP_SY(o), zp_sbz_gird[2].tile, zp_sbz_gird[2].attr);
}

/* 0x2A Small door: opens (8 frames) when Sonic is within 64 px on the side it faces. */
static void sbz_adoor(ActiveObj *o) {
    int16_t dx = g_sonic.x - o->x;
    uint8_t want = 0, p;
    if (dx > -64 && dx < 64) {
        uint8_t xf = (uint8_t)((o->subtype & 0x40U) != 0);
        want = (uint8_t)(dx >= 0 ? !xf : xf);
        if (dx < 0) want = (uint8_t)(xf);
        if (dx >= 0) want = (uint8_t)(!xf);
    }
    p = o->timer;
    if (want) { if (p < 8) p++; }
    else if (p) p--;
    o->timer = p;
    if (p == 0) pr_solid(o, 17, 32, 6);
    else if (g_stand_obj == o) g_stand_obj = 0;
    {
        int16_t sx = ZP_SX(o), sy = ZP_SY(o) - (int16_t)(p << 1);
        if (p < 8) sbz_cells(sx, sy, zp_sbz_adoor, ZP_SBZ_ADOOR_N, 0, 0);
        if (p < 4) {
            hw_put_sprite8(sx - 4, sy, zp_sbz_adoor[1].tile, zp_sbz_adoor[1].attr);
            hw_put_sprite8(sx - 4, sy + 8, zp_sbz_adoor[1].tile, zp_sbz_adoor[1].attr);
        }
    }
}

/* 0x6A Saws and pizza cutters. state bit 1 = speeding saw already shot. */
static void sbz_saw(ActiveObj *o) {
    uint8_t t = (uint8_t)(o->subtype & 7U), xf = (uint8_t)((o->subtype & 0x40U) != 0);
    uint8_t fr = (uint8_t)((g_level_frames / 3U) & 1U), moving = 1;
    int16_t d;
    if (o->state == 0) { o->state = 1; o->base_y = o->y; }
    switch (t) {
    case 1:
        d = (int16_t)OSC_HI(0x0E);
        o->x = o->base_x - (xf ? (0x60 - d) : d);
        if (sbz_onscreen(o) && (g_level_frames & 15U) == 0) hw_play_sfx(SFX_SAW);
        break;
    case 2:
        d = (int16_t)OSC_HI(0x06);
        o->y = o->base_y - (xf ? (0x80 - d) : d);
        if (sbz_onscreen(o) && d == 0x18) hw_play_sfx(SFX_SAW);
        break;
    case 3: case 4:
        if (!(o->state & 2U)) {
            int16_t dy = g_sonic.y - o->y;
            if (t == 3 ? (g_sonic.x - 192 >= o->x) : (g_sonic.x + 224 < o->x)) {
                if (dy > -128 && dy < 128) {
                    o->state |= 2;
                    hw_play_sfx(SFX_SAW);
                }
            }
            moving = 0;
        } else {
            o->x += (t == 3) ? 6 : -6;
        }
        break;
    default:
        break;
    }
    if (!moving) return;
    if (!g_sonic.dead && ((t < 3) || (o->state & 2U))) zp_hurt(o, 18, 18);
    pr_cells(ZP_SX(o), ZP_SY(o), (fr || t >= 3) ? zp_sbz_sawa : zp_sbz_sawb, ZP_SBZ_SAWA_N, 0);
}

/* 0x68 Conveyor belt: pushes grounded Sonic along; subtype high nibble = signed speed, low nibble != 0 = narrow belt. */
static void sbz_conveyor(ActiveObj *o) {
    int16_t hw = (o->subtype & 0x0FU) ? 56 : 128;
    int16_t dx = g_sonic.x - o->x + hw, dy = g_sonic.y - o->y + 48;
    if (dx >= 0 && dx < (hw << 1) && dy >= 0 && dy < 48 && g_sonic.on_ground && !g_sonic.dead) {
        int8_t spd = (int8_t)((int8_t)(o->subtype & 0xF0U) >> 4);
        if (spd) pr_move_x(spd);
    }
}

uint8_t zp_sbz_object(ActiveObj *o) BANKED {
    switch (o->id) {
    case 0x2A: sbz_adoor(o); return 1;
    case 0x68: sbz_conveyor(o); return 1;
    case 0x69: sbz_spin(o); return 1;
    case 0x6A: sbz_saw(o); return 1;
    case 0x6B: sbz_stomp(o); return 1;
    case 0x6C: sbz_vanish(o); return 1;
    case 0x6D: sbz_flame(o); return 1;
    case 0x6E: sbz_electro(o); return 1;
    case 0x70: sbz_girder(o); return 1;
    case 0x52:
        if (g_ram_act.vset_idx == 7) return 0;
        {
            uint8_t k = (uint8_t)((o->subtype >> 4) & 7U);   /* 0x52 flavours: short (<3) / wide (>=3) block */
            if (k > 4) k &= 3U;                               /* converter-ORed X flip (bit 6) */
            if (k >= 3) zp_mblock(o, zp_sbz_mblkw, ZP_SBZ_MBLKW_N, 0);
            else zp_mblock(o, zp_sbz_mblks, ZP_SBZ_MBLKS_N, 0);
        }
        return 1;
    case 0x53:
        zp_cflo(o, zp_sbz_cflo, ZP_SBZ_CFLO_N);
        return 1;
    default:
        return 0;
    }
}
