#ifndef BADNIKS_H
#define BADNIKS_H
/* src/badniks.c (bank 9): ported Moto Bug / Crabmeat / Buzz Bomber / Chopper movement */
#define BADNIK_NOT_MINE 0xFFU
#define BADNIK_FIRE     1U
uint8_t badnik_ghz_step(ActiveObj *o, uint8_t id) BANKED;
#endif
