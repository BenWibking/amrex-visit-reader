#!/bin/bash
# ABOUTME: Smoke test for plugin read options: boundary cache off-switch and
# ABOUTME: invariant mesh declaration. Scans vlogs from a -debug 1 CLI run.

set -euo pipefail
set -x

rm -f ./*.vlog
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -debug 1 \
  -s example_data/check_read_options.py
