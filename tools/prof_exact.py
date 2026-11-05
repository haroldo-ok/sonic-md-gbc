"""Exclusive-time function profiler for sonic-md-gbc (PyBoy).

usage: PERF_SYMDIR=<dir with lcc -Wa-l .lst/.sym/.asm> python3 tools/prof_exact.py ROM NOI [SCENARIO ...]

Static and global functions of every listed module get an entry hook and a hook on each of their return instructions
(`ret`, taken `ret cc`, `reti`, naked `jp (hl)` returns). A shadow call stack keyed by SP attributes every cycle to the
function that is actually executing (exclusive time) and also accumulates inclusive time.
"""
import collections
import os
import re
import sys
import warnings

warnings.filterwarnings("ignore")
from pyboy import PyBoy

rom, noi = sys.argv[1], sys.argv[2]
only = [a for a in sys.argv[3:] if not a.startswith("--")]
symdir = os.environ["PERF_SYMDIR"]

syms = {}
for line in open(noi):
    p = line.split()
    if len(p) >= 3 and p[0] == "DEF":
        syms[p[1]] = int(p[2], 16)


def u16(pb, a):
    return pb.memory[a] | (pb.memory[a + 1] << 8)


# ---- symbol / return-site discovery ------------------------------------------------------------------------------
ENTRY = {}     # (bank, addr) -> name
RETS = {}      # (bank, addr) -> (name, kind)   kind: 'ret' | 'z' | 'nz' | 'c' | 'nc'
for fn in sorted(os.listdir(symdir)):
    if not fn.endswith(".sym"):
        continue
    mod = fn[:-4]
    asm = open(os.path.join(symdir, mod + ".asm")).read()
    funcs = set("_" + f for f in re.findall(r"^; Function (\w+)", asm, re.M))
    offs = {}
    for l in open(os.path.join(symdir, fn)):
        mm = re.match(r"\s+(\d+)\s+(_\w+)\s+([0-9A-F]{8})\s+(G?R)", l)
        if mm:
            offs[mm.group(2)] = (int(mm.group(1)), int(mm.group(3), 16))
    base = area = None
    for nm, (ar, off) in offs.items():
        if nm in funcs and nm in syms:
            base, area = syms[nm] - off, ar
            break
    if base is None:
        continue
    starts = sorted((off, nm) for nm, (ar, off) in offs.items() if nm in funcs and ar == area)
    # return instructions from the listing (offsets of the code area that contains the functions)
    rets = []
    in_code = False
    for l in open(os.path.join(symdir, mod + ".lst")):
        if ".area" in l:
            in_code = ("_CODE" in l)
            continue
        if not in_code:
            continue
        mm = re.match(r"\s+([0-9A-F]{8})\s+[0-9A-F]{2}.*?\]\s+\d+\s+(ret|reti|jp)\b\s*([a-z]*)?\s*,?\s*([^\s;]*)", l)
        if mm:
            off = int(mm.group(1), 16)
            op, a1, a2 = mm.group(2), mm.group(3), mm.group(4)
            if op == "ret":
                rets.append((off, a1 if a1 in ("z", "nz", "c", "nc") else "ret"))
            elif op == "reti":
                rets.append((off, "ret"))
            elif op == "jp" and "(hl)" in l:
                rets.append((off, "jphl"))
    for i, (off, nm) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else 1 << 30
        v = base + off
        ENTRY[((v >> 16) & 0xFF, v & 0xFFFF)] = mod + ":" + nm
        for roff, kind in rets:
            if off <= roff < end:
                rv = base + roff
                RETS[((rv >> 16) & 0xFF, rv & 0xFFFF)] = (mod + ":" + nm, kind)

for extra in ("_vsync", "_wait_vbl_done"):
    if extra in syms:
        v = syms[extra]
        ENTRY[((v >> 16) & 0xFF, v & 0xFFFF)] = "main:_vsync"

