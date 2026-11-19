# SoC grid constraints; scripts/pnr.py sets clk_period (ns). The fabric clock
# and the AXI clock of the I/O tile are asynchronous (io_cdc_fifo crosses
# them); the AXI port is budgeted like the tile's link ports.
current_design soc_top

set io_pct 0.2
create_clock -name clk  -period $clk_period [get_ports clk_i]
create_clock -name aclk -period $clk_period [get_ports aclk_i]
set_clock_uncertainty 0.25 [get_clocks {clk aclk}]
set_clock_groups -asynchronous -group clk -group aclk

set axi_in  [get_ports {s_axi_*_i}]
set axi_out [get_ports {s_axi_*_o}]
set_input_delay  [expr {$clk_period * $io_pct}] -clock aclk $axi_in
set_output_delay [expr {$clk_period * $io_pct}] -clock aclk $axi_out
set_input_delay  [expr {$clk_period * $io_pct}] -clock clk  [get_ports rst_ni]
set_input_delay  [expr {$clk_period * $io_pct}] -clock aclk [get_ports arst_ni]
set_load 0.02 [all_outputs]
