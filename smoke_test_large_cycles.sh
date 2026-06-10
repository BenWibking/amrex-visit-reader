#!/bin/bash
# ABOUTME: Smoke test wrapper for plotfile timestep suffixes larger than LLONG_MAX.
# ABOUTME: Verifies the series includes the huge-suffix directory and its cycle clamps to INT_MAX.

set -x

plotfile="example_data/DiskGalaxy/plt0000020"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin \
  -s example_data/check_large_cycle_suffix.py "$plotfile"
