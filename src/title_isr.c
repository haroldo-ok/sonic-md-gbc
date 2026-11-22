/* Title screen pieces that must live in bank 0:
     - the LYC interrupt that scrolls the background bands which hold no emblem (Deform_GHZ per-band hscroll);
     - the VBlank handler that runs the DMA queue prepared by title_update() (src/title.c) and loads palettes;
     - bank-switching copy helpers for the banked title code. */
#include "engine.h"
#include "title_isr.h"
#include <gb/isr.h>

/* LYC chain (raw STAT vector, no GBDK dispatcher, so no extra WRAM): TISR_LYC[i] is the line *before* a band
   starts; the handler waits for that line's HBlank and writes TISR_SCX[i], so the new SCX applies from the band's
   first line. The list ends with 0xFF. Only enabled on the title screen (IE bit 1). */
void title_lcd_isr(void) __naked {
    __asm
    push af
    push hl
    ld   a, (0xAF00)
    and  a, #15
    add  a, #0x20
    ld   l, a
    ld   h, #0xAF
1$:
    ldh  a, (_STAT_REG + 0)
    and  a, #3
    jr   nz, 1$
    ld   a, (hl)
    ldh  (_SCX_REG + 0), a
    ld   a, l
    sub  a, #0x0F
    ld   l, a
    ld   a, (hl)
    cp   a, #0xFF
    jr   nz, 2$
    ld   l, #0x10
    ld   a, (hl)
2$:
    ldh  (_LYC_REG + 0), a
    ld   a, l
    sub  a, #0x10
    ld   (0xAF00), a
    pop  hl
    pop  af
    reti
    __endasm;
}
ISR_VECTOR(VECTOR_STAT, title_lcd_isr)

/* General-purpose DMA queue, run in VBlank. Entry (8 bytes): max_ly, bank, src lo, src hi, dst lo, dst hi, vbk,
   blocks-1. An entry starts only while LY <= max_ly (0xFF: no limit, used for the second half of a pair that must
   not be split). TQ_DONE receives the number of entries that ran; TQ_COUNT is cleared. */
static void title_run_queue(void) __naked {
    __asm
    ld   hl, #0xAF40
    ld   a, (hl+)
    or   a, a
    ret  z
    ld   c, a
    ld   b, #0
1$:
    ld   a, (hl+)
    ld   d, a
    ldh  a, (_LY_REG + 0)
    cp   a, #144
    jr   c, 9$
    ld   e, a
    ld   a, d
    cp   a, e
    jr   c, 9$
    ld   a, (hl+)
    ld   (0x2000), a
    ld   a, (hl+)
    ldh  (_HDMA2_REG + 0), a
    ld   a, (hl+)
    ldh  (_HDMA1_REG + 0), a
    ld   a, (hl+)
    ldh  (_HDMA4_REG + 0), a
    ld   a, (hl+)
    ldh  (_HDMA3_REG + 0), a
    ld   a, (hl+)
    ldh  (_VBK_REG + 0), a
    ld   a, (hl+)
    ldh  (_HDMA5_REG + 0), a
    inc  b
    dec  c
    jr   nz, 1$
9$:
    ld   a, b
    ld   (0xAF3F), a
    xor  a, a
    ld   (0xAF40), a
    ldh  (_VBK_REG + 0), a
    ldh  a, (__current_bank + 0)
    ld   (0x2000), a
    ret
    __endasm;
}

/* VBlank work of the title screen. Installed ahead of the music tick, so the DMAs get the whole VBlank. */
static void title_vbl_isr(void) {
    if (!(LCDC_REG & LCDCF_OBJ16)) return;
    if (TQ_PAL_N) {
        set_bkg_palette(TQ_PAL_FIRST, TQ_PAL_N, TQ_PAL_BUF);
        if (TQ_PAL_N == 8) {
            set_sprite_palette(0, 8, TQ_PAL_BUF + 32);
            WY_REG = 144;   /* fade-in step: the "SONIC TEAM PRESENTS" card is gone */
        }
        TQ_PAL_N = 0;
    }
    title_run_queue();
}

void title_banked_bkg_data(uint8_t first, uint8_t n, const uint8_t *src, uint8_t bank) {
    uint8_t save = CURRENT_BANK;
    SWITCH_ROM(bank);
    set_bkg_data(first, n, src);
    SWITCH_ROM(save);
}

void title_isr_install(void) {
    CRITICAL {
        TISR_IDX = 0;
        TQ_COUNT = 0;
        TQ_PAL_N = 0;
        LYC_REG = TISR_LYC[0];
        STAT_REG = STATF_LYC;
        /* run before the music tick: re-register it behind the title handler */
        remove_VBL(hw_sound_isr);
        add_VBL(title_vbl_isr);
        add_VBL(hw_sound_isr);
    }
    set_interrupts(VBL_IFLAG | TIM_IFLAG | LCD_IFLAG);
}

void title_isr_remove(void) {
    set_interrupts(VBL_IFLAG | TIM_IFLAG);
    CRITICAL {
        STAT_REG = 0;
        TQ_COUNT = 0;
        remove_VBL(title_vbl_isr);
    }
    SCX_REG = 0;
}
