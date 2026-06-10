#!/bin/bash
# ABOUTME: Smoke test wrapper that opens the synthetic ghost-cell plotfile in VisIt.
# ABOUTME: Verifies scalar data is read from the FAB valid region when MultiFabs have ghost cells.

set -x

plotfile="example_data/GhostCells/plt_ghost00000"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin \
  -s example_data/check_ghost_cell_plotfile.py "$plotfile"
