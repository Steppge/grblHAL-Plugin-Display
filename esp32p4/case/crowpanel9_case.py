# Desk case for the Elecrow CrowPanel Advanced 9" ESP32-P4 (board V1.2), display tilted 30 deg.
# Parts:
#   box    - one piece: floor, back wall, sides, front and upper face; open toward the display.
#            Printed standing on its floor (the slopes are 30 deg from vertical).
#   bezel  - flat frame in front of the glass, covers 2 mm of the black glass border.
#            Printed face down. Four countersunk M3 screws go through the bezel corners and the
#            board holes into heat inserts in four pillars of the box: they hold bezel and board.
#   blind  - two halves, each cable lies between two half circles; closes the cable exit.
#
# World: x to the viewer's right, y toward the back, z up, table at z = 0.
# Display local: X = x, S = along the slope from the lower outer edge, N = depth behind the front surface.

import sys, math
import cadquery as cq

sys.stdout.reconfigure(encoding="utf-8")
OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# --- parameters -------------------------------------------------------------------------------
TILT = 30.0
WALL = 2.0          # side walls, front and upper face
FLOOR_T = 2.5       # floor and back wall
CLR = 0.4           # gap around the board
PCB_W, PCB_H, PCB_T = 222.7, 130.5, 1.6
GLASS_W, GLASS_H = 210.7, 126.5     # glass and LCD module, centred in x, 2 mm above the PCB bottom edge
GLASS_Y0 = 2.0
LIP = 1.6           # bezel thickness in front of the glass
OVERLAP = 2.0       # the bezel covers 2 mm of the black glass border on each side
SKIRT_T, SKIRT_GAP, SKIRT_D = 1.6, 0.25, 6.0   # rim of the bezel around the outside of the box (thickness, gap, depth over the box)
N_PCB = LIP + 7.6   # front of the PCB behind the front surface (bezel + glass 1.5 + LCD module + gap)
H_FRONT = 37.0      # height of the lower front edge above the table
FOOT_H = 5.0        # the front face ends here, then vertical down to the table
L_TOP = 24.0        # upper face, perpendicular to the display: room behind the top edge
INSERT_D, INSERT_L = 4.0, 5.5      # M3 heat inserts
SCREW_D = 3.4
PILLAR = 8.0        # square pillars in the box under the board corners

# board V1.2 layout (Eagle, mm), seen from the component side (= back of the display)
BRD_X0, BRD_X1, BRD_Y0 = -0.59, 222.11, -167.05
HOLES = [(2.344, -164.010), (219.088, -164.043), (219.106, -39.553), (2.410, -39.558)]
SD_X = 17.4                     # microSD card centre (contacts 1-8), card enters at the bottom edge
J7 = (95.8, 126.5, -165.4, -155.9)
USB_C = [(-95.9, -86.6), (-116.8, -107.5)]   # at the right board edge (viewer's left)
USB_OPEN = (13.0, 7.0)          # opening for the plug (along the edge, depth)
PLUG_H = 26.0                   # J7 header + plug + wire bend behind the PCB

# cable exit
EXIT_W, EXIT_H = 44.0, 16.0
CABLE_D = 5.0                   # 4.8 mm cables + clearance
CABLE_PITCH = 14.0
BLIND_W, BLIND_H, BLIND_T = 66.0, 36.0, 3.0

# --- geometry helpers --------------------------------------------------------------------------
t = math.radians(TILT)
s_dir = cq.Vector(0, math.cos(t), math.sin(t))      # up the slope
n_dir = cq.Vector(0, math.sin(t), -math.cos(t))     # into the case, perpendicular to the display
A = cq.Vector(0, 0, H_FRONT)                        # lower front edge of the bezel (front surface)

BORDER = CLR + WALL
L_SLOPE = PCB_H + 2 * BORDER
W_OUT = PCB_W + 2 * BORDER
B = A + s_dir * L_SLOPE
P = A + n_dir * ((H_FRONT - FOOT_H) / math.cos(t))
B2 = B + n_dir * L_TOP
Y_BACK = B2.y
cav_w = W_OUT - 2 * WALL

