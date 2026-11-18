// SoC grid for place and route (scripts/pnr.py grid): the compute tile is a
// hard macro (the tile run's abstract): tile_top is a black box, whose sources
// are parsed (the frontend resolves every module name) but not elaborated.
// Paths relative to the repo root.
-f rtl/tile/tile_syn.f
-f rtl/special/io.f
rtl/soc/soc_top.sv
