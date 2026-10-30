"""Benchmark + function-level profiler for sonic-md-gbc.

usage: python3 tools/perf_bench.py ROM NOI [--profile] [SCENARIO ...]
Reports main-loop frames (g_frame_counter) per 60 VBlanks-equivalent and, with --profile,
approximate cycle share per function (cycles between function entries attributed to the last-entered one).
"""
import sys, os, collections, warnings
warnings.filterwarnings("ignore")
from pyboy import PyBoy

rom, noi = sys.argv[1], sys.argv[2]
profile = "--profile" in sys.argv
only = [a for a in sys.argv[3:] if not a.startswith("--")]

syms = {}
for line in open(noi):
    p = line.split()
    if len(p) >= 3 and p[0] == "DEF":
        syms[p[1]] = int(p[2], 16)

def u16(pb, a):
    return pb.memory[a] | (pb.memory[a + 1] << 8)

# (name, act, mode, warmup, [(frame, press/release, button)], total frames)
SCEN = {
    "GHZ1_run":   dict(act=0,  mode=3, frames=240, hold=["right"], taps=[(60, "a", 10), (130, "a", 10)]),
    "GHZ1_idle":  dict(act=0,  mode=3, frames=120, hold=[], taps=[]),
    "MZ1_run":    dict(act=3,  mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SYZ1_run":   dict(act=6,  mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "LZ1_run":    dict(act=9,  mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SLZ1_run":   dict(act=12, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SBZ1_run":   dict(act=15, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SpecialStage": dict(act=0, mode=5, frames=120, hold=["right"], taps=[]),
}

def run(name, sc):
    if os.path.exists(rom + ".ram"): os.remove(rom + ".ram")
    with PyBoy(rom, window="null", sound_emulated=False) as pb:
        for _ in range(420):
            pb.tick(1, False, False)
        assert pb.memory[syms["_g_sega_pcm_playing"]] == 0, "chant still playing"
        pb.memory[syms["_g_cur_act"]] = sc["act"]
        pb.memory[syms["_g_next_mode"]] = sc["mode"]
        pb.memory[syms["_g_mode_init_done"]] = 0
        for _ in range(90):
            pb.tick(1, False, False)
        prof = collections.Counter()
        calls = collections.Counter()
        state = {"cur": "<idle/other>", "last": 0, "idle_from": None, "idle": 0}
        if profile:
            names = {}
            for s, v in syms.items():
                if not s.startswith("_"):
                    continue
                bank, addr = (v >> 16) & 0xFF, v & 0xFFFF
                if addr >= 0x8000 or v >= 0x60000 and (v >> 16) in range(4, 126) and addr >= 0x4000 and False:
                    continue
                if (bank, addr) in names and names[(bank, addr)] == "_vsync": continue
                names[(bank, addr)] = s
            def hook(ctx):
                c = pb._cycles()
                if state["idle_from"] is not None:
                    state["idle"] += c - state["idle_from"]; state["idle_from"] = None
                    prof["<idle:vsync>"] -= 0
                if ctx == "_vsync":
                    prof[state["cur"]] += c - state["last"]; state["last"] = c
                    state["cur"] = "<idle:vsync>"; state["idle_from"] = c; calls[ctx] += 1
                    return
                prof[state["cur"]] += c - state["last"]
                state["last"] = c
                state["cur"] = ctx
                calls[ctx] += 1
            n = 0
            for (bank, addr), s in names.items():
                # only engine functions: skip data tables (heuristic: name listed in sources)
                if s in ("_hw_sega_pcm_tim_isr", "_hw_sound_isr"): continue
                if s in ENGINE_FUNCS or s == '_vsync':
                    try:
                        pb.hook_register(bank if addr >= 0x4000 else 0, addr, hook, s)
                        n += 1
                    except Exception:
                        pass
            state["last"] = pb._cycles()
        for b in sc["hold"]:
            pb.button_press(b)
        f0 = u16(pb, syms["_g_frame_counter"])
        c0 = pb._cycles()
        taps = {t[0]: t for t in sc["taps"]}
        release = {}
        nfr = 0; f1 = f0
        for f in range(sc["frames"]):
            if f in taps:
                pb.button_press(taps[f][1]); release[f + taps[f][2]] = taps[f][1]
            if f in release:
                pb.button_release(release[f])
            pb.tick(1, False, False)
            if pb.memory[syms["_g_game_mode"]] != sc["mode"]:
                break   # left the measured mode (e.g. Special Stage exit): only count frames spent in it
            nfr += 1; f1 = u16(pb, syms["_g_frame_counter"])
        upd = (f1 - f0) & 0xFFFF
        fps = 60.0 * upd / max(nfr, 1)
        print(f"{name:14s} loop updates {upd:4d} / {nfr} vblank  ->  {fps:5.1f} FPS")
        if profile:
            tot_all = sum(prof.values())
            busy = tot_all - prof["<idle:vsync>"]
            print(f"    busy cycles/loop-update: {busy/max(upd,1):.0f}  (idle {100.0*prof['<idle:vsync>']/tot_all:.0f}% of window)")
            tot = busy
            for k, v in prof.most_common(26):
                if k == "<idle:vsync>": continue
                print(f"    {k:34s} {100.0*v/tot:5.1f}%  calls={calls[k]}")
        return fps

ENGINE_FUNCS = set()
if profile:
    import re
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
    for fn in os.listdir(src):
        if fn.endswith(".c"):
            txt = open(os.path.join(src, fn)).read()
            for m in re.finditer(r"^(?:static\s+|inline\s+)*[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:BANKED\s*)?\{", txt, re.M):
                ENGINE_FUNCS.add("_" + m.group(1))

res = {}
for n, sc in SCEN.items():
    if only and n not in only:
        continue
    res[n] = run(n, sc)
print("MEAN", sum(res.values()) / len(res))
