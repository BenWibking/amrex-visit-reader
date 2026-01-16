#!/bin/bash

## build with ADIOS2 + MPI support on Andes (VisIt's vendored MPICH)
## NOTE: you can't use any non-default modules on Andes -- they don't get loaded when Visit is run in client-server mode
module reset

set -x

cmake -S . -B build -DVISIT_USE_VENDORED_MPICH=ON -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON $1
cmake --build build -j16

# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
