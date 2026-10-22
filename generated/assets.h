#ifndef SONIC_GBC_ASSETS_H
#define SONIC_GBC_ASSETS_H
#include <stdint.h>

#define NUM_VISUAL_SETS 9
#define NUM_PLAYABLE_ACTS 20
#define NUM_SPECIAL_STAGES 6
#define NUM_SONGS 19
#define NUM_SFX 49
#define SFX_DATA_BANK 79
#define PCM_SEGA_BANK 80
#define PCM_SEGA_PACKETS 160
#define SS_TILES_BANK 74

typedef struct {
    uint8_t tiles_bank;
    const uint8_t *bg_tiles0;
    const uint8_t *bg_tiles1;
    const uint8_t *spr_tiles;
    uint8_t meta_bank;
    const uint8_t *map16;
    const uint8_t *col1;
    const uint8_t *col2;
    uint8_t chunk_base_bank;
    uint8_t num_chunks;
    uint16_t num_m16;
    uint16_t bg_pal[32];
    uint16_t uw_pal[32];
} VisualSetDesc;

typedef struct {
    const char *label;
    uint8_t vset_idx;
    uint8_t data_bank;
    const uint8_t *fg_layout;
    const uint8_t *bg_layout;
    const uint8_t *objects;
    uint8_t fg_w, fg_h;
    uint8_t bg_w, bg_h;
    uint16_t start_x, start_y;
    uint16_t left_bound, right_bound;
    int16_t top_bound, bottom_bound;
    uint8_t music_id;
    int16_t water_y;
} ActDesc;

typedef struct {
    uint8_t bank;
    const uint8_t *tiles0;
    const uint8_t *tiles1;
    const uint8_t *map20x18;
    const uint8_t *attr20x18;
    uint16_t bg_pal[32];
} FullScreenDesc;

typedef struct {
    uint8_t bank;
    const uint8_t *maze64x64;
    uint16_t start_x, start_y;
} SpecialStageDesc;

typedef struct {
    uint8_t bank;
    const uint8_t *stream;
} SongDesc;

extern const uint8_t hud_font_tiles[];
extern const uint8_t common_sprite_tiles[];
extern const uint16_t sfx_table_offsets[];
extern const uint8_t all_sfx_blob[];
extern const uint8_t sega_wave_pcm[];
extern const uint8_t ss_vram_tiles[];
extern const uint8_t s1_angle_map[256];
extern const int8_t s1_coll_normal[4096];
extern const int8_t s1_coll_rotated[4096];
extern const int16_t s1_sine_table[320];
extern const uint8_t s1_demo_data[];
extern const uint16_t s1_demo_offsets[];
extern const uint8_t sonic_tiles_part0[];
extern const uint8_t sonic_tiles_part1[];
extern const uint16_t sonic_dplc_offsets[89];
extern const uint8_t sonic_dplc_blob[];
extern const uint16_t sonic_spr_offsets[89];
extern const uint8_t sonic_spr_blob[];
extern const uint16_t sonic_ani_offsets[32];
extern const uint8_t sonic_ani_blob[];

extern const VisualSetDesc g_visual_sets[NUM_VISUAL_SETS];
extern const ActDesc g_acts[NUM_PLAYABLE_ACTS];
extern const FullScreenDesc g_screens[3];
extern const SpecialStageDesc g_special_stages[NUM_SPECIAL_STAGES];
extern const SongDesc g_songs[NUM_SONGS];
extern const uint8_t * const g_chunk_bank_ptrs[256];
#endif
