#pragma bank 7
/* Animated title screen (GM_Title): GHZ background parallax behind the emblem, big Sonic rising and wagging his
   finger, flashing "PRESS START BUTTON", water palette cycle and the palette fade-in.
   Data comes from tools/title_screen.py (generated/title_*.c). Tile rows without emblem pixels are scrolled by
   the LYC handler (src/title_isr.c); the rows holding the emblem keep SCX = 0 and get, for each scroll position,
   precomputed cell graphics that the VBlank handler GDMAs straight from ROM (queue built here).
   SDCC code is slow, so everything per-frame works on flat precomputed tables. */
#include "engine.h"
#include "title_isr.h"
#include "title_data.h"

typedef struct {
    uint16_t pos[18];                       /* 8.8 scroll position of every tile row */
    uint8_t vram_p[TITLE_N_SW_ROWS];        /* scroll position currently in VRAM (per software row) */
    uint16_t m;                             /* frames since the fade-in finished (Tit_MainLoop iterations) */
    uint8_t stp;                            /* frames left on the "SONIC TEAM PRESENTS" card */
    uint8_t fade;                           /* palette fade-in step (TITLE_FADE_FIRST..16) */
    uint8_t cyc_timer, cyc_idx;
    uint8_t son_state, son_half;            /* image shown by the OAM, and its half of OBJ VRAM bank 1 */
    uint8_t son_want, son_up;               /* image wanted; 1 = queued for upload, 2 = uploaded, show it */
    uint8_t psb_on;
    uint8_t rr;                             /* round-robin start for row uploads */
    uint8_t q_tag[TQ_MAX];                  /* per queued DMA: software row index, SON_TAG, or 0xFF */
    uint8_t q_p[TQ_MAX];
} TitleState;
static __at(0xAE00) TitleState s_ts;
#define TS (&s_ts)

/* "SONIC TEAM PRESENTS" stays up while the title loads behind it (GM_Title shows it during the Nemesis loads);
   the fade-in then starts from a dim step instead of pure black, so no frame is ever fully black. */
#define TITLE_STP_FRAMES 50
#define TITLE_FADE_FIRST 4
static const uint16_t s_card_pal[4] = { RGB8(16, 32, 108), RGB8(228, 28, 24), RGB8(252, 216, 0), RGB8(252, 252, 252) };

#define SON_NONE 0xFFU
#define SON_TAG 0xFEU

/* Big Sonic (object 0E): TSon_Delay hides him for the first 28 main-loop frames, TSon_Move shows him at y=$5E
   and moves him up 8 px/frame until y=$16 (frame 37), then AnimateSprite (Ani_TSon, 8 frames per image) plays
   images 1..5 and loops 6, 7. States 0..9 are the rise positions, 10..16 are images 1..7. */
static uint8_t sonic_state_for(uint16_t m) {
    uint8_t k;
    if (m < 28U) return SON_NONE;
    if (m <= 37U) return (uint8_t)(m - 28U);
    if (m < 45U) return 9U;
    if (m >= 45U + 56U) return (uint8_t)((((m - 45U) >> 3) & 1U) ? 15U : 16U);   /* afBack 2: 6, 7, 6, 7 */
    k = (uint8_t)((m - 45U) >> 3);
    return (uint8_t)(10U + k);
}

static void build_oam(void) {
    uint8_t n = 0;
    uint8_t *o = (uint8_t *)shadow_OAM;
    if (TS->son_state != SON_NONE) {
        n = title_son_n[TS->son_state];
        memcpy(o, title_son_oam[(uint8_t)(TS->son_state << 1) | TS->son_half], (uint16_t)n << 2);
    }
    if (TS->psb_on) {
        memcpy(o + ((uint16_t)n << 2), title_psb_oam, TITLE_N_PSB * 4U);
        n = (uint8_t)(n + TITLE_N_PSB);
    }
    g_oam_next = n;
    hw_clear_remaining_oam();
}

static void pal_request(const uint16_t *src, uint8_t first, uint8_t n) {
    memcpy(TQ_PAL_BUF, src, (uint16_t)n << 3);
    if (n == 8) memcpy(TQ_PAL_BUF + 32, src + 32, 64);
    TQ_PAL_FIRST = first;
    TQ_PAL_N = n;
}

