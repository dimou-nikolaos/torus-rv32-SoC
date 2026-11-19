# Worst link paths of a grid run per class of tile output (scripts/pnr.py grid):
# X and Y links, and those through feedthroughs, with placement or global-route
# parasitics (PARASITICS: placement | global_routing). STD_LIB and SETRC are
# the platform's standard-cell liberty and wire RC (scripts/pnr.py).
read_liberty $::env(STD_LIB)
read_liberty $::env(TILE_LIB)
read_db $::env(ODB)
read_sdc $::env(SDC)
source $::env(SETRC)
if {$::env(PARASITICS) eq "global_routing"} { read_guides $::env(GUIDES) }
estimate_parasitics -$::env(PARASITICS)
set f [open $::env(OUT) w]
foreach {name pat} {x_out "*u_tile/x_out_o*" y_out "*u_tile/y_out_o*" x_ft "*u_tile/xft_o*" y_ft "*u_tile/yft_o*"} {
  set pins [get_pins -quiet $pat]
  if {[llength $pins] == 0} { puts $f "$name none"; continue }
  set paths [find_timing_paths -path_delay max -through $pins -group_path_count 2000 -endpoint_path_count 1 -sort_by_slack]
  set n 0; set worst 1e9; set sum 0
  foreach p $paths { set s [get_property $p slack]; incr n; set sum [expr {$sum + $s}]; if {$s < $worst} {set worst $s; set wp $p} }
  puts $f "$name paths $n worst_slack [format %.2f $worst] mean_slack [format %.2f [expr {$sum / max($n,1)}]] start [get_full_name [get_property $wp startpoint]] end [get_full_name [get_property $wp endpoint]]"
}
close $f
