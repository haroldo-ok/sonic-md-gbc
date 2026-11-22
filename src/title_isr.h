#ifndef TITLE_ISR_H
#define TITLE_ISR_H
#include <stdint.h>

/* Title screen RAM lives in MBC5 SRAM bank 1 (`rRAMB = 1`), which otherwise only holds level caches that every act
   load rebuilds; internal WRAM has no room left (`l__DATA` is at its limit).
     $AE00..$AEFF  title state (src/title.c)
     $AF00..$AF2F  LYC interrupt tables
     $AF30..$AF3F  VBlank palette request / DMA queue status
     $AF40..$AFBF  VBlank DMA queue (count + 8-byte entries)
     $B000..$B07F  palette buffer for the VBlank handler */
#define TISR_IDX     (*(volatile uint8_t *)0xAF00U)
#define TISR_LYC     ((volatile uint8_t *)0xAF10U)
#define TISR_SCX     ((volatile uint8_t *)0xAF20U)
#define TQ_PAL_FIRST (*(volatile uint8_t *)0xAF30U)
#define TQ_PAL_N     (*(volatile uint8_t *)0xAF31U)   /* BG palettes to load (8 = BG and OBJ) */
#define TQ_DONE      (*(volatile uint8_t *)0xAF3FU)   /* entries run by the last VBlank */
#define TQ_COUNT     (*(volatile uint8_t *)0xAF40U)
#define TQ_ENTRY     ((uint8_t *)0xAF41U)
#define TQ_MAX       15
#define TQ_PAL_BUF   ((uint16_t *)0xB000U)

void hw_sound_isr(void);
void title_lcd_isr(void);
void title_banked_bkg_data(uint8_t first, uint8_t n, const uint8_t *src, uint8_t bank);
void title_isr_install(void);
void title_isr_remove(void);

/* Banked title screen entry points (src/title.c, bank 7) */
void title_init(void) BANKED;
void title_update(void) BANKED;
void title_exit(void) BANKED;
#endif
