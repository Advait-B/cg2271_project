"""
Doomscroll Breaker - perfboard shield layout + 3D-printable enclosure generator.

Single source of truth for:
  * where every part sits on the perfboard shield (hole grid),
  * every wire (net) between them,
  * the printed case (base tray, top shell, LCD bezel, button plunger, LED light pipe,
    EC11 knob, fit-test coupon).

Rev 2 (30 Oct 2026): MPU-6050 removed, EC11 volume knob added (ESP32 IO33/IO35/IO34),
5050 RGB LED module (KY-009) with light pipe, roomier case that opens with 4 screws.

Run:   pip install manifold3d trimesh numpy
       python3 doomscroll_hw.py            -> writes ./out/*.stl and ./out/layout.json

All units are millimetres.  Frame = FRDM-MCXC444 seen from the top with the USB-C
ports on the LEFT:  +x to the right, +y towards the front (the J3/J4 header edge),
+z up, z = 0 is the top surface of the case floor.

!! Every dimension marked (MEASURE) was estimated from the NXP manual photo or
   typical module sizes.  Measure your real parts with calipers and edit them here
   before printing.  Print a test of just the base first.
"""
import json, math, os
import numpy as np
from manifold3d import Manifold, CrossSection, JoinType
import trimesh

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")

# --------------------------------------------------------------------------
# 1. Hole grid - locked to the FRDM Arduino headers J1 / J3 / J4
# --------------------------------------------------------------------------
P = 2.54
X0 = 59.69          # x of J4-2 (A0) = column 0          (MEASURE, from UM12120 photo)
Y0 = 2.90           # y of J1 outer row = row 0           (MEASURE)
def hx(c): return X0 + P * c
def hy(r): return Y0 + P * r
COL_MIN, COL_MAX = -20, 5     # 26 columns -> letters A..Z
ROW_MIN, ROW_MAX = -14, 20    # 35 rows    -> 1..35
def hole_name(c, r): return f"{chr(ord('A') + c - COL_MIN)}{r - ROW_MIN + 1}"

# --------------------------------------------------------------------------
# 2. Stack heights (z)
# --------------------------------------------------------------------------
FRDM_W, FRDM_D, FRDM_T = 91.3, 54.5, 1.6      # measured from UM12120 photo, scale locked to 2.54 mm header pitch
FRDM_Z = 5.0                                   # standoff height under the FRDM
FRDM_TOP = FRDM_Z + FRDM_T                     # 6.6
RECEPT_H = 8.5                                 # FRDM female header height (MEASURE)
MALE_PLASTIC = 2.5                             # plastic collar of the shield's male pins
SHIELD_Z = FRDM_TOP + RECEPT_H + MALE_PLASTIC  # 17.6 shield underside
SHIELD_T = 1.6
SHIELD_TOP = SHIELD_Z + SHIELD_T               # 19.2
MOD_Z = SHIELD_TOP + 2.5                       # kit modules lie flat on their bent header collar
MOD_T = 1.6
ESP_SOCKET_H = 8.5                             # 1x21 female headers under the ESP32
ESP_Z = SHIELD_TOP + ESP_SOCKET_H              # 27.7
LID_UNDER, LID_TOP = 38.5, 41.0                # 6 mm clear above the ESP32 (was 3 mm)
SPLIT = 13.0                                   # base walls end / top shell starts
BEZEL_TOP = SPLIT + 2.4                        # 15.4

SHIELD = dict(x0=7.0, x1=74.5, y0=-34.5, y1=55.0)   # cut from a 7x9 cm double-sided board

# --------------------------------------------------------------------------
# 3. Parts on the shield
# --------------------------------------------------------------------------
def esp_col(k):            # ESP32-S2-DevKitM-1U pin k (1..21), USB end towards -x
    return 2 - k
ESP_J1 = ["3V3", "IO0", "IO1", "IO2", "IO3", "IO4", "IO5", "IO6", "IO7", "IO8", "IO9", "IO10",
          "IO11", "IO12", "IO13", "IO14", "IO15", "IO16", "IO17", "5V", "GND"]
ESP_J3 = ["GND", "RST", "IO46", "IO45", "RX", "TX", "IO42", "IO41", "IO40", "IO39", "IO38",
          "IO37", "IO36", "IO35", "IO34", "IO33", "IO26", "IO21", "IO20", "IO19", "IO18"]

parts = []
def part(**kw): parts.append(kw); return kw

# --- under-side male headers that plug into the FRDM ----------------------
J1_FUNCS = {2: "PTA1 D0 (console)", 4: "PTA2 D1 (console)", 6: "PTD3 D2 (LCD)", 8: "PTA12 D3",
            10: "PTA4 D4 (SW3)", 12: "PTA5 D5", 14: "PTE29 D6", 16: "PTE30 D7"}
J3_FUNCS = {2: "NC", 4: "GND", 6: "RESET", 8: "P3V3", 10: "P5V0", 12: "GND", 14: "GND", 16: "VIN"}
J4_FUNCS = {1: "PTE20 (LCD)", 2: "PTB0 A0", 3: "PTE21 (LCD)", 4: "PTB1 A1", 5: "PTE22 UART2_TX",
            6: "PTB2 A2", 7: "PTE23 UART2_RX", 8: "PTB3 A3", 9: "PTE0", 10: "PTC2 A4",
            11: "PTE30", 12: "PTC1 A5"}
