#ifndef ENGINE_H
#define ENGINE_H

#include <gbdk/platform.h>
#include <stdint.h>
#include <string.h>
#include "../generated/assets.h"
#include "../generated/supplement.h"

void *safe_copy_bytes(void *dst, const void *src, uint16_t n);
void *safe_set_bytes(void *dst, uint8_t val, uint16_t n);
#undef memcpy
#define memcpy(d, s, n) safe_copy_bytes((d), (s), (uint16_t)(n))
#undef memset
#define memset(d, v, n) safe_set_bytes((d), (uint8_t)(v), (uint16_t)(n))

/* Game Mode IDs matching Sonic 1 `GameModeArray` (`_inc/GameModeArray.asm`) */
#define GM_SEGA          0
#define GM_TITLE         1
#define GM_LEVEL_SELECT  2
#define GM_LEVEL         3
#define GM_ACT_CLEAR     4
#define GM_SPECIAL_STAGE 5
#define GM_CONTINUE      6
#define GM_GAME_OVER     7
#define GM_ENDING        8
#define GM_CREDITS       9
#define GM_SS_RESULTS    10
#define GM_TRY_AGAIN     11

/* Sound IDs matching `s1disasm/Constants.asm` */
#define BGM_GHZ          0x81
#define BGM_LZ           0x82
#define BGM_MZ           0x83
#define BGM_SLZ          0x84
#define BGM_SYZ          0x85
#define BGM_SBZ          0x86
#define BGM_INVINC       0x87
#define BGM_EXTRALIFE    0x88
#define BGM_SS           0x89
#define BGM_TITLE        0x8A
#define BGM_ENDING       0x8B
#define BGM_BOSS         0x8C
#define BGM_FZ           0x8D
#define BGM_ACTCLEAR     0x8E
#define BGM_GAMEOVER     0x8F
#define BGM_CONTINUE     0x90
#define BGM_CREDITS      0x91
#define BGM_DROWNING     0x92
#define BGM_EMERALD      0x93

#define SFX_JUMP         0xA0
#define SFX_LAMPPOST     0xA1
#define SFX_DEATH        0xA3
#define SFX_SKID         0xA4
#define SFX_SPIKES       0xA6
#define SFX_PUSH         0xA7
#define SFX_GOAL         0xA8
#define SFX_SPLASH       0xAA
#define SFX_HITBOSS      0xAC
#define SFX_BUBBLE       0xAD
#define SFX_FIREBALL     0xAE
#define SFX_SHIELD       0xAF
#define SFX_SAW          0xB0
#define SFX_ELECTRIC     0xB1
#define SFX_FLAMETHROW   0xB2
#define SFX_BUMPER       0xB4
#define SFX_RING         0xB5
#define SFX_SMASH        0xB9
#define SFX_GIANTRING    0xC3
#define SFX_BOMB         0xC4
#define SFX_CASH         0xC5
#define SFX_RINGLOSS     0xC6
#define SFX_CHAIN        0xC7
#define SFX_BURN         0xC8
#define SFX_HIDDENBONUS  0xC9
#define SFX_ENTERSPEC    0xCA
#define SFX_WALLSMASH    0xCB
#define SFX_SPRING       0xCC
#define SFX_SWITCH       0xCD
#define SFX_SIGNPOST     0xCF
#define SFX_COLLAPSE     0xB9
#define SFX_BREAK        0xCB
#define SFX_ROLL         0xBE

/* Sonic Animation IDs matching `s1disasm/_anim/Sonic.asm` */
#define ANI_WALK         0x00
#define ANI_RUN          0x01
#define ANI_ROLL         0x02
#define ANI_ROLL2        0x03
#define ANI_PUSH         0x04
#define ANI_WAIT         0x05
#define ANI_BALANCE      0x06
#define ANI_LOOKUP       0x07
#define ANI_DUCK         0x08
#define ANI_STOP         0x0D
#define ANI_FLOAT1       0x0E
#define ANI_SPRING       0x10
#define ANI_LEAP1        0x12
#define ANI_GETAIR       0x15
#define ANI_DROWN        0x17
#define ANI_DEATH        0x18
#define ANI_HURT         0x1A
#define ANI_SLIDE        0x1B

