#!/bin/bash
# ABOUTME: Smoke test that domain boundary info is built lazily, not during
# ABOUTME: metadata population. Scans vlogs produced by a -debug 1 CLI run.

set -euo pipefail
set -x

rm -f ./*.vlog
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin -debug 1 \
  -s example_data/check_lazy_domain_boundaries.py
