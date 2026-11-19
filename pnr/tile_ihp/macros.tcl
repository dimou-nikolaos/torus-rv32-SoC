# Places the SRAM macros for the IHP cross floorplan (cross.tcl). Banks 0..3 go
# bottom-left, bottom-right, top-left, top-right; half h of a bank (lanes 2h and
# 2h+1) is macro row h, row 0 next to the horizontal arm. The pins (bottom edge
# in R0) face the arm: R0 above it, mirrored in X below it.
if {[info exists ::env(TILE_FLOORPLAN_TCL)]} {
  source $::env(TILE_FLOORPLAN_TCL)
} else {
  source [file join [file dirname [info script]] cross.tcl]
}

set placed 0
foreach inst [[ord::get_db_block] getInsts] {
  set name [$inst getName]
  if {![string match "RM_IHPSG13_*" [[$inst getMaster] getName]]} continue
  if {![regexp {g_bank\\?\[(\d)\\?\].*g_half\\?\[(\d)\\?\]} $name -> b h]} {
    error "macros.tcl: cannot parse $name"
  }
  set right [expr {$b % 2}]
  set top   [expr {$b / 2}]
  set x [expr {$right ? $chan_x1 : $chan_x0 - $sram_w}]
  if {$top} {
    set y [expr {$chan_y1 + $h * ($sram_h + $gap_y)}]
  } else {
    set y [expr {$chan_y0 - $sram_h - $h * ($sram_h + $gap_y)}]
  }
  # On the site grid of the rows (from the core origin), so rows cut cleanly
  set x [expr {$margin + round(($x - $margin) / 0.48) * 0.48}]
  set y [expr {$margin + round(($y - $margin) / 3.78) * 3.78}]
  place_macro -macro_name $name -location [list $x $y] -orientation [expr {$top ? "R0" : "MX"}]
  incr placed
}
if {$placed != 4 * $macro_rows} {
  error "macros.tcl: placed $placed SRAM macros, expected [expr {4 * $macro_rows}]"
}
