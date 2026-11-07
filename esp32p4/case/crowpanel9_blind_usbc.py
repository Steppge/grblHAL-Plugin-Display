# Blende mit USB-C-Buchse für das CrowPanel-9"-Gehäuse (crowpanel9_box_v3), ersetzt die zweiteilige
# Blende mit Kabeldurchführung (crowpanel9_blind_upper/_lower).
#
# Die Buchse ist die gleiche wie am SmartPanel-GD32 (VISSQH-Breakout zur Tafelmontage, nur + und -
# für den externen Akkulader): Flansch außen versenkt, Schrauben von außen in Dome innen.
# Hinter der Blende ist die Rückwandöffnung 44 × 16 mm leer, die Dome ragen dort hinein.
#
# Koordinaten wie im Gehäuse: x quer, y nach hinten (Rückwand außen bei y = 129,17), z nach oben.
# Druck flach, Außenseite nach unten, ohne Stützmaterial.
#
#   python crowpanel9_blind_usbc.py [Ausgabeordner]
#   schreibt crowpanel9_blind_usbc.step/.stl und prüft gegen crowpanel9_box_v3.step

import os
import sys
import cadquery as cq

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
HERE = os.path.dirname(os.path.abspath(__file__))

# Blende wie die alte: 66 × 36 × 3 mm, Ecken R3, Mitte bei x 0 / z 26,5
WALL_Y = 129.17             # Rückwand außen = Innenseite der Blende
W, H, T = 66.0, 36.0, 3.0
CZ = 26.5
CORNER_R = 3.0
M3_HOLE = 3.4               # Durchgang für M3 in die Einpressmuttern der Rückwand
M3_POS = [(sx * 28.0, CZ + sz * 13.0) for sx in (-1, 1) for sz in (-1, 1)]

# Rückwandöffnung (gemessen): x ±22, z 18,5 … 34,5
OPEN_X, OPEN_Z = (-22.0, 22.0), (18.5, 34.5)

# USB-C-Breakout VISSQH: Flansch 20 × 6,8 × 1,5 mm, Buchse 9 × 3,2 mm in der Mitte (Maße wie SmartPanel)
USBC_CUT = (9.4, 3.7)       # Öffnung für Buchse und Lötplatine
USBC_FLANGE = (20.6, 7.4, 1.5)   # Tasche außen (Flansch + Luft, 0,2 mm mehr als am SmartPanel)
USBC_HOLE_DIST = 16.0       # Mitte-Mitte der Flanschlöcher
DOME_D, DOME_L = 4.1, 5.0   # Dome innen für die Schrauben
SCREW_PILOT = 1.7           # Kernloch für 1,9-mm-Schrauben, selbstschneidend (PETG)


def ybox(x0, x1, y0, y1, z0, z1):
    return cq.Workplane().box(x1 - x0, y1 - y0, z1 - z0).translate(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))


def ycyl(x, z, y0, y1, d):
    return cq.Workplane("XZ").circle(d / 2).extrude(-(y1 - y0)).translate((x, y0, z))


# Platte
blind = ybox(-W / 2, W / 2, WALL_Y, WALL_Y + T, CZ - H / 2, CZ + H / 2).edges("|Y").fillet(CORNER_R)
for x, z in M3_POS:
    blind = blind.cut(ycyl(x, z, WALL_Y - 1, WALL_Y + T + 1, M3_HOLE))

# Dome innen (ragen in die Rückwandöffnung)
for s in (-1, 1):
    blind = blind.union(ycyl(s * USBC_HOLE_DIST / 2, CZ, WALL_Y - DOME_L, WALL_Y + 0.1, DOME_D))

# Flanschtasche außen, Durchbruch für die Buchse, Kernlöcher
blind = blind.cut(ybox(-USBC_FLANGE[0] / 2, USBC_FLANGE[0] / 2, WALL_Y + T - USBC_FLANGE[2], WALL_Y + T + 1,
                       CZ - USBC_FLANGE[1] / 2, CZ + USBC_FLANGE[1] / 2))
blind = blind.cut(ybox(-USBC_CUT[0] / 2, USBC_CUT[0] / 2, WALL_Y - DOME_L - 1, WALL_Y + T + 1,
                       CZ - USBC_CUT[1] / 2, CZ + USBC_CUT[1] / 2).edges("|Y").fillet(1.2))
for s in (-1, 1):
    blind = blind.cut(ycyl(s * USBC_HOLE_DIST / 2, CZ, WALL_Y - DOME_L - 1, WALL_Y + T - USBC_FLANGE[2] + 0.01, SCREW_PILOT))

# Prüfungen: Dome in der Öffnung, keine Überschneidung mit dem Gehäuse
for s in (-1, 1):
    x = s * USBC_HOLE_DIST / 2
    assert OPEN_X[0] + DOME_D / 2 < x < OPEN_X[1] - DOME_D / 2 and OPEN_Z[0] + DOME_D / 2 < CZ < OPEN_Z[1] - DOME_D / 2
box_path = os.path.join(HERE, "crowpanel9_box_v3.step")
if os.path.exists(box_path):
    box = cq.importers.importStep(box_path).val()
    # 0,02 mm nach außen versetzt: die plane Auflage auf der Rückwand zählt nicht als Überschneidung
    clash = box.intersect(blind.translate((0, 0.02, 0)).val()).Volume()
    print(f"Überschneidung mit dem Gehäuse: {clash:.3f} mm³")
    assert clash < 0.01, "Blende stößt ins Gehäuse"
print(f"Wand unter der Flanschtasche {T - USBC_FLANGE[2]:.1f} mm, Schraubenlänge im Material "
      f"{T - USBC_FLANGE[2] + DOME_L:.1f} mm")

cq.exporters.export(blind, os.path.join(OUT, "crowpanel9_blind_usbc.step"))
cq.exporters.export(blind, os.path.join(OUT, "crowpanel9_blind_usbc.stl"), tolerance=0.05, angularTolerance=0.1)
print("geschrieben:", os.path.join(OUT, "crowpanel9_blind_usbc.step/.stl"))
