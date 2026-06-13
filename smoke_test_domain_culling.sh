#!/bin/bash
# ABOUTME: Smoke test that spatial and data extents metadata cull domains
# ABOUTME: before I/O for the Box operator and Contour plots.

set -euo pipefail
set -x

rm -f ./*.vlog
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -debug 1 \
  -s example_data/check_domain_culling.py box

rm -f ./*.vlog
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -debug 1 \
  -s example_data/check_domain_culling.py contour

rm -f ./*.vlog
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -debug 1 \
  -s example_data/check_domain_culling.py contour_geometry
