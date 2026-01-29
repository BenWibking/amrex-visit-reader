#!/bin/bash

set -x
plotfile="${1:-example_data/Nyx_LyA/plt00000}"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -s example_data/check_particle_plotfile_open.py "$plotfile" <<'EOF'
Exit()
EOF

plotfile="example_data/DiskGalaxy/plt0000020"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -s example_data/check_particle_plotfile_open.py "$plotfile" <<'EOF'
Exit()
EOF
