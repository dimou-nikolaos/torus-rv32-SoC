# Power grid for sky130hd designs with OpenRAM SRAM macros: the platform's grid
# (ORFS platforms/sky130hd/pdn.tcl) plus the macros' power pins (vccd1, vssd1,
# rings on met4) and met5 straps over each macro to reach them. The macros
# obstruct met1..met4 only.
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {^VDD$} -power
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {VPWR}
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {VPB}
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {^vccd1$}
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {^VSS$} -ground
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {VGND}
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {VNB}
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {^vssd1$}
global_connect

set_voltage_domain -name {CORE} -power {VDD} -ground {VSS}

define_pdn_grid -name {grid} -voltage_domains {CORE} -pins {met5}
add_pdn_stripe -grid {grid} -layer {met1} -width {0.48} -pitch {5.44} -offset {0} -followpins
add_pdn_stripe -grid {grid} -layer {met4} -width {1.600} -pitch {27.140} -offset {13.570}
add_pdn_stripe -grid {grid} -layer {met5} -width {1.600} -pitch {27.200} -offset {13.600}
add_pdn_connect -grid {grid} -layers {met1 met4}
add_pdn_connect -grid {grid} -layers {met4 met5}

define_pdn_grid -name {sram} -voltage_domains {CORE} -macro -cells {sky130_sram_.*} \
  -halo {2.0 2.0 2.0 2.0} -grid_over_boundary
add_pdn_stripe -grid {sram} -layer {met5} -width {1.600} -pitch {27.200} -offset {13.600}
add_pdn_connect -grid {sram} -layers {met4 met5}
