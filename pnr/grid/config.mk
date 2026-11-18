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
