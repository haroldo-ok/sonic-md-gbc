#pragma bank 2
#include "engine.h"
#include <stddef.h>
#include <string.h>

static uint8_t box_overlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                           int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    int16_t dx = ax - bx;
    int16_t dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx < (aw + bw)) && (dy < (ah + bh));
}

void objects_reset_for_act(void) BANKED {
    memset(g_objs, 0, sizeof(g_objs));
    memset(g_lost_rings, 0, sizeof(g_lost_rings));
    memset(g_obj_taken, 0, sizeof(g_obj_taken));
    ghz_objects_reset();
    g_signpost_passed = 0;
    g_act_clear_done = 0;
    g_boss_active = 0;
    g_boss_defeated = 0;
}


/* ===================================================================================================
 * Collectible stage rings rendered as CGB background tiles (moved out of fixed bank 0).
 *
 * Every frame each visible ring contributes one quadrant to four adjacent BG cells. Cells are kept in two
 * alternating sets (`cur` = being filled this frame, `prev` = what is currently on screen). Each set has a
 * 1 KiB direct-lookup grid in the otherwise unused level-mode SRAM window `$A000..$A7FF` (the Special Stage
 * maze lives there only inside Special Stages). A grid entry is trusted only if it points back at a cell with
 * the same position, so stale data (e.g. from a Special Stage) can never be misread and nothing needs clearing.
 * This replaces the former O(n^2) list scans and per-cell VRAM reads with O(1) lookups; the flush only touches
 * VRAM for cells that appeared, vanished, changed quadrant mask, or sit in a column/row that terrain streaming
 * just rewrote.
 * =================================================================================================== */
#define MAX_BG_RING_CELLS (MAX_ACTIVE_OBJS * 4U)
#define RING_GRID(set) ((uint8_t *)(0xA000U + ((uint16_t)(set) << 10)))
#define RING_VRAM(pos) ((volatile uint8_t *)(0x9800U + (uint16_t)(pos)))

uint8_t g_ring_force_redraw = 1;
uint8_t g_ring_dirty_col[4];
uint8_t g_ring_dirty_row[4];
static uint16_t s_rc[2][MAX_BG_RING_CELLS]; /* bits 0..4 x, 5..9 y, 10..13 quadrant mask */
static uint8_t s_rn[2];
static uint8_t s_rcur;

/* Per-frame sequence of queued ring cells (x | y << 5), kept in free level-mode SRAM ($B200..$B3FF). Stage rings
   do not move, so while the camera stays inside the same tiles the sequence repeats exactly; building the four-cell
   overlay list is then deferred and, if the whole sequence matches what is already on screen, skipped entirely. */
#define RING_SEQ ((uint16_t *)0xB200U)
static uint8_t s_seq_n;      /* length of the sequence whose cells are currently drawn (s_rc[prev]) */
static uint8_t s_seq_k;      /* rings queued so far this frame */
static uint8_t s_seq_match;  /* 1 while every ring queued this frame equals RING_SEQ[k] (cells not built yet) */

static void hw_begin_bkg_rings(void) {
    s_rn[s_rcur] = 0;
    s_seq_k = 0;
    s_seq_match = 1;
}

/* Cell-list builder (hand-written). For each queued ring key (x | y << 5) it adds one quadrant bit to each of the
   four cells (rowm|xm: 0x0400, rowm|x: 0x0800, row|xm: 0x1000, row|x: 0x2000), where xm/rowm are the previous
   column/row modulo 32. Adding: grid[pos] = j is trusted only if j < n and cells[j] holds pos (then OR the bit);
   otherwise, while n < MAX_BG_RING_CELLS, append pos|bit and point grid[pos] at it. */
typedef char ring_cells_check[(MAX_BG_RING_CELLS == 96U) ? 1 : -1];
static uint16_t *s_rb_cells;
static uint8_t *s_rb_grid;
static uint8_t s_rb_n, s_rb_bit, s_rb_cnt, s_rb_x, s_rb_xm, s_rb_row_lo, s_rb_row_hi, s_rb_rowm_lo, s_rb_rowm_hi;
/* sdcccall(1): seq in DE, cnt in A. */
static void ring_build_asm(const uint16_t *seq, uint8_t cnt) __naked {
    (void)seq; (void)cnt;
    __asm
    or   a, a
    ret  z
    ld   (_s_rb_cnt), a
1101$:
    ld   a, (de)
    inc  de
    ld   c, a
    ld   a, (de)
    inc  de
    push de
    ld   b, a
    ld   a, c
    and  a, #0xE0
    ld   l, a
    ld   (_s_rb_row_lo), a
    ld   a, b
    and  a, #0x03
    ld   h, a
    ld   (_s_rb_row_hi), a
    ld   a, l
    sub  a, #0x20
    ld   e, a
    ld   (_s_rb_rowm_lo), a
    ld   a, h
    sbc  a, #0
    and  a, #0x03
    ld   d, a
    ld   (_s_rb_rowm_hi), a
    ld   a, c
    and  a, #31
    ld   (_s_rb_x), a
    dec  a
    and  a, #31
    ld   (_s_rb_xm), a
    or   a, e
    ld   e, a
    ld   a, #0x04
    call 1150$
    ld   a, (_s_rb_rowm_lo)
    ld   e, a
    ld   a, (_s_rb_rowm_hi)
    ld   d, a
    ld   a, (_s_rb_x)
    or   a, e
    ld   e, a
    ld   a, #0x08
    call 1150$
    ld   a, (_s_rb_row_lo)
    ld   e, a
    ld   a, (_s_rb_row_hi)
    ld   d, a
    ld   a, (_s_rb_xm)
    or   a, e
    ld   e, a
    ld   a, #0x10
    call 1150$
    ld   a, (_s_rb_row_lo)
    ld   e, a
    ld   a, (_s_rb_row_hi)
    ld   d, a
    ld   a, (_s_rb_x)
    or   a, e
    ld   e, a
    ld   a, #0x20
    call 1150$
    pop  de
    ld   a, (_s_rb_cnt)
    dec  a
    ld   (_s_rb_cnt), a
    jr   nz, 1101$
    ret
1150$:
    ld   (_s_rb_bit), a
    ld   a, (_s_rb_grid)
    add  a, e
    ld   l, a
    ld   a, (_s_rb_grid + 1)
    adc  a, d
    ld   h, a
    ld   c, (hl)
    ld   a, (_s_rb_n)
    ld   b, a
    ld   a, c
    cp   a, b
    jr   nc, 1152$
    push hl
    ld   l, c
    ld   h, #0
    add  hl, hl
    ld   a, (_s_rb_cells)
    add  a, l
    ld   l, a
    ld   a, (_s_rb_cells + 1)
    adc  a, h
    ld   h, a
    ld   a, (hl+)
    cp   a, e
    jr   nz, 1151$
    ld   a, (hl)
    and  a, #0x03
    cp   a, d
    jr   nz, 1151$
    ld   a, (_s_rb_bit)
    or   a, (hl)
    ld   (hl), a
    pop  hl
    ret
1151$:
    pop  hl
1152$:
    ld   a, b
    cp   a, #96
    ret  nc
    ld   (hl), b
    ld   l, b
    ld   h, #0
    add  hl, hl
    ld   a, (_s_rb_cells)
    add  a, l
    ld   l, a
    ld   a, (_s_rb_cells + 1)
    adc  a, h
    ld   h, a
    ld   (hl), e
    inc  hl
    ld   a, (_s_rb_bit)
    or   a, d
    ld   (hl), a
    ld   a, b
    inc  a
    ld   (_s_rb_n), a
    ret
    __endasm;
}

