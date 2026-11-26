#ifndef SOLID_S1_H
#define SOLID_S1_H
/* Port of `SolidObject` returning its contact result, for objects that react to where Sonic touches them
   (spikes, Egg Prison). Include after props_common.h; each including bank gets its own copy. */
/* `SolidObject` with its contact result: 0 none, 1 side, -1 top or bottom. Landing on top attaches Sonic (`g_stand_obj`). */
static int8_t solid_s1(ActiveObj *o, int16_t d1, int16_t d2) {
    int16_t h = pr_h();
    int16_t d0, d3, d4, d5, t;

    if (g_stand_obj == o) {
        d0 = g_sonic.x - o->x + d1;
        if (g_sonic.ysp < 0 || d0 < 0 || d0 >= (d1 << 1)) {
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
        if (d3 >= 0) {
            if (d3 < 0x10) {
                d3 -= 4;
                t = d1 - 11 + g_sonic.x - o->x;          /* obActWid range */
                if (t >= 0 && t < ((d1 - 11) << 1) && g_sonic.ysp >= 0) {
                    pr_land(o, g_sonic.y - d3 - 1);
                    return -1;
                }
            }
            return 0;
        }
        if (g_sonic.ysp < 0) {
            pr_set_y(g_sonic.y - d3);
            g_sonic.ysp = 0;
        }
        return -1;
    }
    if (t <= 4) return 0;
    if (d0 != 0) {
        if (d0 > 0 ? (g_sonic.xsp >= 0) : (g_sonic.xsp < 0)) {
            g_sonic.xsp = 0;
            g_sonic.gsp = 0;
        }
        pr_move_x(-d0);
    }
    if (g_sonic.on_ground && (g_joy & (d0 > 0 ? J_RIGHT : J_LEFT))) g_sonic.anim = ANI_PUSH;
    return 1;
}

#endif