def brd2local(bx, by):
    """board coordinates (component side view) -> (X, S) on the display, viewer's right = +X"""
    return -(bx - (BRD_X0 + BRD_X1) / 2), (by - BRD_Y0) + BORDER

def loc(X, S, N):
    return A + cq.Vector(X, 0, 0) + s_dir * S + n_dir * N

disp_plane = cq.Plane(origin=A.toTuple(), xDir=(1, 0, 0), normal=(-n_dir).toTuple())   # z out of the display

def dbox(x0, x1, s0, s1, n0, n1):
    """box in display coordinates"""
    return cq.Workplane(disp_plane).center((x0 + x1) / 2, (s0 + s1) / 2) \
        .rect(x1 - x0, s1 - s0).extrude(-(n1 - n0)).translate((n_dir * n0).toTuple())

def dcyl(X, S, n0, n1, d):
    return cq.Workplane(disp_plane).center(X, S).circle(d / 2).extrude(-(n1 - n0)).translate((n_dir * n0).toTuple())

def ybox(xc, zc, w, h, y0, y1):
    return cq.Workplane("XY").box(w, y1 - y0, h).translate((xc, (y0 + y1) / 2, zc))

def ycyl(xc, zc, d, y0, y1):
    return cq.Workplane("XY").add(cq.Solid.makeCylinder(d / 2, y1 - y0, cq.Vector(xc, y0, zc), cq.Vector(0, 1, 0)))

def vol(w):
    try:
        return w.val().Volume()
    except Exception:
        return 0.0

# --- box ----------------------------------------------------------------------------------------
profile = [(A.y, A.z), (B.y, B.z), (B2.y, B2.z), (B2.y, 0), (P.y, 0), (P.y, P.z)]
outer = cq.Workplane("YZ").polyline(profile).close().extrude(W_OUT / 2, both=True)

cav = cq.Workplane("YZ").polyline(profile).close().extrude(cav_w / 2, both=True)
cav = cav.cut(dbox(-W_OUT, W_OUT, -50, WALL, -50, 400))                 # front face
cav = cav.cut(dbox(-W_OUT, W_OUT, L_SLOPE - WALL, 400, -50, 400))       # upper face
cav = cav.cut(cq.Workplane("XY").box(W_OUT, 1000, FLOOR_T, centered=(True, True, False)))   # floor
cav = cav.cut(ybox(0, 200, W_OUT, 400, Y_BACK - FLOOR_T, Y_BACK + 1))  # back wall
box = outer.cut(cav)
box = box.cut(dbox(-W_OUT, W_OUT, -50, L_SLOPE + 50, -50, LIP))        # the bezel lies in front

# pillars under the board corners: vertical from the floor up to the back of the PCB
pcb_back = N_PCB + PCB_T
for hx, hy in HOLES:
    X, S = brd2local(hx, hy)
    c = loc(X, S, pcb_back)
    xw = math.copysign(cav_w / 2 - PILLAR / 2, X)          # against the side wall
    col = cq.Workplane("XY").box(PILLAR, PILLAR, c.z + 20, centered=(True, True, False)).translate((xw, c.y, 0))
    col = col.cut(dbox(-W_OUT, W_OUT, -50, L_SLOPE + 50, -50, pcb_back))  # top follows the board
    box = box.union(col.intersect(outer))
    box = box.cut(dcyl(X, S, pcb_back - 0.1, pcb_back + INSERT_L, INSERT_D))

# openings for the two USB-C sockets (they stick out 1.1 mm over the board edge)
for y0, y1 in USB_C:
    _, s0 = brd2local(0, y0); _, s1 = brd2local(0, y1)
    sc, nc = (s0 + s1) / 2, N_PCB + 2.7
    box = box.cut(dbox(-W_OUT / 2 - 1, -cav_w / 2 + 1, sc - USB_OPEN[0] / 2, sc + USB_OPEN[0] / 2,
                       nc - USB_OPEN[1] / 2, nc + USB_OPEN[1] / 2))