void title_init(void) BANKED {
    uint8_t i, r;

    /* "SONIC TEAM PRESENTS" card in the window while everything loads behind it */
    SCX_REG = 0;
    g_oam_next = 0;
    hw_clear_remaining_oam();
    set_bkg_palette(7, 1, s_card_pal);
    hw_show_transition_card("SONIC TEAM", "PRESENTS", 0);
    set_bkg_palette(0, 7, &title_fade_pals[(uint16_t)TITLE_FADE_FIRST << 6]);
    SCY_REG = 0;
    g_oam_next = 0;
    hw_clear_remaining_oam();

    for (i = 0; i < TITLE_N_TILE_CHUNKS; i++) {
        const TitleTileChunk *c = &title_tile_chunks[i];
        VBK_REG = c->vbank;
        title_banked_bkg_data(c->first, c->count, c->data, c->bank);
    }
    for (r = 0; r < 18; r++) {
        VBK_REG = 1;
        set_bkg_tiles(0, r, 32, 1, &title_attr[(uint16_t)r * 32U]);
        VBK_REG = 0;
        set_bkg_tiles(0, r, 32, 1, &title_map[(uint16_t)r * 32U]);
        TS->pos[r] = 0;
    }
    /* Software rows at scroll position 0 */
    for (i = 0; i < TITLE_N_SW_ROWS; i++) {
        uint16_t t = title_sw_tdst[i];
        uint8_t first = (t >= 0x9000U) ? (uint8_t)((t - 0x9000U) >> 4) : (uint8_t)(128U + ((t - 0x8800U) >> 4));
        VBK_REG = title_sw_vbank[i];
        title_banked_bkg_data(first, title_sw_n[i], title_sw_rec_ptr[(uint16_t)i << 7], title_sw_rec_bank[(uint16_t)i << 7]);
        TS->vram_p[i] = 0;
    }
    VBK_REG = 0;

    /* "PRESS START BUTTON" tiles: OBJ VRAM bank 0, tiles 0.. (Sonic's gameplay slots, rebuilt at every act load) */
    set_sprite_data(0, (uint8_t)(TITLE_N_PSB * 2U), title_psb_tiles);

    TS->stp = TITLE_STP_FRAMES;
    TS->fade = TITLE_FADE_FIRST - 1U;
    TS->cyc_timer = 0;
    TS->cyc_idx = 0;
    TS->son_state = SON_NONE;
    TS->son_half = 1;
    TS->son_want = SON_NONE;
    TS->son_up = 0;
    TS->psb_on = 0;
    TS->m = 0;
    TS->rr = 0;
    TQ_DONE = 0;

    for (i = 0; i < TITLE_N_LYC; i++) {
        TISR_SCX[i] = 0;
        TISR_LYC[i] = title_lyc_line[i];
    }
    TISR_LYC[TITLE_N_LYC] = 0xFF;

    /* 8x16 sprites for big Sonic; title_exit() restores 8x8. */
    LCDC_REG |= LCDCF_OBJ16;
    title_isr_install();
}

void title_exit(void) BANKED {
    title_isr_remove();
    WY_REG = 144;   /* the "SONIC TEAM PRESENTS" card may still be up */
    LCDC_REG &= (uint8_t)~LCDCF_OBJ16;
    memset((uint8_t *)shadow_OAM, 0, 160);   /* no stale title tile numbers left in hidden OAM slots */
    g_oam_next = 0;
    hw_clear_remaining_oam();
}

static uint8_t *queue_entry(uint8_t *e, uint8_t max_ly, uint8_t bank, const uint8_t *src, uint16_t dst,
                            uint8_t vbk, uint8_t blocks) {
    *e++ = max_ly;
    *e++ = bank;
    *e++ = (uint8_t)(uint16_t)src;
    *e++ = (uint8_t)((uint16_t)src >> 8);
    *e++ = (uint8_t)dst;
    *e++ = (uint8_t)(dst >> 8);
    *e++ = vbk;
    *e++ = (uint8_t)(blocks - 1U);
    return e;
}