SCEN = {
    "GHZ1_run": dict(act=0, mode=3, frames=240, hold=["right"], taps=[(60, "a", 10), (130, "a", 10)]),
    "MZ1_run": dict(act=3, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SYZ1_run": dict(act=6, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "LZ1_run": dict(act=9, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SLZ1_run": dict(act=12, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
    "SBZ1_run": dict(act=15, mode=3, frames=240, hold=["right"], taps=[(80, "a", 10)]),
}


def run(name, sc):
    if os.path.exists(rom + ".ram"):
        os.remove(rom + ".ram")
    pb = PyBoy(rom, window="null", sound_emulated=False)
    for _ in range(420):
        pb.tick(1, False, False)
    pb.memory[syms["_g_cur_act"]] = sc["act"]
    pb.memory[syms["_g_next_mode"]] = sc["mode"]
    pb.memory[syms["_g_mode_init_done"]] = 0
    for _ in range(90):
        pb.tick(1, False, False)
    excl = collections.Counter()
    incl = collections.Counter()
    calls = collections.Counter()
    st = {"stack": [], "last": pb._cycles(), "on": True}
    rf = pb.register_file

    def charge():
        c = pb._cycles()
        d = c - st["last"]
        st["last"] = c
        stk = st["stack"]
        top = stk[-1][0] if stk else "<main/unhooked>"
        excl[top] += d
        seen = set()
        for nm, _ in stk:
            if nm not in seen:
                incl[nm] += d
                seen.add(nm)

    def unwind():
        sp = rf.SP
        stk = st["stack"]
        while stk and stk[-1][1] < sp:
            stk.pop()

    def on_entry(nm):
        charge()
        unwind()
        stk = st["stack"]
        if stk and stk[-1][1] == rf.SP:      # tail call (`jp _f`): replaces the current frame
            stk.pop()
        stk.append((nm, rf.SP))
        calls[nm] += 1

    def on_ret(info):
        nm, kind = info
        f = rf.F
        taken = (kind in ("ret", "jphl") or (kind == "z" and f & 0x80) or (kind == "nz" and not f & 0x80) or
                 (kind == "c" and f & 0x10) or (kind == "nc" and not f & 0x10))
        charge()
        if taken:
            stk = st["stack"]
            sp = rf.SP          # a `ret` here pops the return address at SP: every frame entered at >= this SP ends
            while stk and stk[-1][1] <= sp:
                stk.pop()

    n = 0
    for (bank, addr), nm in ENTRY.items():
        try:
            pb.hook_register(bank if addr >= 0x4000 else 0, addr, on_entry, nm)
            n += 1
        except Exception:
            pass
    for (bank, addr), info in RETS.items():
        if (bank, addr) in ENTRY:
            continue
        try:
            pb.hook_register(bank if addr >= 0x4000 else 0, addr, on_ret, info)
        except Exception:
            pass
    for b in sc["hold"]:
        pb.button_press(b)
    f0 = u16(pb, syms["_g_frame_counter"])
    st["last"] = pb._cycles()
    excl.clear(); incl.clear(); calls.clear()
    taps = {t[0]: t for t in sc["taps"]}
    release = {}
    nfr = int(os.environ.get("PROF_FRAMES", sc["frames"]))
    for f in range(nfr):
        if f in taps:
            pb.button_press(taps[f][1]); release[f + taps[f][2]] = taps[f][1]
        if f in release:
            pb.button_release(release[f])
        pb.tick(1, False, False)
    f1 = u16(pb, syms["_g_frame_counter"])
    upd = (f1 - f0) & 0xFFFF
    idle = excl.get("main:_vsync", 0) + excl.get("<idle>", 0)
    tot = sum(excl.values())
    busy = tot - idle
    print(f"{name}: {upd} updates in {nfr} vblanks -> {60.0 * upd / nfr:.1f} FPS; "
          f"busy {busy / max(upd, 1):.0f} cycles/update")
    print(f"    {'function':44s} {'excl%':>6s} {'incl%':>6s} {'cyc/call':>9s} calls")
    for k, v in excl.most_common(30):
        if k == "main:_vsync":
            continue
        print(f"    {k:44s} {100.0 * v / busy:6.1f} {100.0 * incl[k] / busy:6.1f} {v / max(calls[k], 1):9.0f} {calls[k]}")
    pb.stop(save=False)


for n, sc in SCEN.items():
    if only and n not in only:
        continue
    run(n, sc)
