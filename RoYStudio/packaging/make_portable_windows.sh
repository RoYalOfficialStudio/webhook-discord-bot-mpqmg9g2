#!/usr/bin/env bash
# Builds RoYStudio-<version>-PORTABLE-win64.zip from a finished Windows build directory.
#   packaging/make_portable_windows.sh [build-win dir] [output dir]
# Needs: a Windows x64 build (cmake/mingw-w64-x86_64.cmake, Release) and, for the test kit
# content, either a Linux build of roy_cli (build/roy_cli) or a way to run roy_cli.exe
# (Wine, or WSL interop). Only free tools are used: bash, zip (or python3), sha256sum.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BW="$(cd "${1:-$ROOT/build-win}" && pwd)"
OUT="${2:-$ROOT/dist}"
VER="$(sed -n 's/^project(RoYStudio VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
NAME="RoYStudio-$VER-PORTABLE-win64"
STAGE="$OUT/$NAME"
P="$ROOT/packaging/portable"

for f in RoYStudio.exe RoYPluginHost.exe roy_cli.exe roy_mp3lame.dll roy_tests.exe roy_soak.exe; do
  [ -f "$BW/$f" ] || { echo "missing $BW/$f - build first: cmake --build $BW" >&2; exit 1; }
done
[ -d "$BW/portable_plugins/RoYTestPlugins.vst3" ] || { echo "missing portable_plugins/RoYTestPlugins.vst3 (target roy_vst3_test_safe)" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"/{Plugins,Projects,Samples,licenses,TestKit/AutomatedTests}
# program
cp "$BW"/{RoYStudio.exe,RoYPluginHost.exe,roy_cli.exe,roy_mp3lame.dll} "$STAGE/"
cp "$P"/{START_ROY_STUDIO.bat,README_TEST.txt,RoYStudio.portable,LICENSE_DECISION_PENDING.txt} "$STAGE/"
# licences of all third-party components
cp "$ROOT/third_party/THIRD_PARTY_NOTICES.md" "$STAGE/licenses/"
for lic in clap/LICENSE imgui/LICENSE.txt lame/COPYING lame/LICENSE miniaudio/LICENSE nlohmann/LICENSE.MIT vst3sdk/LICENSE.txt; do
  mkdir -p "$STAGE/licenses/$(dirname "$lic")"
  cp "$ROOT/third_party/$lic" "$STAGE/licenses/$lic"
done
# safe RoY TEST plugins (no crash/hang classes) - found automatically via <exe dir>/Plugins
cp -r "$BW/test_plugins/roy_test_gain.clap" "$STAGE/Plugins/"
cp -r "$BW/portable_plugins/RoYTestPlugins.vst3" "$STAGE/Plugins/"
printf 'RoY TEST plugins for the first test (free, self-made, no crash/hang test classes).\r\nPut your own .vst3 / .clap plugins here too if you like - RoY scans this folder.\r\n' > "$STAGE/Plugins/README.txt"
printf 'Your RoY Studio projects are saved here (portable mode).\r\nCopy this folder somewhere safe before deleting the RoY Studio folder.\r\n' > "$STAGE/Projects/README.txt"

# test kit content (self-generated): tones, synthetic vocal, drum samples, MIDI, test project
KIT="$STAGE/TestKit"
if [ -x "$ROOT/build/roy_cli" ]; then
  "$ROOT/build/roy_cli" make-testkit "$KIT" >/dev/null
elif command -v wslpath >/dev/null 2>&1; then
  "$BW/roy_cli.exe" make-testkit "$(wslpath -w "$KIT")" >/dev/null
else
  wine "$BW/roy_cli.exe" make-testkit "Z:$KIT" >/dev/null
fi
mv "$KIT/Samples/Drums" "$STAGE/Samples/Drums"
rmdir "$KIT/Samples"
cp "$P/TestKit/TEST_RESULTS.md" "$P"/TestKit/*.bat "$KIT/"
# automated tests (incl. the deliberately crashing/hanging test plugins) - kept apart from Plugins\
AT="$KIT/AutomatedTests"
cp "$BW"/{roy_tests.exe,roy_soak.exe,RoYPluginHost.exe,roy_cli.exe,roy_mp3lame.dll} "$AT/"
mkdir -p "$AT/test_plugins"
cp -r "$BW"/test_plugins/*.clap "$BW"/test_plugins/*.vst3 "$AT/test_plugins/"
cp "$BW"/test_plugins/roy_test_stem_engine.exe "$AT/test_plugins/"
printf 'Automated test suite. Start it with TestKit\\RUN_AUTOMATED_TESTS.bat.\r\nThe test plugins in test_plugins\\ include ones that crash or hang ON PURPOSE.\r\n' > "$AT/README.txt"

# build information + checksums
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
{
  printf 'RoY Studio %s - UNSIGNED DEVELOPMENT/TEST BUILD\r\n' "$VER"
  printf 'source commit: %s (branch claude/bold-franklin-fybkj5)\r\n' "$COMMIT"
  printf 'built: %s UTC\r\n' "$(date -u +%Y-%m-%dT%H:%M:%S)"
  printf 'toolchain: mingw-w64 %s (x86_64, static runtime), CMake %s, Release\r\n' \
    "$(x86_64-w64-mingw32-g++-posix -dumpversion 2>/dev/null || x86_64-w64-mingw32-g++ -dumpversion 2>/dev/null || echo ?)" "$(cmake --version | head -1 | awk '{print $3}')"
  printf 'licence of RoY Studio: LICENSE DECISION PENDING\r\n'
} > "$STAGE/BUILD_INFO.txt"
(cd "$STAGE" && find . -type f \( -name '*.exe' -o -name '*.dll' -o -name '*.vst3' -o -name '*.clap' \) -print0 | sort -z \
  | xargs -0 sha256sum | sed 's|  \./|  |; s|/|\\|g; s|$|\r|' > SHA256SUMS.txt)

# zip
rm -f "$OUT/$NAME.zip"
if command -v zip >/dev/null 2>&1; then
  (cd "$OUT" && zip -qr -X "$NAME.zip" "$NAME")
else
  (cd "$OUT" && python3 -m zipfile -c "$NAME.zip" "$NAME")
fi
echo "$OUT/$NAME.zip"
sha256sum "$OUT/$NAME.zip"
