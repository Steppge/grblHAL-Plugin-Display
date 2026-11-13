# CrowPanel 9" Gehäuse: Drehknopf Ø 50 mm für den EC11 (gerändelte Achse Ø 6 mm mit Schlitz),
# wie der Knopf des PC-Decks. Der Drehgeber sitzt an der Gehäuseseite, die Achse steht 18 mm heraus.
# Aufruf: python knopf50.py [Ausgabeordner]  ->  crowpanel9_knopf50.step/.stl, crowpanel9_knopf50.png
# Oben eine Fingermulde zum schnellen Drehen mit dem Zeigefinger.
# Gedruckt wird aufrecht (Deckfläche mit Mulde oben, ohne Stützen); unten überbrückt der Drucker nur die
# Mutter-Aussparung Ø 12,6. Der Knopf ist massiv, das Gewicht regelt die Füllung im Slicer.
#
# Höhen ab Gehäuseoberfläche (z = 0): Achse endet 18 mm darüber, die Mutter (SW 9,9) sitzt auf der Wand.
import math
import os
import sys

import cadquery as cq

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

SHAFT_TOP = 18.0      # Achsende über der Gehäuseoberfläche
SHAFT_D = 6.0         # Achse (Riffel)
BORE_KNURL = 6.0      # Bohrung im Riffelbereich: leichter Presssitz, Schlitz federt
BORE_FREE = 6.4       # darunter: glatter Teil der Achse, Luft
KNURL_LEN = 7.0       # Riffelbereich ab Achsende
SLOT_W = 0.9          # Steg in den Schlitz der Achse (Schlitz 1,0 mm)
SLOT_DEPTH = 4.0      # Steglänge ab Achsende (kürzer als der Schlitz, damit der Knopf ganz aufsitzt)
TOP_WALL = 2.0        # Deckfläche über dem Achsende (großer Knopf: etwas dicker)

NUT_AF = 9.9          # Schlüsselweite Mutter
NUT_RECESS_D = 12.6   # Aussparung unten: Mutter über Eck 11,43 + Scheibe + Luft
NUT_RECESS_H = 4.0
BUSH_D = 7.8          # Gewindebuchse M7 des EC11 (steht über die Mutter hinaus)
BUSH_TOP = 6.5        # Buchse bis so hoch über der Gehäuseoberfläche frei
GAP = 0.8             # Luft zwischen Knopf und Gehäuse

KNOB_D = 50.0
TOP_CHAMFER = 1.5
RIBS = 36             # Griffrillen außen
RIB_R = 1.2           # Radius der Rillen
DIMPLE_D = 18.0       # Fingermulde: Durchmesser an der Oberfläche
DIMPLE_DEPTH = 3.5    # Tiefe
DIMPLE_POS = 13.0     # Mitte der Mulde ab der Achse

z0 = GAP
z1 = SHAFT_TOP + TOP_WALL
h = z1 - z0

knob = cq.Workplane("XY").workplane(offset=z0).circle(KNOB_D / 2).extrude(h)
knob = knob.faces(">Z").edges().chamfer(TOP_CHAMFER)
knob = knob.faces("<Z").edges().chamfer(0.4)

# Griffrillen außen (enden unter der Fase)
for k in range(RIBS):
    a = 2 * math.pi * k / RIBS
    groove = (cq.Workplane("XY").workplane(offset=z0 - 1)
              .center(KNOB_D / 2 * math.cos(a), KNOB_D / 2 * math.sin(a))
              .circle(RIB_R).extrude(h - TOP_CHAMFER))
    knob = knob.cut(groove)

# Fingermulde oben (Kugelabschnitt), zeigt zugleich die Stellung
dimple_r = DIMPLE_D / 2
sphere_r = (dimple_r ** 2 + DIMPLE_DEPTH ** 2) / (2 * DIMPLE_DEPTH)
dimple = cq.Workplane("XY").add(cq.Solid.makeSphere(
    sphere_r, cq.Vector(0, -DIMPLE_POS, z1 - DIMPLE_DEPTH + sphere_r), angleDegrees1=-90, angleDegrees2=90))
knob = knob.cut(dimple)

# Aussparung für Mutter und Scheibe
knob = knob.cut(cq.Workplane("XY").workplane(offset=z0 - 1).circle(NUT_RECESS_D / 2).extrude(NUT_RECESS_H + 1))

# Freiraum für die Gewindebuchse über der Mutter
knob = knob.cut(cq.Workplane("XY").workplane(offset=z0).circle(BUSH_D / 2).extrude(BUSH_TOP - z0))

# Bohrung: unten frei, oben (Riffel) Presssitz, bis zum Achsende
z_knurl = SHAFT_TOP - KNURL_LEN
knob = knob.cut(cq.Workplane("XY").workplane(offset=z0).circle(BORE_FREE / 2).extrude(z_knurl - z0))
knob = knob.cut(cq.Workplane("XY").workplane(offset=z_knurl).circle(BORE_KNURL / 2).extrude(KNURL_LEN))

# Steg für den Schlitz (quer durch die Bohrung, vom Achsende nach unten)
blade = (cq.Workplane("XY").workplane(offset=SHAFT_TOP - SLOT_DEPTH)
         .rect(BORE_KNURL + 0.4, SLOT_W).extrude(SLOT_DEPTH))
