# amrex-visit-plugin (beta)

This is a parallel database plugin for VisIt that reads AMReX plotfiles using the native AMReX I/O layer.

Supported:
* Cartesian AMR hierarchies stored in AMReX plotfiles
* Cell-centered scalar variables
* Cell-centered vector variables (detected from `_x/_y/_z` naming conventions)
* Particle species (Modern AMReX formats)

Unplanned:
* Legacy BoxLib/AMReX `Version_One_Dot_*` particle headers
* non-Cartesian geometry
* Face-centered variables
* Edge-centered variables
* Node-centered variables

> **Note:** AMReX encodes the mesh dimensionality via the compile-time flag `AMREX_SPACEDIM`, so the plugin must be built separately for 2D and 3D plotfiles.

## Building

1. First, make sure all of the submodules are up to date:
   ```
   git submodule update --init
   ```

2. Copy `VisItLibraryDependencies.cmake` from your VisIt installation to the root of the repository.

3. *(macOS only)* Run:
   ```
   ./fix_VisItLibraryDependencies_macos.sh
   ```

4. Finally, build with (edit `VISIT_PLUGIN_DIR` and `VISIT_PLUGIN_VS_INSTALL_FILE` according to your installation):
   ```
   mkdir build && cd build
   cmake .. -GNinja -DVISIT_PLUGIN_DIR="${HOME}/.visit/3.4.2/darwin-arm64/plugins" -DVISIT_PLUGIN_VS_INSTALL_FILE="/Applications/VisIt.app/Contents/Resources/3.4.2/darwin-arm64/include/PluginVsInstall.cmake"
   ninja
   ```
   The plugin should be installed to your `~/.visit` directory, where VisIt should detect and load it automatically. After recompiling the plugin, you may have to restart VisIt in order to use the new version of the plugin.

   To skip caching the structured domain boundary metadata (and therefore disable VisIt's ghost-synthesis metadata path), add `-DAMREX_DISABLE_STRUCTURED_BOUNDARY_CACHE=ON` when invoking CMake.

## Testing

1. Extract the example data using the provided script:
   ```
   cd example_data
   ./extract_example_data.sh
   ```

2. Load any `plt[0-9]*` plotfile directory in the `example_data/` directory in VisIt (clicking `Open` in the main window). The plugin scans the parent directory for sibling `plt*` entries and aggregates them into a time series. If the GUI doesn't allow selecting directories, choose the plotfile `Header` file (for example, `plt00000/Header`). If VisIt fails to auto-detect the reader, use `Open As...` (GUI) or `visit -o <plotfile_dir>,amrex-plotfile` on the CLI.
3. Plot the meshes and variables exposed in VisIt to confirm AMR level decomposition and scalar/vector values look correct.
