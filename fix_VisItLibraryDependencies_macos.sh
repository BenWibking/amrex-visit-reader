#!/bin/sh

# this script fixes VisItLibraryDependencies.cmake on macOS

set -x
sed -i -e 's/-9\.2/\.9\.2/g' VisItLibraryDependencies.cmake
