#pragma bank 9
/* Spikes (`36 Spikes.asm`), every layout, flip and movement type.
 *
 * Object fields: subtype = `tools/spike_tiles.py::spike_subtype` (bits 0-1 movement, 2-4 `Spikes_Config` row, 6 X flip,
 * 7 Y flip); base_x / base_y = `spikes_origX/Y`; vy = `spikes_move_pos` (0..32 px); state bit 0 =
 * `spikes_move_direction`; timer = `spikes_move_delay` (frames).
 *
 * Behaviour follows the original with its two documented fixes (`FixBugs` in the disassembly): spikes are solid all
 * round but only the pointed end hurts, and a Sonic still flashing from a hit is not hurt again. Without them a hurt
 * Sonic who landed back on the same spikes lost his rings and died in one sequence, and touching the base of ceiling
 * or sideways spikes hurt. */
#include "engine.h"
#include "props_common.h"
#include "solid_s1.h"

/* `Spikes_Config` width / 2 per layout; frame 1 and 5 are sideways */
static const uint8_t k_spk_w[6] = { 40 / 2, 32 / 2, 8 / 2, 56 / 2, 128 / 2, 32 / 2 };
/* `Map_Spike` piece offsets (left edge of each upright 8x32 piece / top edge of each sideways 32x8 piece) */
static const int8_t k_spk_u0[3] = { -0x14, -4, 0x0C };
static const int8_t k_spk_u3[3] = { -0x1C, -4, 0x14 };
static const int8_t k_spk_u4[6] = { -0x40, -0x28, -0x10, 8, 0x20, 0x38 };
static const int8_t k_spk_s1[3] = { -0x14, -4, 0x0C };
static const int8_t k_spk_mid[1] = { -4 };

#define SPK_PAL 5   /* steel OBJ palette */

/* `Spikes_WaitAndMove`: out 32 px in 8 px steps, wait a second, back in, wait a second. */
static void spk_move(ActiveObj *o) {
    if (o->timer) {
        if (--o->timer == 0) {
            int16_t dx = o->x - g_cam_x, dy = o->y - g_cam_y;
            if (dx > -32 && dx < 352 && dy > -32 && dy < 288) hw_play_sfx(SFX_SPIKESMOVE);
        }
        return;
    }
    if (o->state & 1) {
        if (o->vy >= 8) {
            o->vy -= 8;
            return;
        }
        o->vy = 0;
        o->state &= (uint8_t)~1U;
    } else {
        if (o->vy < 24) {
            o->vy += 8;
            return;
        }
        o->vy = 32;
        o->state |= 1;
    }
    o->timer = 60;
}

static void spk_hurt(void) {
    uint8_t had_rings;
    if (g_sonic.dead || g_sonic.invuln || g_sonic.invinc || (g_cheats & CHEAT_INVINCIBLE)) return;
    /* `Spikes_Hurt`: undo this frame's fall so the knock-back starts from where Sonic touched the points */
    pr_set_y(g_sonic.y - (g_sonic.ysp >> 8));
    g_stand_obj = 0;
    had_rings = g_rings != 0 && !g_sonic.shield;
    sonic_hurt_or_kill(0);
    if (!had_rings) hw_play_sfx(SFX_SPIKES);    /* `HurtSonic` / `KillSonic`: `sfx_HitSpikes` when the hurter is spikes */
}

static void spk_draw(ActiveObj *o, uint8_t frame, uint8_t fx, uint8_t fy) {
    const int8_t *p;
    uint8_t n;
    int16_t sx = (o->x - g_cam_x) >> 1;
    int16_t sy = (o->y - g_cam_y) >> 1;
    if (sx < -72 || sx > 232 || sy < -24 || sy > 152) return;
    if (frame == 1 || frame == 5) {
        /* sideways: rows of [tip | base], tip on the left unless X-flipped */
        uint8_t t0 = fx ? SPR_TILE_SPIKES + 3 : SPR_TILE_SPIKES + 2;
        uint8_t t1 = fx ? SPR_TILE_SPIKES + 2 : SPR_TILE_SPIKES + 3;
        uint8_t a = SPK_PAL | (fx ? 0x20 : 0) | (fy ? 0x40 : 0);
        if (frame == 1) { p = k_spk_s1; n = 3; } else { p = k_spk_mid; n = 1; }
        while (n--) {
            int8_t py = fy ? (int8_t)(-*p - 8) : *p;
            hw_put_sprite8(sx - 8, sy + (py >> 1) - 2, t0, a);
            hw_put_sprite8(sx, sy + (py >> 1) - 2, t1, a);
            p++;
        }
        return;
    }
    {
        /* upright: columns of [tip / base], tip on top unless Y-flipped */
        uint8_t t0 = fy ? SPR_TILE_SPIKES + 1 : SPR_TILE_SPIKES;
        uint8_t t1 = fy ? SPR_TILE_SPIKES : SPR_TILE_SPIKES + 1;
        uint8_t a = SPK_PAL | (fx ? 0x20 : 0) | (fy ? 0x40 : 0);
        if (frame == 0) { p = k_spk_u0; n = 3; }
        else if (frame == 3) { p = k_spk_u3; n = 3; }
        else if (frame == 4) { p = k_spk_u4; n = 6; }
        else { p = k_spk_mid; n = 1; }
        while (n--) {
            int8_t px = fx ? (int8_t)(-*p - 8) : *p;
            hw_put_sprite8(sx + (px >> 1) - 2, sy - 8, t0, a);
            hw_put_sprite8(sx + (px >> 1) - 2, sy, t1, a);
            p++;
        }
    }
}

BANKREF(spikes_object)
void spikes_object(ActiveObj *o) BANKED {
    uint8_t frame = (uint8_t)((o->subtype >> 2) & 7U);
    uint8_t mv = (uint8_t)(o->subtype & 3U);
    uint8_t fx = (uint8_t)((o->subtype >> 6) & 1U);
    uint8_t fy = (uint8_t)(o->subtype >> 7);
    int8_t hit;

    if (frame > 5) frame = 0;
    if (mv == 1) {
        spk_move(o);
        o->y = o->base_y + o->vy;
    } else if (mv == 2) {
        spk_move(o);
        o->x = o->base_x + o->vy;
    }

    if (frame == 1 || frame == 5) {
        hit = solid_s1(o, 32 / 2 + 11, frame == 1 ? 40 / 2 : 8 / 2);
        if (hit > 0) {
            int16_t d0 = g_sonic.x - o->x;
            if (fx) d0 = -d0;
            if (d0 <= 0) spk_hurt();
        }
    } else {
        hit = solid_s1(o, (int16_t)k_spk_w[frame] + 11, 32 / 2);
        if (g_stand_obj == o || hit < 0) {
            int16_t d0 = g_sonic.y - o->y;
            if (fy) d0 = -d0;
            if (d0 <= 0) spk_hurt();
        }
    }
    spk_draw(o, frame, fx, fy);
}
