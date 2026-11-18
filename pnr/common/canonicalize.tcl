# Replaces ORFS's synth_canonicalize.tcl step (scripts/pnr.py runs it ahead of
# `make synth`, which then starts from its RTLIL). The same as ORFS's slang
# path, with one exception: modules in PNR_FLAT are elaborated on their own
# without hierarchy and the rest with --keep-hierarchy around them as black
# boxes. core_wrap taps the core's event bundle by hierarchical reference,
# which yosys-slang rejects across a kept module boundary (also in its best
# effort mode); every other module boundary is kept.
# Environment: PNR_LIBS (standard cells), PNR_MACRO_LIBS, PNR_FILES,
# PNR_INCDIRS, PNR_DEFINES, PNR_TOP, PNR_PARAMS (name value ...), PNR_FLAT,
# PNR_OUT.
yosys -import

set libs [concat $::env(PNR_LIBS) $::env(PNR_MACRO_LIBS)]
read_liberty -overwrite -setattr liberty_cell -lib {*}$libs
read_liberty -overwrite -setattr liberty_cell -unit_delay -wb -ignore_miss_func -ignore_buses {*}$libs

set common [list -D SYNTHESIS --compat=vcs --ignore-assertions]
foreach d $::env(PNR_INCDIRS) { lappend common "-I$d" }
foreach d $::env(PNR_DEFINES) { lappend common -D $d }
# Macros are black boxes even where a Verilog model is around (as in ORFS)
foreach lib $::env(PNR_MACRO_LIBS) {
  set fh [open $lib r]
  foreach {_ cell} [regexp -all -inline {\n\s*cell\s*\(\s*"?([^")\s]+)"?\s*\)} [read $fh]] {
    lappend common --blackboxed-module $cell
  }
  close $fh
}

set args [concat $common [list --keep-hierarchy --top $::env(PNR_TOP)]]
dict for {k v} $::env(PNR_PARAMS) { lappend args -G "$k=$v" }
foreach entry [split $::env(PNR_FLAT) ";"] {
  if {[lindex $entry 0] ne ""} { lappend args --blackboxed-module [lindex $entry 0] }
}
read_slang {*}$args {*}$::env(PNR_FILES)
setattr -unset init

# PNR_FLAT: module name followed by the parameters its instances carry (their
# defaults: the flat module is elaborated with those), "core_wrap A B ; ..."
foreach entry [split $::env(PNR_FLAT) ";"] {
  set params [lassign $entry m]
  if {$m eq ""} continue
  # = selects black boxes too
  select -assert-mod-count 1 =$m
  delete =$m
  read_slang {*}$common --top $m {*}$::env(PNR_FILES)
  setattr -unset init
  foreach p $params { setparam -unset $p t:$m }
}

hierarchy -check -top $::env(PNR_TOP)
opt_clean -purge
write_rtlil $::env(PNR_OUT)