/* Build the four overlay cells of `cnt` queued rings from RING_SEQ[first..]. */
static void ring_build_cells(uint8_t first, uint8_t cnt) {
    s_rb_cells = s_rc[s_rcur];
    s_rb_grid = RING_GRID(s_rcur);
    s_rb_n = s_rn[s_rcur];
    ring_build_asm(&RING_SEQ[first], cnt);
    s_rn[s_rcur] = s_rb_n;
}

static void hw_queue_bkg_ring(int16_t world_x, int16_t world_y) {
    uint16_t key;
    uint8_t k = s_seq_k;
    /* Only populate the 32x32 world tile window around the visible viewport. */
    if ((uint16_t)(world_x - g_cam_x + 16) > 352U || (uint16_t)(world_y - g_cam_y + 16) > 304U) return;
    key = (uint16_t)((((uint16_t)world_x >> 4) & 31U) | (((uint16_t)world_y << 1) & 0x03E0U));
    if (s_seq_match) {
        if (k < s_seq_n && RING_SEQ[k] == key) { s_seq_k = (uint8_t)(k + 1U); return; }
        /* Diverged from the drawn sequence: materialize the matching prefix, then continue normally. */
        s_seq_match = 0;
        s_seq_n = 0;   /* RING_SEQ is being overwritten: it no longer describes what is on screen */
        ring_build_cells(0, k);
    }
    RING_SEQ[k] = key;
    s_seq_k = (uint8_t)(k + 1U);
    ring_build_cells(k, 1);
}

/* Remove the overlay tile (if still ours) at BG cell `pos`. */
static void ring_clear_cell(uint16_t pos) {
    volatile uint8_t *p = RING_VRAM(pos);
    uint8_t old;
    VBK_REG = 0;
    while (STAT_REG & STATF_BUSY) {}
    old = *p;
    if (old >= BKG_RING_TILE_BASE) {
        while (STAT_REG & STATF_BUSY) {}
        *p = 0;
        VBK_REG = 1;
        while (STAT_REG & STATF_BUSY) {}
        *p = 0;
        VBK_REG = 0;
    }
}

/* Draw one overlay cell (existing terrain wins; attribute rewritten to clear stale terrain attrs). */
static void ring_draw_cell(uint16_t cell) {
    uint16_t pos = (uint16_t)(cell & 0x03FFU);
    volatile uint8_t *p = RING_VRAM(pos);
    uint8_t old, tile = (uint8_t)(BKG_RING_TILE_BASE + (cell >> 10));
    while (STAT_REG & STATF_BUSY) {}
    old = *p;
    if (old == 0U || old >= BKG_RING_TILE_BASE) {
        if (old != tile) {
            while (STAT_REG & STATF_BUSY) {}
            *p = tile;
        }
        VBK_REG = 1;
        while (STAT_REG & STATF_BUSY) {}
        *p = 0x0BU; /* Bank 1, palette 3: sky-matched zero + warm ring colors */
        VBK_REG = 0;
    }
}

static uint16_t *s_fl_pc, *s_fl_nc;
static uint8_t *s_fl_pg, *s_fl_ng;
static uint8_t s_fl_pn, s_fl_nn, s_fl_force, s_fl_k;

/* For every previous cell whose position is not in the new set (new grid lookup validated against the new list),
   call ring_clear_cell(pos). */
static void ring_remove_loop(void) __naked {
    __asm
    ld   a, (_s_fl_pn)
    or   a, a
    ret  z
    ld   (_s_fl_k), a
    ld   hl, #_s_fl_pc
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
1201$:
    ld   a, (hl+)
    ld   e, a
    ld   a, (hl+)
    and  a, #0x03
    ld   d, a
    push hl
    ld   a, (_s_fl_ng)
    add  a, e
    ld   l, a
    ld   a, (_s_fl_ng + 1)
    adc  a, d
    ld   h, a
    ld   c, (hl)
    ld   a, (_s_fl_nn)
    ld   b, a
    ld   a, c
    cp   a, b
    jr   nc, 1205$
    ld   l, c
    ld   h, #0
    add  hl, hl
    ld   a, (_s_fl_nc)
    add  a, l
    ld   l, a
    ld   a, (_s_fl_nc + 1)
    adc  a, h
    ld   h, a
    ld   a, (hl+)
    cp   a, e
    jr   nz, 1205$
    ld   a, (hl)
    and  a, #0x03
    cp   a, d
    jr   z, 1206$
1205$:
    call _ring_clear_cell
1206$:
    pop  hl
    ld   a, (_s_fl_k)
    dec  a
    ld   (_s_fl_k), a
    jr   nz, 1201$
    ret
    __endasm;
}

/* For every new cell: unless forced, skip it when the previous set holds the identical cell (prev grid lookup
   validated against the previous list) and neither its column nor its row is marked dirty; else ring_draw_cell. */
static void ring_draw_loop(void) __naked {
    __asm
    ld   a, (_s_fl_nn)
    or   a, a
    ret  z
    ld   (_s_fl_k), a
    ld   hl, #_s_fl_nc
    ld   a, (hl+)
    ld   h, (hl)
    ld   l, a
1301$:
    ld   a, (hl+)
    ld   e, a
    ld   a, (hl+)
    ld   d, a
    push hl
    ld   a, (_s_fl_force)
    or   a, a
    jp   nz, 1305$
    ld   a, d
    and  a, #0x03
    ld   b, a
    ld   a, (_s_fl_pg)
    add  a, e
    ld   l, a
    ld   a, (_s_fl_pg + 1)
    adc  a, b
    ld   h, a
    ld   c, (hl)
    ld   a, (_s_fl_pn)
    ld   b, a
    ld   a, c
    cp   a, b
    jr   nc, 1305$
    ld   l, c
    ld   h, #0
    add  hl, hl
    ld   a, (_s_fl_pc)
    add  a, l
    ld   l, a
    ld   a, (_s_fl_pc + 1)
    adc  a, h
    ld   h, a
    ld   a, (hl+)
    cp   a, e
    jr   nz, 1305$
    ld   a, (hl)
    cp   a, d
    jr   nz, 1305$
    ld   a, e
    and  a, #31
    ld   c, a
    rrca
    rrca
    rrca
    and  a, #0x03
    ld   hl, #_g_ring_dirty_col
    add  a, l
    ld   l, a
    ld   a, #0
    adc  a, h
    ld   h, a
    ld   a, c
    and  a, #7
    ld   c, a
    ld   a, #1
    inc  c
1302$:
    dec  c
    jr   z, 1303$
    add  a, a
    jr   1302$
1303$:
    and  a, (hl)
    jr   nz, 1305$
    ld   a, d
    and  a, #0x03
    add  a, a
    add  a, a
    add  a, a
    ld   c, a
    ld   a, e
    swap a
    rrca
    and  a, #7
    or   a, c
    ld   c, a
    rrca
    rrca
    rrca
    and  a, #0x03
    ld   hl, #_g_ring_dirty_row
    add  a, l
    ld   l, a
    ld   a, #0
    adc  a, h
    ld   h, a
    ld   a, c
    and  a, #7
    ld   c, a
    ld   a, #1
    inc  c
1307$:
    dec  c
    jr   z, 1308$
    add  a, a
    jr   1307$
1308$:
    and  a, (hl)
    jr   z, 1306$
1305$:
    call _ring_draw_cell
1306$:
    pop  hl
    ld   a, (_s_fl_k)
    dec  a
    ld   (_s_fl_k), a
    jp   nz, 1301$
    ret
    __endasm;
}

