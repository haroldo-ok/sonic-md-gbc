"""Deterministic per-update state comparison between two ROMs.
Input is injected into g_joy/g_joy_prev/g_joy_pressed at the entry of screens_update_mode (keyed by update index),
and g_sonic + ring count + camera are recorded at the same point, so both ROMs see identical inputs per game update
regardless of how many VBlanks each update takes.
usage: [MAP=1] [TOTAL=n] python3 tools/compare_trace.py ROM_A NOI_A ROM_B NOI_B [scenario ...]
MAP=1 also compares both VRAM banks, shadow OAM, the object table and lost rings after every update."""
import sys, os, warnings
warnings.filterwarnings("ignore")
from pyboy import PyBoy
ra, na, rb, nb = sys.argv[1:5]
R, L, U, D, A = 0x01, 0x02, 0x04, 0x08, 0x10
def taps(start, step, dur, end=2000):
    return [(t, dur) for t in range(start, end, step)]
SC = {
 "GHZ1_run":  dict(act=0,  hold=R, taps=[(60,10),(130,10),(200,10)]),
 "GHZ1_jump": dict(act=0,  hold=R, taps=taps(30,45,12)),
 "GHZ1_left": dict(act=0,  hold=L, taps=[(40,10)]),
 "GHZ1_roll": dict(act=0,  hold=R, taps=taps(50,70,20), downs=[(t,25) for t in range(90,2000,140)]),
 "GHZ2_run":  dict(act=1,  hold=R, taps=taps(50,90,12)),
 "GHZ3_run":  dict(act=2,  hold=R, taps=taps(50,90,12)),
 "MZ1_run":   dict(act=3,  hold=R, taps=taps(80,170,10)),
 "MZ2_run":   dict(act=4,  hold=R, taps=taps(50,90,12)),
 "SYZ1_run":  dict(act=6,  hold=R, taps=taps(80,170,10)),
 "SYZ2_run":  dict(act=7,  hold=R, taps=taps(50,90,12)),
 "LZ1_run":   dict(act=9,  hold=R, taps=taps(80,170,10)),
 "SLZ1_run":  dict(act=12, hold=R, taps=taps(80,170,10)),
 "SBZ1_run":  dict(act=15, hold=R, taps=taps(80,170,10)),
 "SBZ2_run":  dict(act=16, hold=R, taps=taps(50,90,12)),
}
for _a in range(19):
    for _seed in range(3):
        SC[f"FUZZ{_a}_{_seed}"] = dict(act=_a, hold=0, fuzz=_a * 7 + _seed)
DEFAULT = ["GHZ1_run", "GHZ1_jump", "GHZ1_left", "GHZ1_roll", "GHZ2_run", "MZ1_run", "SYZ1_run", "LZ1_run", "SLZ1_run", "SBZ1_run", "SBZ2_run"]
scens = sys.argv[5:] or DEFAULT
TOTAL = int(os.environ.get("TOTAL", "600"))
MAP = bool(os.environ.get("MAP"))
def load(noi):
    d = {}
    for line in open(noi):
        p = line.split()
        if len(p) >= 3 and p[0] == "DEF": d[p[1]] = int(p[2], 16)
    return d
import random
def joy_for(sc, k):
    if "fuzz" in sc:
        r = random.Random(sc["fuzz"] * 100003 + k // 6)   # input changes every 6 updates
        v = r.random()
        j = R if v < 0.55 else (L if v < 0.75 else (D if v < 0.85 else 0))
        if r.random() < 0.35: j |= A
        return j
    j = sc["hold"]
    for (t, d) in sc.get("taps", []):
        if t <= k < t + d: j |= A
    for (t, d) in sc.get("downs", []):
        if t <= k < t + d: j = (j & ~(R | L)) | D
    return j
def run(rom, noi, sc):
    syms = load(noi)
    if os.path.exists(rom + ".ram"): os.remove(rom + ".ram")
    out = []
    sa = syms["_g_sonic"]; ga = syms["_g_rings"]
    v = syms["_screens_update_mode"]; bank, addr = (v >> 16) & 0xFF, v & 0xFFFF
    with PyBoy(rom, window="null", sound_emulated=False) as pb:
        mem = pb.memory
        u16 = lambda a: mem[a] | (mem[a+1] << 8)
        for _ in range(420): pb.tick(1, False, False)
        st = {"on": False, "k": 0, "prev": 0}
        def cb(_):
            if not st["on"] or mem[syms["_g_game_mode"]] != 3: return
            k = st["k"]
            if k == 0:   # align the global loop counter (boot-time dependent) so both ROMs share its phase
                fc = syms["_g_frame_counter"]; mem[fc] = 0x00; mem[fc + 1] = 0x10
            sb = bytearray(mem[sa:sa+40]); sb[29] = sb[30] = 0   # invinc counts VBlanks (wall-clock), not updates
            rec = bytes(sb) + bytes(mem[ga:ga+2]) + bytes(mem[syms["_g_level_frames"]:syms["_g_level_frames"]+2])
            if MAP: rec += bytes(mem[0, 0x8000:0x9FFF]) + bytes(mem[1, 0x8000:0x9FFF]) + bytes([mem[0xFF4F]]) + bytes(mem[syms["_shadow_OAM"]:syms["_shadow_OAM"]+160]) + bytes(mem[syms["_g_objs"]:syms["_g_objs"]+528]) + bytes(mem[syms["_g_lost_rings"]:syms["_g_lost_rings"]+96])
            out.append(rec)
            j = joy_for(sc, k)
            mem[syms["_g_joy"]] = j; mem[syms["_g_joy_prev"]] = st["prev"]; mem[syms["_g_joy_pressed"]] = (j ^ st["prev"]) & j
            st["prev"] = j; st["k"] = k + 1
        pb.hook_register(bank if addr >= 0x4000 else 0, addr, cb, None)
        mem[syms["_g_cur_act"]] = sc["act"]; mem[syms["_g_next_mode"]] = 3; mem[syms["_g_mode_init_done"]] = 0
        st["on"] = True
        guard = 0
        while st["k"] < TOTAL and guard < TOTAL * 4 + 600:
            pb.tick(1, False, False); guard += 1
    return out
bad = 0
for sn in scens:
    Aa = run(ra, na, SC[sn]); Bb = run(rb, nb, SC[sn])
    n = min(len(Aa), len(Bb)); first = next((i for i in range(2, n) if Aa[i] != Bb[i]), None)
    print(f"{sn}: {n} updates, " + ("identical" if first is None else f"FIRST DIFF at update {first}: " + str([(i, Aa[first][i], Bb[first][i]) for i in range(len(Aa[first])) if Aa[first][i] != Bb[first][i]][:12])), flush=True)
    bad += first is not None
sys.exit(1 if bad else 0)