/* Verified Common Sprite Tile Slots in VRAM Bank 0 (`32..127` — 96 tiles, 0 padding!) */
#define SPR_TILE_RING        32   /* 32..35  (4 x 8x8 spinning ring frames 0..3) */
#define SPR_TILE_MONITOR     36   /* 36..55  (5 x 16x16 2x2 monitors: +0 Ring, +4 Shoes, +8 Shield, +12 Invinc, +16 1UP) */
#define SPR_TILE_SPRING      56   /* 56..63  (2 x 16x16 2x2 springs: +0 Vertical, +4 Horizontal) */
#define SPR_TILE_SPIKES      64   /* 64..67  (1 x 16x16 2x2 3-spike steel cluster) */
#define SPR_TILE_LAMP        68   /* 68..71  (1 x 16x16 2x2 checkered Starpost) */
#define SPR_TILE_SHOT_ENERGY 72   /* 72..73  (2 x 8x8 Buzz Bomber / Crabmeat glowing shot) */
#define SPR_TILE_SHOT_FIRE   74   /* 74..75  (2 x 8x8 blazing comet fireball) */
#define SPR_TILE_SHOT_BOMB   76   /* 76      (1 x 8x8 fuse bomb) */
#define SPR_TILE_SHOT_PLASMA 77   /* 77..78  (2 x 8x8 FZ electric plasma orb) */
#define SPR_TILE_SIGNPOST    79   /* 79..95  (17 tiles: 24x24 5-frame Signpost + Pole 95) */
#define SPR_TILE_SHIELD      96   /* 96..99  (1 x 16x16 2x2 glowing blue aura shield) */
#define SPR_TILE_STARS       100  /* 100..103 (4 x 8x8 4-point invincibility / ring sparkles) */
#define SPR_TILE_EXPLODE     104  /* 104..111 (2 x 16x16 2x2 fiery badnik explosions: +0 burst, +4 cloud) */
#define SPR_TILE_POINTS      112  /* 112..115 (4 x 8x8 pixel-crisp 100, 200, 500, 1000 popups) */
#define SPR_TILE_ANIMAL      116  /* 116..119 (4 x 8x8 hopping Flicky & Rabbit animals) */
#define SPR_TILE_BUBBLE_BIG  120  /* 120..123 (1 x 16x16 2x2 big breathable air bubble) */
#define SPR_TILE_BUBBLE      124  /* 124..127 (4 x 8x8 small rising bubbles) */

/* Verified Zone Sprite Tile Slots in VRAM Bank 1 (`0..127` — 128 tiles per Zone!) */
#define ZSPR_ENEMY1       0       /* 0..7    (2 x 16x16 2x2 Badnik #1) */
#define ZSPR_ENEMY2       8       /* 8..15   (2 x 16x16 2x2 Badnik #2) */
#define ZSPR_ENEMY3       16      /* 16..23  (2 x 16x16 2x2 Badnik #3) */
#define ZSPR_ENEMY4       24      /* 24..31  (2 x 16x16 2x2 Badnik #4) */
#define ZSPR_ENEMY5       32      /* 32..39  (2 x 16x16 2x2 Badnik #5) */
#define ZSPR_PROP         40      /* 40..67  (7 x 16x16 2x2 Zone Props & Hazards: +0..+24) */
#define ZSPR_BOSS         68      /* 68..83  (2 x 32x16 4x2 Composite Dr. Eggman Pod + Head / FZ Cylinder) */
#define ZSPR_BOSS_WPN     84      /* 84..99  (4 x 16x16 2x2 Boss Weapons: +0 Ball, +4 Spike/Pipe, +8 Cyl, +12 Chain) */
#define ZSPR_GIANTRING    100     /* 100..115 (4 x 16x16 2x2 Spinning Golden Giant Ring frames 0..3) */
#define ZSPR_CAPSULE      116     /* 116..127 (116..121 24x16 Capsule, 122..123 16x8 Red Button, 124..127 Open) */

