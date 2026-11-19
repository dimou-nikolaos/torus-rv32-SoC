# Power grid of the IHP SoC grid: standard-cell rails and straps for the cells
# in the channels and the I/O band (as pnr/grid/pdn.tcl for sky130). The tiles'
# own grids (TopMetal pins of the abstract) are not connected: the grid study
# times wires, not IR drop.
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {^VDD$} -power
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {^VSS$} -ground
global_connect
set_voltage_domain -name {CORE} -power {VDD} -ground {VSS}
define_pdn_grid -name {grid} -voltage_domains {CORE}
add_pdn_stripe -grid {grid} -layer {Metal1} -width {0.44} -pitch {7.56} -offset {0} -followpins
add_pdn_stripe -grid {grid} -layer {TopMetal1} -width {2.200} -pitch {75.6} -offset {13.600}
add_pdn_connect -grid {grid} -layers {Metal1 TopMetal1}