# microSD: slot through the front face in line with the card, below it a finger dish. The front face
# is thickened inside around the dish (below the socket, clear of the connector J4 next to it).
sdx, _ = brd2local(SD_X, BRD_Y0)
box = box.union(dbox(sdx - 12, sdx + 12, WALL - 0.01, WALL + 3, pcb_back + 2.5, pcb_back + 20).intersect(outer))
box = box.cut(dbox(sdx - 6.5, sdx + 6.5, -1, BORDER + 0.5, pcb_back - 0.3, pcb_back + 2.5))
DISH_R, DISH_DEPTH, DISH_N = 12.0, 3.0, 4.5      # dish centre 3.4 mm below the slot centre: it does not open the wall above the slot
dish = cq.Solid.makeSphere(DISH_R, loc(sdx, DISH_DEPTH - DISH_R, pcb_back + DISH_N), angleDegrees1=-90, angleDegrees2=90)
box = box.cut(cq.Workplane("XY").add(dish))

# cable exit in the back wall, ring for the blind inserts
ez = FLOOR_T + 6 + BLIND_H / 2
yb = Y_BACK
box = box.union(ybox(0, ez, BLIND_W, BLIND_H, yb - 6, yb - FLOOR_T + 0.01))
box = box.cut(ybox(0, ez, EXIT_W, EXIT_H, yb - 20, yb + 1))
bh = [(sx * (BLIND_W / 2 - 5), ez + sz * (BLIND_H / 2 - 5)) for sx in (-1, 1) for sz in (-1, 1)]
for bx, bz in bh:
    box = box.cut(ycyl(bx, bz, INSERT_D, yb - INSERT_L, yb + 0.1))

# vents in the upper back wall
for i in range(6):
    box = box.cut(ybox(-50 + i * 20, B2.z - 18, 4, 24, yb - FLOOR_T - 1, yb + 1).edges("|Y").fillet(1.9))

# --- bezel --------------------------------------------------------------------------------------
gy0 = BORDER + GLASS_Y0
bezel = dbox(-W_OUT / 2, W_OUT / 2, 0, L_SLOPE, 0, LIP)
SK = SKIRT_GAP + SKIRT_T
skirt = dbox(-W_OUT / 2 - SK, W_OUT / 2 + SK, -SK, L_SLOPE + SK, 0, LIP + SKIRT_D)     .cut(dbox(-W_OUT / 2 - SKIRT_GAP, W_OUT / 2 + SKIRT_GAP, -SKIRT_GAP, L_SLOPE + SKIRT_GAP, LIP, LIP + SKIRT_D + 1))
bezel = bezel.union(skirt)
bezel = bezel.cut(dbox(-GLASS_W / 2 + OVERLAP, GLASS_W / 2 - OVERLAP, gy0 + OVERLAP, gy0 + GLASS_H - OVERLAP, -1, LIP + 1))
for hx, hy in HOLES:
    X, S = brd2local(hx, hy)
    bezel = bezel.union(dcyl(X, S, LIP - 0.01, N_PCB, 2 * 2.4 + 0.6))       # pillar onto the PCB corner
    bezel = bezel.cut(dcyl(X, S, -1, N_PCB + 1, SCREW_D))
    cone = cq.Solid.makeCone(3.3, SCREW_D / 2, 3.3 - SCREW_D / 2, loc(X, S, -0.01), n_dir)   # 90 deg countersink
    bezel = bezel.cut(cq.Workplane("XY").add(cone))

# --- blind: two halves, each cable between two half circles --------------------------------------
blind = cq.Workplane("XY").box(BLIND_W, BLIND_T, BLIND_H).edges("|Y").fillet(3)
for cx in (-CABLE_PITCH / 2, CABLE_PITCH / 2):
    blind = blind.cut(ycyl(cx, 0, CABLE_D, -5, 5))
for bx, bz in bh:
    blind = blind.cut(ycyl(bx, bz - ez, SCREW_D, -5, 5))