void hw_ring_reset(void) BANKED {
    uint8_t cur = s_rcur, prv = (uint8_t)(cur ^ 1U);
    uint8_t i, pn = s_rn[prv];
    if (pn) {
        vsync();
        rRAMB = 1;
        for (i = 0; i < pn; i++) ring_clear_cell((uint16_t)(s_rc[prv][i] & 0x03FFU));
    }
    s_rn[0] = s_rn[1] = 0;
    s_seq_n = 0; s_seq_k = 0; s_seq_match = 1;
    g_ring_force_redraw = 1;
}

void hw_flush_bkg_rings(void) BANKED {
    uint8_t cur = s_rcur, prv = (uint8_t)(cur ^ 1U);
    uint16_t *nc = s_rc[cur], *pc = s_rc[prv];
    uint8_t *ng = RING_GRID(cur), *pg = RING_GRID(prv);
    uint8_t nn, pn = s_rn[prv];
    uint8_t force = g_ring_force_redraw;
    uint8_t unchanged = 0;
    uint8_t i;
    uint8_t dirty = (uint8_t)(g_ring_dirty_col[0] | g_ring_dirty_col[1] | g_ring_dirty_col[2] | g_ring_dirty_col[3] |
                              g_ring_dirty_row[0] | g_ring_dirty_row[1] | g_ring_dirty_row[2] | g_ring_dirty_row[3]);

    if (s_seq_match) {
        if (s_seq_k == s_seq_n && !force && !dirty) {
            /* Exactly the rings already on screen and nothing streamed over them: no VRAM work, keep the drawn set. */
            s_seq_k = 0;
            return;
        }
        ring_build_cells(0, s_seq_k);   /* deferred build of the (prefix-)matching sequence */
    }
    s_seq_n = s_seq_k;
    s_seq_k = 0;
    s_seq_match = 1;
    nn = s_rn[cur];

    VBK_REG = 0;

    /* Fast path: identical cell list (same order, same quadrant masks), nothing streamed over it, no forced redraw.
       This is the common case while the camera is not crossing a tile boundary. */
    if (!force && nn == pn && !dirty) {
        uint16_t *a = nc, *b = pc;
        for (i = nn; i != 0U; i--) {
            if (*a++ != *b++) break;
        }
        unchanged = (i == 0U);
    }

    if (!unchanged) {
        /* Remove cells whose last contributing stage ring was collected or streamed out, then draw only new /
           changed cells and cells on columns/rows that terrain streaming just rewrote (hand-written loops). */
        s_fl_pc = pc; s_fl_pn = pn; s_fl_pg = pg;
        s_fl_nc = nc; s_fl_nn = nn; s_fl_ng = ng;
        s_fl_force = force;
        ring_remove_loop();
        ring_draw_loop();
    }

    g_ring_force_redraw = 0;
    g_ring_dirty_col[0] = g_ring_dirty_col[1] = g_ring_dirty_col[2] = g_ring_dirty_col[3] = 0;
    g_ring_dirty_row[0] = g_ring_dirty_row[1] = g_ring_dirty_row[2] = g_ring_dirty_row[3] = 0;
    s_rcur = prv;     /* what we just drew is now "prev"; refill the other set next frame */
    s_rn[prv] = 0;
    VBK_REG = 0;
}

static uint8_t is_badnik_id(uint8_t id) {
    return (id == 0x1E || id == 0x1F || id == 0x22 || id == 0x2B || id == 0x2C ||
            id == 0x2D || id == 0x40 || id == 0x42 || id == 0x43 || id == 0x4D ||
            id == 0x4F || id == 0x50 || id == 0x55 || id == 0x5F || id == 0x60 ||
            id == 0x78);
}

static uint8_t badnik_sprite_and_pal(uint8_t id, uint8_t *out_pal) {
    uint8_t v = g_ram_act.vset_idx;
    /* Exact 1-to-1 VisualSet sprite bank mapping (`vset_idx` 0..8 matching loaded VRAM Bank 1!) */
    if (v == 0 || v == 8) { /* GHZ / Ending */
        if (id == 0x40) { *out_pal = 3; return ZSPR_ENEMY1; } /* Moto Bug (Red/White/Black) */
        if (id == 0x1F) { *out_pal = 3; return ZSPR_ENEMY2; } /* Crabmeat */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY3; } /* Buzz Bomber (Cobalt Blue) */
        if (id == 0x2B) { *out_pal = 3; return ZSPR_ENEMY4; } /* Chopper */
        *out_pal = 7; return ZSPR_ENEMY5;                     /* Newtron (Vivid Emerald Green!) */
    }
    if (v == 1 || v == 7) { /* LZ / SBZ Act 3 */
        if (id == 0x2D) { *out_pal = 4; return ZSPR_ENEMY1; } /* Burrobot */
        if (id == 0x2C) { *out_pal = 3; return ZSPR_ENEMY2; } /* Jaws */
        if (id == 0x60) { *out_pal = 5; return ZSPR_ENEMY3; } /* Orbinaut */
        if (id == 0x62) { *out_pal = 5; return ZSPR_ENEMY4; } /* Gargoyle */
        *out_pal = 7; return ZSPR_ENEMY5;
    }
    if (v == 2) { /* MZ */
        if (id == 0x78) { *out_pal = 3; return ZSPR_ENEMY1; } /* Caterkiller */
        if (id == 0x50) { *out_pal = 3; return ZSPR_ENEMY2; } /* Yadrin */
        if (id == 0x55) { *out_pal = 5; return ZSPR_ENEMY3; } /* Basaran */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 3; return ZSPR_ENEMY5;                     /* Splats */
    }
    if (v == 3) { /* SLZ */
        if (id == 0x5F) { *out_pal = 5; return ZSPR_ENEMY1; } /* Bomb */
        if (id == 0x60) { *out_pal = 3; return ZSPR_ENEMY2; } /* SLZ Red Orbinaut */
        if (id == 0x5D) { *out_pal = 5; return ZSPR_ENEMY3; } /* Fan */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 3; return ZSPR_ENEMY5;                     /* Crabmeat */
    }
    if (v == 4) { /* SYZ */
        if (id == 0x43) { *out_pal = 4; return ZSPR_ENEMY1; } /* Roller */
        if (id == 0x1F) { *out_pal = 3; return ZSPR_ENEMY2; } /* Crabmeat */
        if (id == 0x50) { *out_pal = 3; return ZSPR_ENEMY3; } /* Yadrin */
        if (id == 0x22) { *out_pal = 4; return ZSPR_ENEMY4; } /* Buzz Bomber */
        *out_pal = 7; return ZSPR_ENEMY5;
    }
    /* SBZ1 / SBZ2 / FZ */
    if (id == 0x1E) { *out_pal = 2; return ZSPR_ENEMY1; }     /* Ball Hog (Yellow/Gold) */
    if (id == 0x78) { *out_pal = 3; return ZSPR_ENEMY2; }     /* Caterkiller */
    if (id == 0x5F) { *out_pal = 5; return ZSPR_ENEMY3; }     /* Bomb */
    if (id == 0x60) { *out_pal = 5; return ZSPR_ENEMY4; }     /* Orbinaut */
    *out_pal = 4; return ZSPR_ENEMY5;                         /* Burrobot */
}

