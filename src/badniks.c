#pragma bank 9
/* Green Hill badnik movement, ported from the disassembly (the old code approximated it):
 *
 *   0x40 Moto Bug   (`40 Badnik - Moto Bug.asm`)   drives at 1 px/frame, follows the floor every frame, and at a step
 *                    of -8 / +12 px or more (a ledge or wall) stops for a second and turns round. The old version
 *                    checked the floor every 8 px, so Moto Bugs drove off ledges and hung in the air.
 *   0x1F Crabmeat   (`1F Badnik - Crabmeat.asm`)   scuttles at 0.5 px/frame for up to 128 frames, checking the floor
 *                    16 px ahead every other frame; on a ledge / wall / timeout it stops, waits a second, fires two
 *                    arcing balls, waits a second, and walks the other way. (Old: no floor check at all, and moving
 *                    right `vx >> 8` of $80 was 0, so a Crabmeat that turned right froze.)
 *   0x22 Buzz Bomber (`22 Badnik - Buzz Bomber.asm`) flies at 4 px/frame for 128 frames, hovers a second, turns.
 *                    When it passes within 96 px of Sonic it stops, waits half a second and fires one missile
 *                    diagonally down (no refiring until it turns), instead of firing on a global timer.
 *   0x2B Chopper    (`2B Badnik - Chopper.asm`)    leaps at -$700 with $18 gravity and lands back exactly at its
 *                    spawn height (the old leap rested 24 px lower).
 *
 * Object fields: state bit 7 = facing right (`status_xflip`), bit 6 = initialised, bit 0 = sub-routine (0 wait /
 * 1 move), bit 1 = Crabmeat firing flag / Buzz "fired" flag, bit 2 = Buzz "near Sonic"; timer = delay; hp = x
 * (Chopper: y) sub-pixel; vx / vy = speed (8.8). State values never reach 99 (the "destroyed" marker).
 */
#include "engine.h"
#include "badniks.h"

#define ST_RIGHT 0x80U
#define ST_INIT  0x40U
#define ST_MOVE  0x01U
#define ST_FLAG  0x02U
#define ST_NEAR  0x04U

/* `SpeedToPos` on X with the sub-pixel kept in `hp` */
static void bx_move(ActiveObj *o) {
    uint16_t s = (uint16_t)o->hp + (uint8_t)o->vx;
    o->x += (int16_t)(o->vx >> 8) + (int16_t)(s >> 8);
    o->hp = (uint8_t)s;
}

/* `ObjFloorDist` at (x, y + h): distance to the floor (negative = embedded) */
static int16_t bx_floor(int16_t x, int16_t y) {
    uint8_t ang = 0;
    return hw_floor_distance(x, y, &ang);
}

static uint8_t bx_on_screen(ActiveObj *o) {
    int16_t dx = o->x - g_cam_x, dy = o->y - g_cam_y;
    return (uint8_t)(dx > -24 && dx < 344 && dy > -24 && dy < 280);
}

/* First frame: `ObjectFall` until the floor (the object list places walkers a little above it). */
static void bx_settle(ActiveObj *o, int16_t h) {
    int16_t d = bx_floor(o->x, o->y + h);
    if (d > -16 && d < 16) o->y += d;          /* placements already sit on the floor: only small corrections */
}

static uint8_t motobug(ActiveObj *o) {
    if (!(o->state & ST_INIT)) {
        o->state = (uint8_t)(ST_INIT | ((o->subtype & 0x40U) ? ST_RIGHT : 0));
        o->state ^= ST_RIGHT;                    /* `bchg #status_xflip_bit` on landing */
        bx_settle(o, 14);
        o->timer = 0;
        o->vx = 0;
        o->hp = 0;
    }
    if (!(o->state & ST_MOVE)) {                 /* `Moto_Action_Ledge` */
        if (o->timer) { o->timer--; return 0; }
        o->state |= ST_MOVE;
        o->state ^= ST_RIGHT;
        o->vx = (o->state & ST_RIGHT) ? 0x100 : -0x100;
        return 0;
    }
    bx_move(o);                                  /* `Moto_Action_Drive` */
    {
        int16_t d = bx_floor(o->x, o->y + 14);
        if (d < -8 || d >= 12) {
            o->state &= (uint8_t)~ST_MOVE;
            o->timer = 59;
            o->vx = 0;
        } else {
            o->y += d;
        }
    }
    return 0;
}

