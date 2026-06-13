# amrex-visit-plugin (beta)

This is a parallel database plugin for VisIt that reads AMReX plotfiles through
AMReX's native I/O layer. VisIt registers the reader as `amrex-plotfile`.

## Supported Data

Supported:

* Cartesian AMR hierarchies stored in AMReX plotfiles
* Cell-centered scalar variables
* Cell-centered vector variables, detected from `_x/_y/_z` naming conventions
* Particle species in modern AMReX plotfile layouts

Not supported:

* Legacy BoxLib/AMReX `Version_One_Dot_*` particle headers
* Non-Cartesian geometry
* Face-centered variables
* Edge-centered variables
* Node-centered variables

AMReX encodes mesh dimensionality through the compile-time `AMREX_SPACEDIM`
setting. The current CMake build configures AMReX for 3D plotfiles.

## Read Options

The reader exposes two options in VisIt's file-open options dialog (or via
`SetDefaultFileOpenOptions("amrex-plotfile", ...)` in the CLI):

* `Build domain boundaries for ghost synthesis` (default on). Builds and
  caches the structured domain boundary object VisIt uses to synthesize
  ghost zones between same-level patches. The object costs
  O(#domains x #neighbors) memory on every engine rank, which dominates
  per-rank memory for plotfiles with very large domain counts (it is never
  built in the metadata server). Turn it off for hero-scale datasets; plots,
  queries, and AMR nesting ghosts still work, but operators that need
  synthesized boundary ghosts (for example Inverse Ghost Zone on
  single-level data) will see no ghost zones, and the combined MinMax query
  reports values without its usual formatted location string on
  single-level data.
* `Mesh structure is invariant across timesteps` (default off). Declares
  that the patch layout and variable list are identical for every plotfile
  in the series, letting VisIt skip metadata re-reads and SIL rebuilds on
  timestep changes. Only enable it when the run never regrids.

The plugin also publishes per-domain spatial extents and, when the VisMF
headers record per-FAB min/max values, per-domain data extents. Operators
that restrict their selection (Box, Slice, Clip, Threshold, Contour) then
read only the domains that can contribute, which is the recommended way to
explore datasets too large to load in full.

## Requirements

Install VisIt before building the plugin. The build needs VisIt's development
CMake files, libraries, MPI wrappers, and plugin install layout.

Required local tools:

* CMake 3.18 or newer
* A C++17 compiler
* Ninja for the macOS helper script
* `curl`, `tar`, `make`, and a POSIX shell for the MPICH header bootstrap

The first configure/build may download AMReX through CMake `FetchContent` and
may download MPICH 3.3.1 headers into `build/_deps/mpich-install`.

## Build

Initialize the bundled `cpptrace` submodule first:

```sh
git submodule update --init extern/cpptrace
```

The preferred entry points are the repository build scripts. They set the VisIt
plugin paths, use VisIt's MPI runtime, point CMake at VisIt's
`PluginVsInstall.cmake` and `VisItLibraryDependencies.cmake`, and build into
`build/`. OpenMPI builds are not supported; keep
`VISIT_USE_VENDORED_MPICH=ON`.

### macOS

For a standard VisIt.app install:

```sh
./build_macos.sh
```

Defaults:

```sh
VISIT_VERSION=3.5.0
VISIT_ARCH=darwin-arm64
VISIT_ROOT=/Applications/VisIt.app/Contents/Resources/${VISIT_VERSION}/${VISIT_ARCH}
VISIT_PLUGIN_DIR=${HOME}/.visit/${VISIT_VERSION}/${VISIT_ARCH}/plugins
VISIT_PLUGIN_VS_INSTALL_FILE=${VISIT_ROOT}/include/PluginVsInstall.cmake
VISIT_LIBRARY_DEPENDENCIES_FILE=${VISIT_ROOT}/include/VisItLibraryDependencies.cmake
```

Override these environment variables when VisIt is installed somewhere else:

```sh
VISIT_VERSION=3.5.0 \
VISIT_ARCH=darwin-x86_64 \
VISIT_ROOT=/path/to/VisIt/Contents/Resources/3.5.0/darwin-x86_64 \
./build_macos.sh
```

On macOS the script writes a filtered
`build/VisItLibraryDependencies.macos.cmake` when VisIt's VTK library naming
does not match the dependency file. You no longer need to copy
`VisItLibraryDependencies.cmake` into the repository or run a separate fixup
script.

Extra CMake options can be passed after the script name:

```sh
./build_macos.sh -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### Andes

On OLCF Andes:

```sh
./build_andes.sh
```

Defaults:

```sh
VISIT_VERSION=3.5.0
VISIT_ROOT=/sw/andes/visit/${VISIT_VERSION}/linux-x86_64
VISIT_PLUGIN_DIR=${HOME}/.visit/${VISIT_VERSION}/linux-x86_64/plugins
VISIT_PLUGIN_VS_INSTALL_FILE=${VISIT_ROOT}/include/PluginVsInstall.cmake
VISIT_LIBRARY_DEPENDENCIES_FILE=${VISIT_ROOT}/include/VisItLibraryDependencies.cmake
MPI_LIBRARY_DIR=${VISIT_ROOT}/lib
```

The Andes script resets loaded modules, uses VisIt's vendored MPICH runtime,
and uses `${VISIT_ROOT}/include/mpich/include` when those headers are present.
If the headers are missing, it runs `fetch_mpich_headers.sh` and points CMake at
the locally installed headers under `build/_deps/mpich-install/include`.

### Manual CMake

Use the scripts when possible. For other VisIt installations, configure CMake
with the same paths explicitly:

```sh
./fetch_mpich_headers.sh

cmake -S . -B build -GNinja \
  -DVISIT_USE_VENDORED_MPICH=ON \
  -DVISIT_PLUGIN_DIR="${HOME}/.visit/3.5.0/linux-x86_64/plugins" \
  -DVISIT_PLUGIN_VS_INSTALL_FILE="/path/to/visit/include/PluginVsInstall.cmake" \
  -DVISIT_LIBRARY_DEPENDENCIES_FILE="/path/to/visit/include/VisItLibraryDependencies.cmake" \
  -DVISIT_ROOT_INCLUDE_DIR="/path/to/visit/include" \
  -DVISIT_LIBRARY_DIR="/path/to/visit/lib" \
  -DVISIT_BINARY_DIR="/path/to/visit/bin" \
  -DMPICH_HEADERS_ROOT="${PWD}/build/_deps/mpich-install/include" \
  -DMPI_LIBRARY_DIR="/path/to/visit/lib"

cmake --build build
```

The build step installs the plugin libraries into the configured
`VISIT_PLUGIN_DIR` and copies the AMReX shared library beside the database
plugin. Restart VisIt after rebuilding so it reloads the plugin.

Useful CMake options:

* `-DAMREX_GIT_TAG=<tag>` chooses the AMReX version fetched by CMake. The
  default is `24.05`.

## Testing

Extract the public example datasets:

```sh
cd example_data
./extract_example_data.sh
cd ..
```

Open a plotfile directory in VisIt and force the `amrex-plotfile` reader if
auto-detection does not select it:

```sh
visit -o example_data/Nyx_LyA/plt00000,amrex-plotfile
```

If the GUI does not allow selecting directories, choose the plotfile `Header`
file, for example `example_data/Nyx_LyA/plt00000/Header`.

When a plotfile directory is opened, the plugin scans its parent directory for
sibling `plt*` entries and exposes them as a time series.

The repository also includes VisIt CLI smoke tests. These scripts assume the
macOS VisIt.app command path and can be edited for other installations:

```sh
./smoke_test.sh
./smoke_test_particles.sh
./smoke_test_particles_clearcache.sh
./smoke_test_field_revisit.sh
./smoke_test_large_cycles.sh
./smoke_test_ghost_cells.sh
./smoke_test_lazy_boundaries.sh
./smoke_test_read_options.sh
./smoke_test_domain_culling.sh
```

For manual validation, plot meshes, scalar variables, vector variables, and
particle species. After build or layout changes, check both metadata discovery
and rendering/query operations in VisIt.