part(id="J1", name="J1 header (1x8 male, under shield)", kind="header", side="bottom",
     pins=[(f"J1-{k}", 6 - k // 2, 0, J1_FUNCS[k]) for k in range(2, 17, 2)])
part(id="J2", name="J2-2 single pin (bend ~1 mm towards +x)", kind="header", side="bottom",
     pins=[("J2-2", -4, 0, "PTA13 D8 - off-grid by 1.5 mm")])
part(id="J3", name="J3 header (1x8 male, under shield)", kind="header", side="bottom",
     pins=[(f"J3-{k}", k // 2 - 10, 19, J3_FUNCS[k]) for k in range(2, 17, 2)])
part(id="J4", name="J4 header (2x6 male, under shield)", kind="header", side="bottom",
     pins=[(f"J4-{k}", (k // 2 - 1) if k % 2 == 0 else (k - 1) // 2, 19 if k % 2 == 0 else 18, J4_FUNCS[k])
           for k in range(1, 13)])

# --- ESP32 in two 1x21 female sockets --------------------------------------
esp_cx = (hx(esp_col(1)) + hx(esp_col(21))) / 2
esp_cy = (hy(-3) + hy(-12)) / 2
ESP_L, ESP_W = 54.0, 25.4                       # Espressif: 54 x 25.4 mm
part(id="esp", name="ESP32-S2-DevKitM-1U (in 2x 1x21 female sockets)", kind="esp32", side="top",
     body=dict(x0=esp_cx - ESP_L / 2, x1=esp_cx + ESP_L / 2, y0=esp_cy - ESP_W / 2, y1=esp_cy + ESP_W / 2,
               z=ESP_Z, t=1.6),
     pins=[(f"J1-{k} {ESP_J1[k-1]}", esp_col(k), -3, ESP_J1[k - 1]) for k in range(1, 22)] +
          [(f"J3-{k} {ESP_J3[k-1]}", esp_col(k), -12, ESP_J3[k - 1]) for k in range(1, 22)])

# --- Joystick KY-023 -------------------------------------------------------
JOY_PINS = ["GND", "+5V", "VRx", "VRy", "SW"]
part(id="joy", name="Joystick KY-023", kind="joystick", side="top",
     body=dict(x0=hx(-15) - 13, x1=hx(-15) + 13, y0=hy(1) + 1.5, y1=hy(1) + 35.5, z=MOD_Z, t=MOD_T),
     pins=[(JOY_PINS[i], -17 + i, 1, JOY_PINS[i]) for i in range(5)], color="#1d1d1d",
     feature=dict(x=hx(-15), y=hy(1) + 20.0))
# --- RGB LED KY-016 --------------------------------------------------------
LED_PINS = ["R", "G", "B", "GND"]
part(id="led", name="5050 RGB LED KY-009", kind="led5050", side="top",
     body=dict(x0=hx(-9) + 1.5, x1=hx(-9) + 20.0, y0=(hy(2) + hy(5)) / 2 - 7.5, y1=(hy(2) + hy(5)) / 2 + 7.5,
               z=MOD_Z, t=MOD_T),
     pins=[(LED_PINS[i], -9, 2 + i, LED_PINS[i]) for i in range(4)], color="#1d1d1d",
     feature=dict(x=hx(-9) + 10.75, y=(hy(2) + hy(5)) / 2))
# --- Button KY-004 ---------------------------------------------------------
part(id="btn", name="Button KY-004", kind="button", side="top",
     body=dict(x0=hx(-9) + 1.5, x1=hx(-9) + 20.0, y0=hy(10) - 7.5, y1=hy(10) + 7.5, z=MOD_Z, t=MOD_T),
     pins=[("S", -9, 9, "S"), ("+", -9, 10, "+"), ("-", -9, 11, "-")], color="#1d1d1d",
     feature=dict(x=hx(-9) + 12.0, y=hy(10)))
# --- Passive buzzer KY-006 -------------------------------------------------
part(id="buz", name="Passive buzzer KY-006", kind="buzzer", side="top",
     body=dict(x0=hx(-9) + 1.5, x1=hx(-9) + 20.0, y0=hy(17) - 7.5, y1=hy(17) + 7.5, z=MOD_Z, t=MOD_T),
     pins=[("S", -9, 16, "S"), ("+", -9, 17, "+ (NC)"), ("-", -9, 18, "-")], color="#1d1d1d",
     feature=dict(x=hx(-9) + 12.0, y=hy(17)))
# --- Big sound sensor KY-037 ----------------------------------------------
SND_PINS = ["AO", "G", "+", "DO"]
sx = (hx(1) + hx(4)) / 2
part(id="snd", name="Big sound sensor KY-037", kind="sound", side="top",
     body=dict(x0=sx - 7.5, x1=sx + 7.5, y0=hy(1) + 1.5, y1=hy(1) + 45.5, z=MOD_Z, t=MOD_T),
     pins=[(SND_PINS[i], 1 + i, 1, SND_PINS[i]) for i in range(4)], color="#1d1d1d",
     feature=dict(x=sx, y=hy(1) + 40.0))
# --- 1x3 header for the panel-mounted USB-C (laptop / HID) -----------------
part(id="usb", name="1x3 header -> panel USB-C breakout (HID cable)", kind="header", side="top",
     pins=[("D-", -15, -14, "USB D-"), ("D+", -14, -14, "USB D+"), ("GND", -13, -14, "GND")])

# --- 1x4 header for the lid-mounted EC11 volume knob (unplugs when the lid comes off)
part(id="enc_hdr", name="1x4 header -> EC11 knob harness", kind="header", side="top",
     pins=[("B", -12, -14, "EC11 B (DT)"), ("A", -11, -14, "EC11 A (CLK)"), ("SW", -10, -14, "EC11 switch"), ("GND", -9, -14, "EC11 C + switch common")])

# sanity: unique holes, inside shield
used = {}
for p in parts:
    for (lbl, c, r, fn) in p["pins"]:
        key = (c, r)
        assert key not in used, f"hole clash {hole_name(c,r)}: {used[key]} vs {p['id']}.{lbl}"
        used[key] = f"{p['id']}.{lbl}"
        assert SHIELD["x0"] + 1 < hx(c) < SHIELD["x1"] - 1 and SHIELD["y0"] + 1 < hy(r) < SHIELD["y1"] - 1, (p["id"], lbl)

# --------------------------------------------------------------------------
# 4. Nets (wires on the solder side)
# --------------------------------------------------------------------------
def esp(lbl):
    for (l, c, r, fn) in next(p for p in parts if p["id"] == "esp")["pins"]:
        if l.endswith(" " + lbl) or l == lbl: return ("esp", l)
    raise KeyError(lbl)
NETS = [
    dict(name="GND", color="#222222", nodes=[("J3", "J3-12"), ("J3", "J3-4"), esp("GND"), ("esp", "J3-1 GND"),
         ("enc_hdr", "GND"), ("joy", "GND"), ("snd", "G"), ("led", "GND"), ("btn", "-"), ("buz", "-"), ("usb", "GND")]),
    dict(name="MCX 3V3", color="#d62828", nodes=[("J3", "J3-8"), ("joy", "+5V"), ("snd", "+"), ("btn", "+")]),
    dict(name="Joystick VRy -> A0 (PTB0)", color="#2a9d8f", nodes=[("J4", "J4-2"), ("joy", "VRy")]),
    dict(name="LED R -> PTA5", color="#e63946", nodes=[("J1", "J1-12"), ("led", "R")]),
    dict(name="LED G -> PTE30", color="#38b000", nodes=[("J1", "J1-16"), ("led", "G")]),
    dict(name="LED B -> PTB1", color="#3a86ff", nodes=[("J4", "J4-4"), ("led", "B")]),
    dict(name="Buzzer -> PTB2 (TPM2)", color="#9d4edd", nodes=[("J4", "J4-6"), ("buz", "S")]),
    dict(name="Clap DO -> PTA12", color="#ffb703", nodes=[("J1", "J1-8"), ("snd", "DO")]),
    dict(name="Button -> PTA13", color="#fb6f92", nodes=[("J2", "J2-2"), ("btn", "S")]),
    dict(name="UART PTE22 TX -> ESP IO2", color="#00b4d8", nodes=[("J4", "J4-5"), ("esp", "J1-4 IO2")]),
    dict(name="UART PTE23 RX <- ESP IO1", color="#0077b6", nodes=[("J4", "J4-7"), ("esp", "J1-3 IO1")]),
    dict(name="Encoder A -> IO33", color="#80ed99", nodes=[("esp", "J3-16 IO33"), ("enc_hdr", "A")]),
    dict(name="Encoder B -> IO35", color="#57cc99", nodes=[("esp", "J3-14 IO35"), ("enc_hdr", "B")]),
    dict(name="Encoder SW -> IO34", color="#f4a261", nodes=[("esp", "J3-15 IO34"), ("enc_hdr", "SW")]),
    dict(name="USB D- (IO19)", color="#ffffff", nodes=[("esp", "J3-20 IO19"), ("usb", "D-")]),
    dict(name="USB D+ (IO20)", color="#90e0ef", nodes=[("esp", "J3-19 IO20"), ("usb", "D+")]),
    # optional extras that unlock the software upgrades (dashed in the viewer)
    dict(name="(optional) Mic AO -> A3 (PTB3)", color="#ffd166", optional=True, nodes=[("J4", "J4-8"), ("snd", "AO")]),
    dict(name="(optional) Joystick SW -> PTC1", color="#ef476f", optional=True, nodes=[("J4", "J4-12"), ("joy", "SW")]),
    dict(name="(optional) Joystick VRx -> A4 (PTC2)", color="#06d6a0", optional=True, nodes=[("J4", "J4-10"), ("joy", "VRx")]),
]

def pin_lookup(pid, lbl):
    p = next(p for p in parts if p["id"] == pid)
    for (l, c, r, fn) in p["pins"]:
        if l == lbl: return c, r
    raise KeyError((pid, lbl))

for n in NETS:   # minimum spanning tree in Manhattan metric -> wire list
    pts = [pin_lookup(*nd) for nd in n["nodes"]]
    inT, edges = {0}, []
    while len(inT) < len(pts):
        best = None
        for i in inT:
            for j in range(len(pts)):
                if j in inT: continue
                d = abs(pts[i][0] - pts[j][0]) + abs(pts[i][1] - pts[j][1])
                if best is None or d < best[0]: best = (d, i, j)
        inT.add(best[2]); edges.append((best[1], best[2]))
    n["holes"] = [hole_name(*p) for p in pts]
    n["pts"] = pts
    n["edges"] = edges

# --------------------------------------------------------------------------
# 4b. Top-side jumper-wire routing (A* on the hole grid)
#     Wires lie on the component side and are tack-soldered onto each pin's joint.
# --------------------------------------------------------------------------
import heapq
BACK_POST_CELLS = [(-20, -14), (5, -14)]
def route_nets():
    blocked = set(used.keys())
    for (pc, pr) in BACK_POST_CELLS:                       # screw bosses land here
        for dc in (-1, 0, 1):
            for dr in (-1, 0, 1): blocked.add((pc + dc, pr + dr))
    occ = {}                                                # cell -> list of net idx
    under = {}                                              # cells hidden under a module body
    for pp in parts:
        b = pp.get("body")
        if not b: continue
        for c in range(COL_MIN, COL_MAX + 1):
            for r in range(ROW_MIN, ROW_MAX + 1):
                if b["x0"] - 0.5 < hx(c) < b["x1"] + 0.5 and b["y0"] - 0.5 < hy(r) < b["y1"] + 0.5:
                    under[(c, r)] = 1.2 if pp["id"] == "esp" else 2.2
    landing_taken = set()
    def inb(c, r): return COL_MIN <= c <= COL_MAX and ROW_MIN <= r <= ROW_MAX
    def free_nbrs(cell):
        c, r = cell
        out = []
        for dc, dr in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            q = (c + dc, r + dr)
            if inb(*q) and q not in blocked and q not in landing_taken and q not in occ: out.append(q)
        return out
    def astar(sources, targets, ni):
        targets = set(targets)
        pq, best, prev = [], {}, {}
        for s in sources:
            st = (s, None); best[st] = 0; heapq.heappush(pq, (0, 0, s, None))
        tick = 0
        while pq:
            f, g, cell, d = heapq.heappop(pq)
            if g > best.get((cell, d), 1e9): continue
            if cell in targets:
                path = [cell]; key = (cell, d)
                while key in prev: key = prev[key]; path.append(key[0])
                return path[::-1]
            c, r = cell
            for dd in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (c + dd[0], r + dd[1])
                if not inb(*q): continue
                if q not in targets and (q in blocked or q in landing_taken): continue
                cost = 1.0 + (1.6 if (d is not None and dd != d) else 0) + under.get(q, 0)
                others = [k for k in occ.get(q, []) if k != ni]
                if others: cost += 7.0 + (12.0 if [k for k in occ.get(cell, []) if k != ni] else 0)
                ng = g + cost
                if ng < best.get((q, dd), 1e9):
                    best[(q, dd)] = ng; prev[(q, dd)] = (cell, d)
                    h = min(abs(q[0] - t[0]) + abs(q[1] - t[1]) for t in targets)
                    tick += 1
                    heapq.heappush(pq, (ng + h, ng, q, dd))
        raise RuntimeError("no route")
    order = sorted(range(len(NETS)), key=lambda i: (NETS[i].get("optional", False), NETS[i]["name"] in ("GND", "MCX 3V3"),
                                                    sum(abs(NETS[i]["pts"][a][0] - NETS[i]["pts"][b][0]) + abs(NETS[i]["pts"][a][1] - NETS[i]["pts"][b][1]) for a, b in NETS[i]["edges"])))
    for ni in order:
        n = NETS[ni]
        pins = n["pts"]
        landing = [tuple(pt) for pt in pins]   # wires are tack-soldered straight onto each pin's joint
        tree, segs = set(), []
        seq = [0] + [b for (a, b) in n["edges"]]
        for k, idx in enumerate(seq):
            if k == 0: continue
            if k == 1:
                path = astar([landing[seq[0]]], [landing[idx]], ni)
            else:   # daisy-chain: join the nearest pin that is already wired
                done = [landing[q] for q in seq[:k]]
                path = astar([landing[idx]], done, ni)[::-1]
            cells = []
            for cell in path:
                layer = len([k2 for k2 in occ.get(cell, []) if k2 != ni]) if cell not in landing else 0
                cells.append([cell[0], cell[1], layer])
            tree.update(path)
            segs.append(dict(cells=cells, end_landing=(k == 1)))
            for cell in path:
                occ.setdefault(cell, [])
                if ni not in occ[cell]: occ[cell].append(ni)
        n["segments"] = segs
        n["wire_mm"] = round(sum(len(s["cells"]) - 1 for s in segs) * P + 6 * len(segs), 0)
route_nets()

# --------------------------------------------------------------------------
# 5. Enclosure (rev 2)
#    base tray (floor + walls to SPLIT) | top shell (walls + lid) | LCD ledge bezel
#    Opening: 4 x M3x25 from underneath hold the top shell; 2 x M3x8 CSK hold the bezel.
# --------------------------------------------------------------------------
WALL, FLOOR = 2.4, 2.4
CLR = 0.3                                   # printing clearance for every sliding / plug fit
IN = dict(x0=-2.0, x1=98.8, y0=-43.0, y1=65.5)          # inner cavity (>=1.5 mm round boards, 6 mm above)
OUTB = dict(x0=IN["x0"] - WALL, x1=IN["x1"] + WALL, y0=IN["y0"] - WALL, y1=IN["y1"] + WALL)
RISER_X0, RISER_X1 = 75.5, 77.9                          # step between tall section and LCD ledge
SPLIT = 15.0                                             # base walls end / top shell starts (3 mm above USB slots)
BEZEL_TOP = SPLIT + 2.4
CORNER_R = 4.0
SEG = 48

def box(x0, x1, y0, y1, z0, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])
def rbox(x0, x1, y0, y1, z0, z1, r=CORNER_R):
    r = min(r, (x1 - x0) / 2 - 0.01, (y1 - y0) / 2 - 0.01)
    cs = CrossSection.square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]).translate([x0 + r, y0 + r])
    cs = cs.offset(r, JoinType.Round, circular_segments=SEG)
    return Manifold.extrude(cs, z1 - z0).translate([0, 0, z0])
def cyl(x, y, z0, z1, r, r2=None, seg=SEG):
    return Manifold.cylinder(z1 - z0, r, r if r2 is None else r2, seg).translate([x, y, z0])
def slot_x(xa, xb, y, z, w, h):
    """stadium-shaped hole running along x (wall ports): w along y, h along z"""
    r = min(w, h) / 2
    def c(yy, zz):
        return Manifold.cylinder(xb - xa, r, r, SEG).rotate([0, 90, 0]).translate([xa, yy, zz])
    if w >= h: return Manifold.hull(c(y - w / 2 + r, z) + c(y + w / 2 - r, z))
    return Manifold.hull(c(y, z - h / 2 + r) + c(y, z + h / 2 - r))
def rrect_hole(cx, cy, w, h, z0, z1, r=1.0, chamfer=0.0):
    m = rbox(cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2, z0, z1, r)
    if chamfer > 0:
        top = rbox(cx - w / 2 - chamfer, cx + w / 2 + chamfer, cy - h / 2 - chamfer, cy + h / 2 + chamfer,
                   z1 - 0.001, z1 + 0.01, r + chamfer)
        m = Manifold.hull(m.trim_by_plane([0, 0, -1], -(z1 - chamfer)) + top) + m
    return m

# FRDM: mounting holes (rubber-foot positions, measured) - PEEL THE 4 RUBBER FEET OFF
FRDM_MH = [(4.05, 3.1), (87.25, 3.1), (4.05, 51.6), (87.25, 51.6)]
USB_C = dict(w=8.94, h=3.26)                                   # USB-C receptacle face (spec)
PORT_J13 = dict(y=15.0, z=FRDM_TOP + 1.63, w=13.0, h=7.5)      # MCU-Link USB-C  (power + flash)  measured
PORT_J12 = dict(y=29.6, z=FRDM_TOP + 1.63, w=13.0, h=7.5)      # MCU USB-C (spare)               measured
ESP_X0 = esp_cx - ESP_L / 2
PORT_ESP = dict(y=esp_cy, z=ESP_Z + 1.6 + 1.4, w=12.5, h=8.5)  # ESP32 micro-USB (flash / debug)
PORT_HID = dict(y=-29.5, z=SHIELD_TOP + 1.6 + 1.63, w=9.8, h=4.0)  # panel USB-C breakout -> laptop
SMA = dict(x=58.0, z=28.0, d=6.6)                              # optional U.FL->SMA bulkhead, back wall
RECESS = 1.0                                                   # outer wall thinned to 1.4 mm round ports

BACK_POSTS = [(hx(-20), hy(-14)), (hx(5), hy(-14))]            # shield screwed down here (drill A1 + Z1 to 3.2 mm)
SHELL_SCREWS = [(1.0, -40.0), (72.5, -40.0), (1.0, 62.5), (72.5, 62.5)]    # bosses overlap the walls by 0.5 mm
BEZEL_SCREWS = [(95.8, 62.5), (95.8, -40.0)]
BOSS_R = 3.5

def get(pid): return next(p for p in parts if p["id"] == pid)
JOY = get("joy")["feature"]; LEDF = get("led")["feature"]; BTNF = get("btn")["feature"]
BUZF = get("buz")["feature"]; MICF = get("snd")["feature"]
LCD_WIN = dict(cx=82.4, cy=36.05, w=10.0, h=23.5)              # over DS1 glass (78.3-86.5 x 25.2-46.9)
ENC = dict(x=27.0, y=53.0, hole=7.5)                           # EC11, M7x0.75 bushing through the lid
EC11 = dict(body_w=13.4, body_d=12.4, body_h=6.5, pins=5.0, bush_r=3.5, bush_l=7.0, shaft_r=3.0, shaft_l=20.0, nut_af=9.5, nut_t=2.0)

# switch / plunger / light pipe
SW_TOP = MOD_Z + MOD_T + 5.0                                   # 6x6x5 tact switch on KY-004
PLUNGER = dict(r_shaft=4.0, r_flange=5.5, flange_t=1.5, z0=SW_TOP + 0.3, top=LID_TOP + 2.5)
SLEEVE_BOTTOM = PLUNGER["z0"] + PLUNGER["flange_t"] + 1.5
LED_TOP = MOD_Z + MOD_T + 1.6                                  # 5050 package height
PIPE = dict(r=4.5, cap_r=6.0, cap_t=1.0, z0=LED_TOP + 2.1)
SHROUD_BOTTOM = LED_TOP + 1.5

def grille(cx, cy, z0, z1, ring_r=6.0, hole_r=0.9):
    m = cyl(cx, cy, z0, z1, hole_r, seg=16)
    for ring, n in ((ring_r / 2, 6), (ring_r, 12)):
        for i in range(n):
            a = 2 * math.pi * i / n
            m += cyl(cx + ring * math.cos(a), cy + ring * math.sin(a), z0, z1, hole_r, seg=16)
    return m

def port_profile(y, z, w, h, roof):
    """rounded-rect cross-section (in the y-z plane) whose ROOF side is square, so the
    printed top of the hole is a straight bridge instead of an overhanging arc.
    roof = +1: roof at +z (part printed upright); -1: roof at -z (part printed upside-down)"""
    r = min(1.6, h / 2 - 0.01)
    cs = CrossSection.square([w - 2 * r, h - 2 * r]).translate([y - w / 2 + r, z - h / 2 + r]).offset(r, JoinType.Round, circular_segments=24)
    sq = CrossSection.square([w, h / 2]).translate([y - w / 2, z if roof > 0 else z - h / 2])
    return cs + sq
def port(p, xa, xb, roof):
    cs = port_profile(p["y"], p["z"], p["w"], p["h"], roof)
    return Manifold.extrude(cs, xb - xa).transform([[0, 0, 1, xa], [1, 0, 0, 0], [0, 1, 0, 0]])   # (y,z,x) -> rotation
def port_leadin(p, roof, c=0.8):
    """45-degree lead-in on the outside of a wall port (sides + floor only, roof stays flat)"""
    a = Manifold.extrude(port_profile(p["y"], p["z"], p["w"], p["h"], roof), 0.01).transform([[0, 0, 1, OUTB["x0"] + c], [1, 0, 0, 0], [0, 1, 0, 0]])
    zc = p["z"] - roof * c / 2
    b = Manifold.extrude(port_profile(p["y"], zc, p["w"] + 2 * c, p["h"] + c, roof), 0.01).transform([[0, 0, 1, OUTB["x0"] - 0.01], [1, 0, 0, 0], [0, 1, 0, 0]])
    return Manifold.hull(a + b)

def make_base():
    b = rbox(OUTB["x0"], OUTB["x1"], OUTB["y0"], OUTB["y1"], -FLOOR, SPLIT)
    b -= rbox(IN["x0"], IN["x1"], IN["y0"], IN["y1"], 0, SPLIT + 1, 1.6)
    # FRDM standoffs (M3 self-tapping, optional) + corner locating ribs (board drops in, can't slide)
    for (x, y) in FRDM_MH:
        b += cyl(x, y, 0, FRDM_Z, 3.2)
        b -= cyl(x, y, 1.0, FRDM_Z + 0.1, 1.25)
    g, t, arm, top = 0.4, 1.2, 7.0, FRDM_TOP + 1.2
    for (cx, sx) in ((0.0, -1), (FRDM_W, 1)):
        for (cy, sy) in ((0.0, -1), (FRDM_D, 1)):
            xo = cx + sx * g; yo = cy + sy * g
            xa, xb = sorted([xo, xo + sx * t]); ya, yb = sorted([yo, yo + sy * t])
            b += box(xa, xb, min(ya, yb, cy - sy * arm), max(ya, yb, cy - sy * arm), 0, top)
            b += box(min(xa, xb, cx - sx * arm), max(xa, xb, cx - sx * arm), ya, yb, 0, top)
    # shield posts (M3x8 from the top through the shield)
    for (x, y) in BACK_POSTS:
        b += cyl(x, y, 0, SHIELD_Z - 0.1, 3.5)
        b -= cyl(x, y, SHIELD_Z - 10.5, SHIELD_Z + 1, 1.25)
    # bosses for the top-shell screws (M3x25 from underneath, countersunk)
    for (x, y) in SHELL_SCREWS:
        b += cyl(x, y, 0, SPLIT, BOSS_R)
        b -= cyl(x, y, -FLOOR - 1, SPLIT + 1, 1.7)
        b -= cyl(x, y, -FLOOR - 0.01, -FLOOR + 1.6, 3.3, 1.7)
    # bosses for the bezel screws (from the top)
    for (x, y) in BEZEL_SCREWS:
        b += cyl(x, y, 0, SPLIT, BOSS_R)
        b -= cyl(x, y, 3.0, SPLIT + 1, 1.25)
    for p in (PORT_J13, PORT_J12):
        b -= port(p, OUTB["x0"] - 1, IN["x0"] + 1, +1)
    for (x, y) in [(14, -33), (84, -33), (14, 56), (84, 56)]:   # 10 mm rubber-foot recesses
        b -= cyl(x, y, -FLOOR - 0.01, -FLOOR + 1.0, 5.3)
    return b

def make_top():
    t = rbox(OUTB["x0"], RISER_X1, OUTB["y0"], OUTB["y1"], SPLIT, LID_TOP)
    t += box(RISER_X1 - CORNER_R, RISER_X1, OUTB["y0"], OUTB["y1"], SPLIT, LID_TOP)
    t -= rbox(IN["x0"], RISER_X0, IN["y0"], IN["y1"], SPLIT - 1, LID_UNDER, 1.6)
    t -= box(RISER_X0 - 0.01, RISER_X1 + 1, OUTB["y0"] - 1, OUTB["y1"] + 1, SPLIT - 1, BEZEL_TOP)  # sits on bezel
    for (x, y) in SHELL_SCREWS:
        t += cyl(x, y, SPLIT, LID_UNDER + 0.1, BOSS_R)
        t -= cyl(x, y, SPLIT - 1, SPLIT + 14, 1.25)
    # locating tabs that drop 2.5 mm inside the base walls
    tab = 2.5
    for (x0, x1, y0, y1, wx0, wx1, wy0, wy1) in [
            (30, 42, IN["y0"] + CLR, IN["y0"] + CLR + 1.2, 30, 42, IN["y0"] - 0.5, IN["y0"] + CLR + 1.2),
            (30, 42, IN["y1"] - CLR - 1.2, IN["y1"] - CLR, 30, 42, IN["y1"] - CLR - 1.2, IN["y1"] + 0.5),
            (IN["x0"] + CLR, IN["x0"] + CLR + 1.2, -2, 8, IN["x0"] - 0.5, IN["x0"] + CLR + 1.2, -2, 8)]:
        t += box(x0, x1, y0, y1, SPLIT - tab, SPLIT + 1.5)          # finger that drops inside the base wall
        t += box(wx0, wx1, wy0, wy1, SPLIT + 0.01, LID_UNDER + 0.1) # full-height rib: no overhang when printed
    # joystick
    t -= cyl(JOY["x"], JOY["y"], LID_UNDER - 1, LID_TOP + 1, 12.5)
    t -= cyl(JOY["x"], JOY["y"], LID_TOP - 1.2, LID_TOP + 0.01, 12.5, 13.7)
    # button sleeve
    t += cyl(BTNF["x"], BTNF["y"], SLEEVE_BOTTOM, LID_UNDER + 0.1, 6.2)
    t -= cyl(BTNF["x"], BTNF["y"], SLEEVE_BOTTOM - 1, LID_TOP + 1, PLUNGER["r_shaft"] + CLR)
    # 5050 LED light pipe: shroud down to 1.5 mm above the LED, counterbore for the cap
    t += cyl(LEDF["x"], LEDF["y"], SHROUD_BOTTOM, LID_UNDER + 0.1, 6.5)
    t -= cyl(LEDF["x"], LEDF["y"], SHROUD_BOTTOM - 1, LID_TOP + 1, PIPE["r"] + CLR)
    # EC11 knob bushing
    t -= cyl(ENC["x"], ENC["y"], LID_UNDER - 1, LID_TOP + 1, ENC["hole"] / 2)
    # sound grilles
    t -= grille(MICF["x"], MICF["y"], LID_UNDER - 1, LID_TOP + 1, 4.5, 0.9)
    t -= grille(BUZF["x"], BUZF["y"], LID_UNDER - 1, LID_TOP + 1, 5.5, 0.9)
    # left-wall ports
    for p in (PORT_ESP, PORT_HID):
        t -= port(p, OUTB["x0"] - 1, IN["x0"] + 1, -1)
    # SMA bulkhead hole as a teardrop pointing at -z (= up while the shell prints upside-down)
    rr = SMA["d"] / 2
    drop = CrossSection.circle(rr, 40) + CrossSection([[(-rr * 0.7071, -rr * 0.7071), (0, -rr * 1.4142), (rr * 0.7071, -rr * 0.7071)]])
    t -= Manifold.extrude(drop.translate([SMA["x"], SMA["z"]]), WALL + 2).transform([[1, 0, 0, 0], [0, 0, -1, OUTB["y0"] + WALL + 1], [0, 1, 0, 0]])
    t -= box(RISER_X0 - 0.01, RISER_X1 + 1, OUTB["y0"] - 1, OUTB["y1"] + 1, SPLIT - 1, BEZEL_TOP)  # keep the bezel seat clear
    return t

def make_bezel():
    z0, z1 = SPLIT, BEZEL_TOP
    bz = rbox(RISER_X0, OUTB["x1"], OUTB["y0"], OUTB["y1"], z0, z1)
    bz += box(RISER_X0, RISER_X0 + CORNER_R, OUTB["y0"], OUTB["y1"], z0, z1)
    bz -= rrect_hole(LCD_WIN["cx"], LCD_WIN["cy"], LCD_WIN["w"], LCD_WIN["h"], z0 - 1, z1, 1.0, chamfer=1.4)
    for (x, y) in BEZEL_SCREWS:
        bz -= cyl(x, y, z0 - 1, z1 + 1, 1.7)
        bz -= cyl(x, y, z1 - 1.6, z1 + 0.01, 1.7, 3.3)
    return bz

def make_plunger():
    p = PLUNGER
    L_ = p["top"] - p["z0"]
    m = cyl(0, 0, 0, p["flange_t"], p["r_flange"])
    m += cyl(0, 0, p["flange_t"], L_ - 0.6, p["r_shaft"])
    m += cyl(0, 0, L_ - 0.6, L_, p["r_shaft"], p["r_shaft"] - 0.6)
    return m

def make_lightpipe():          # modelled cap-up (assembled); printed cap-down
    m = cyl(0, 0, 0, LID_TOP - PIPE["z0"], PIPE["r"])
    m += cyl(0, 0, LID_TOP - PIPE["z0"], LID_TOP - PIPE["z0"] + PIPE["cap_t"], PIPE["cap_r"])   # domed-look cap sits on the lid
    return m

def make_knob(kind="D"):
    """EC11 knob, 20 mm, modelled upright; printed top-down. kind: 'D' (6 mm D-shaft, 4.5 flat) or 'knurl' (18T)"""
    H, R, bore_d = 15.0, 10.0, 12.5
    m = cyl(0, 0, 0, H - 1.0, R, seg=72) + cyl(0, 0, H - 1.0, H, R, R - 1.0, seg=72)
    for i in range(24):                                    # grip flutes
        a = 2 * math.pi * i / 24
        m -= cyl(R * math.cos(a), R * math.sin(a), -1, H + 1, 0.9, seg=12)
    m -= Manifold.hull(box(-0.8, 0.8, R - 4.5, R + 1, H, H + 1) + box(-0.01, 0.01, R - 4.5, R + 1, H - 0.8, H))   # V pointer groove
    if kind == "D":
        bore = cyl(0, 0, -1, bore_d, 3.0 + 0.15, seg=40)
        bore -= box(1.5 + 0.15, 5, -5, 5, -2, bore_d + 1)  # flat at 4.5 mm (+clearance)
    else:
        bore = cyl(0, 0, -1, bore_d, 3.0 - 0.05, seg=40)   # press-fit onto knurls
    m -= bore
    m -= cyl(0, 0, -0.01, 0.8, 3.0 + 0.9, 3.0 + 0.1, seg=40)   # lead-in chamfer
    return m

def make_coupon():
    """15-minute fit test: lid holes + a slice of the port wall + one screw pilot."""
    T = LID_TOP - LID_UNDER
    plate = rbox(0, 78, 0, 24, 0, T, 2.0)
    holes = [(10, 12, ENC["hole"] / 2), (26, 12, PLUNGER["r_shaft"] + CLR), (42, 12, PIPE["r"] + CLR)]
    for (x, y, r) in holes: plate -= cyl(x, y, -1, T + 1, r)
    plate += cyl(58, 12, 0, 8, BOSS_R); plate -= cyl(58, 12, 1.0, 9, 1.25)      # M3 self-tap test
    wall = box(0, 78, 24 - WALL, 24, 0, 18)
    for (x, z, w, h) in [(14, 9, PORT_J13["w"], PORT_J13["h"]), (36, 9, PORT_ESP["w"], PORT_ESP["h"]), (57, 9, PORT_HID["w"], PORT_HID["h"])]:
        wall -= Manifold.extrude(port_profile(x, z, w, h, +1), WALL + 2).transform([[1, 0, 0, 0], [0, 0, -1, 25], [0, 1, 0, 0]])
    return plate + wall

# --------------------------------------------------------------------------
# 6. Hardware keep-out model (what is physically inside) - used to prove nothing clashes
# --------------------------------------------------------------------------
def stadium_x(xa, xb, y, z, w, h): return slot_x(xa, xb, y, z, w, h)
def hardware():
    H = {}
    F = box(0, FRDM_W, 0, FRDM_D, FRDM_Z, FRDM_TOP)
    for p in (PORT_J13, PORT_J12):
        F += box(-1.4, 6.2, p["y"] - USB_C["w"] / 2, p["y"] + USB_C["w"] / 2, FRDM_TOP, FRDM_TOP + USB_C["h"])
    for (x0, x1, y0, y1) in [(hx(-2) - 1.27, hx(5) + 1.27, hy(0) - 1.27, hy(1) + 1.27),          # J1
                             (27.69 - 1.27, 50.55 + 1.27, hy(0) - 1.27, hy(1) + 1.27),          # J2
                             (hx(-9) - 1.27, hx(-2) + 1.27, hy(18) - 1.27, hy(19) + 1.27),      # J3
                             (hx(0) - 1.27, hx(5) + 1.27, hy(18) - 1.27, hy(19) + 1.27),        # J4
                             (53.8 - 1.27, 71.4 + 1.27, 15 - 1.27, 15 + 1.27), (53.8 - 1.27, 71.4 + 1.27, 37.8 - 1.27, 37.8 + 1.27)]:
        F += box(x0, x1, y0, y1, FRDM_TOP, FRDM_TOP + RECEPT_H)
    F += box(76.7, 88.1, 21.7, 50.4, FRDM_TOP, FRDM_TOP + 2.6)                              # LCD
    for (x, y) in [(9.4, 4.0), (76.0, 4.0), (76.7, 48.3)]:
        F += box(x - 1.9, x + 1.9, y - 1.9, y + 1.9, FRDM_TOP, FRDM_TOP + 2.3)              # SW1-3
    H["FRDM-MCXC444"] = F
    H["USB-C plugs (J13, J12)"] = sum((stadium_x(-30, -1.4, p["y"], p["z"], 12.4, 6.6) for p in (PORT_J13, PORT_J12)), Manifold())
    S_ = box(SHIELD["x0"], SHIELD["x1"], SHIELD["y0"], SHIELD["y1"], SHIELD_Z, SHIELD_TOP)
    for (x, y) in BACK_POSTS: S_ -= cyl(x, y, SHIELD_Z - 1, SHIELD_TOP + 1, 1.6)
    H["Shield"] = S_
    H["Shield screws M3x8"] = sum((cyl(x, y, SHIELD_TOP, SHIELD_TOP + 3.0, 2.75) for (x, y) in BACK_POSTS), Manifold())
    M_ = Manifold()
    for p in parts:
        b = p.get("body")
        if p["id"] == "esp": continue
        if b: M_ += box(b["x0"], b["x1"], b["y0"], b["y1"], b["z"], b["z"] + b["t"])
        if p["side"] == "top" and p["id"] not in ("esp",):
            for (_, c, r, _) in p["pins"]:
                M_ += box(hx(c) - 1.27, hx(c) + 1.27, hy(r) - 1.27, hy(r) + 1.27, SHIELD_TOP, MOD_Z)
    zt = MOD_Z + MOD_T
    M_ += box(JOY["x"] - 10.5, JOY["x"] + 10.5, JOY["y"] - 10.5, JOY["y"] + 10.5, zt, zt + 11.5)
    M_ += cyl(JOY["x"], JOY["y"], zt + 11.5, zt + 31.0, 8.6)                                # dome, stick, cap (tilts +-25 deg)
    M_ += box(LEDF["x"] - 2.5, LEDF["x"] + 2.5, LEDF["y"] - 2.5, LEDF["y"] + 2.5, zt, LED_TOP)
    M_ += box(BTNF["x"] - 3, BTNF["x"] + 3, BTNF["y"] - 3, BTNF["y"] + 3, zt, SW_TOP)
    M_ += cyl(BUZF["x"], BUZF["y"], zt, zt + 9.0, 6.1)
    sb = get("snd")["body"]
    M_ += cyl(MICF["x"], MICF["y"], zt, zt + 7.0, 5.0)
    M_ += box(MICF["x"] - 4.8, MICF["x"] + 4.8, sb["y0"] + 7, sb["y0"] + 16.5, zt, zt + 10.0)   # 3296W trimmer
    for hid in ("usb", "enc_hdr"):                                                         # Dupont plugs on J5/J6
        ps = get(hid)["pins"]; xs = [hx(q[1]) for q in ps]; ys = [hy(q[2]) for q in ps]
        M_ += box(min(xs) - 2.3, max(xs) + 2.3, min(ys) - 2.3, max(ys) + 2.3, SHIELD_TOP, SHIELD_TOP + 2.5 + 14.0)
    H["Kit modules + Dupont plugs"] = M_
    e = get("esp")["body"]
    E = box(e["x0"], e["x1"], e["y0"], e["y1"], ESP_Z, ESP_Z + 1.6)
    for r in (-3, -12): E += box(hx(-19) - 1.27, hx(1) + 1.27, hy(r) - 1.27, hy(r) + 1.27, SHIELD_TOP, ESP_Z)
    E += box(e["x1"] - 20.5, e["x1"] - 4.5, esp_cy - 8, esp_cy + 8, ESP_Z + 1.6, ESP_Z + 1.6 + 3.2)
    E += box(e["x0"] - 1.0, e["x0"] + 4.6, esp_cy - 3.9, esp_cy + 3.9, ESP_Z + 1.6, ESP_Z + 1.6 + 2.8)
    E += box(e["x1"] - 5.5, e["x1"] - 0.5, e["y0"] + 1, e["y0"] + 7, ESP_Z + 1.6, ESP_Z + 1.6 + 1.5)   # U.FL + pigtail
    H["ESP32-S2-DevKitM-1U"] = E
    H["micro-USB plug"] = stadium_x(-30, e["x0"] - 1.0, PORT_ESP["y"], PORT_ESP["z"], 10.8, 7.6)
    hy_ = PORT_HID["y"]
    HB = box(-1.9, 10.0, hy_ - 6.0, hy_ + 6.0, SHIELD_TOP, SHIELD_TOP + 1.6)
    HB += box(-1.9, 5.5, hy_ - USB_C["w"] / 2, hy_ + USB_C["w"] / 2, SHIELD_TOP + 1.6, SHIELD_TOP + 1.6 + USB_C["h"])
    H["HID USB-C breakout"] = HB
    H["HID USB-C plug"] = stadium_x(-30, -1.9, hy_, PORT_HID["z"], 8.3, 2.6)   # plug tongue shell 8.25 x 2.4
    EN = box(ENC["x"] - EC11["body_w"] / 2, ENC["x"] + EC11["body_w"] / 2, ENC["y"] - EC11["body_d"] / 2, ENC["y"] + EC11["body_d"] / 2,
             LID_UNDER - EC11["body_h"], LID_UNDER)
    EN += box(ENC["x"] - 6, ENC["x"] + 6, ENC["y"] - 7.5, ENC["y"] + 7.5, LID_UNDER - EC11["body_h"] - EC11["pins"] - 3.0, LID_UNDER - EC11["body_h"])
    EN += cyl(ENC["x"], ENC["y"], LID_UNDER, LID_UNDER + EC11["bush_l"], EC11["bush_r"])
    EN += cyl(ENC["x"], ENC["y"], LID_UNDER + EC11["bush_l"], LID_UNDER + EC11["shaft_l"], EC11["shaft_r"])
    H["EC11 encoder (+ solder tails, wires)"] = EN
    H["EC11 nut"] = cyl(ENC["x"], ENC["y"], LID_TOP, LID_TOP + EC11["nut_t"], 5.5, seg=6)
    return H

def to_trimesh(m):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    f = np.asarray(mesh.tri_verts)
    return trimesh.Trimesh(vertices=v, faces=f, process=False)

def export(m, name, print_xform=None, assembled=True):
    os.makedirs(os.path.join(OUT, "assembled"), exist_ok=True)
    if assembled: to_trimesh(m).export(os.path.join(OUT, "assembled", name + ".stl"))
    pm = m if print_xform is None else print_xform(m)
    bb = pm.bounding_box()
    pm = pm.translate([-(bb[0] + bb[3]) / 2, -(bb[1] + bb[4]) / 2, -bb[2]])
    to_trimesh(pm).export(os.path.join(OUT, name + ".stl"))
    bb = pm.bounding_box()
    return dict(name=name, size=[round(bb[3] - bb[0], 1), round(bb[4] - bb[1], 1), round(bb[5] - bb[2], 1)],
                volume_cm3=round(m.volume() / 1000, 1))

def build():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".stl"): os.remove(os.path.join(OUT, f))
    base, top, bezel = make_base(), make_top(), make_bezel()
    plunger = make_plunger().translate([BTNF["x"], BTNF["y"], PLUNGER["z0"]])
    pipe = make_lightpipe().translate([LEDF["x"], LEDF["y"], PIPE["z0"]])
    knobD = make_knob("D").translate([ENC["x"], ENC["y"], LID_UNDER + EC11["shaft_l"] - 12.5])   # sits where a 20 mm shaft bottoms out
    printed = dict(case_base=base, case_top=top, case_lcd_bezel=bezel, button_plunger=plunger, led_light_pipe=pipe, ec11_knob_D_shaft=knobD)
    info = [
        export(base, "case_base"),
        export(top, "case_top", lambda m: m.rotate([180, 0, 0])),
        export(bezel, "case_lcd_bezel"),
        export(plunger, "button_plunger"),
        export(pipe, "led_light_pipe", lambda m: m.rotate([180, 0, 0])),
        export(knobD, "ec11_knob_D_shaft", lambda m: m.rotate([180, 0, 0])),
        export(make_knob("knurl"), "ec11_knob_knurled_shaft", lambda m: m.rotate([180, 0, 0]), assembled=False),
        export(make_coupon(), "fit_test_coupon", assembled=False),
    ]
    return printed, info

if __name__ == "__main__":
    printed, info = build()
    for i in info: print(i)
    hw = hardware()
    report = {}
    for pn, pm in printed.items():
        for hn, hm in hw.items():
            v = (pm ^ hm).volume()
            if pn.startswith("ec11_knob") and hn.startswith("EC11"): continue   # knob is meant to sit on the shaft
            if v > 0.05: report[f"{pn} x {hn}"] = round(v, 2)
    names = list(printed)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            v = (printed[names[i]] ^ printed[names[j]]).volume()
            if v > 2.0: report[f"{names[i]} x {names[j]}"] = round(v, 2)
    print("CLASHES:", report if report else "none")
    layout = dict(
        grid=dict(P=P, X0=X0, Y0=Y0, col_min=COL_MIN, col_max=COL_MAX, row_min=ROW_MIN, row_max=ROW_MAX),
        z=dict(FRDM_Z=FRDM_Z, FRDM_T=FRDM_T, RECEPT_H=RECEPT_H, SHIELD_Z=SHIELD_Z, SHIELD_T=SHIELD_T,
               MOD_Z=MOD_Z, ESP_Z=ESP_Z, LID_UNDER=LID_UNDER, LID_TOP=LID_TOP, SPLIT=SPLIT, BEZEL_TOP=BEZEL_TOP),
        frdm=dict(w=FRDM_W, d=FRDM_D, mh=FRDM_MH, lcd=dict(x0=78.3, x1=86.5, y0=25.2, y1=46.9),
                  usb=[dict(name="J13 MCU-Link USB-C", y=PORT_J13["y"]), dict(name="J12 MCU USB-C", y=PORT_J12["y"])],
                  headers=dict(J1=dict(x0=hx(-2) - 1.27, x1=hx(5) + 1.27, y=hy(0) + 1.27),
                               J2=dict(x0=27.69 - 1.27, x1=50.55 + 1.27, y=hy(0) + 1.27),
                               J3=dict(x0=hx(-9) - 1.27, x1=hx(-2) + 1.27, y=hy(19) - 1.27),
                               J4=dict(x0=hx(0) - 1.27, x1=hx(5) + 1.27, y=hy(19) - 1.27))),
        shield=SHIELD, parts=parts, nets=NETS, holes_named={f"{c},{r}": hole_name(c, r) for (c, r) in used},
        stl=info, case=dict(IN=IN, OUT=OUTB, RISER=[RISER_X0, RISER_X1]), enc=ENC,
        ports=dict(J13=PORT_J13, J12=PORT_J12, ESP=PORT_ESP, HID=PORT_HID, SMA=SMA), posts=BACK_POSTS, clashes=report)
    with open(os.path.join(OUT, "layout.json"), "w") as f:
        json.dump(layout, f, indent=1)
    print("wrote", OUT)
