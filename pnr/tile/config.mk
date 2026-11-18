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
export IO_CONSTRAINTS = $(PNR_DIR)io.tcl

# 32 macros of 0.28 mm2 (9.1 mm2) and about 0.8 mm2 of standard cells
export CORE_UTILIZATION ?= 60
export PLACE_DENSITY    ?= 0.55
export MACRO_ROWS_HALO_X ?= 20
export MACRO_ROWS_HALO_Y ?= 20
