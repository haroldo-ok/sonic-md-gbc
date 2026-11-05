#pragma bank 125
/* Baked decorations (`tools/bake_decor.py`): static scenery sprites (GHZ bridge stumps / edge walls, SLZ lava
   throwers) are composited into spare BG tiles; every covered map cell is patched here while its column / row streams
   in (`enqueue_world_span`, src/main.c). Cells are sorted by (bx, by); a binary search finds the span's first cell. */
#include "engine.h"
#include "decor.h"

uint16_t g_decor_cnt = 0;
static const DecorCell *s_decor_p = 0;    /* sorted by (bx, by) */
static const DecorCell *s_decor_r = 0;    /* sorted by (by, bx) */
static const uint8_t *s_decor_cb = 0, *s_decor_rb = 0;   /* per-column / per-row presence bits */
static uint16_t s_decor_cbn = 0;
static uint8_t s_decor_rbn = 0;

void decor_select_act(uint8_t act) BANKED {
    s_decor_p = act < 20U ? g_decor_tab[act] : 0;
    s_decor_r = act < 20U ? g_decor_rtab[act] : 0;
    s_decor_cb = act < 20U ? g_decor_colbits[act] : 0;
    s_decor_rb = act < 20U ? g_decor_rowbits[act] : 0;
    s_decor_cbn = act < 20U ? g_decor_colbytes[act] : 0;
    s_decor_rbn = act < 20U ? g_decor_rowbytes[act] : 0;
    g_decor_cnt = s_decor_p ? g_decor_n[act] : 0;
}

/* `t` = the span's 23 tile bytes, followed by its 23 attribute bytes. Columns search the (bx, by) table on bx,
   rows the (by, bx) table on by; only the cells of that one column / row are then visited. */
void decor_apply_b(uint8_t *t, int16_t wx0, int16_t wy0, uint8_t is_col) BANKED {
    const DecorCell *p;
    uint16_t lo = 0, hi = g_decor_cnt, mid, rem, d;
    uint8_t n = is_col ? 20U : 23U;
    if (wx0 < -(int16_t)n || wy0 < -(int16_t)n) return;
    if (is_col) {
        /* column spans only ever touch column wx0: reject it with one bit test */
        if (wx0 < 0 || (uint16_t)((uint16_t)wx0 >> 3) >= s_decor_cbn ||
            !(s_decor_cb[(uint16_t)wx0 >> 3] & (uint8_t)(1U << ((uint8_t)wx0 & 7U)))) return;
        p = s_decor_p;
        while (lo < hi) {
            mid = (lo + hi) >> 1;
            if ((int16_t)p[mid].bx < wx0) lo = mid + 1; else hi = mid;
        }
        p += lo;
        rem = g_decor_cnt - lo;
        while (rem-- && (int16_t)p->bx == wx0) {
            d = (uint16_t)(p->by - (uint16_t)wy0);
            if (d < n) { t[d] = p->tile; t[d + 23U] = p->attr; }
            p++;
        }
    } else {
        if (wy0 < 0 || (uint16_t)((uint16_t)wy0 >> 3) >= s_decor_rbn ||
            !(s_decor_rb[(uint16_t)wy0 >> 3] & (uint8_t)(1U << ((uint8_t)wy0 & 7U)))) return;
        p = s_decor_r;
        while (lo < hi) {
            mid = (lo + hi) >> 1;
            if ((int16_t)p[mid].by < wy0) lo = mid + 1; else hi = mid;
        }
        p += lo;
        rem = g_decor_cnt - lo;
        while (rem-- && (int16_t)p->by == wy0) {
            d = (uint16_t)(p->bx - (uint16_t)wx0);
            if (d < n) { t[d] = p->tile; t[d + 23U] = p->attr; }
            p++;
        }
    }
}
