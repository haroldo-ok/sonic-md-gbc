#pragma bank 9
/* Egg Prison (`3E Prison Capsule.asm`): the body (subtype 0) and the button on top (subtype 1) are separate objects
 * in every act 3's object list. Both are solid (`SolidObject`). Standing on the button once the boss is beaten
 * pushes it 8 px down (`Pri_Switch`), the capsule explodes for a second (`Pri_Explosion`, `sfx_Bomb` every 8 frames),
 * opens (`v_bossstatus` = 2: Sonic standing on it is dropped), releases animals (`Pri_Animals`) and then ends the act.
 * The old version opened as soon as Sonic touched any part of it, was not solid, and the defeated boss also turned
 * itself into a second, floating capsule (now it escapes instead; see bosses_and_gimmicks.c). */
#include "engine.h"
#include "props_common.h"
#include "solid_s1.h"

#define PRI_CLOSED 0
#define PRI_BLAST  1
#define PRI_OPEN   2

static uint8_t s_pri = PRI_CLOSED;
static uint8_t s_pri_timer = 0;

static void pri_release(ActiveObj *o) {
    if (g_stand_obj == o) {
        g_stand_obj = 0;
        g_sonic.on_ground = 0;
    }
}

BANKREF(prison_object)
void prison_object(ActiveObj *o) BANKED {
    int16_t sx = (o->x - g_cam_x) >> 1;
    int16_t sy = (o->y - g_cam_y) >> 1;

    if (!g_boss_defeated) s_pri = PRI_CLOSED;     /* new act / boss not beaten yet */

    if (o->subtype != 0) {
        /* `Pri_Switch` */
        if (s_pri == PRI_CLOSED) {
            o->y = o->base_y;
            solid_s1(o, 24 / 2 + 11, 16 / 2);
            if (g_stand_obj == o && g_boss_defeated) {
                o->y = o->base_y + 8;
                pri_release(o);
                g_sonic.ysp = 0;
                s_pri = PRI_BLAST;
                s_pri_timer = 60;
                g_sonic.lock_ctrl = 60;          /* `f_lockctrl` while the capsule blows */
            }
        }
        /* the half-scale body art is shorter than the original's: seat the button on it (old body drew it at -14) */
        sy = ((o->y - g_cam_y) >> 1) + 5;
        hw_put_sprite8(sx - 8, sy - 4, ZSPR_CAPSULE + 6, 0x08 | 1);
        hw_put_sprite8(sx, sy - 4, ZSPR_CAPSULE + 7, 0x08 | 1);
        return;
    }

    /* `Pri_BodyMain`: solid until opened */
    if (s_pri != PRI_OPEN) {
        solid_s1(o, 64 / 2 + 11, 48 / 2);
    } else {
        pri_release(o);
    }

    if (s_pri == PRI_BLAST) {
        /* `Pri_Explosion`: random puffs over the capsule, `sfx_Bomb` every 8 frames */
        if ((s_pri_timer & 7) == 0) hw_play_sfx(SFX_BOMB);
        hw_put_sprite16(sx - 12 + (int16_t)((g_frame_counter * 7U) & 23U), sy - 12 + (int16_t)((g_frame_counter * 5U) & 15U),
                        SPR_TILE_EXPLODE + ((s_pri_timer & 4) ? 4 : 0), 1);
        if (--s_pri_timer == 0) {
            s_pri = PRI_OPEN;
            s_pri_timer = 150;
        }
    } else if (s_pri == PRI_OPEN && s_pri_timer) {
        /* `Pri_Animals`: animals hop out, then `GotThroughAct` */
        if ((s_pri_timer & 7) == 0 && s_pri_timer > 60) {
            spawn_enemy_projectile(o->x, o->y - 8, (s_pri_timer & 8) ? 0x1C0 : -0x1C0, -0x380, PROJ_ANIMAL_HOP);
        }
        if (--s_pri_timer == 0) {
            g_act_clear_done = 1;
            g_next_mode = GM_ACT_CLEAR;
        }
    }

    if (s_pri == PRI_OPEN) {
        hw_put_sprite16(sx, sy, ZSPR_CAPSULE + 8, 0x08 | 2);
    } else {
        hw_put_sprite_cols2rows(sx - 12, sy - 8, ZSPR_CAPSULE, 3u, 0x08 | 2);
    }
}