static void update_single_badnik(ActiveObj *o, uint8_t id, uint8_t sonic_spinning) {
    uint8_t pal = 3;
    uint8_t spr = badnik_sprite_and_pal(id, &pal);
    uint8_t spike_top = (id == 0x50 || id == 0x5F || id == 0x60);
    uint8_t anim_off = ((g_frame_counter >> 3) & 1) ? 4 : 0;
    int16_t sx, sy;

    if (id == 0x2B) {
        /* Chopper: Leap vertically out of GHZ river under bridges! */
        o->y += (o->vy >> 8);
        o->vy += 22;
        if (o->y >= o->base_y + 24) {
            o->y = o->base_y + 24;
            o->vy = -0x0680;
        }
    } else if (id == 0x22) {
        /* Buzz Bomber: Sweep -> Pause & Fire Diagonal Energy Bolt! */
        uint8_t cyc = (uint8_t)((g_frame_counter + o->spawn_slot * 19) & 127);
        if (cyc < 80) {
            o->x += (o->vx >> 8);
            if (o->x < o->base_x - 56 || o->x > o->base_x + 56) o->vx = -o->vx;
        } else if (cyc == 96) {
            int16_t dir_x = (g_sonic.x < o->x) ? -0x200 : 0x200;
            spawn_enemy_projectile(o->x, o->y + 8, dir_x, 0x200, PROJ_ENERGY);
            hw_play_sfx(SFX_FIREBALL);
        }
    } else if (id == 0x1F) {
        /* Crabmeat: Scuttle -> Halt & Fire Twin Parabolic Energy Shots! */
        uint8_t cyc = (uint8_t)((g_frame_counter + o->spawn_slot * 23) & 127);
        if (cyc < 84) {
            o->x += (o->vx >> 8);
            if (o->x < o->base_x - 36 || o->x > o->base_x + 36) o->vx = -o->vx;
        } else if (cyc == 100) {
            spawn_enemy_projectile(o->x - 10, o->y - 4, -0x180, -0x280, PROJ_ENERGY);
            spawn_enemy_projectile(o->x + 10, o->y - 4,  0x180, -0x280, PROJ_ENERGY);
        }
    } else if (id == 0x42) {
        /* Newtron: Materialize when Sonic approaches -> Fire Horizontal Shot! */
        int16_t dx = g_sonic.x - o->x;
        if (dx > -128 && dx < 128) {
            if (o->state == 0) {
                o->state = 1;
                o->timer = 24;
            } else if (o->state == 1 && --o->timer == 0) {
                o->state = 2;
                spawn_enemy_projectile(o->x, o->y, (dx < 0) ? -0x280 : 0x280, 0, PROJ_ENERGY);
            }
        }
    } else if (id == 0x2D) {
        /* Burrobot: Ambush leap straight out of ground when Sonic nears! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0 && dx > -72 && dx < 72) {
            o->state = 1;
            o->vy = -0x0580;
        } else if (o->state == 1) {
            uint8_t ang = 0;
            int16_t d;
            o->vy += 28;
            o->y += (o->vy >> 8);
            d = hw_floor_distance(o->x, o->y + 14, &ang);
            if (o->vy > 0 && d <= 0) {
                o->y += d;
                o->vy = 0;
                o->state = 2;
            }
        } else if (o->state == 2) {
            o->x += (g_sonic.x < o->x) ? -1 : 1;
        }
    } else if (id == 0x1E) {
        /* Ball Hog: Launch bouncing black time-bombs every 96 frames! */
        if (((g_frame_counter + o->spawn_slot * 13) & 95) == 0) {
            spawn_enemy_projectile(o->x, o->y + 6, (o->subtype & 0x10) ? 0x140 : -0x140, -0x100, PROJ_BOMB);
        }
    } else if (id == 0x5F) {
        /* Bomb: Proximity Fuse Detonates into 4 Diagonal Shrapnel Bolts! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0 && dx > -64 && dx < 64) {
            o->state = 1;
            o->timer = 45;
        } else if (o->state == 1) {
            if (--o->timer == 0) {
                spawn_enemy_projectile(o->x, o->y, -0x1E0, -0x280, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y,  0x1E0, -0x280, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y, -0x140, -0x380, PROJ_ENERGY);
                spawn_enemy_projectile(o->x, o->y,  0x140, -0x380, PROJ_ENERGY);
                hw_play_sfx(SFX_BREAK);
                o->active = 0;
                return;
            }
        }
    } else if (id == 0x55) {
        /* `55 Badnik - Basaran.asm`: Hangs motionless upside-down on ceiling until Sonic approaches, then swoops! */
        int16_t dx = g_sonic.x - o->x;
        if (o->state == 0) {
            anim_off = 0;
            if (dx > -96 && dx < 96) {
                o->state = 1;
                o->vx = (dx < 0) ? -0x1C0 : 0x1C0;
                o->vy = 0x200;
            }
        } else {
            o->x += (o->vx >> 8);
            if (o->y < g_sonic.y - 6) o->y += 2;
            else o->y += (hw_sin((uint8_t)(g_frame_counter << 3)) >> 7);
        }
    } else if (id == 0x4F) {
        /* `4F Unused - Splats.asm`: Cut pogo-hopping rabbit Badnik bouncing across terrain! */
        uint8_t ang = 0;
        int16_t d;
        o->vy += 28;
        o->x += (o->vx >> 8);
        o->y += (o->vy >> 8);
        d = hw_floor_distance(o->x, o->y + 14, &ang);
        if (o->vy > 0 && d <= 0) {
            o->y += d;
            o->vy = -0x420;
        }
        if (o->x < o->base_x - 64 || o->x > o->base_x + 64) o->vx = -o->vx;
    } else if (id == 0x60) {
        /* Orbinaut: 4 orbiting spikeballs composited directly in frames 0 & 1 (`ZSPR_ENEMY`)! */
        o->x += (o->vx >> 8);
        if (o->x < o->base_x - 48 || o->x > o->base_x + 48) o->vx = -o->vx;
    } else {
        int16_t step = (id == 0x43) ? (o->vx < 0 ? -3 : 3) : (o->vx < 0 ? -1 : 1);
        o->x += step;
        if (o->x < o->base_x - 56 || o->x > o->base_x + 56) {
            o->vx = -o->vx;
        }
        if ((id == 0x40 || id == 0x50 || id == 0x78) && ((o->x & 7) == 0)) {
            uint8_t ang = 0;
            int16_t d = hw_floor_distance(o->x, o->y + 14, &ang);
            if (d >= -12 && d <= 12) o->y += d;
            else o->vx = -o->vx;
        }
    }

    sx = (o->x - g_cam_x) >> 1;
    sy = (o->y - g_cam_y) >> 1;

    if (!g_sonic.dead && box_overlap(g_sonic.x, g_sonic.y, 11, 16, o->x, o->y, 13, 13)) {
        uint8_t protected_spike = (id == 0x5F) ||
                                  (spike_top && g_sonic.invinc == 0 && !g_sonic.on_ground && g_sonic.y < o->y - 6);
        if (sonic_spinning && !protected_spike) {
            if (!g_sonic.on_ground && g_sonic.ysp > 0) {
                g_sonic.ysp = -g_sonic.ysp;
            } else if (g_sonic.ysp < 0) {
                g_sonic.ysp += 0x100;
            }
            spawn_score_popup(o->x, o->y - 8, g_sonic.air_combo);
            spawn_enemy_projectile(o->x, o->y - 4, (o->spawn_slot & 1) ? 0x160 : -0x160, -0x380, PROJ_ANIMAL_HOP);
            if (g_sonic.air_combo < 3) g_sonic.air_combo++;
            hw_play_sfx(SFX_BREAK);
            g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
            o->state = 99;
            o->timer = 24;
            return;
        } else {
            sonic_hurt_or_kill(0);
        }
    }

    hw_put_sprite16(sx, sy, spr + anim_off, 0x08 | pal | (o->vx > 0 ? 0x20 : 0));
}

/* Stage ring (`0x25`, not exploding): same checks, in the same order, as the generic `step_one_active_obj`
   path, but without its large prologue/stack frame. Rings are the majority of active objects. */
