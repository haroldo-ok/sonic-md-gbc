#ifndef SUPPLEMENT_H
#define SUPPLEMENT_H
#include <stdint.h>

#define SUPPLEMENT_ANIM_BANK  124
#define SUPPLEMENT_REV00_BANK 125

#define DAC_KICK_PKTS     16
#define DAC_SNARE_PKTS    16
#define DAC_TIMPANI_PKTS  16

extern const uint8_t dac_kick_wave[];
extern const uint8_t dac_snare_wave[];
extern const uint8_t dac_timpani_wave[];
extern const uint16_t dac_timpani_periods[4];
extern const uint16_t s1_title_water_cycle[];
extern const uint16_t s1_osc_init_values[32];

extern const uint8_t anim_ghz_flower[128];
extern const uint8_t anim_ghz_waterfall[128];
extern const uint8_t anim_mz_lava[128];
extern const uint8_t anim_mz_torch[128];
extern const uint8_t anim_sbz_smoke[128];
extern const uint8_t anim_end_flower[128];

extern const uint8_t rev00_ghz3_objs[];
extern const uint8_t rev00_lz1_objs[];
extern const uint8_t rev00_sega_logo_tiles[];

#endif
