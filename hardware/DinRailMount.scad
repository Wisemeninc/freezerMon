// freezerMon — DIN-rail mount for LilyGO T-A7608E-H (ESP32 version, with GPS)
// ---------------------------------------------------------------------------
// Two-piece, support-free print for Bambu Lab (or any FDM) printers:
//   part = "tray"  — board cradle: 4× M3 standoffs on the official hole grid,
//                    22 mm tall so the 18650 holder on the PCB back hangs free,
//                    stiffening skirt, zip-tie slots for antenna/sensor cables
//   part = "clip"  — TS35 (top-hat 35 mm) DIN clip, fixed top hook + printed
//                    spring latch with screwdriver release tab
//   part = "both"  — assembly preview (do not print this)
//
// Rail orientation: the tray floor carries a 4-boss cross, so the SAME tray and
// clip mount the unit either way — set `mount` for the preview, and at assembly
// bolt the clip straight (horizontal) or turned 90° (vertical). No reprint needed.
//
// Board dimensions from LilyGO's official drawing (T-A7608X-ESP32.dxf):
//   PCB 33.07 × 110.54 mm, M3 holes (R1.73) on a 27.72 × 105.59 mm grid.
//
// Print: PETG recommended (freezer room / cabinet duty), 0.2 mm layers,
// 4 walls, 40 % infill on the clip. Tray prints cavity-up, clip channel-up —
// no supports. Hardware: 4× M3×6 (board→standoffs, self-tap into 2.7 mm
// holes) + 2× M3×10 countersunk (clip→tray bosses).
// ---------------------------------------------------------------------------

part  = "both";           // "tray" | "clip" | "both"
mount = "horizontal";     // preview/assembly only: how the unit hangs on the rail.
                          //   "horizontal" — board landscape, long axis ALONG the rail
                          //   "vertical"   — board portrait, long axis ACROSS the rail
                          // The same tray + clip serve both; for vertical you simply
                          // bolt the clip on rotated 90° (it picks up the other boss pair).

/* ---------- board (measured: LilyGO official DXF) ---------- */
board_l   = 110.54;       // PCB length
board_w   = 33.07;        // PCB width
hole_dx   = 105.59;       // mounting-hole grid, long axis
hole_dy   = 27.72;        // mounting-hole grid, short axis
screw_d   = 2.7;          // M3 self-tap pilot in standoffs

/* ---------- tray ---------- */
clearance   = 0.6;        // PCB outline to skirt, per side
floor_t     = 3.5;
skirt_h     = 9;          // stiffening skirt above floor
skirt_t     = 2.4;
standoff_h  = 22;         // 18650 holder on PCB back needs ~20.5 mm
standoff_d  = 8;
ziptie_w    = 4.5;        // fits 3.6 mm ties
ziptie_t    = 2.2;

/* ---------- DIN clip (TS35×7.5 top-hat rail) ---------- */
rail_w        = 35.0;     // rail outer height (across)
rail_fit      = 0.4;      // pocket clearance
lip_t         = 1.3;      // rail lip metal thickness + margin
clip_t        = 7;        // clip plate thickness
clip_len      = 55;       // along the rail
clip_screw_dx = 20;       // clip↔tray screw spacing (M3, countersunk)

/* ---------- derived ---------- */
inner_l = board_l + 2*clearance;
inner_w = board_w + 2*clearance;
outer_l = inner_l + 2*skirt_t;
outer_w = inner_w + 2*skirt_t;
clip_w  = rail_w + rail_fit + 2*6;   // plate width across rail incl. hook stock

// clip-screw cross pattern: X pair = landscape mount, Y pair = vertical (90°) mount
clip_screw_pts = [ [ clip_screw_dx/2, 0], [-clip_screw_dx/2, 0],
                   [0,  clip_screw_dx/2], [0, -clip_screw_dx/2] ];

$fn = 64;

