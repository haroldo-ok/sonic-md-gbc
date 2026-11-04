#pragma bank 125
#include "zp_common.h"
#include "zp_mz.h"

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
static uint8_t mz_grass_h(uint8_t fr, uint8_t idx) {
    uint8_t h;
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
    return h;
}

static int16_t mz_grass_top(ActiveObj *o, uint8_t fr, int16_t d1) {
    int16_t d0 = g_sonic.x - o->x + d1;
    if (d0 < 0) d0 = 0;
    return o->y - (int16_t)mz_grass_h(fr, (uint8_t)(d0 >> 1));
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
        if (n8 == 0x20 && o->state == 0) { o->state = 1; o->timer = 0; hw_play_sfx(SFX_BURN); }   /* starts burning */
    }
    zp_stand_slope(o, d1, mz_grass_top(o, fr, d1));
    sx = ZP_SX(o);
    sy = ZP_SY(o);
    c = fr == 0 ? zp_mz_grass0 : (fr == 1 ? zp_mz_grass1 : zp_mz_grass2);
    n = fr == 0 ? ZP_MZ_GRASS0_N : (fr == 1 ? ZP_MZ_GRASS1_N : ZP_MZ_GRASS2_N);
    pr_cells(sx, sy, c, n, 0);
    {   /* body blocks under the grass top (`Map_LGrass` pieces that use the plain 32x32 level block) */
        static const int8_t k_body[3][8] = {
            { -64, 0, -32, -16, 0, -16, 32, 0 },
            { -64, 0, -32, -16, 0, -32, 0, 0 },
            { -32, -16, 0, -16, -32, 16, 0, 16 },
        };
        uint8_t b, nb = (uint8_t)(fr == 1 ? 3 : 4);
        for (b = 0; b < nb; b++)
            pr_cells(sx + ((k_body[fr][b << 1] + 32) >> 1), sy + ((k_body[fr][(b << 1) + 1] + 16) >> 1), zp_mz_gbody, ZP_MZ_GBODY_N, 0);
    }
    if (o->state == 1) {
        /* `35 Burning Grass`: a parent flame sweeps 1px/frame along the slope, dropping a child flame every 16px.
           timer = sweep distance, children sit at 8, 24, 40 ... */
        uint8_t k, t = o->timer, fr2 = (uint8_t)((g_frame_counter >> 3) & 1U);
        int16_t nud = (int16_t)(hw_sin(o->hp) >> 4), ox = o->x - 64;
        if (t < 120U) o->timer = (uint8_t)(t + 1U);
        for (k = 0; k < 8; k++) {
            uint8_t xr;
            int16_t fx, fy;
            if (k < 7) { xr = (uint8_t)(8U + (k << 4)); if (xr > t) break; }
            else xr = t;
            if (k == 7 && t > 116U) xr = 116;
            fx = ox + xr;
            fy = o->base_y + 5 - (int16_t)mz_grass_h(fr, (uint8_t)((xr + 12U) >> 1)) + nud;
            if (!g_sonic.dead) {
                int16_t dx = g_sonic.x - fx, dy = g_sonic.y - fy;
                if (dx > -14 && dx < 14 && dy > -22 && dy < 14) sonic_hurt_or_kill(0);
            }
            if (((k ^ g_frame_counter) & 1U) == 0 || k == 7)
                pr_cells((fx - g_cam_x) >> 1, (fy - g_cam_y) >> 1, fr2 ? zp_mz_fire1 : zp_mz_fire0, 2, 0);
        }
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



uint8_t zp_mz2_object(ActiveObj *o) BANKED {
    switch (o->id) {
    case 0x2F: mz_grass(o); return 1;
    case 0x46: mz_brick(o); return 1;
    case 0x51: mz_smash(o); return 1;
    default: return 0;
    }
}
