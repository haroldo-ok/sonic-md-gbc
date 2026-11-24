#ifndef SFX_PLAYER_H
#define SFX_PLAYER_H
#include <stdint.h>

/* 3-voice real-time layered PCM sound effect mixer on CGB Channel 3 (Wave RAM)
 * plus Bank 8 expressive BGM timbre/wave/drum helpers. */
#define SFX_MAX_VOICES 3U

typedef struct {
    uint8_t active;
    uint8_t sfx_id;
    uint8_t bank;
    uint8_t prio;
    uint16_t blocks;
    const uint8_t *ptr;
} SfxPcmVoice;

extern volatile uint8_t g_pcm_sfx_active;
extern volatile uint8_t g_ch3_restore_bass;
extern SfxPcmVoice g_sfx_voices[SFX_MAX_VOICES];
extern uint8_t g_sfx_own;   /* kept at 0 so BGM CH1/CH2/CH4 are never stolen */

extern uint8_t g_ch1_duty;
extern uint8_t g_ch1_env;
extern uint8_t g_ch2_duty;
extern uint8_t g_ch2_env;
extern uint8_t g_ch3_wave_idx;

void sfx_pcm_start_b8(uint8_t sfx_id);
void sfx_pcm_stop_b0(void);
void sfx_pcm_tim_tick(void);

void bgm_install_wave_b8(void);
uint8_t bgm_cmd_b8(uint8_t cmd, uint8_t can_wave_drum);

#define sfx_stop_b0() sfx_pcm_stop_b0()
#endif
