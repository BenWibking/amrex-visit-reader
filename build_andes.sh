## build with ADIOS2 + MPI support on Andes
## NOTE: you can't use any non-default modules on Andes -- they don't get loaded when Visit is run in client-server mode
module reset

# build Blosc2
# ...

# build ADIOS2 with Blosc2 support
# ...

set -x
#CXX=g++ CC=gcc ## DO NOT SET
cmake -S . -B build $1
cmake --build build -j16

# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
