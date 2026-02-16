#!/bin/bash

set -x

plotfile="example_data/DiskGalaxy/plt0000020"
/Applications/VisIt.app/Contents/Resources/bin/visit -cli -nowin \
  -s example_data/check_particle_plotfile_clearcache.py "$plotfile"