/* Verified Half-Scale Special Stage Tile Slots in VRAM Bank 0 (`0..64`) */
#define SS_TILE_BG        0
#define SS_TILE_WALL      1
#define SS_TILE_GOAL      17
#define SS_TILE_RING      25
#define SS_TILE_RBLOCK    33
#define SS_TILE_UPDOWN    34
#define SS_TILE_GLASS     35
#define SS_TILE_EMERALD   49

#define MAX_ACTIVE_OBJS  24
#define MAX_LOST_RINGS   8
#define MAX_PROJS        8
#define MAX_SCORE_POPS   4

/* Projectile Kinds (`ProjObj.kind`) */
#define PROJ_ENERGY      1
#define PROJ_FIREBALL    2
#define PROJ_BOMB        3
#define PROJ_PLASMA      4
#define PROJ_DEBRIS      5
#define PROJ_SPLASH      6
#define PROJ_ANIMAL_HOP  7

typedef struct {
    int32_t x_sub;       /* 24.8 world X (bytes 0..3) */
    int32_t y_sub;       /* 24.8 world Y (bytes 4..7) */
    int16_t x;           /* integer world X (bytes 8..9 — read directly by PyBoy E2E test!) */
    int16_t y;           /* integer world Y (bytes 10..11) */
    int16_t xsp;         /* X velocity (8.8 fixed point) */
    int16_t ysp;         /* Y velocity (8.8 fixed point) */
    int16_t gsp;         /* Ground speed inertia (8.8 fixed point) */
    uint8_t angle;       /* 0..255 terrain angle */
    uint8_t on_ground;
    uint8_t rolling;
    uint8_t facing_left;
    uint8_t loop_plane;
    uint8_t anim;
    uint8_t prev_anim;
    uint8_t anim_idx;
    uint8_t anim_timer;
    uint8_t frame;
    uint8_t invuln;
    uint16_t invinc;
    uint16_t shoes;
    uint8_t shield;
    uint8_t dead;
    uint8_t lock_ctrl;
    uint8_t spindash;
    uint16_t spindash_rev;
    uint16_t air_frames;
    uint8_t underwater;
    uint8_t air_combo;
    uint8_t jumping;     /* 1 ONLY when airborne via player Jump button (`Sonic_JumpHeight`, `01 Sonic.asm:1274`) */
} SonicState;

typedef struct {
    uint8_t active;
    uint8_t id;
    uint8_t subtype;
    uint16_t spawn_slot;
    int16_t x;
    int16_t y;
    int16_t base_x;
    int16_t base_y;
    int16_t vx;
    int16_t vy;
    uint8_t state;
    uint8_t timer;
    uint8_t hp;
} ActiveObj;

typedef struct {
    uint8_t active;
    int16_t x;
    int16_t y;
    int16_t vx;
    int16_t vy;
    uint8_t timer;
} LostRing;

typedef struct {
    uint8_t active;
    uint8_t kind;
    int16_t x;
    int16_t y;
    int16_t vx;
    int16_t vy;
    uint8_t timer;
} ProjObj;

typedef struct {
    uint8_t active;
    uint8_t step;
    int16_t x;
    int16_t y;
    uint8_t timer;
} ScorePop;

/* Shared Global Engine State (defined in `src/main.c`) */
extern volatile uint8_t g_game_mode;
extern volatile uint8_t g_next_mode;
extern volatile uint8_t g_mode_init_done;
extern volatile uint16_t g_frame_counter;
extern volatile uint8_t g_cur_act;
extern volatile uint8_t g_cur_ss;
extern volatile uint8_t g_demo_mode;
extern uint16_t g_demo_timer;
extern uint16_t g_demo_pos;
extern uint8_t g_demo_btn;
extern uint8_t g_demo_btn_timer;