static void step_ring_obj(ActiveObj *o) {
    int16_t dx = o->x - g_cam_x;
    int16_t sx, sy;
    if ((uint16_t)(dx + 160) > 640U) { o->active = 0; return; }
    sx = dx >> 1;
    sy = (o->y - g_cam_y) >> 1;
    if ((uint16_t)(sx + 20) > 200U || (uint16_t)(sy + 24) > 176U) return;
    if (!g_sonic.dead) {
        int16_t rdx = g_sonic.x - o->x;
        if ((uint16_t)(rdx + 17) < 35U) {
            int16_t rdy = g_sonic.y - o->y;
            if ((uint16_t)(rdy + 23) < 47U) {
                g_rings++;
                g_score += 10;
                g_hud_dirty = 1;
                hw_play_sfx(SFX_RING);
                g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                o->active = 0;
                return;
            }
        }
    }
    hw_queue_bkg_ring(o->x, o->y);
}

/* First active slot at or after `o` (or &g_objs[MAX_ACTIVE_OBJS]). Inactive slots are skipped in a tight loop.
   sdcccall(1): o in DE, result in BC. */
typedef char active_obj_size_check[(sizeof(ActiveObj) == 20U && MAX_ACTIVE_OBJS == 24 && MAX_LOST_RINGS == 8) ? 1 : -1];
static ActiveObj *obj_next_active(ActiveObj *o) __naked {
    (void)o;
    __asm
    ld   h, d
    ld   l, e
    ld   de, #20
701$:
    ld   a, l
    cp   a, #<(_g_objs + 480)
    jr   nz, 702$
    ld   a, h
    cp   a, #>(_g_objs + 480)
    jr   z, 703$
702$:
    ld   a, (hl)
    or   a, a
    jr   nz, 703$
    add  hl, de
    jr   701$
703$:
    ld   b, h
    ld   c, l
    ret
    __endasm;
}

/* Lean pre-pass for generic objects: the range-deactivation and off-viewport skip that used to open
   `step_one_active_obj` (identical conditions, in the same order). Returns 1 when the object should run, with its
   half-scale screen position in s_obj_sx / s_obj_sy. Keeping this out of the big function avoids paying its
   38-byte stack frame for objects that are skipped. */
static int16_t s_obj_sx, s_obj_sy;
static uint8_t s_pf_id;
/* Hand-written equivalent of:
 *   dx = o->x - g_cam_x;
 *   if ((uint16_t)(dx + 160) > 640 && id not in {3D,3E,0D,7C}) { o->active = 0; return 0; }
 *   s_obj_sx = dx >> 1; s_obj_sy = (o->y - g_cam_y) >> 1;
 *   if (((uint16_t)(sx + 20) > 200 || (uint16_t)(sy + 24) > 176) && id not in {3D,7A,71,85,0D,3E,4B,7D}) return 0;
 *   return 1;
 * ActiveObj layout: active +0, id +1, x +5, y +7. sdcccall(1): o in DE, result in A. */
typedef char active_obj_layout_check[(offsetof(ActiveObj, id) == 1U && offsetof(ActiveObj, x) == 5U &&
                                      offsetof(ActiveObj, y) == 7U) ? 1 : -1];
static uint8_t obj_prefilter(ActiveObj *o) __naked {
    (void)o;
    __asm
    ld   h, d
    ld   l, e
    inc  hl
    ld   a, (hl)
    ld   (_s_pf_id), a
    push de
    ld   bc, #4
    add  hl, bc
    ld   a, (_g_cam_x)
    ld   c, a
    ld   a, (_g_cam_x + 1)
    ld   b, a
    ld   a, (hl+)
    sub  a, c
    ld   e, a
    ld   a, (hl+)
    sbc  a, b
    ld   d, a
    ld   a, (_g_cam_y)
    ld   c, a
    ld   a, (_g_cam_y + 1)
    ld   b, a
    ld   a, (hl+)
    sub  a, c
    ld   c, a
    ld   a, (hl)
    sbc  a, b
    ld   b, a
    ld   hl, #160
    add  hl, de
    ld   a, h
    cp   a, #2
    jr   c, 720$
    jr   nz, 721$
    ld   a, l
    cp   a, #0x81
    jr   c, 720$
721$:
    ld   a, (_s_pf_id)
    cp   a, #0x3D
    jr   z, 720$
    cp   a, #0x3E
    jr   z, 720$
    cp   a, #0x0D
    jr   z, 720$
    cp   a, #0x7C
    jr   z, 720$
    pop  hl
    xor  a, a
    ld   (hl), a
    ret
720$:
    pop  hl
    sra  d
    rr   e
    sra  b
    rr   c
    ld   hl, #_s_obj_sx
    ld   (hl), e
    inc  hl
    ld   (hl), d
    ld   hl, #_s_obj_sy
    ld   (hl), c
    inc  hl
    ld   (hl), b
    ld   hl, #20
    add  hl, de
    ld   a, h
    or   a, a
    jr   nz, 730$
    ld   a, l
    cp   a, #201
    jr   nc, 730$
    ld   hl, #24
    add  hl, bc
    ld   a, h
    or   a, a
    jr   nz, 730$
    ld   a, l
    cp   a, #177
    jr   nc, 730$
    ld   a, #1
    ret
730$:
    ld   a, (_s_pf_id)
    cp   a, #0x3D
    jr   z, 731$
    cp   a, #0x7A
    jr   z, 731$
    cp   a, #0x71
    jr   z, 731$
    cp   a, #0x85
    jr   z, 731$
    cp   a, #0x0D
    jr   z, 731$
    cp   a, #0x3E
    jr   z, 731$
    cp   a, #0x4B
    jr   z, 731$
    cp   a, #0x7D
    jr   z, 731$
    cp   a, #0x11
    jr   z, 731$
    cp   a, #0x18
    jr   z, 731$
    cp   a, #0x1A
    jr   z, 731$
    xor  a, a
    ret
731$:
    ld   a, #1
    ret
    __endasm;
}

