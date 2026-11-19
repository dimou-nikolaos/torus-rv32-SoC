# Pin sides for the cross floorplan: the NoC pins of io.tcl at the ends of the
# channel arms they enter (cross.tcl), the tile's own links in the first half
# of each arm end (bottom, left), the feedthroughs in the second.
# cross.tcl, or its copy with pnr.py --geom overrides
if {[info exists ::env(TILE_FLOORPLAN_TCL)]} {
  source $::env(TILE_FLOORPLAN_TCL)
} else {
  source [file join [file dirname [info script]] cross.tcl]
}
set ym [expr {($chan_y0 + $chan_y1) / 2}]
set xm [expr {($chan_x0 + $chan_x1) / 2}]
set ::tile_regions [list \
  left      "left:$chan_y0-$ym"    right     "right:$chan_y0-$ym" \
  top       "top:$chan_x0-$xm"     bottom    "bottom:$chan_x0-$xm" \
  left_ft   "left:$ym-$chan_y1"    right_ft  "right:$ym-$chan_y1" \
  top_ft    "top:$xm-$chan_x1"     bottom_ft "bottom:$xm-$chan_x1"]
source [file join [file dirname [info script]] .. tile io.tcl]
