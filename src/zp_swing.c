#pragma bank 124
/* Marble Zone swinging platforms (`15 Swinging Platforms.asm`): the same `pr_swing` port GHZ uses (plank on a chain of
 * `subtype & $F` links, swinging on oscillator $1A, X-flip = subtype bit 6), with MZ's art from `tools/mz_swing.py`
 * (tiles 32..39 of the MZ sprite bank). Other zones' 0x15 still use the generic pendulum in `bosses_and_gimmicks.c`. */
#define PR_WANT_SWING
#include "engine.h"
#include "props_common.h"
#include "zp_mz_swing.h"

BANKREF(zp_swing_object)
uint8_t zp_swing_object(ActiveObj *o) BANKED {
    if (g_ram_act.vset_idx != 2) return 0;
    pr_swing(o, zp_mz_swing, ZP_MZ_SWING_N, zp_mz_chain, zp_mz_anchor);
    return 1;
}
