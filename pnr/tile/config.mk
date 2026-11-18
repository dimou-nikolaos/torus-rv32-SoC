# Compute tile in sky130hd (ORFS). Run through scripts/pnr.py, which passes
# VERILOG_FILES and VERILOG_INCLUDE_DIRS (expanded from rtl/tile/tile_pnr.f),
# SDC_FILE (generated with the clock period), WORK_HOME and FLOW_VARIANT.
export DESIGN_NAME     = tile_top
export DESIGN_NICKNAME = tile
export PLATFORM        = sky130hd

# Defines (SPM_SKY130) and top parameters (NX 4 NY 4: ring lengths of a 4x4
# compute grid with the I/O row) are in scripts/pnr.py, which reads the sources.
export SYNTH_HDL_FRONTEND = slang
export SYNTH_HIERARCHICAL = 1
# Sources are read by pnr/common/canonicalize.tcl (run by scripts/pnr.py), not
# by ORFS's canonicalize step: see there.

SRAM_DIR = $(PDK_ROOT)/sky130A/libs.ref/sky130_sram_macros
export ADDITIONAL_LEFS = $(SRAM_DIR)/lef/sky130_sram_2kbyte_1rw1r_32x512_8.lef
export ADDITIONAL_LIBS = $(SRAM_DIR)/lib/sky130_sram_2kbyte_1rw1r_32x512_8_TT_1p8V_25C.lib
export ADDITIONAL_GDS  = $(SRAM_DIR)/gds/sky130_sram_2kbyte_1rw1r_32x512_8.gds

PNR_DIR = $(dir $(DESIGN_CONFIG))
export PDN_TCL        = $(PNR_DIR)../common/pdn_sram.tcl

# Floorplan: auto (ORFS macro placer, utilization below) or cross (hand placed,
# pnr/tile/cross.tcl; scripts/pnr.py --floorplan cross sets TILE_FLOORPLAN and
# the DIE_AREA and CORE_AREA computed from cross.tcl)
TILE_FLOORPLAN ?= auto
ifeq ($(TILE_FLOORPLAN),cross)
export MACRO_PLACEMENT_TCL = $(PNR_DIR)macros.tcl
export IO_CONSTRAINTS      = $(PNR_DIR)io_cross.tcl
# 416 X-link pins per side in the 420 um arm: pins on adjacent met3 tracks
export PLACE_PINS_ARGS     = -min_distance 1 -min_distance_in_tracks
else
export IO_CONSTRAINTS = $(PNR_DIR)io.tcl
# 32 macros of 0.28 mm2 (9.1 mm2) and about 0.95 mm2 of standard cells
export CORE_UTILIZATION ?= 60
endif
export PLACE_DENSITY    ?= 0.55
export MACRO_ROWS_HALO_X ?= 20
export MACRO_ROWS_HALO_Y ?= 20

# Global routing does not clear its last overflow at the arm ends (832 pins on
# adjacent tracks) within 30 iterations (4.5 hours): accept it and let detailed
# routing, which works on real tracks, resolve it (its DRC count is reported).
export GLOBAL_ROUTE_ARGS = -congestion_iterations 10 -allow_congestion -verbose
