"""Source-line profile of chosen functions: every instruction of the function (from its `lcc -Wa-l` listing) gets a
hook that counts executions; cost = executions x listed cycles, grouped by the preceding `;src/file.c:N:` comment.

usage: PERF_SYMDIR=dir python3 tools/prof_lines.py ROM NOI ACT FRAMES module:_function [...]
"""
import collections
import os
import re
import sys
import warnings

warnings.filterwarnings("ignore")
from pyboy import PyBoy

rom, noi, act, frames = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
targets = sys.argv[5:]
symdir = os.environ["PERF_SYMDIR"]
syms = {}
for line in open(noi):
    p = line.split()
    if len(p) >= 3 and p[0] == "DEF":
        syms[p[1]] = int(p[2], 16)

hooks = []   # (bank, addr, key, cycles)
for t in targets:
    mod, fn = t.split(":")
    offs = {}
    for l in open(os.path.join(symdir, mod + ".sym")):
        mm = re.match(r"\s+(\d+)\s+(_\w+)\s+([0-9A-F]{8})\s+(G?R)", l)
        if mm:
            offs[mm.group(2)] = (int(mm.group(1)), int(mm.group(3), 16))
    base = None
    for nm, (ar, off) in offs.items():
        if nm in syms:
            base = syms[nm] - off
            area = ar
            break
    lines = open(os.path.join(symdir, mod + ".lst")).read().splitlines()
    start = None
    for i, l in enumerate(lines):
        if re.search(r"\s%s::?\s*$" % re.escape(fn), l):
            start = i
            break
    src = "?"
    for l in lines[start + 1:]:
        if re.search(r"; Function ", l):
            break
        m = re.search(r";(src/[\w./]+:\d+):", l)
        if m:
            src = m.group(1)
            continue
        mm = re.match(r"\s+([0-9A-F]{8})\s+[0-9A-F]{2}[^\[]*\[\s*(\d+)(?:/\s*(\d+))?\]", l)
        if mm:
            off = int(mm.group(1), 16)
            cyc = int(mm.group(2))
            v = base + off
            hooks.append(((v >> 16) & 0xFF, v & 0xFFFF, src, cyc))

cnt = collections.Counter()
pb = PyBoy(rom, window="null", sound_emulated=False)
for _ in range(420):
    pb.tick(1, False, False)
pb.memory[syms["_g_cur_act"]] = act
pb.memory[syms["_g_next_mode"]] = 3
pb.memory[syms["_g_mode_init_done"]] = 0
for _ in range(90):
    pb.tick(1, False, False)
cost = {}
for bank, addr, src, cyc in hooks:
    key = (bank, addr)
    cost[key] = (src, cyc)
    try:
        pb.hook_register(bank if addr >= 0x4000 else 0, addr, lambda k: cnt.__setitem__(k, cnt[k] + 1), key)
    except Exception:
        pass
pb.button_press("right")
for f in range(frames):
    if f % 70 == 40:
        pb.button_press("a")
    if f % 70 == 50:
        pb.button_release("a")
    pb.tick(1, False, False)
per = collections.Counter()
for k, n in cnt.items():
    src, cyc = cost[k]
    per[src] += n * cyc
tot = sum(per.values())
print(f"total {tot} cycles in {frames} frames ({tot / frames:.0f}/frame)")
for src, c in per.most_common(40):
    print(f"  {src:28s} {c / frames:8.0f}/frame  {100.0 * c / tot:5.1f}%")
