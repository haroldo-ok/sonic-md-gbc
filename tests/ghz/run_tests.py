import sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gt import GT
rom, noi, tag = sys.argv[1:4]
OUT = "verification/"
res = []
def check(name, ok, info=""):
    res.append(ok); print(("PASS " if ok else "FAIL ") + name, info)

# ---------------- GHZ1 bridge (1192, 904): 12 logs, walk range x in [1192-104, 1192+88) -------------------
g = GT(rom, noi, 0)
g.warm(1192, 904 - 60)
g.tick(40, shot=True)
st = g.stand
check("bridge: lands (stand set)", st != 0 and g.on_ground == 1, f"stand={st:#x} y={g.y} og={g.on_ground}")
y0 = g.y
check("bridge: top y", abs(y0 - (904 - 8 - 19)) <= 14, f"y={y0} expect~{904-27}")
g.tick(40, "right", shot=False)
g.tick(1, "right", shot=True)
check("bridge: walking keeps stand", g.stand != 0 and g.on_ground == 1, f"x={g.x} y={g.y}")
yy = g.y
g.tick(150, "right")
g.tick(1, shot=True)
check("bridge: walks off right end & falls/lands", g.x > 1192 + 88, f"x={g.x} y={g.y} stand={g.stand:#x}")
g.tick(30, "left")
g.tick(1, shot=True)
g.save(OUT + f"bridge_{tag}.png", scale=1, cols=4)
g.close()

# ---------------- GHZ1 moving platforms ---------------------------------------------------------------------
g = GT(rom, noi, 0)
g.warm(4640, 488 - 70)
ob0 = g.find(0x18, near=4640); g.place(g.obj(ob0[1])['x'], g.obj(ob0[1])['y'] - 40)
g.tick(5)
xs = []
g.tick(60, shot=True)
st = g.stand
check("plat1: stand set", st != 0 and g.on_ground == 1, f"stand={st:#x} x={g.x} y={g.y}")
x_a = g.x; ob = g.find(0x18, near=4640)
for k in range(30):
    g.tick(2)
    xs.append((g.x, ob and g.obj(ob[1])["x"]))
dx_sonic = g.x - x_a
o2 = g.obj(ob[1])
check("plat1: Sonic carried with platform", abs((g.x - o2["x"])) < 60 and g.on_ground == 1, f"sonic x {x_a}->{g.x} plat x={o2['x']}")
g.tick(1, shot=True)
g.close()

g = GT(rom, noi, 0)
g.warm(5728, 488)
ob0 = g.find(0x18, near=5728); g.place(g.obj(ob0[1])['x'], g.obj(ob0[1])['y'] - 40)
g.tick(60, shot=True)
ys = []
for k in range(40):
    g.tick(3); ys.append(g.y)
check("plat2: vertical carry", g.stand != 0 and max(ys) - min(ys) > 8, f"y range {min(ys)}..{max(ys)}")
g.close()

# falling platform (5024, 392) sub 3
g = GT(rom, noi, 0)
g.warm(5024, 392)
g.place(5024, 392 - 60)
g.tick(40)
ok1 = g.stand != 0
oy0 = g.find(0x18, near=5024)[2]["y"]
g.tick(30)
g.tick(1, shot=True)
g.tick(40)
g.tick(1, shot=True)
check("plat3: stands then falls", ok1 and g.on_ground == 0 or g.y > 392, f"stand0={ok1} y={g.y} og={g.on_ground}")
g.save(OUT + f"plat3_{tag}.png", scale=1, cols=4)
g.close()

# ---------------- GHZ1 ledge (3792, 576) ---------------------------------------------------------------------
g = GT(rom, noi, 0)
g.warm(3792 + 10, 576)
g.place(3802, 576 - 80)
g.tick(30, shot=True)
st = g.stand
check("ledge: lands", st != 0 and g.on_ground == 1, f"stand={st:#x} y={g.y} x={g.x}")
yl = g.y
g.tick(5, shot=True)
g.tick(4, shot=True)
g.tick(6, shot=True)
g.tick(10, shot=True)
g.tick(20, shot=True)
check("ledge: collapses (Sonic falls)", g.y > yl + 20 or g.on_ground == 0, f"y {yl}->{g.y} og={g.on_ground}")
g.save(OUT + f"ledge_{tag}.png", scale=1, cols=4)
g.close()

# ---------------- rock (2509, 765) ---------------------------------------------------------------------------
g = GT(rom, noi, 0)
g.warm(2509 - 70, 765)
g.place(2439, 755)
g.tick(40, "right", shot=True)
g.tick(40, "right", shot=True)
check("rock: blocks walking", g.x < 2509, f"x={g.x}")
g.warm(2509 + 5, 765 - 80)
g.tick(40, shot=True)
check("rock: stand on top", g.stand != 0 and g.on_ground == 1, f"y={g.y} stand={g.stand:#x} expect~{765-16-19}")
g.save(OUT + f"rock_{tag}.png", scale=1, cols=4)
g.close()

# ---------------- GHZ2 smash wall (592..752, 672) ---------------------------------------------------------------
g = GT(rom, noi, 1)
g.warm(540, 672)
g.place(540, 662)
g.tick(60, "right", shot=True)
check("wall: slow walk is blocked", g.x < 592, f"x={g.x}")
g.warm(470, 672 - 10)
g.tick(10)
for k in range(60):
    g.tick(1, "right" if k < 50 else "down")
    if k == 40: g.tick(1, "down", "right", shot=True)
g.tick(1, shot=True)
print("wall: x after roll", g.x, "rolling", g.rolling)
g.save(OUT + f"wall_{tag}.png", scale=1, cols=4)
g.close()
print("TOTAL", sum(res), "/", len(res))
