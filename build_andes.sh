#!/bin/bash

## build with ADIOS2 + MPI support on Andes
## NOTE: you can't use any non-default modules on Andes -- they don't get loaded when Visit is run in client-server mode
module reset

set -x
#CXX=g++ CC=gcc ## DO NOT SET
MPI_C_COMPILER=$(command -v mpicc)
MPI_CXX_COMPILER=$(command -v mpicxx)
MPIEXEC_EXECUTABLE=$(command -v mpiexec)
if [[ -z "$MPI_C_COMPILER" || -z "$MPI_CXX_COMPILER" || -z "$MPIEXEC_EXECUTABLE" ]]; then
  echo "OpenMPI wrappers not found in PATH; load the VisIt/OpenMPI module." >&2
  exit 1
fi
cmake -S . -B build -DVISIT_USE_VENDORED_MPICH=OFF -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON \
  -DMPI_C_COMPILER="${MPI_C_COMPILER}" -DMPI_CXX_COMPILER="${MPI_CXX_COMPILER}" \
  -DMPIEXEC_EXECUTABLE="${MPIEXEC_EXECUTABLE}" $1
cmake --build build -j16

# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
