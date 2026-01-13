#!/bin/sh
set -eu

MPICH_VERSION="3.3.1"
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="${ROOT_DIR}/build/_deps"
TARBALL="${DEPS_DIR}/mpich-${MPICH_VERSION}.tar.gz"
SRC_DIR="${DEPS_DIR}/mpich-src"
BUILD_DIR="${DEPS_DIR}/mpich-build"
PREFIX_DIR="${DEPS_DIR}/mpich-install"

if [ -f "${PREFIX_DIR}/include/mpi.h" ]; then
    echo "MPICH headers already installed at ${PREFIX_DIR}/include"
    exit 0
fi

mkdir -p "${DEPS_DIR}"

if [ ! -f "${TARBALL}" ]; then
    curl -L -o "${TARBALL}" \
        "https://www.mpich.org/static/downloads/${MPICH_VERSION}/mpich-${MPICH_VERSION}.tar.gz"
fi

rm -rf "${SRC_DIR}" "${BUILD_DIR}"
tar -xzf "${TARBALL}" -C "${DEPS_DIR}"
mv "${DEPS_DIR}/mpich-${MPICH_VERSION}" "${SRC_DIR}"

mkdir -p "${BUILD_DIR}" "${PREFIX_DIR}"
cd "${BUILD_DIR}"
"${SRC_DIR}/configure" --prefix="${PREFIX_DIR}" --disable-fortran --disable-cxx

NPROC=1
if command -v sysctl >/dev/null 2>&1; then
    NPROC="$(sysctl -n hw.ncpu)"
fi

make -j "${NPROC}"
make install
