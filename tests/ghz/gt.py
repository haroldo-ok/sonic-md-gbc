"""PyBoy harness for GHZ prop tests."""
import sys, os, struct, warnings
warnings.filterwarnings("ignore")
from pyboy import PyBoy
from PIL import Image
KEYS = ["a","b","select","start","right","left","up","down"]
class GT:
    def __init__(self, rom, noi, act, tag="t"):
        self.syms = {}
        for l in open(noi):
            p = l.split()
            if len(p) >= 3 and p[0] == "DEF": self.syms[p[1]] = int(p[2], 16)
        if os.path.exists(rom + ".ram"): os.remove(rom + ".ram")
        self.pb = PyBoy(rom, window="null", sound_emulated=False)
        for _ in range(300): self.pb.tick(1, False, False)
        m = self.pb.memory
        m[self.syms["_g_cur_act"]] = act; m[self.syms["_g_next_mode"]] = 3; m[self.syms["_g_mode_init_done"]] = 0
        for _ in range(120): self.pb.tick(1, False, False)
        self.GS = self.syms["_g_sonic"]; self.GO = self.syms["_g_objs"]
        self.held = set(); self.imgs = []
    def w16(self, a, v): self.pb.memory[a] = v & 255; self.pb.memory[a+1] = (v >> 8) & 255
    def w32(self, a, v):
        for i in range(4): self.pb.memory[a+i] = (v >> (8*i)) & 255
    def r16(self, a): return struct.unpack("<h", bytes(self.pb.memory[a:a+2]))[0]
    def place(self, x, y):
        self.w32(self.GS, x << 8); self.w32(self.GS+4, y << 8); self.w16(self.GS+8, x); self.w16(self.GS+10, y)
        for o in (12, 14, 16): self.w16(self.GS+o, 0)
    def warm(self, x, y):
        xs = list(range(max(x - 900, 100), x, 100)) + [x]
        for xx in xs:
            for _ in range(8):
                self.place(xx, max(y - 120, 16))
                self.w16(self.syms["_g_cam_x"], max(xx - 80, 0)); self.w16(self.syms["_g_cam_y"], max(y - 200, 0))
                self.tick(2)
        for _ in range(6):
            self.place(x, max(y - 120, 16)); self.tick(2)
        self.place(x, y)
    @property
    def x(self): return self.r16(self.GS+8)
    @property
    def y(self): return self.r16(self.GS+10)
    @property
    def xsp(self): return self.r16(self.GS+12)
    @property
    def ysp(self): return self.r16(self.GS+14)
    @property
    def gsp(self): return self.r16(self.GS+16)
    @property
    def on_ground(self): return self.pb.memory[self.GS+19]
    @property
    def rolling(self): return self.pb.memory[self.GS+20]
    @property
    def stand(self): return self.r16(self.syms["_g_stand_obj"]) & 0xFFFF
    def press(self, *ks):
        for k in self.held - set(ks): self.pb.button_release(k)
        for k in set(ks) - self.held: self.pb.button_press(k)
        self.held = set(ks)
    def tick(self, n=1, *ks, shot=False):
        self.press(*ks)
        for i in range(n):
            self.pb.tick(1, shot and i == n-1, False)
        if shot: self.imgs.append(self.pb.screen.image.copy().convert("RGB"))
    def obj(self, i):
        b = bytes(self.pb.memory[self.GO+i*20:self.GO+i*20+20])
        return dict(active=b[0], id=b[1], sub=b[2], x=struct.unpack("<h", b[5:7])[0], y=struct.unpack("<h", b[7:9])[0],
                    bx=struct.unpack("<h", b[9:11])[0], by=struct.unpack("<h", b[11:13])[0], vx=struct.unpack("<h", b[13:15])[0],
                    vy=struct.unpack("<h", b[15:17])[0], state=b[17], timer=b[18], hp=b[19])
    def find(self, oid, near=None):
        best = None
        for i in range(24):
            o = self.obj(i)
            if o["active"] and o["id"] == oid:
                d = abs(o["x"] - near) if near is not None else 0
                if best is None or d < best[0]: best = (d, i, o)
        return best
    def save(self, path, scale=2, cols=4):
        if not self.imgs: return
        w, h = self.imgs[0].size
        rows = (len(self.imgs) + cols - 1) // cols
        W = Image.new("RGB", (w*scale*cols, h*scale*rows))
        for i, im in enumerate(self.imgs):
            W.paste(im.resize((w*scale, h*scale), Image.NEAREST), ((i % cols)*w*scale, (i//cols)*h*scale))
        W.save(path)
    def close(self): self.pb.stop(save=False)
