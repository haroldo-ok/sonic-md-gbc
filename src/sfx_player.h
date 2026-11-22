#ifndef SFX_PLAYER_H
#define SFX_PLAYER_H
#include <stdint.h>

/* Sound effect stream player (src/sfx_player.c, in SFX_GB_BANK next to generated/sfx_gb.c).
 * Call the *_b8 functions with SFX_GB_BANK mapped (hw_play_sfx / hw_sound_tick in main.c do that). */
extern uint8_t g_sfx_own;               /* GB channels lent to the SFX: bit0 CH1, bit1 CH2, bit3 CH4 */
extern const uint8_t *g_sfx_ptr;
extern uint8_t g_sfx_prio;              /* SMPS v_sndprio */

void sfx_start_b8(uint8_t sfx_id, uint8_t jump_playing);
void sfx_tick_b8(uint8_t jump_playing);

#define sfx_stop_b0() do { g_sfx_own = 0; g_sfx_ptr = 0; g_sfx_prio = 0; } while (0)
#endif
