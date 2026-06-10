# Build scripts drop all extra CMake flags except the first one

- Severity: Medium
- Status: Fixed — `build_andes.sh` now forwards `"$@"`; `build_macos.sh`
  had already been fixed the same way. Verified with `bash -n` (Andes
  build not runnable from this machine).
- Component: `build_macos.sh`, `build_andes.sh`

## Summary
Both scripts append only `$1` to the `cmake` configure command. If a caller passes multiple extra flags (for example `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DAMREX_DISABLE_STRUCTURED_BOUNDARY_CACHE=ON`), only the first is applied and the rest are silently ignored.

## Impact
- Builds can be misconfigured without obvious errors.
- Users think they passed flags that were never applied.

## Proposed Fix
Forward all trailing arguments with `"$@"`.

```diff
diff --git a/build_macos.sh b/build_macos.sh
index f2fd65d..d43487e 100644
--- a/build_macos.sh
+++ b/build_macos.sh
@@ -5,7 +5,7 @@ set -x
 # fetch MPICH headers (not bundled with VisIt)
 ./fetch_mpich_headers.sh
 
-cmake -S . -B build -GNinja -DVISIT_USE_VENDORED_MPICH=ON -DVISIT_PLUGIN_DIR="${HOME}/.visit/3.4.2/darwin-arm64/plugins" -DVISIT_PLUGIN_VS_INSTALL_FILE="/Applications/VisIt.app/Contents/Resources/3.4.2/darwin-arm64/include/PluginVsInstall.cmake" $1
+cmake -S . -B build -GNinja -DVISIT_USE_VENDORED_MPICH=ON -DVISIT_PLUGIN_DIR="${HOME}/.visit/3.4.2/darwin-arm64/plugins" -DVISIT_PLUGIN_VS_INSTALL_FILE="/Applications/VisIt.app/Contents/Resources/3.4.2/darwin-arm64/include/PluginVsInstall.cmake" "$@"
 
 cmake --build build
 # the plugin should actually be installed to ~/.visit in the build step
diff --git a/build_andes.sh b/build_andes.sh
index a4ee247..f74ebe0 100755
--- a/build_andes.sh
+++ b/build_andes.sh
@@ -24,7 +24,7 @@ cmake -S . -B build \
   -DMPICH_HEADERS_ROOT="${MPICH_HEADERS_ROOT}" \
   -DMPI_LIBRARY_DIR=/sw/sources/visit/andes/3.4.2/build_dir/third_party/mpich/3.3.1/linux-x86_64_gcc-8.5/lib \
-  -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON $1
+  -DAMReX_PIC=ON -DAMReX_BUILD_SHARED_LIBS=ON "$@"
 cmake --build build -j16
 
 # the plugin should actually be installed to ~/.visit in the build step
```
