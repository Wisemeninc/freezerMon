# DIN-rail mount — LilyGO T-A7608E-H

Two-piece, support-free FDM print (designed for Bambu Lab, works on any printer).
Source: `DinRailMount.scad` (parametric OpenSCAD). Ready-to-slice: `TrayT-A7608.stl`, `DinClipTS35.stl`.

Board dimensions are taken from LilyGO's official drawing (`T-A7608X-ESP32.dxf`):
PCB 33.07 × 110.54 mm, M3 corner holes on a 27.72 × 105.59 mm grid — the standoffs
land exactly on them.

## Parts

| Part | File | Print orientation |
|------|------|-------------------|
| Board cradle | `TrayT-A7608.stl` | as exported (floor on bed, standoffs up) |
| TS35 DIN clip | `DinClipTS35.stl` | as exported (channel up) |

The 22 mm standoffs leave the 18650 holder on the PCB back hanging free above
the tray floor; airflow windows sit under the battery and modem zones.

## Rail orientation (horizontal *or* vertical)

The tray floor carries a **4-boss cross**, so one printed tray + clip mounts the
unit either way on the rail — no reprint, no second part:

- **Horizontal** (landscape): bolt the clip on straight. Board long axis runs
  *along* the rail.
- **Vertical** (portrait): bolt the same clip on turned **90°**; its two screws
  pick up the other boss pair. Board long axis runs *across* the rail (vertical).

Set `mount = "horizontal"` / `"vertical"` in the SCAD only if you want the
`part="both"` assembly preview to match — it does not change the printed parts.

## Print settings

- **Material:** PETG (cold-room duty; PLA gets brittle below 0 °C)
- Layer 0.2 mm, 4 perimeters, 40 % infill on the clip (strength in the spring arm), 15 %+ on the tray
- No supports, no brim needed (add a brim on the clip if your PETG warps)

## Hardware

- 4 × M3×6 self-tapping (board → standoff pilot holes, Ø2.7)
- 2 × M3×10 countersunk self-tapping (clip → tray floor bosses)

## Assembly

1. Screw the board onto the four standoffs (18650 holder facing down into the tray).
2. Screw the clip to the tray floor through **one** boss pair — straight for a
   horizontal (landscape) hang, or turned 90° onto the other pair for a vertical
   (portrait) hang. The screw heads sit inside the rail channel — keep them flush.
   (The unused pair stays open; it just adds a little ventilation.)
3. On the rail: hook the **fixed edge** (the side without the tab) over the top
   lip of the TS35 rail, then press the bottom down until the spring latch snaps
   over the lower lip. To release, pull the tab outward (or lever a flat
   screwdriver under it) and tilt the unit off.
4. Route antenna pigtails / DS18B20 / reed-switch cables through the open ends;
   zip-tie slots in the floor edges take 3.6 mm ties for strain relief.

## Tuning (edit `DinRailMount.scad`, re-export)

| Symptom | Parameter |
|---------|-----------|
| Board tight/loose in skirt | `clearance` (default 0.6/side) |
| Clip tight/loose on rail | `rail_fit` (default 0.4) |
| Latch too stiff / too weak | `lip_t` engagement, slot geometry in `clip()` |
| Taller battery/underside parts | `standoff_h` (default 22) |

Re-export:

```
openscad -o TrayT-A7608.stl  -D 'part="tray"' DinRailMount.scad
openscad -o DinClipTS35.stl -D 'part="clip"' DinRailMount.scad
```
