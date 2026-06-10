#!/bin/bash

## build with ADIOS2 + MPI support on Andes (VisIt's vendored MPICH)
## NOTE: you can't use any non-default modules on Andes -- they don't get loaded when Visit is run in client-server mode
module reset

set -ex

VISIT_VERSION="${VISIT_VERSION:-3.5.0}"
VISIT_ROOT="${VISIT_ROOT:-/sw/andes/visit/${VISIT_VERSION}/linux-x86_64}"
VISIT_PLUGIN_DIR="${VISIT_PLUGIN_DIR:-${HOME}/.visit/${VISIT_VERSION}/linux-x86_64/plugins}"
VISIT_PLUGIN_VS_INSTALL_FILE="${VISIT_PLUGIN_VS_INSTALL_FILE:-${VISIT_ROOT}/include/PluginVsInstall.cmake}"
VISIT_LIBRARY_DEPENDENCIES_FILE="${VISIT_LIBRARY_DEPENDENCIES_FILE:-${VISIT_ROOT}/include/VisItLibraryDependencies.cmake}"
MPI_LIBRARY_DIR="${MPI_LIBRARY_DIR:-${VISIT_ROOT}/lib}"

if [ ! -f "${VISIT_PLUGIN_VS_INSTALL_FILE}" ]; then
  echo "Missing PluginVsInstall.cmake: ${VISIT_PLUGIN_VS_INSTALL_FILE}" >&2
  exit 1
fi

if [ ! -f "${VISIT_LIBRARY_DEPENDENCIES_FILE}" ]; then
  echo "Missing VisItLibraryDependencies.cmake: ${VISIT_LIBRARY_DEPENDENCIES_FILE}" >&2
  exit 1
fi

if [ ! -f "${MPI_LIBRARY_DIR}/libmpi.so" ]; then
  echo "Missing MPI library: ${MPI_LIBRARY_DIR}/libmpi.so" >&2
  exit 1
fi

MPICH_HEADERS_ROOT_DEFAULT="${VISIT_ROOT}/include/mpich/include"
MPICH_HEADERS_ROOT="${MPICH_HEADERS_ROOT:-$MPICH_HEADERS_ROOT_DEFAULT}"
if [ ! -f "${MPICH_HEADERS_ROOT}/mpi.h" ]; then
  # Fall back to locally fetched headers if the system path is unavailable.
  ./fetch_mpich_headers.sh
  MPICH_HEADERS_ROOT="$(pwd)/build/_deps/mpich-install/include"
fi

cmake -S . -B build \
  -DVISIT_USE_VENDORED_MPICH=ON \
  -DVISIT_PLUGIN_DIR="${VISIT_PLUGIN_DIR}" \
  -DVISIT_PLUGIN_VS_INSTALL_FILE="${VISIT_PLUGIN_VS_INSTALL_FILE}" \
  -DVISIT_LIBRARY_DEPENDENCIES_FILE="${VISIT_LIBRARY_DEPENDENCIES_FILE}" \
  -DVISIT_ROOT_INCLUDE_DIR="${VISIT_ROOT}/include" \
  -DVISIT_LIBRARY_DIR="${VISIT_ROOT}/lib" \
  -DVISIT_BINARY_DIR="${VISIT_ROOT}/bin" \
  -DMPICH_HEADERS_ROOT="${MPICH_HEADERS_ROOT}" \
  -DMPI_LIBRARY_DIR="${MPI_LIBRARY_DIR}" \
  -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON "$@"
cmake --build build -j16

# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