extern uint8_t g_joy;
extern uint8_t g_joy_prev;
extern uint8_t g_joy_pressed;
extern uint8_t g_paused;

extern volatile uint8_t g_debug_unlocked;
extern volatile uint8_t g_debug_active;
extern uint8_t g_debug_item;
extern uint8_t g_button_state;
extern volatile uint16_t g_pal_cycles;
extern volatile uint16_t g_anim_tile_uploads;
extern volatile uint16_t g_dac_drum_packets;
extern volatile uint8_t g_rev_mode;
extern volatile uint8_t g_jp_credits;

extern int16_t g_dyn_left_bound;
extern int16_t g_dyn_right_bound;
extern int16_t g_dyn_top_bound;
extern int16_t g_dyn_bottom_bound;
extern uint16_t g_osc_val[16];
extern uint16_t g_osc_dir;

extern SonicState g_sonic;
extern ActiveObj g_objs[MAX_ACTIVE_OBJS];
extern LostRing g_lost_rings[MAX_LOST_RINGS];
extern ProjObj g_projs[MAX_PROJS];
extern ScorePop g_pops[MAX_SCORE_POPS];
extern uint8_t g_obj_taken[64];

extern int16_t g_cam_x;
extern int16_t g_cam_y;
extern uint32_t g_score;
extern uint32_t g_high_score;
extern uint8_t g_score_life_awarded;
extern uint16_t g_rings;
extern uint8_t g_lives;
extern uint8_t g_continues;
extern uint8_t g_emeralds;
extern uint8_t g_unlocked_act;
extern uint8_t g_ss_got_emerald;
extern uint16_t g_level_frames;
extern uint8_t g_hud_dirty;
extern uint8_t g_next_ring_life;

extern uint8_t g_checkpoint_active;
extern uint8_t g_checkpoint_act;
extern int16_t g_checkpoint_x;
extern int16_t g_checkpoint_y;

extern uint8_t g_signpost_passed;
extern uint8_t g_act_clear_done;
extern uint16_t g_time_bonus;
extern uint16_t g_ring_bonus;
extern volatile uint8_t g_boss_active;
extern volatile uint8_t g_boss_defeated;
extern uint16_t g_syz_blocks_alive;
extern int16_t g_water_y;

extern VisualSetDesc g_ram_vset;
extern ActDesc g_ram_act;
extern uint8_t g_oam_next;
extern const uint16_t g_default_obj_pals[32];

extern volatile uint8_t g_cur_bgm;
extern volatile uint8_t g_cur_sfx;
extern volatile uint16_t g_sound_events;
extern volatile uint16_t g_sound_isr_ticks;
extern volatile uint8_t g_sega_pcm_playing;

