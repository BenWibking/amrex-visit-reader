#!/bin/bash

## build with ADIOS2 + MPI support on Andes (VisIt's vendored MPICH)
## NOTE: you can't use any non-default modules on Andes -- they don't get loaded when Visit is run in client-server mode
module reset

set -x

MPICH_HEADERS_ROOT_DEFAULT="/sw/sources/visit/andes/3.4.2/build_dir/third_party/mpich/3.3.1/linux-x86_64_gcc-8.5/include"
MPICH_HEADERS_ROOT="${MPICH_HEADERS_ROOT:-$MPICH_HEADERS_ROOT_DEFAULT}"
if [ ! -f "${MPICH_HEADERS_ROOT}/mpi.h" ]; then
  # Fall back to locally fetched headers if the system path is unavailable.
  ./fetch_mpich_headers.sh
  MPICH_HEADERS_ROOT="$(pwd)/build/_deps/mpich-install/include"
fi

cmake -S . -B build \
  -DVISIT_USE_VENDORED_MPICH=ON \
  -DVISIT_ROOT_INCLUDE_DIR=/sw/andes/visit/3.4.2/linux-x86_64/include \
  -DVISIT_LIBRARY_DIR=/sw/andes/visit/3.4.2/linux-x86_64/lib \
  -DVISIT_BINARY_DIR=/sw/andes/visit/3.4.2/linux-x86_64/bin \
  -DMPICH_HEADERS_ROOT="${MPICH_HEADERS_ROOT}" \
  -DMPI_LIBRARY_DIR=/sw/sources/visit/andes/3.4.2/build_dir/third_party/mpich/3.3.1/linux-x86_64_gcc-8.5/lib \
  -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON $1
cmake --build build -j16

# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