/* =========================== TRAY =========================== */
module tray() {
  difference() {
    union() {
      // floor
      linear_extrude(floor_t) rrect(outer_l, outer_w, 3);
      // skirt
      linear_extrude(floor_t + skirt_h)
        difference() {
          rrect(outer_l, outer_w, 3);
          rrect(inner_l, inner_w, 2);
        }
      // standoffs on the official hole grid
      for (sx = [-1, 1], sy = [-1, 1])
        translate([sx*hole_dx/2, sy*hole_dy/2, 0])
          cylinder(h = floor_t + standoff_h, d = standoff_d);
      // clip screw bosses — cross pattern so ONE tray serves both rail orientations:
      // the X pair carries the clip landscape, the Y pair carries it rotated 90° (vertical).
      for (p = clip_screw_pts)
        translate([p[0], p[1], 0])
          cylinder(h = floor_t, d = 9);
    }
    // standoff pilot holes (M3 self-tap)
    for (sx = [-1, 1], sy = [-1, 1])
      translate([sx*hole_dx/2, sy*hole_dy/2, floor_t + standoff_h - 8])
        cylinder(h = 9, d = screw_d);
    // clip screws: M3 clearance through the floor (both boss pairs)
    for (p = clip_screw_pts)
      translate([p[0], p[1], -1])
        cylinder(h = floor_t + 2, d = 3.2);
    // zip-tie slots through the floor, along both long edges
    for (sx = [-1, 0, 1], sy = [-1, 1])
      translate([sx*(inner_l/2 - 18), sy*(inner_w/2 - ziptie_w/2 - 1), -1])
        linear_extrude(floor_t + 2)
          square([ziptie_t, ziptie_w], center = true);
    // lightening / airflow windows under the modem & battery zone
    for (sx = [-1, 1])
      translate([sx*inner_l/4, 0, -1])
        linear_extrude(floor_t + 2) rrect(inner_l/3, inner_w - 14, 4);
    // skirt cable openings, both short ends (USB-C, JST, antenna pigtails)
    for (sx = [-1, 1])
      translate([sx*outer_l/2, 0, floor_t + skirt_h/2 + 1.5])
        cube([skirt_t*4, inner_w - 10, skirt_h], center = true);
  }
}

/* =========================== DIN CLIP =========================== */
// Printed channel-up. Hook the fixed (+Y) side over the rail's top lip, press
// down: the spring arm (-Y) cams open and snaps over the lower lip. Pull the
// tab (or lever a screwdriver under it) to release.
//
// Built as ONE extruded footprint — slab + spring arm + release tab — with a
// single slot freeing the arm, so the solid is manifold by construction.
module clip() {
  inner  = (rail_w + rail_fit) / 2;    // pocket half-width (17.7)
  bar_w  = 6;                          // hook stock width
  slot_w = 3.2;                        // gap that frees the spring arm
  bridge = 8;                          // arm anchor at the +X end
  hook_h = lip_t + 2.2;                // hook stock height above the plate
  half_w = inner + bar_w;              // plate half-width across the rail
  tab_l  = 11;

  difference() {
    union() {
      // one footprint: plate + arm + tab, minus the arm-freeing slot
      linear_extrude(clip_t)
        difference() {
          union() {
            rrect(clip_len, 2*half_w, 3);
            // release tab, overlapping the arm's free end by 3 mm
            translate([-clip_len/2 - tab_l/2 + 3, -(inner + bar_w/2)])
              rrect(tab_l + 6, bar_w, 2);
          }
          // slot between slab and arm; the last `bridge` mm stay attached
          translate([-clip_len/2 - tab_l - 1, -inner])
            square([clip_len + tab_l + 1 - bridge, slot_w]);
        }
      // hook stock bars
      for (sy = [-1, 1])
        translate([-clip_len/2, sy > 0 ? inner : -inner - bar_w, clip_t])
          cube([clip_len, bar_w, hook_h]);
      // fixed hook nose (+Y, deep engagement) — 45° underside chamfer
      translate([0, inner, clip_t]) nose(clip_len, -1, 2.5);
      // latch nose on the arm (-Y, snap engagement with lead-in)
      translate([0, -inner, clip_t]) nose(clip_len - bridge - 4, 1, 1.8);
    }
    // countersunk M3 through-holes into the tray bosses
    for (sx = [-1, 1])
      translate([sx*clip_screw_dx/2, 0, 0]) {
        translate([0, 0, -1]) cylinder(h = clip_t + 2, d = 3.4);
        translate([0, 0, clip_t - 1.8]) cylinder(h = 2.5, d1 = 3.4, d2 = 6.6);
      }
  }
}

// Hook nose bar: cross-section rises 45° from the channel floor to the lip
// seat, then slopes back up to the stock top (lead-in for the snap).
// dir = -1 noses toward -Y (from the +Y stock), +1 noses toward +Y.
module nose(len, dir, ov) {
  hook_h = lip_t + 2.2;
  translate([-len/2, 0, 0])
    rotate([90, 0, 90])
      linear_extrude(len)
        polygon([
          [-dir*2, 0],                                    // rooted 2 mm inside the stock
          [dir*min(ov, lip_t), min(ov, lip_t)],           // 45° chamfer
          [dir*ov, lip_t + 0.2],                          // lip seat tip
          [-dir*2, hook_h + 1.6],                         // lead-in slope back
        ]);
}

/* =========================== util =========================== */
module rrect(l, w, r) {
  offset(r) offset(-r) square([l, w], center = true);
}

/* =========================== emit =========================== */
if (part == "tray") tray();
else if (part == "clip") clip();
else {
  tray();
  // preview: clip flipped onto the tray back, rotated for the chosen mount
  translate([0, 0, -0.2]) mirror([0, 0, 1])
    rotate([0, 0, mount == "vertical" ? 90 : 0]) clip();
}
