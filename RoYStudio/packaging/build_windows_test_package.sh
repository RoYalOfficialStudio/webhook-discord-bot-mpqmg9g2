#!/usr/bin/env bash
# Reproducible build of the Windows test packages on Ubuntu 24.04 or WSL (Ubuntu from the
# Microsoft Store - free). Uses only free tools from the official Ubuntu archive:
#   mingw-w64 (GCC cross compiler), cmake, ninja, nsis, zip, wine (only for the test kit content
#   when no Linux build is present) - no Visual Studio, no paid SDKs.
# Result: dist/RoYStudio-<ver>-PORTABLE-win64.zip and build-win/RoYStudio-<ver>-win64.exe (installer)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if ! command -v x86_64-w64-mingw32-g++-posix >/dev/null || ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
  echo "installing free build tools from the Ubuntu archive (needs sudo)..."
  sudo apt-get update
  sudo apt-get install -y g++ cmake ninja-build mingw-w64 g++-mingw-w64-x86-64-posix nsis zip libglfw3-dev libx11-dev
fi
# 1) Linux tools (only roy_cli is needed: it generates the test kit content)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DROY_BUILD_GUI=OFF -DROY_BUILD_TESTS=OFF
cmake --build build --target roy_cli
# 2) Windows x64 build (static runtime, all targets incl. tests and test plugins)
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release -DROY_BUILD_GUI=ON -DROY_BUILD_TESTS=ON
cmake --build build-win
# 3) packages
packaging/make_portable_windows.sh build-win dist
cmake --build build-win --target package
echo
echo "PORTABLE: $ROOT/dist/RoYStudio-*-PORTABLE-win64.zip"
echo "INSTALLER: $ROOT/build-win/RoYStudio-*-win64.exe"