blind = blind.translate((0, yb + BLIND_T / 2, ez))
blind_lo = blind.intersect(cq.Workplane("XY").box(200, 400, 200).translate((0, 0, ez - 100)))
blind_hi = blind.intersect(cq.Workplane("XY").box(200, 400, 200).translate((0, 0, ez + 100)))

# --- dummy board for the fit check ----------------------------------------------------------------
dummy = dbox(-PCB_W / 2, PCB_W / 2, BORDER, BORDER + PCB_H, N_PCB, pcb_back)
dummy = dummy.union(dbox(-GLASS_W / 2, GLASS_W / 2, gy0, gy0 + GLASS_H, LIP, N_PCB))
jx0, jx1 = sorted(brd2local(x, 0)[0] for x in J7[:2])
_, js0 = brd2local(0, J7[2]); _, js1 = brd2local(0, J7[3])
dummy = dummy.union(dbox(jx0 - 1, jx1 + 1, js0 - 1, js1 + 1, pcb_back, pcb_back + PLUG_H))
dummy_parts = dbox(-PCB_W / 2 + 12, PCB_W / 2 - 12, BORDER + 4, BORDER + PCB_H - 1, pcb_back, pcb_back + 9)   # parts on the back (corners are free)
dummy = dummy.union(dummy_parts)
for y0, y1 in USB_C:
    _, s0 = brd2local(0, y0); _, s1 = brd2local(0, y1)
    dummy = dummy.union(dbox(-PCB_W / 2 - 1.1, -PCB_W / 2 + 8, s0, s1, N_PCB + 0.6, N_PCB + 4.8))

if __name__ != "__main__":
    raise SystemExit      # imported for the parts version: stop before the checks and exports

# --- checks and export ----------------------------------------------------------------------------
print("outer: W %.1f  depth %.1f (footprint %.1f)  height %.1f" % (W_OUT, Y_BACK - A.y, Y_BACK - P.y, B.z))
bb = box.val().BoundingBox(); print("box %.1f x %.1f x %.1f" % (bb.xlen, bb.ylen, bb.zlen))
print("interference board/box   %.2f mm3" % vol(dummy.intersect(box)))
print("interference board/bezel %.2f mm3" % vol(dummy.intersect(bezel)))
print("interference bezel/box   %.2f mm3" % vol(bezel.intersect(box)))
print("plug end to floor: %.1f mm" % (loc(0, js0, pcb_back + PLUG_H).z - FLOOR_T))

cq.exporters.export(dummy, f"{OUT}/board_dummy.stl", tolerance=0.1, angularTolerance=0.2)
asm = cq.Assembly()
asm.add(box, name="box", color=cq.Color(0.3, 0.3, 0.33))
asm.add(bezel, name="bezel", color=cq.Color(0.2, 0.2, 0.22))
asm.add(blind_lo, name="blind_lower", color=cq.Color(0.8, 0.4, 0.1))
asm.add(blind_hi, name="blind_upper", color=cq.Color(0.9, 0.55, 0.15))
asm.add(dummy, name="board_dummy", color=cq.Color(0.1, 0.4, 0.8, 0.5))
asm.export(f"{OUT}/crowpanel9_case_assembly.step")
for name, w in (("box", box), ("bezel", bezel), ("blind_lower", blind_lo), ("blind_upper", blind_hi)):
    cq.exporters.export(w, f"{OUT}/crowpanel9_{name}.step")
    cq.exporters.export(w, f"{OUT}/crowpanel9_{name}.stl", tolerance=0.05, angularTolerance=0.1)
print("exported")

# extra check: real parts near the SD dish (microSD socket, J4)
_sd = dbox(*sorted(brd2local(x, 0)[0] for x in (11.1, 26.8)), brd2local(0, -166.9)[1], brd2local(0, -151.9)[1], pcb_back, pcb_back + 1.9)
_j4 = dbox(*sorted(brd2local(x, 0)[0] for x in (31.1, 39.1)), brd2local(0, -164.3)[1], brd2local(0, -156.4)[1], pcb_back, pcb_back + 8.4)
print("SD socket/box %.2f  J4/box %.2f mm3" % (vol(_sd.intersect(box)), vol(_j4.intersect(box))))
