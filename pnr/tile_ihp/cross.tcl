# Hand floorplan of the compute tile in IHP SG13G2 ("cross", as pnr/tile/cross.tcl
# for sky130): one bank in each corner, its two RM_IHPSG13_1P_512x64 macros
# (64 bits each) stacked, and the logic in a cross of channels between them: X
# ring links enter the horizontal arm from west and east, Y ring links the
# vertical arm from north and south. Every signal pin of the macro is on its
# bottom edge (Metal2), which faces the horizontal arm; the outer macro's pins
# escape through the gap between the two. The macros obstruct Metal1-Metal3
# and part of Metal4, so Metal5 still routes over them.
# Sourced by macros.tcl and io_cross.tcl, read by scripts/pnr.py; sizes in um.
set sram_w 784.48
set sram_h 191.34
set margin 30.24   ;# die edge to core: a multiple of the site (0.48 x 3.78)
set inset  25.0    ;# core edge to macros
set macro_rows 2   ;# per bank (32 KB)
set gap_y  60.0    ;# between the macro rows of a bank: the outer macro's pin escape
set chan_v 1200.0  ;# vertical arm of the cross (Y links); 700/800 placed at 84% utilization, too dense
set chan_h 1300.0  ;# horizontal arm (X links)

set bank_w $sram_w
set bank_h [expr {$macro_rows * $sram_h + ($macro_rows - 1) * $gap_y}]
set die_w  [expr {2 * ($margin + $inset) + 2 * $bank_w + $chan_v}]
set die_h  [expr {2 * ($margin + $inset) + 2 * $bank_h + $chan_h}]
# Channel spans (x for the vertical arm, y for the horizontal arm)
set chan_x0 [expr {$margin + $inset + $bank_w}]
set chan_x1 [expr {$chan_x0 + $chan_v}]
set chan_y0 [expr {$margin + $inset + $bank_h}]
set chan_y1 [expr {$chan_y0 + $chan_h}]