void title_update(void) BANKED {
    uint8_t i, k, st, psb, nq;
    uint16_t *pos;
    const uint16_t *spd;
    uint8_t *e;

    /* Results of the last VBlank's DMA queue */
    if (TQ_COUNT == 0) {
        for (i = 0; i < TQ_DONE; i++) {
            k = TS->q_tag[i];
            if (k < TITLE_N_SW_ROWS) TS->vram_p[k] = TS->q_p[i];
            else if (k == SON_TAG) TS->son_up = 2;
        }
        TQ_DONE = 0;
    }

    if (TS->stp) {
        TS->stp--;
        return;
    }
    if (TS->fade < TITLE_FADE_STEPS) {
        /* PaletteFadeIn: every colour climbs towards its value; objects and deformation wait for it.
           The VBlank handler hides the card (window) together with the first fade palette. */
        TS->fade++;
        pal_request(&title_fade_pals[(uint16_t)TS->fade << 6], 0, 8);
        return;
    }

    /* PalCycle_Title: rotate the water colours every 6 frames (first step on the first main-loop frame). */
    if (TS->cyc_timer == 0) {
        TS->cyc_timer = 6;
        TS->cyc_idx = (uint8_t)((TS->cyc_idx + 1U) & 3U);
        if (!TQ_PAL_N) pal_request(&title_water_cycle[TS->cyc_idx << 2], TITLE_WATER_PAL, 1);
        g_pal_cycles++;
    }
    TS->cyc_timer--;

    /* Deform_GHZ: advance every band; the LYC handler reads the SCX values directly */
    pos = TS->pos;
    spd = title_row_speed;
    for (i = 18; i != 0; i--) *pos++ += *spd++;
    for (i = 0; i < TITLE_N_LYC; i++) {
        k = title_lyc_row[i];
        TISR_SCX[i] = (k == 0xFFU) ? 0U : (uint8_t)(TS->pos[k] >> 8);
    }

    /* Big Sonic: a new image is GDMA'd into the idle half of OBJ VRAM bank 1 during VBlank; the next update
       points the sprites at it, so tiles and OAM always switch together (one frame later than the object). */
    st = sonic_state_for(TS->m);
    psb = (uint8_t)(((TS->m + 1U) >> 5) & 1U);   /* Ani_PSBTM: invisible 32 frames, text 32 frames */
    i = 0;
    if (TS->son_up == 2) {
        TS->son_state = TS->son_want;
        TS->son_half ^= 1U;
        TS->son_up = 0;
        i = 1;
    }
    if (st != TS->son_want && TS->son_up == 0) {
        TS->son_want = st;
        if (st == SON_NONE) { TS->son_state = SON_NONE; i = 1; }
        else TS->son_up = 1;
    }
    if (psb != TS->psb_on) {
        TS->psb_on = psb;
        i = 1;
    }
    if (i) build_oam();
    TS->m++;

    /* This frame's VBlank DMA queue: Sonic's new image first, then the software rows, round-robin. */
    if (TQ_COUNT) return;
    e = TQ_ENTRY;
    nq = 0;
    if (TS->son_up == 1) {
        k = TS->son_want;
        e = queue_entry(e, title_son_maxly[k], title_son_bank[k], title_son_tiles[k],
                        TS->son_half ? 0x8000U : 0x8400U, 1, (uint8_t)(title_son_n[k] << 1));
        TS->q_tag[nq++] = SON_TAG;
    }
    i = TS->rr;
    for (k = TITLE_N_SW_ROWS; k != 0 && (uint8_t)(nq + 2U) <= TQ_MAX; k--) {
        uint8_t p, n, bank;
        uint16_t idx;
        const uint8_t *src;
        if (++i >= TITLE_N_SW_ROWS) i = 0;
        p = (uint8_t)((TS->pos[title_sw_y[i]] >> 8) & (TITLE_SW_PERIOD - 1U));
        if (TS->vram_p[i] == p) continue;
        n = title_sw_n[i];
        idx = ((uint16_t)i << 7) | p;
        src = title_sw_rec_ptr[idx];
        bank = title_sw_rec_bank[idx];
        e = queue_entry(e, title_sw_maxly[i], bank, src, title_sw_tdst[i], title_sw_vbank[i], n);
        TS->q_tag[nq++] = 0xFFU;
        e = queue_entry(e, 0xFFU, bank, src + ((uint16_t)n << 4),
                        (uint16_t)(0x9800U + ((uint16_t)title_sw_y[i] << 5)), 1, 2);
        TS->q_tag[nq] = i;
        TS->q_p[nq++] = p;
        TS->rr = i;
    }
    TQ_COUNT = nq;
}
