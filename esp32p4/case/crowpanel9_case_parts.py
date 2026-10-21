# Parts version of the desk case: the one-piece box of crowpanel9_case.py cut into flat panels that
# are glued together (superglue), so no large part has to be printed on its floor.
#   floor_left / floor_right - floor in two halves, tongue and groove in the middle, front lip
#   side_left / side_right   - side walls with the board pillars and ribs that locate the panels
#   front                    - face below the display with the microSD slot and the finger dish
#   back                     - back wall with the cable exit and the vents
#   top                      - narrow face above the display
# The bezel and the blind are the same as in the one-piece version.

import sys, math
import cadquery as cq

sys.stdout.reconfigure(encoding="utf-8")
OUT = sys.argv[1] if len(sys.argv) > 1 else "."
SRC = sys.argv[2] if len(sys.argv) > 2 else "crowpanel9_case.py"

g = {"__name__": "parts"}
try:
    exec(open(SRC, encoding="utf-8").read(), g)
except SystemExit:
    pass
globals().update({k: v for k, v in g.items() if not k.startswith("__")})

RIB_T, RIB_H = 1.2, 1.5         # locating ribs on the side walls
TONGUE_T, TONGUE_L = 0.9, 3.0   # floor joint
GROOVE_CLR = 0.2

xin = cav_w / 2

def big(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))

# --- side walls (with the pillars, which stop on the floor) ----------------------------------------
side_zone = big(-500, -xin, -500, 500, -500, 500).union(big(xin, 500, -500, 500, -500, 500))
for hx, hy in HOLES:
    X, S = brd2local(hx, hy)
    c = loc(X, S, pcb_back)
    xw = math.copysign(xin - PILLAR / 2, X)
    side_zone = side_zone.union(big(xw - PILLAR / 2 - 0.01, xw + PILLAR / 2 + 0.01, c.y - PILLAR / 2 - 0.01, c.y + PILLAR / 2 + 0.01, FLOOR_T, 500))
sides = box.intersect(side_zone)
rest = box.cut(side_zone)

# --- panels -------------------------------------------------------------------------------------------
back_zone = big(-500, 500, Y_BACK - FLOOR_T, 500, -500, B2.z + 0.5) \
    .union(big(-BLIND_W / 2 - 1, BLIND_W / 2 + 1, Y_BACK - 7, 500, FLOOR_T, ez + BLIND_H / 2 + 1))
back = rest.intersect(back_zone)
rest = rest.cut(back_zone)
floor_zone = big(-500, 500, -500, 500, -500, FLOOR_T).union(big(-500, 500, -500, P.y + 4, -500, FOOT_H))
floor = rest.intersect(floor_zone)
rest = rest.cut(floor_zone)
top_zone = dbox(-500, 500, L_SLOPE - WALL - 0.01, 500, -500, 500)
top = rest.intersect(top_zone)
front = rest.cut(top_zone)

# --- floor halves: tongue on the right half, groove in the left half -------------------------------
zt = (FLOOR_T - TONGUE_T) / 2
floor_l = floor.intersect(big(-500, 0, -500, 500, -500, 500))
floor_r = floor.intersect(big(0, 500, -500, 500, -500, 500))
y0f, y1f = P.y + 0.5, Y_BACK - FLOOR_T - 0.5
floor_r = floor_r.union(big(-TONGUE_L, 0.01, y0f, y1f, zt, zt + TONGUE_T))
floor_l = floor_l.cut(big(-TONGUE_L - GROOVE_CLR, 0.01, y0f - GROOVE_CLR, y1f + GROOVE_CLR,
                          zt - GROOVE_CLR / 2, zt + TONGUE_T + GROOVE_CLR / 2))

# --- ribs on the side walls, just inside each panel --------------------------------------------------
def ribs_for(sgn):
    xr0, xr1 = (xin - RIB_H, xin + 0.01) if sgn > 0 else (-xin - 0.01, -xin + RIB_H)
    r = big(xr0, xr1, P.y + 4, Y_BACK - FLOOR_T, FLOOR_T, FLOOR_T + RIB_T)                  # over the floor
    r = r.union(big(xr0, xr1, Y_BACK - FLOOR_T - RIB_T, Y_BACK - FLOOR_T, FLOOR_T, B2.z - 2))   # in front of the back
    # in front of the front face and under the top face: only behind the board (it uses the gap there)
    r = r.union(dbox(xr0, xr1, WALL, WALL + RIB_T, pcb_back + 6, 400).intersect(big(-500, 500, -500, 500, FOOT_H + 1, 500)))
    r = r.union(dbox(xr0, xr1, L_SLOPE - WALL - RIB_T, L_SLOPE - WALL, pcb_back + 6, 400).intersect(big(-500, 500, -500, Y_BACK - FLOOR_T - 0.01, -500, 500)))
    return r.intersect(outer)

side_l = sides.intersect(big(-500, 0, -500, 500, -500, 500)).union(ribs_for(-1))
side_r = sides.intersect(big(0, 500, -500, 500, -500, 500)).union(ribs_for(1))

# --- checks ------------------------------------------------------------------------------------------
parts = {"floor_left": floor_l, "floor_right": floor_r, "side_left": side_l, "side_right": side_r,
         "front": front, "back": back, "top": top}
print("volume one-piece box %.0f, parts %.0f mm3" % (vol(box), sum(vol(p) for k, p in parts.items()) - vol(big(-TONGUE_L, 0, y0f, y1f, zt, zt + TONGUE_T))))
names = list(parts)
for i, a in enumerate(names):
    bb = parts[a].val().BoundingBox()
    hit = [b for b in names[i + 1:] if vol(parts[a].intersect(parts[b])) > 0.01]
    print("%-12s %6.1f x %6.1f x %6.1f  overlaps: %s" % (a, bb.xlen, bb.ylen, bb.zlen, hit or "-"))
for k, p in parts.items():
    print("board/%s %.2f mm3" % (k, vol(dummy.intersect(p))))

asm = cq.Assembly()
colors = [(0.45, 0.45, 0.5), (0.5, 0.5, 0.56), (0.35, 0.35, 0.4), (0.35, 0.35, 0.4), (0.55, 0.45, 0.4), (0.4, 0.5, 0.45), (0.5, 0.42, 0.55)]
for (k, p), c in zip(parts.items(), colors):
    asm.add(p, name=k, color=cq.Color(*c))
    cq.exporters.export(p, f"{OUT}/crowpanel9_part_{k}.step")
    cq.exporters.export(p, f"{OUT}/crowpanel9_part_{k}.stl", tolerance=0.05, angularTolerance=0.1)
asm.add(bezel, name="bezel", color=cq.Color(0.2, 0.2, 0.22))
asm.add(blind_lo, name="blind_lower", color=cq.Color(0.8, 0.4, 0.1))
asm.add(blind_hi, name="blind_upper", color=cq.Color(0.9, 0.55, 0.15))
asm.export(f"{OUT}/crowpanel9_parts_assembly.step")
print("exported")
