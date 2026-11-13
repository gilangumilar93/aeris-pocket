# AERIS Pocket enclosure
# FreeCAD 1.0 parametric script. Units are millimetres.
#
#   freecadcmd aeris_enclosure.py
# or, inside FreeCAD: Macro → Execute.
#
# Writes aeris_bottom.step/.stl and aeris_top.step/.stl next to this file.
# Edit P and re-run. Board is 45 × 55. The shell adds wall + gap.
# The UV hole stays open: PETG and PLA block UVA/UVB. Acetate is only
# for the OLED rebate (visible light).

from __future__ import print_function

import os

# Stack, z = 0 at the outer bottom skin:
#   1.4 skin + 6.2 cell + 0.4 gap + 1.6 pcb = board top at 9.6
#   split at 10.4, outer top at 17.8
#   WROOM (3.1) and the OLED module (~3.2, wires not a header) live in the lid.
P = dict(
    board_w=45.0,
    board_l=55.0,
    wall=2.0,
    gap=0.5,
    outer_h=17.8,
    bottom_h=10.4,
    skin=1.4,
    cell_w=34.0,
    cell_l=50.0,
    cell_t=6.2,
    cell_x=2.8,
    cell_y=5.0,
    pcb_z=8.0,          # underside of the PCB, on the posts
    usb_cx=14.5,        # enclosure X; board USB center is x = 12
    usb_w=9.4,
    usb_z=8.2,
    usb_h=3.4,
    btn_y=44.6,
    btn_w=4.6,
    btn_z=11.2,
    btn_h=3.0,
    sgp_x=14.5,
    sgp_y=51.6,
    uv_x=32.5,
    uv_y=51.6,
    uv_d=5.0,
    oled_x=16.5,
    oled_y=34.5,
    oled_w=28.0,
    oled_l=13.0,
    rebate=0.6,
    vent_n=6,
    vent_w=8.0,
    vent_slot=1.2,
    vent_pitch=2.2,
    boss_r=2.5,
    boss_hole=1.7,
    screw_clear=2.3,
)


def _here():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        return os.getcwd()


def build():
    import FreeCAD as App
    import Part
    import MeshPart

    ow = P["board_w"] + 2 * (P["wall"] + P["gap"])
    ol = P["board_l"] + 2 * (P["wall"] + P["gap"])
    bottom_h = P["bottom_h"]
    top_span = P["outer_h"] - bottom_h
    inset = P["wall"]

    def box(x, y, z, w, l, h):
        return Part.makeBox(w, l, h, App.Vector(x, y, z))

    def cyl(x, y, z, r, h):
        return Part.makeCylinder(r, h, App.Vector(x, y, z))

    bottom = box(0, 0, 0, ow, ol, bottom_h)
    bottom = bottom.cut(box(inset, inset, P["skin"], ow - 2 * inset, ol - 2 * inset, bottom_h))

    # Cell pocket. Floor under the cell stays `skin` thick. Top of the cell
    # sits 0.4 mm under the PCB.
    bottom = bottom.cut(box(P["cell_x"], P["cell_y"], P["skin"], P["cell_w"], P["cell_l"], P["cell_t"]))

    # Posts. PCB underside lands on pcb_z. Hole is a self-tapping M2 pilot.
    margin = 4.2
    centers = [
        (margin, margin),
        (ow - margin, margin),
        (margin, ol - margin),
        (ow - margin, ol - margin),
    ]
    for cx, cy in centers:
        post = cyl(cx, cy, P["skin"], P["boss_r"], P["pcb_z"] - P["skin"])
        hole = cyl(cx, cy, P["skin"] - 0.01, P["boss_hole"] / 2.0, bottom_h)
        bottom = bottom.fuse(post).cut(hole)

    # USB window in the front wall. The ceiling is a 3.4 mm bridge — PETG
    # prints it; clean the lip with a knife if it sags.
    bottom = bottom.cut(box(
        P["usb_cx"] - P["usb_w"] / 2.0,
        -0.2,
        P["usb_z"],
        P["usb_w"],
        inset + 0.6,
        P["usb_h"],
    ))
    bottom = bottom.cut(box(
        -0.2,
        P["btn_y"] - P["btn_w"] / 2.0,
        P["btn_z"],
        inset + 0.6,
        P["btn_w"],
        P["btn_h"],
    ))

    top = box(0, 0, bottom_h, ow, ol, top_span)
    top = top.cut(box(inset, inset, bottom_h, ow - 2 * inset, ol - 2 * inset, top_span - P["wall"]))

    for cx, cy in centers:
        top = top.cut(cyl(cx, cy, bottom_h - 0.1, P["screw_clear"] / 2.0, top_span + 0.3))

    face = bottom_h + top_span
    # Rebate first (shallow, from the outer face), then the through-window.
    top = top.cut(box(
        P["oled_x"] - 1.2,
        P["oled_y"] - 1.2,
        face - P["rebate"],
        P["oled_w"] + 2.4,
        P["oled_l"] + 2.4,
        P["rebate"] + 0.05,
    ))
    top = top.cut(box(
        P["oled_x"],
        P["oled_y"],
        face - P["wall"] - 0.2,
        P["oled_w"],
        P["oled_l"],
        P["wall"] + 0.4,
    ))

    top = top.cut(cyl(
        P["uv_x"], P["uv_y"], face - P["wall"] - 0.2, P["uv_d"] / 2.0, P["wall"] + 0.4
    ))

    total = (P["vent_n"] - 1) * P["vent_pitch"]
    y0 = P["sgp_y"] - total / 2.0
    for i in range(P["vent_n"]):
        top = top.cut(box(
            P["sgp_x"] - P["vent_w"] / 2.0,
            y0 + i * P["vent_pitch"] - P["vent_slot"] / 2.0,
            face - P["wall"] - 0.2,
            P["vent_w"],
            P["vent_slot"],
            P["wall"] + 0.4,
        ))

    here = _here()
    doc = App.newDocument("aeris")
    fb = doc.addObject("Part::Feature", "bottom")
    fb.Shape = bottom
    ft = doc.addObject("Part::Feature", "top")
    ft.Shape = top
    doc.recompute()
    Part.export([fb], os.path.join(here, "aeris_bottom.step"))
    Part.export([ft], os.path.join(here, "aeris_top.step"))
    MeshPart.meshFromShape(Shape=bottom, LinearDeflection=0.08, AngularDeflection=0.1).write(
        os.path.join(here, "aeris_bottom.stl")
    )
    MeshPart.meshFromShape(Shape=top, LinearDeflection=0.08, AngularDeflection=0.1).write(
        os.path.join(here, "aeris_top.stl")
    )
    print("AERIS shell %0.1f × %0.1f × %0.1f mm" % (ow, ol, P["outer_h"]))
    print("split at z=%0.1f, pcb underside z=%0.1f" % (bottom_h, P["pcb_z"]))


if __name__ == "__main__":
    build()
