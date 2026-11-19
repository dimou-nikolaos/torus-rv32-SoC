# SoC grid in sky130hd (ORFS): soc_top with the compute tile as a hard macro.
# Run through scripts/pnr.py grid, which also passes the tile abstract
# (ADDITIONAL_LEFS, ADDITIONAL_LIBS), the generated tile placement
# (MACRO_PLACEMENT_TCL) and DIE_AREA / CORE_AREA for the chosen order.
export DESIGN_NAME     = soc_top
export DESIGN_NICKNAME = grid
export PLATFORM        = sky130hd
export SYNTH_HDL_FRONTEND = slang
export SYNTH_HIERARCHICAL = 1

PNR_DIR = $(dir $(DESIGN_CONFIG))
# Standard-cell power in the channels only: the tile abstract's own grid ends
# on met5, which this study (wires and timing between tiles) leaves unconnected
export PDN_TCL = $(PNR_DIR)pdn.tcl
export PLACE_DENSITY ?= 0.30

# The fenced I/O row is dense (router FIFOs) and keeps a little global-route
# overflow (about 0.3% of the resources, none in the link channels): accept it,
# the grid study measures link wires and timing after global routing.
export GLOBAL_ROUTE_ARGS = -congestion_iterations 10 -allow_congestion -verbose
# The repair after global routing peaks at 22 GB here (more than the machine
# leaves): placement and CTS have repaired the design already, and the study
# measures the global-route wires as they are.
export SKIP_INCREMENTAL_REPAIR = 1
# Antenna repair after global routing (jumpers, diodes) is for signoff, not for
# this study, and it ran out of memory on the 32 KB sky130 grid (19 GB at its
# legalization)
export SKIP_ANTENNA_REPAIR = 1