static uint8_t step_one_active_obj(ActiveObj *o, uint8_t sonic_spinning) {
    int16_t cam_x = g_cam_x;
    int16_t cam_y = g_cam_y;
    int16_t sx = s_obj_sx, sy = s_obj_sy;
    uint8_t id = o->id;

    /* Explosion / Animal Release State (state == 99) */
    if (o->state == 99) {
        if (o->timer > 12) {
            uint8_t ex_frame = (o->timer > 18) ? SPR_TILE_EXPLODE : (SPR_TILE_EXPLODE + 4);
            hw_put_sprite16(sx, sy, ex_frame, (o->timer > 18) ? 1 : 2);
        } else {
            o->y -= 3;
            o->x += (o->vx >= 0) ? 2 : -2;
            hw_put_sprite8(((o->x - cam_x) >> 1) - 4, ((o->y - cam_y) >> 1) - 4,
                           SPR_TILE_ANIMAL + ((g_frame_counter >> 2) & 3), 4 | (o->vx >= 0 ? 0x20 : 0));
        }
        if (--o->timer == 0) o->active = 0;
        return 0;
    }

    /* --- GOLDEN RING (`0x25` — Zero-call inline hot path for 12..16 active rings/frame!) --- */
    if (id == 0x25) {
        if (!g_sonic.dead) {
            int16_t rdx = g_sonic.x - o->x;
            if (rdx > -18 && rdx < 18) {
                int16_t rdy = g_sonic.y - o->y;
                if (rdy > -24 && rdy < 24) {
                    g_rings++;
                    g_score += 10;
                    g_hud_dirty = 1;
                    hw_play_sfx(SFX_RING);
                    g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                    o->active = 0;
                    return 0;
                }
            }
        }
        /* Render as a composed background-tile overlay; collision/collection above is unchanged. */
        hw_queue_bkg_ring(o->x, o->y);
        return 0;
    }

        /* --- ITEM MONITOR (`0x26` — `26 Monitor.asm` with true icon per subtype!) --- */
        if (id == 0x26) {
            uint8_t sub = o->subtype & 0x0F;
            uint8_t mon_tile, mon_pal;
            if (sonic_spinning && box_overlap(g_sonic.x, g_sonic.y, 12, 18, o->x, o->y, 14, 14)) {
                if (g_sonic.ysp > 0) g_sonic.ysp = -g_sonic.ysp;
                if (sub == 0x01) {
                    sonic_hurt_or_kill(0);
                } else if (sub == 0x02) {
                    g_lives++;
                    g_hud_dirty = 1;
                    hw_play_bgm(BGM_EXTRALIFE);
                } else if (sub == 0x03) {
                    g_sonic.shoes = 1200;
                    hw_play_sfx(SFX_SPRING);
                } else if (sub == 0x04) {
                    g_sonic.shield = 1;
                    hw_play_sfx(SFX_SHIELD);
                } else if (sub == 0x05) {
                    __critical {
                        g_invinc_vblanks_pending = 0;
                        g_sonic.invinc = 1200;
                    }
                    hw_play_bgm(BGM_INVINC);
                } else if (sub == 0x07) {
                    g_rings += 50;
                    g_sonic.shield = 1;
                    g_sonic.shoes = 1200;
                    __critical {
                        g_invinc_vblanks_pending = 0;
                        g_sonic.invinc = 1200;
                    }
                    g_hud_dirty = 1;
                    hw_play_bgm(BGM_INVINC);
                } else {
                    g_rings += 10;
                    g_score += 100;
                    g_hud_dirty = 1;
                    hw_play_sfx(SFX_RING);
                }
                hw_play_sfx(SFX_BREAK);
                g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                o->state = 99;
                o->timer = 22;
        return 0;
            }
            if (!sonic_spinning && g_sonic.ysp >= 0 &&
                box_overlap(g_sonic.x, g_sonic.y + 16, 10, 6, o->x, o->y - 12, 14, 6)) {
                g_sonic.y = o->y - 30;
                g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                g_sonic.ysp = 0;
                g_sonic.on_ground = 1;
            }
            if (sub == 0x03)      { mon_tile = SPR_TILE_MONITOR + 4;  mon_pal = 1; } /* Speed Shoes (Red) */
            else if (sub == 0x04) { mon_tile = SPR_TILE_MONITOR + 8;  mon_pal = 4; } /* Shield (Cyan-Blue) */
            else if (sub == 0x05 || sub == 0x07) { mon_tile = SPR_TILE_MONITOR + 12; mon_pal = 2; } /* Invincibility (Gold) */
            else if (sub == 0x01 || sub == 0x02) { mon_tile = SPR_TILE_MONITOR + 16; mon_pal = (sub == 0x01) ? 1 : 0; } /* 1UP Sonic / Eggman */
            else                  { mon_tile = SPR_TILE_MONITOR;      mon_pal = 2; } /* Super 10-Ring (Gold) */
            hw_put_sprite16(sx, sy, mon_tile, mon_pal);
        return 0;
        }

        /* --- BOUNCE SPRING (`0x41` — Exact `s1disasm/_incObj/41 Springs.asm` Up, LR, & Down in Red/Yellow!) --- */
        if (id == 0x41) {
            uint8_t raw_sub = o->subtype & 0x3F;              /* Mask out `objpos` HV-flip bits (`0x40`/`0x80`)! */
            uint8_t xflip = (o->subtype & 0x40) ? 1 : 0;      /* `status_xflip_bit`: 1 = faces Left, 0 = faces Right */
            uint8_t horiz = (raw_sub & 0x10) ? 1 : 0;         /* `btst #4, d0` -> `Spring_LR` */
            uint8_t upside_down = (!horiz && ((raw_sub & 0x20) || (o->subtype & 0x80))) ? 1 : 0; /* `Spring_Down` */
            uint8_t sp_pal = (raw_sub & 0x02) ? 2 : 1;        /* `btst #1, d0`: Yellow (`OBP2`) vs Red (`OBP1`) */
            int16_t power = (raw_sub & 0x02) ? 0x0A00 : 0x1000; /* Exact `Spring_Powers`: Red `-$1000`, Yellow `-$0A00` */
            if (o->timer > 0) o->timer--;

            if (!g_sonic.dead) {
                if (horiz) {
                    /* `Spring_LR` (`41 Springs.asm:129-165`): `d1 = 19, d2 = 14`, launch `-power` if `xflip` else `+power`,
                     * push clear of wall, copy to `gsp`, `locktime = 15`, flip facing, `id_Walk` if unrolled! */
                    if (o->timer == 0 && box_overlap(g_sonic.x, g_sonic.y, 10, 14, o->x, o->y, 10, 14)) {
                        if (xflip) {
                            g_sonic.xsp = -power;
                            g_sonic.x = o->x - 16;
                            g_sonic.facing_left = 1;
                        } else {
                            g_sonic.xsp = power;
                            g_sonic.x = o->x + 16;
                            g_sonic.facing_left = 0;
                        }
                        g_sonic.x_sub = (int32_t)g_sonic.x << 8;
                        g_sonic.gsp = g_sonic.xsp;
                        g_sonic.lock_ctrl = 15;
                        if (!g_sonic.rolling) g_sonic.anim = ANI_WALK;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                } else if (upside_down) {
                    /* `Spring_Down` (`41 Springs.asm:179-215`): ceiling spring launches downward at `+power`! */
                    if (o->timer == 0 && g_sonic.y >= o->y - 4 &&
                        box_overlap(g_sonic.x, g_sonic.y, 11, 19, o->x, o->y, 16, 10)) {
                        g_sonic.y = o->y + 16;
                        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                        g_sonic.ysp = power;
                        g_sonic.on_ground = 0;
                        g_sonic.jumping = 0;
                        g_sonic.rolling = 0;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                } else {
                    /* `Spring_Up` (`41 Springs.asm:88-115`): `d1 = 27, d2 = 8`; launch upward at `-power` (`jumping = 0`!) */
                    if (g_sonic.ysp >= -0x100 && g_sonic.y <= o->y + 10 &&
                        box_overlap(g_sonic.x, g_sonic.y, 11, 19, o->x, o->y, 16, 10)) {
                        g_sonic.y = o->y - 8;
                        g_sonic.y_sub = (int32_t)g_sonic.y << 8;
                        g_sonic.ysp = -power;
                        g_sonic.on_ground = 0;
                        g_sonic.jumping = 0;
                        g_sonic.rolling = 0;
                        g_sonic.angle = 0;
                        g_sonic.anim = ANI_SPRING;
                        o->timer = 8;
                        hw_play_sfx(SFX_SPRING);
                    }
                }
            }
            if (horiz) {
                hw_put_sprite16(sx + (o->timer ? (xflip ? -2 : 2) : 0), sy,
                                SPR_TILE_SPRING + 4, sp_pal | (xflip ? 0x20 : 0));
            } else {
                hw_put_sprite16(sx, sy + (o->timer ? (upside_down ? 2 : -2) : 0),
                                SPR_TILE_SPRING, sp_pal | (upside_down ? 0x40 : 0));
            }
        return 0;
        }

        /* --- SPIKES (`0x36` — `36 Spikes.asm` including moving retractable & ceiling spikes!) --- */
        if (id == 0x36) {
            int16_t off_y = ((o->subtype & 0x0F) == 1 && ((g_frame_counter & 63) >= 32)) ? 16 : 0;
            if (!g_sonic.dead && box_overlap(g_sonic.x, g_sonic.y, 10, 16, o->x, o->y + off_y, 14, 14)) {
                sonic_hurt_or_kill(0);
            }
            hw_put_sprite16(sx, sy + (off_y >> 1), SPR_TILE_SPIKES, 5);
        return 0;
        }

        /* --- LAMPPOST / CHECKPOINT (`0x79`) --- */
        if (id == 0x79) {
            if (!o->state && box_overlap(g_sonic.x, g_sonic.y, 12, 24, o->x, o->y, 10, 24)) {
                o->state = 1;
                g_checkpoint_active = 1;
                g_checkpoint_act = g_cur_act;
                g_checkpoint_x = o->x;
                g_checkpoint_y = o->y;
                hw_play_sfx(SFX_LAMPPOST);
            }
            hw_put_sprite16(sx, sy, SPR_TILE_LAMP, o->state ? 1 : 4);
        return 0;
        }

        /* --- END-OF-ACT SIGNPOST (`0x0D`) & HIDDEN BONUS POINTS (`0x7D`) --- */
        if (id == 0x7D) {
            if (g_signpost_passed && !o->state && box_overlap(g_sonic.x, g_sonic.y, 16, 20, o->x, o->y, 16, 16)) {
                o->state = 1;
                spawn_score_popup(o->x, o->y, 3); /* 1,000 Hidden Bonus! */
                hw_play_sfx(SFX_CASH);
                o->active = 0;
            }
        return 0;
        }
        if (id == 0x0D) {
            uint8_t sp_frame;
            /* Lock camera horizontally onto end-of-act Signpost clearing just like `0D Signpost.asm`! */
            if (o->x >= g_dyn_right_bound && g_cam_x >= g_dyn_right_bound - 16) {
                g_dyn_left_bound = g_dyn_right_bound;
            }
            if (!o->state && g_sonic.x >= o->x) {
                o->state = 1;
                o->timer = 104;
                g_signpost_passed = 1;
                hw_play_sfx(SFX_SIGNPOST);
            }
            if (o->state == 1) {
                static const uint8_t s_spin_seq[8] = { 0, 1, 2, 3, 4, 1, 2, 3 };
                sp_frame = s_spin_seq[(o->timer >> 1) & 7];
                /* Twinkling golden ring glints popping around the spinning sign (`0D Signpost.asm`) */
                {
                    int8_t gx = (int8_t)(((o->timer * 7) & 15) - 8);
                    int8_t gy = (int8_t)(((o->timer * 5) & 11) - 6);
                    hw_put_sprite8(sx + gx - 4, sy + gy - 4, SPR_TILE_STARS + ((o->timer >> 1) & 3), 2);
                }
                if (--o->timer == 0) {
                    o->state = 2;
                    o->timer = 30;
                    sp_frame = 4;
                    hw_play_sfx(SFX_RING);
                }
            } else if (o->state == 2) {
                sp_frame = 4; /* Hold Sonic's triumphant smiling face clearly on the Goal Post! */
                if (o->timer > 0) {
                    if (--o->timer == 0) {
                        g_act_clear_done = 1;
                    }
                }
            } else {
                sp_frame = 0; /* Dr. Eggman 24x16 oval sign before Sonic passes! */
            }

            /* Render full half-scale `24x24` Goal Post (`24x16` sign plate + `8x8` steel pole at `sy + 8`) */
            if (sp_frame == 0) {
                /* Eggman 24x16 (`OBP1` crimson/white/black; right cap = H-flip of left cap) */
                hw_put_sprite8(sx - 12, sy - 8, SPR_TILE_SIGNPOST + 0, 1);
                hw_put_sprite8(sx - 12, sy,     SPR_TILE_SIGNPOST + 1, 1);
                hw_put_sprite8(sx - 4,  sy - 8, SPR_TILE_SIGNPOST + 2, 1);
                hw_put_sprite8(sx - 4,  sy,     SPR_TILE_SIGNPOST + 3, 1);
                hw_put_sprite8(sx + 4,  sy - 8, SPR_TILE_SIGNPOST + 0, 1 | 0x20);
                hw_put_sprite8(sx + 4,  sy,     SPR_TILE_SIGNPOST + 1, 1 | 0x20);
            } else if (sp_frame == 1) {
                /* Spin1 16x16 (`45 deg`) */
                hw_put_sprite8(sx - 8, sy - 8, SPR_TILE_SIGNPOST + 4, 1);
                hw_put_sprite8(sx - 8, sy,     SPR_TILE_SIGNPOST + 5, 1);
                hw_put_sprite8(sx,     sy - 8, SPR_TILE_SIGNPOST + 6, 1);
                hw_put_sprite8(sx,     sy,     SPR_TILE_SIGNPOST + 7, 1);
            } else if (sp_frame == 2) {
                /* Spin2 8x16 (`90 deg` edge-on rim) */
                hw_put_sprite8(sx - 4, sy - 8, SPR_TILE_SIGNPOST + 8, 2);
                hw_put_sprite8(sx - 4, sy,     SPR_TILE_SIGNPOST + 9, 2);
            } else if (sp_frame == 3) {
                /* Spin3 16x16 (`135 deg` H-flip of Spin1) */
                hw_put_sprite8(sx - 8, sy - 8, SPR_TILE_SIGNPOST + 6, 0 | 0x20);
                hw_put_sprite8(sx - 8, sy,     SPR_TILE_SIGNPOST + 7, 0 | 0x20);
                hw_put_sprite8(sx,     sy - 8, SPR_TILE_SIGNPOST + 4, 0 | 0x20);
                hw_put_sprite8(sx,     sy,     SPR_TILE_SIGNPOST + 5, 0 | 0x20);
            } else {
                /* Sonic 24x16 (`OBP0` royal Sonic blue + white eyes + quills) */
                hw_put_sprite8(sx - 12, sy - 8, SPR_TILE_SIGNPOST + 10, 0);
                hw_put_sprite8(sx - 12, sy,     SPR_TILE_SIGNPOST + 11, 0);
                hw_put_sprite8(sx - 4,  sy - 8, SPR_TILE_SIGNPOST + 12, 0);
                hw_put_sprite8(sx - 4,  sy,     SPR_TILE_SIGNPOST + 13, 0);
                hw_put_sprite8(sx + 4,  sy - 8, SPR_TILE_SIGNPOST + 14, 0);
                hw_put_sprite8(sx + 4,  sy,     SPR_TILE_SIGNPOST + 15, 0);
            }
            /* Vertical steel Goal Post pole (`$38..$39`) anchored right onto the ground at `sy + 8..16`! */
            hw_put_sprite8(sx - 4, sy + 8, SPR_TILE_SIGNPOST + 16, 3);
        return 0;
        }

        /* --- GIANT SPECIAL STAGE RING (`0x4B`) & STARBURST FLASH (`0x7C` — `7C Giant Ring Flash.asm`) --- */
        if (id == 0x7C) {
            uint8_t ang = (uint8_t)(o->timer << 4);
            hw_put_sprite8(sx - ((hw_cos(ang) * 4) >> 8) - 4, sy - 4, SPR_TILE_STARS + (o->timer & 3), 2);
            hw_put_sprite8(sx + ((hw_cos(ang) * 4) >> 8) - 4, sy - 4, SPR_TILE_STARS + ((o->timer + 2) & 3), 1);
            if (--o->timer == 0) {
                o->active = 0;
                g_next_mode = GM_SPECIAL_STAGE;
        return 1;
            }
        return 0;
        }
        if (id == 0x4B) {
            if (g_rings >= 50) {
                if (box_overlap(g_sonic.x, g_sonic.y, 12, 18, o->x, o->y, 16, 16)) {
                    g_obj_taken[o->spawn_slot >> 3] |= (uint8_t)(1 << (o->spawn_slot & 7));
                    add_score_points(1000);
                    hw_play_sfx(SFX_GIANTRING);
                    g_signpost_passed = 1;
                    g_act_clear_done = 1;
                    o->id = 0x7C;
                    o->timer = 14;
        return 0;
                }
                hw_put_sprite16(sx, sy, ZSPR_GIANTRING + (((g_frame_counter >> 2) & 3) << 2), 0x08 | 2);
            }
        return 0;
        }

        /* --- EGG PRISON CAPSULE (`0x3E` — Golden 24x16 Capsule + Separate Red Release Button `122`!) --- */
        if (id == 0x3E) {
            if (!o->state && g_boss_defeated &&
                box_overlap(g_sonic.x, g_sonic.y, 16, 24, o->x, o->y, 24, 28)) {
                o->state = 1;
                o->timer = 90;
                if (g_sonic.ysp > 0) g_sonic.ysp = -0x380;
                hw_play_sfx(SFX_BREAK);
            }
            if (o->state == 1) {
                if ((o->timer & 7) == 0) {
                    spawn_enemy_projectile(o->x, o->y - 8, (o->timer & 8) ? 0x1C0 : -0x1C0, -0x380, PROJ_ANIMAL_HOP);
                }
                if (--o->timer == 0) {
                    g_act_clear_done = 1;
                    g_next_mode = GM_ACT_CLEAR;
        return 1;
                }
            }
            if (o->subtype != 0) {
                hw_put_sprite8(sx - 8, sy - 4 + (o->state ? 2 : 0), ZSPR_CAPSULE + 6, 0x08 | 1);
                hw_put_sprite8(sx,     sy - 4 + (o->state ? 2 : 0), ZSPR_CAPSULE + 7, 0x08 | 1);
            } else {
                hw_put_sprite8(sx - 8, sy - 14 + (o->state ? 3 : 0), ZSPR_CAPSULE + 6, 0x08 | 1);
                hw_put_sprite8(sx,     sy - 14 + (o->state ? 3 : 0), ZSPR_CAPSULE + 7, 0x08 | 1);
                if (o->state) {
                    hw_put_sprite16(sx, sy, ZSPR_CAPSULE + 8, 0x08 | 2);
                } else {
                    hw_put_sprite_cols2rows(sx - 12, sy - 8, ZSPR_CAPSULE, 3u, 0x08 | 2);
                }
            }
        return 0;
        }

        /* --- GREEN HILL INTERACTIVE PROPS (bridge, platforms, collapsing ledge, rock, smash walls, bridge stumps) --- */
        if (g_ram_act.vset_idx == 0 &&
            (id == 0x11 || id == 0x18 || id == 0x1A || id == 0x1C || id == 0x3B || id == 0x3C)) {
            ghz_object(o, sx, sy);
            return 0;
        }

        /* --- FAST INLINE SCENERY PROP (`0x1C` Bridge Stump / Decoration) — ZERO cross-bank call! --- */
        if (id == 0x1C) {
            hw_put_sprite8(sx - 4, sy - 4, ZSPR_PROP, 0x08 | 6);
        return 0;
        }

        /* --- ALL 14 DISTINCT SONIC 1 BADNIK AIs (`s1disasm/_incObj/`) IN SAME BANK (`Bank 2`) FIRST! --- */
        if (is_badnik_id(id)) {
            update_single_badnik(o, id, sonic_spinning);
        return 0;
        }

        /* --- DELEGATE BOSSES & INTERACTIVE ZONE GIMMICKS TO BANK 126 --- */
        (void)gimmick_handle_object(o, sx, sy, sonic_spinning);
    return 0;
}

void objects_update_and_draw(void) BANKED {
    uint8_t i;
    uint8_t sonic_spinning = (g_sonic.rolling || g_sonic.anim == ANI_ROLL || g_sonic.invinc > 0);
    int16_t sonic_x = g_sonic.x;
    int16_t sonic_y = g_sonic.y;
    int16_t cam_x = g_cam_x;
    int16_t cam_y = g_cam_y;

    /* 1. Draw Sonic Sprite + Shield/Invincibility Halo (`hw_draw_sonic_sprite` in Bank 0) */
    hw_draw_sonic_sprite();

    /* 1b. Guarantee CGB 10-sprites/scanline hardware priority for end-of-act Goal Post (`0x0D`) */
    if (cam_x >= g_dyn_right_bound - 320 && g_objs[0].id != 0x0D) {
        for (i = 1; i < MAX_ACTIVE_OBJS; i++) {
            if (g_objs[i].active && g_objs[i].id == 0x0D) {
                ActiveObj tmp = g_objs[0];
                g_objs[0] = g_objs[i];
                g_objs[i] = tmp;
                break;
            }
        }
    }

    /* 2. Check & Spawn Act 3 / Final Zone Boss (`GHZ3`, `MZ3`, `SYZ3`, `LZ3`, `SLZ3`, `FZ`) */
    if (g_boss_active || g_cur_act == 2 || g_cur_act == 5 || g_cur_act == 8 ||
        g_cur_act == 11 || g_cur_act == 14 || g_cur_act == 18) {
        boss_check_and_update(sonic_spinning);
    }

    /* 3. Update & Draw Scattered Lost Rings (`37 Rings.asm`) — pointer walk (`0` software multiplies!) */
    if (g_lost_rings[0].active | g_lost_rings[1].active | g_lost_rings[2].active | g_lost_rings[3].active |
        g_lost_rings[4].active | g_lost_rings[5].active | g_lost_rings[6].active | g_lost_rings[7].active) {
        LostRing *r = g_lost_rings;
        for (i = 0; i < MAX_LOST_RINGS; i++, r++) {
            int16_t sx, sy;
            if (!r->active) continue;
            if (r->timer == 0) {
                r->active = 0;
                continue;
            }
            r->timer--;
            r->vy += 24;
            r->x += (r->vx >> 8);
            r->y += (r->vy >> 8);
            if (r->vy > 0) {
                uint8_t ang = 0;
                int16_t d = hw_floor_distance(r->x, r->y + 8, &ang);
                if (d <= 0) {
                    r->y += d;
                    r->vy = -(r->vy - (r->vy >> 2));
                }
            }
            if (!g_sonic.dead && r->timer < 156 && box_overlap(sonic_x, sonic_y, 10, 16, r->x, r->y, 8, 8)) {
                r->active = 0;
                g_rings++;
                g_hud_dirty = 1;
                hw_play_sfx(SFX_RING);
                continue;
            }
            sx = (r->x - cam_x) >> 1;
            sy = (r->y - cam_y) >> 1;
            if (r->timer > 40 || (g_frame_counter & 1)) {
                hw_put_sprite8(sx - 2, sy - 2, SPR_TILE_RING + ((g_frame_counter >> 2) & 3), 2);
            }
        }
    }

    /* 4. Update & Draw Active Level Objects — pointer walk (`0` software `__mulint` per frame!) */
    hw_begin_bkg_rings();
    {
        ActiveObj *o = obj_next_active(g_objs);
        while (o != &g_objs[MAX_ACTIVE_OBJS]) {
            if (o->id == 0x25 && o->state != 99) step_ring_obj(o);
            else if (obj_prefilter(o) && step_one_active_obj(o, sonic_spinning)) return;
            o = obj_next_active(o + 1);
        }
    }

    /* 5. Update & Draw Active Projectiles, Brick Debris, Score Popups & Drowning Numbers (skip cross-bank call when all empty!) */
    if (g_sonic.underwater ||
        g_projs[0].active || g_projs[1].active || g_projs[2].active || g_projs[3].active ||
        g_projs[4].active || g_projs[5].active || g_projs[6].active || g_projs[7].active ||
        g_pops[0].active || g_pops[1].active || g_pops[2].active || g_pops[3].active) {
        projectiles_and_popups_update();
    }
}
