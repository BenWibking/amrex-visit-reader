#!/bin/bash

## do a standard build without MPI
set -ex

VISIT_VERSION="${VISIT_VERSION:-3.5.0}"
VISIT_ARCH="${VISIT_ARCH:-darwin-arm64}"
VISIT_ROOT="${VISIT_ROOT:-/Applications/VisIt.app/Contents/Resources/${VISIT_VERSION}/${VISIT_ARCH}}"
VISIT_PLUGIN_DIR="${VISIT_PLUGIN_DIR:-${HOME}/.visit/${VISIT_VERSION}/${VISIT_ARCH}/plugins}"
VISIT_PLUGIN_VS_INSTALL_FILE="${VISIT_PLUGIN_VS_INSTALL_FILE:-${VISIT_ROOT}/include/PluginVsInstall.cmake}"
VISIT_LIBRARY_DEPENDENCIES_FILE="${VISIT_LIBRARY_DEPENDENCIES_FILE:-${VISIT_ROOT}/include/VisItLibraryDependencies.cmake}"

if [ ! -f "${VISIT_PLUGIN_VS_INSTALL_FILE}" ]; then
  echo "Missing PluginVsInstall.cmake: ${VISIT_PLUGIN_VS_INSTALL_FILE}" >&2
  exit 1
fi

if [ ! -f "${VISIT_LIBRARY_DEPENDENCIES_FILE}" ]; then
  echo "Missing VisItLibraryDependencies.cmake: ${VISIT_LIBRARY_DEPENDENCIES_FILE}" >&2
  exit 1
fi

FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE="${VISIT_LIBRARY_DEPENDENCIES_FILE}"
if [ "$(uname -s)" = "Darwin" ]; then
  mkdir -p build
  FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE="$(pwd)/build/VisItLibraryDependencies.macos.cmake"
  VTK_DASH_LIBS=("${VISIT_ROOT}"/lib/libvtkCommonCore-*.dylib)
  VTK_DOT_LIBS=("${VISIT_ROOT}"/lib/libvtkCommonCore.*.dylib)
  if [ -e "${VTK_DASH_LIBS[0]}" ]; then
    sed -E 's/(vtk[A-Za-z0-9_]+)\.([0-9]+)\.([0-9]+)/\1-\2.\3/g' \
      "${VISIT_LIBRARY_DEPENDENCIES_FILE}" > "${FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE}"
  elif [ -e "${VTK_DOT_LIBS[0]}" ]; then
    sed -E 's/(vtk[A-Za-z0-9_]+)-([0-9]+)\.([0-9]+)/\1.\2.\3/g' \
      "${VISIT_LIBRARY_DEPENDENCIES_FILE}" > "${FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE}"
  else
    cp "${VISIT_LIBRARY_DEPENDENCIES_FILE}" "${FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE}"
  fi
fi

# fetch MPICH headers (not bundled with VisIt)
./fetch_mpich_headers.sh

cmake -S . -B build -GNinja \
  -DVISIT_USE_VENDORED_MPICH=ON \
  -DVISIT_PLUGIN_DIR="${VISIT_PLUGIN_DIR}" \
  -DVISIT_PLUGIN_VS_INSTALL_FILE="${VISIT_PLUGIN_VS_INSTALL_FILE}" \
  -DVISIT_LIBRARY_DEPENDENCIES_FILE="${FILTERED_VISIT_LIBRARY_DEPENDENCIES_FILE}" \
  "$@"

cmake --build build
# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
