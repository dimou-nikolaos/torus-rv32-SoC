# Compute tile constraints; scripts/pnr.py sets clk_period (ns) ahead of this.
# One fabric clock. Link ports budget io_pct of the period on the far side
# (the neighbour's register and the wire between): the grid runs (M5) time the
# real tile-to-tile wires. The coordinates are tie-offs of the grid.
current_design tile_top

set io_pct 0.2
create_clock -name clk -period $clk_period [get_ports clk_i]
set_clock_uncertainty 0.25 [get_clocks clk]

set io_inputs [all_inputs -no_clocks]
set_input_delay  [expr {$clk_period * $io_pct}] -clock clk $io_inputs
set_output_delay [expr {$clk_period * $io_pct}] -clock clk [all_outputs]
set_false_path -from [get_ports {coord_x_i[*] coord_y_i[*]}]

set_load 0.02 [all_outputs]
set_driving_cell -lib_cell sg13g2_buf_4 $io_inputs