/* NONBANKED Bank-0 Hardware & Asset Streaming Services (`src/main.c`) */
void hw_play_bgm(uint8_t bgm_id);
void hw_play_sfx(uint8_t sfx_id);
void hw_sound_tick(void);
void hw_sound_isr(void);
void hw_sega_pcm_tim_isr(void);
void hw_start_sega_chant(void);
void hw_stop_sega_chant(void);
void hw_fast_vram_row20(uint8_t ry, const uint8_t *row_t, const uint8_t *row_a);
void hw_fill_win_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t fill_tile) BANKED;
void hw_print_win(uint8_t x, uint8_t y, const char *str);
void hw_clear_remaining_oam(void);
void hw_put_sprite8(int16_t sx, int16_t sy, uint8_t tile, uint8_t attr);
void hw_put_sprite16(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t attr);
void hw_put_sprite_cols2rows(int16_t sx, int16_t sy, uint8_t base_tile, uint8_t ncols, uint8_t attr);
void hw_show_transition_card(const char *line1, const char *line2, uint8_t act_num);
void hw_update_hud(void) BANKED;
void hw_load_fullscreen_screen(uint8_t scr_idx);
void hw_load_act(uint8_t act_idx);
void hw_refresh_full_viewport(void);
void hw_stream_viewport_delta(int16_t old_cx, int16_t old_cy);
void hw_set_underwater_palette(uint8_t underwater) BANKED;
void hw_step_palette_cycle(void) BANKED;
void hw_animate_level_gfx(void);
void hw_fade_step(uint8_t step_idx) BANKED;
void hw_load_special_stage_vram(uint8_t ss_idx, int16_t *out_sx, int16_t *out_sy);
void hw_copy_ss_maze(uint8_t ss_idx, uint8_t *dst4096);
void hw_init_osc_from_rom(uint16_t *dst16);
uint8_t hw_get_raw_chunk_id(int16_t world_x, int16_t world_y);
uint16_t hw_get_chunk_word(int16_t world_x, int16_t world_y, uint8_t loop_alt);
int16_t hw_find_floor(int16_t py, int16_t px, uint8_t solid_bit, uint16_t d6_eor, int16_t a3_step, uint8_t *out_ang);
int16_t hw_find_wall(int16_t py, int16_t px, uint8_t solid_bit, uint16_t d6_eor, int16_t a3_step, uint8_t *out_ang);
int16_t hw_floor_distance(int16_t px, int16_t py, uint8_t *out_angle);
int16_t hw_ceil_surface_distance(int16_t px, int16_t py, uint8_t *out_angle);
int16_t hw_rwall_distance(int16_t px, int16_t py, uint8_t *out_angle);
int16_t hw_lwall_distance(int16_t px, int16_t py, uint8_t *out_angle);
int16_t hw_ceil_distance(int16_t px, int16_t py);
int16_t hw_wall_distance(int16_t px, int16_t py, int8_t dir);
int16_t hw_sin(uint8_t angle);
int16_t hw_cos(uint8_t angle);
uint8_t hw_read_demo_input(uint8_t demo_idx);
void hw_step_sonic_anim(void);
void hw_draw_sonic_sprite(void);
void hw_scan_spawn_objects(void);

/* Bank 1: Sonic 360° Physics & Debug Mode (`src/physics.c`) */
void physics_init_sonic(int16_t sx, int16_t sy) BANKED;
void physics_update_level(void) BANKED;
void sonic_hurt_or_kill(uint8_t instant_kill) BANKED;

/* Bank 2: Level Objects & All 16 Sonic 1 Badnik AIs (`src/objects.c`) */
void objects_reset_for_act(void) BANKED;
void objects_update_and_draw(void) BANKED;

/* Bank 3: Screens, 360° Special Stages, Results, 9-Card Credits, Try Again & Secret JP Credits (`src/screens_and_ss.c`) */
void screens_init_mode(uint8_t mode) BANKED;
void screens_update_mode(uint8_t mode) BANKED;

/* Bank 122: All 6 Zone Bosses (+ SYZ3 10-Block Floor), All Zone & Cut Gimmicks (`src/bosses_and_gimmicks.c`) */
void boss_and_gimmick_reset(void) BANKED;
void boss_check_and_update(uint8_t sonic_spinning) BANKED;
uint8_t gimmick_handle_object(ActiveObj *o, int16_t sx, int16_t sy, uint8_t sonic_spinning) BANKED;
void projectiles_and_popups_update(void) BANKED;
void spawn_enemy_projectile(int16_t x, int16_t y, int16_t vx, int16_t vy, uint8_t kind) BANKED;
void spawn_score_popup(int16_t x, int16_t y, uint8_t combo_step) BANKED;

/* Bank 123: DynamicLevelEvents, 16-Channel Oscillators, AddPoints High-Score & Battery SRAM (`src/dle_osc_sram.c`) */
void dle_init_for_act(void) BANKED;
void dle_update_act_boundaries(void) BANKED;
void osc_reset(void) BANKED;
void osc_step_all(void) BANKED;
void add_score_points(uint16_t pts) BANKED;
void sram_load_or_init(void) BANKED;
void sram_save_progress(void) BANKED;

#endif
