# Repository Guidelines

## Project Structure & Module Organization
Core reader logic and data layout transformations live in `avtamrexFileFormat.C/.h`. Generated VisIt metadata shims (`amrex*PluginInfo.C`) wire the plugin into the host and rarely need manual edits. Vendored dependencies (AMReX reference sources and sample datasets) sit under `extern/` and are pulled in by the top-level `CMakeLists.txt`. Build outputs go to `build/`, while `.amrex` fixtures and helper scripts remain in `example_data/`. Keep `VisItLibraryDependencies.cmake` beside `CMakeLists.txt` so VisIt’s toolchain resolves its libraries.

## Build, Test, and Development Commands
- `git submodule update --init` — fetches the required externals.
- `cmake -S . -B build -GNinja` — configure after pointing the VisIt paths at the top of `CMakeLists.txt` to your install.
- `ninja -C build` — builds and installs the plugin into `~/.visit`.
- `example_data/extract_example_data.sh` — unpacks reference data for manual checks.
Run `./fix_VisItLibraryDependencies_macos.sh` once on macOS to match dynamic library ids.

## Coding Style & Naming Conventions
Follow the VisIt C++ conventions already present: two-space indentation, braces on new lines for functions, `CamelCase` types such as `avtamrexFileFormat`, and `snake_case` locals. Favor STL utilities over hand-rolled helpers, keep includes ordered, and limit comments to intent-level notes. Regenerate rather than hand-edit the `amrex*PluginInfo` files if VisIt XML definitions change.

## Testing Guidelines
Automated tests are not yet wired up; validation happens in VisIt. After extracting fixtures, open the `.amrex` descriptors and confirm meshes, particles, and axis-label overrides render correctly. Exercise any new data layout paths with both default and overridden axis orders. If you add scripts or regression assets, place them under `example_data/` (or a new `tests/`) and document how to invoke them.

## Commit & Pull Request Guidelines
History favors concise, imperative commits (for example, “fix path”). Keep changes focused, and add a body when touching build configuration or data layout behavior. Pull requests should state the VisIt version used, datasets opened, and any manual verification steps. Attach screenshots for UI-visible changes, and link to issues or upstream VisIt tickets when applicable.

## Configuration Tips
Update `VISIT_PLUGIN_DIR` and the VisIt include path near the top of `CMakeLists.txt` before configuring. Ensure `VisItLibraryDependencies.cmake` matches the VisIt release you target, and re-run CMake after switching versions.