static uint8_t crabmeat(ActiveObj *o) {
    if (!(o->state & ST_INIT)) {
        o->state = (uint8_t)(ST_INIT | ((o->subtype & 0x40U) ? ST_RIGHT : 0));
        bx_settle(o, 16);
        o->timer = 0;
        o->vx = 0;
        o->hp = 0;
    }
    if (!(o->state & ST_MOVE)) {                 /* `Crab_Action_WaitFire` */
        if (o->timer) { o->timer--; return 0; }
        if (bx_on_screen(o)) {
            o->state ^= ST_FLAG;
            if (!(o->state & ST_FLAG)) {         /* flag was set: fire, stay a second on the firing frame */
                o->timer = 59;
                return BADNIK_FIRE;
            }
        }
        o->state |= ST_MOVE;
        o->timer = 127;
        o->state ^= ST_RIGHT;
        o->vx = (o->state & ST_RIGHT) ? 0x80 : -0x80;
        return 0;
    }
    if (o->timer == 0) {                         /* `Crab_Action_Scuttle`: walked ~2 s without finding a ledge */
        goto stop;
    }
    o->timer--;
    bx_move(o);
    o->state ^= 0x08U;                           /* `bchg #0,crab_flags`: ledge check / align on alternate frames */
    if (o->state & 0x08U) {
        int16_t d = bx_floor(o->x + ((o->state & ST_RIGHT) ? 16 : -16), o->y + 16);
        if (d < -8 || d >= 12) goto stop;
    } else {
        int16_t d = bx_floor(o->x, o->y + 16);
        if (d > -16 && d < 16) o->y += d;
    }
    return 0;
stop:
    o->state &= (uint8_t)~ST_MOVE;
    o->timer = 59;
    o->vx = 0;
    return 0;
}

static uint8_t buzzbomber(ActiveObj *o) {
    if (!(o->state & ST_INIT)) {
        o->state = (uint8_t)(ST_INIT | ((o->subtype & 0x40U) ? ST_RIGHT : 0));
        o->timer = 0;
        o->vx = 0;
        o->hp = 0;
    }
    if (!(o->state & ST_MOVE)) {                 /* `Buzz_Action_Wait` */
        if (o->timer) { o->timer--; return 0; }
        if (o->state & ST_NEAR) {                /* `Buzz_Action_Fire` */
            o->state = (uint8_t)((o->state & ~ST_NEAR) | ST_FLAG);
            o->timer = 59;
            return BADNIK_FIRE;
        }
        o->state |= ST_MOVE;
        o->timer = 127;
        o->vx = (o->state & ST_RIGHT) ? 0x400 : -0x400;
        return 0;
    }
    if (o->timer == 0) {                         /* `.changeDirection` */
        o->state = (uint8_t)((o->state ^ ST_RIGHT) & ~(ST_MOVE | ST_FLAG | ST_NEAR));
        o->timer = 59;
        o->vx = 0;
        return 0;
    }
    o->timer--;
    bx_move(o);
    if (!(o->state & (ST_FLAG | ST_NEAR))) {
        int16_t dx = g_sonic.x - o->x;
        if (dx < 0) dx = -dx;
        if (dx < 96 && bx_on_screen(o)) {
            o->state = (uint8_t)((o->state | ST_NEAR) & ~ST_MOVE);
            o->timer = 29;
            o->vx = 0;
        }
    }
    return 0;
}

static uint8_t chopper(ActiveObj *o) {
    uint16_t s;
    if (!(o->state & ST_INIT)) {
        o->state = ST_INIT;
        o->vy = -0x700;
        o->hp = 0;
        o->y = o->base_y;
    }
    s = (uint16_t)o->hp + (uint8_t)o->vy;        /* `SpeedToPos` on Y */
    o->y += (int16_t)(o->vy >> 8) + (int16_t)(s >> 8);
    o->hp = (uint8_t)s;
    o->vy += 0x18;
    if (o->y > o->base_y) {
        o->y = o->base_y;
        o->vy = -0x700;
        o->hp = 0;
    }
    return 0;
}

BANKREF(badnik_ghz_step)
uint8_t badnik_ghz_step(ActiveObj *o, uint8_t id) BANKED {
    if (id == 0x40) return motobug(o);
    if (id == 0x1F) return crabmeat(o);
    if (id == 0x22) return buzzbomber(o);
    if (id == 0x2B) return chopper(o);
    return BADNIK_NOT_MINE;
}
