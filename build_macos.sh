## do a standard build without MPI
set -x

cmake -S . -B build -GNinja -DVISIT_PLUGIN_DIR="${HOME}/.visit/3.4.2/darwin-arm64/plugins" -DVISIT_PLUGIN_VS_INSTALL_FILE="/Applications/VisIt.app/Contents/Resources/3.4.2/darwin-arm64/include/PluginVsInstall.cmake" $1

cmake --build build
# the plugin should actually be installed to ~/.visit in the build step
#cmake --install build
