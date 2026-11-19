# Power grid for the IHP tile: the platform's grid (ORFS
# platforms/ihp-sg13g2/pdn.tcl) without its core ring (a tile abuts its
# neighbours), and the SRAM macros' Metal4 supply pins tied up to TopMetal1.
set_voltage_domain -name {CORE} -power {VDD} -ground {VSS}

define_pdn_grid -name {grid} -voltage_domains {CORE} -pins {TopMetal1 TopMetal2}
add_pdn_stripe -grid {grid} -layer {Metal1} -width {0.44} -pitch {7.56} -offset {0} -followpins
add_pdn_stripe -grid {grid} -layer {TopMetal1} -width {2.200} -pitch {75.6} -offset {13.600}
add_pdn_stripe -grid {grid} -layer {TopMetal2} -width {2.200} -pitch {75.6} -offset {13.600}
add_pdn_connect -grid {grid} -layers {Metal1 TopMetal1}
add_pdn_connect -grid {grid} -layers {TopMetal1 TopMetal2}

define_pdn_grid -name {sram} -voltage_domains {CORE} -macro -cells {RM_IHPSG13_.*} \
  -halo {2.0 2.0 2.0 2.0} -grid_over_boundary
add_pdn_connect -grid {sram} -layers {Metal4 TopMetal1}
