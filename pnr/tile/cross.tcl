# Hand floorplan of the compute tile ("cross"): one bank of SRAM macros in each
# corner, two macros wide and macro_rows high (32 KB: four macros per bank, one
# deep, two rows; 64 KB: eight, four rows), and the logic in a cross of
# channels between them: X ring links enter the horizontal arm from west and
# east, Y ring links the vertical arm from north and south. Every macro is
# mirrored so that its port-0 pin edges (bottom and left in R0) face the
# channels; gaps between macros give the pins of the outer macros access.
# Each arm carries its dimension's through traffic, the tile's own links and
# the feedthroughs (832 wires end to end), besides the banks' wiring, and each
# gap between macro rows the escape of a macro row's pins (dout, din, wmask,
# address on the bottom edge). At 64 KB, with 420/620 um arms and 45 um gaps
# global routing did not converge, three quarters of the overflow in the gaps
# (sky130hd routes at 80% of the tracks).
# Sourced by macros.tcl and io_cross.tcl; sizes in um.
set sram_w 683.1
set sram_h 416.54
set margin 15.0    ;# die edge to core
set inset  35.0    ;# core edge to macros: room for a met4 power strap (27 um pitch) in that gap
set macro_rows 2   ;# per bank: 2 for 32 KB scratchpads (TORUS_SPM_KB, the default), 4 for 64 KB
set gap_x  100.0   ;# between the two macro columns of a bank
set gap_y  110.0   ;# between macro rows of a bank: a row's ~140 pins escape along it
set chan_v 560.0   ;# vertical arm of the cross (Y links)
set chan_h 760.0   ;# horizontal arm (X links)

set bank_w [expr {2 * $sram_w + $gap_x}]
set bank_h [expr {$macro_rows * $sram_h + ($macro_rows - 1) * $gap_y}]
set die_w  [expr {2 * ($margin + $inset) + 2 * $bank_w + $chan_v}]
set die_h  [expr {2 * ($margin + $inset) + 2 * $bank_h + $chan_h}]
# Channel spans (x for the vertical arm, y for the horizontal arm)
set chan_x0 [expr {$margin + $inset + $bank_w}]
set chan_x1 [expr {$chan_x0 + $chan_v}]
set chan_y0 [expr {$margin + $inset + $bank_h}]
set chan_y1 [expr {$chan_y0 + $chan_h}]
