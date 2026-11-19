# Global connections (GLOBAL_CONNECT_TCL): standard cells as in the platform,
# and the SRAM macros' supplies (array and periphery) on the same nets.
add_global_connection -net {VDD} -pin_pattern {^VDD$} -power
add_global_connection -net {VDD} -pin_pattern {^VDDPE$}
add_global_connection -net {VDD} -pin_pattern {^VDDCE$}
add_global_connection -net {VSS} -pin_pattern {^VSS$} -ground
add_global_connection -net {VSS} -pin_pattern {^VSSE$}
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {^VDD!$} -power
add_global_connection -net {VDD} -inst_pattern {.*} -pin_pattern {^VDDARRAY!$} -power
add_global_connection -net {VSS} -inst_pattern {.*} -pin_pattern {^VSS!$} -ground