knob = knob.union(blade)

# Prüfungen
bb = knob.val().BoundingBox()
assert abs(bb.zmin - z0) < 0.01 and abs(bb.zmax - z1) < 0.01
assert NUT_RECESS_D / 2 > NUT_AF / math.sqrt(3) + 0.3, "Mutter passt nicht in die Aussparung"
assert BUSH_TOP < SHAFT_TOP - KNURL_LEN, "Buchsen-Freiraum reicht in den Riffelbereich"
assert DIMPLE_POS + dimple_r <= KNOB_D / 2 - TOP_CHAMFER - 1.0, "Mulde zu nah am Rand"
assert DIMPLE_POS - dimple_r >= BORE_KNURL / 2 + 1.0, "Mulde zu nah an der Bohrung"
print(f"Knopf Ø {KNOB_D} x {h:.1f} mm, Unterkante {z0} mm über dem Gehäuse, Oberkante {z1:.1f} mm, "
      f"Volumen {knob.val().Volume():.0f} mm³")

# Druck aufrecht: Unterkante auf z = 0
printed = knob.translate((0, 0, -z0))
cq.exporters.export(printed, os.path.join(OUT, "crowpanel9_knopf50.step"))
cq.exporters.export(printed, os.path.join(OUT, "crowpanel9_knopf50.stl"), tolerance=0.05, angularTolerance=0.1)

# Bild: Ansicht schräg von unten + Schnitt durch die Mitte (mit Achse, Mutter und Gehäusewand)
try:
    import vtk
    from PIL import Image, ImageDraw, ImageFont
    from vtk.util.numpy_support import vtk_to_numpy

    shaft = cq.Workplane("XY").circle(SHAFT_D / 2).extrude(SHAFT_TOP)
    shaft = shaft.cut(cq.Workplane("XY").workplane(offset=SHAFT_TOP - 5).rect(SHAFT_D + 1, 1.0).extrude(5))
    nut = cq.Workplane("XY").polygon(6, NUT_AF / math.cos(math.pi / 6)).extrude(2.0).cut(
        cq.Workplane("XY").circle(3.6).extrude(2.0))
    plate = cq.Workplane("XY").workplane(offset=-3).rect(70, 70).extrude(3).cut(
        cq.Workplane("XY").workplane(offset=-3).circle(3.6).extrude(3))
    half = cq.Workplane("XY").box(90, 45, 60).translate((0, 22.5, 0))

    def actor(w, col):
        poly = w.val().toVtkPolyData(0.02, 0.1)
        nm = vtk.vtkPolyDataNormals()
        nm.SetInputData(poly)
        nm.SetFeatureAngle(30)
        nm.SplittingOn()
        m = vtk.vtkPolyDataMapper()
        m.SetInputConnection(nm.GetOutputPort())
        a = vtk.vtkActor()
        a.SetMapper(m)
        a.GetProperty().SetColor(*col)
        return a

    def render(parts, direction, up, size=(560, 480)):
        r = vtk.vtkRenderer()
        r.SetBackground(1, 1, 1)
        for w, c in parts:
            r.AddActor(actor(w, c))
        win = vtk.vtkRenderWindow()
        win.SetOffScreenRendering(1)
        win.SetSize(*size)
        win.AddRenderer(r)
        cam = r.GetActiveCamera()
        cam.SetFocalPoint(0, 0, 5)
        cam.SetPosition(direction[0] * 150, direction[1] * 150, 5 + direction[2] * 150)
        cam.SetViewUp(*up)
        cam.ParallelProjectionOn()
        r.ResetCamera()
        cam.SetParallelScale(cam.GetParallelScale() * 0.85)
        win.Render()
        grab = vtk.vtkWindowToImageFilter()
        grab.SetInput(win)
        grab.Update()
        img = grab.GetOutput()
        w_, h_, _ = img.GetDimensions()
        arr = vtk_to_numpy(img.GetPointData().GetScalars()).reshape(h_, w_, -1)[::-1]
        return Image.fromarray(arr[:, :, :3].copy())

    grey, dark, metal = (0.30, 0.32, 0.38), (0.55, 0.57, 0.62), (0.75, 0.75, 0.78)
    a = render([(knob, grey)], (0.5, -0.8, 0.8), (0, 0, 1))
    b = render([(knob.cut(half), grey), (shaft.cut(half), metal), (nut.cut(half), metal),
                (plate.cut(half), dark)], (0, 1, 0), (0, 0, 1))
    pic = Image.new("RGB", (a.width + b.width, a.height + 40), "white")
    pic.paste(a, (0, 40))
    pic.paste(b, (a.width, 40))
    d = ImageDraw.Draw(pic)
    try:
        font = ImageFont.truetype("arial.ttf", 18)
    except OSError:
        font = ImageFont.load_default()
    d.text((10, 10), "Drehknopf Ø 50 mit Fingermulde", fill="black", font=font)
    d.text((a.width + 10, 10), f"Schnitt: Gehäuse, Mutter SW {NUT_AF}, Achse bis {SHAFT_TOP} mm", fill="black",
           font=font)
    pic.save(os.path.join(OUT, "crowpanel9_knopf50.png"))
except ImportError as e:
    print("Kein Bild:", e)
