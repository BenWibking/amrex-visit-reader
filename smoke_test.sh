#!/bin/bash

set -x
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -s example_data/check_parallel_plotfile_open.py <<'EOF'
Exit()
EOF
