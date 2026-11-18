# Hand floorplan of the compute tile ("cross"): one bank of eight SRAM macros
# in each corner, two macros wide and four high, and the logic in a cross of
# channels between them: X ring links enter the horizontal arm from west and
# east, Y ring links the vertical arm from north and south. Every macro is
# mirrored so that its port-0 pin edges (bottom and left in R0) face the
# channels; gaps between macros give the pins of the outer macros access.
# Sourced by macros.tcl and io_cross.tcl; sizes in um.
set sram_w 683.1
set sram_h 416.54
set margin 15.0    ;# die edge to core
set inset  10.0    ;# core edge to macros (the core snaps inward to the rows)
set gap_x  50.0    ;# between the two macro columns of a bank
set gap_y  45.0    ;# between macro rows of a bank
set chan_v 420.0   ;# vertical arm of the cross (Y links)
set chan_h 620.0   ;# horizontal arm (X links): 832 pins per end, own links and feedthroughs

set bank_w [expr {2 * $sram_w + $gap_x}]
set bank_h [expr {4 * $sram_h + 3 * $gap_y}]
set die_w  [expr {2 * ($margin + $inset) + 2 * $bank_w + $chan_v}]
set die_h  [expr {2 * ($margin + $inset) + 2 * $bank_h + $chan_h}]
# Channel spans (x for the vertical arm, y for the horizontal arm)
set chan_x0 [expr {$margin + $inset + $bank_w}]
set chan_x1 [expr {$chan_x0 + $chan_v}]
set chan_y0 [expr {$margin + $inset + $bank_h}]
set chan_y1 [expr {$chan_y0 + $chan_h}]
