#!/bin/bash
# ABOUTME: Smoke test wrapper that runs the field timestep-revisit regression check.
# ABOUTME: Verifies field plots still render after returning to a previously viewed timestep.

set -x

plotfile="example_data/DiskGalaxy/plt0000020"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin \
  -s example_data/check_field_timestep_revisit.py "$plotfile"
