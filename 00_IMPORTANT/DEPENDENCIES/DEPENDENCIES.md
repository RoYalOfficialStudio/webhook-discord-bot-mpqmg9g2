# RoY Studio – External Dependencies

All dependencies are vendored as source in `RoYStudio/third_party/`. No binaries, no DLLs, no plugins
from unknown sources. Hashes below were computed after download.

| DEPENDENCY | VERSION | SOURCE | LICENSE | PURPOSE | HASH (SHA-256) | INSTALLATION METHOD |
|---|---|---|---|---|---|---|
| miniaudio | 0.11.22 (2025-02-24) | https://github.com/mackron/miniaudio (tag `0.11.22`, raw file) | Public Domain or MIT-0 | Audio device I/O (WASAPI/DirectSound/ALSA/Pulse/JACK/CoreAudio/null), WAV/FLAC/MP3 decoding | `miniaudio.h`: 9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410 | Single header copied into `third_party/miniaudio/` |
| nlohmann/json | 3.11.3 | https://github.com/nlohmann/json/releases/tag/v3.11.3 (release asset `json.hpp`) | MIT | Project file format, state serialization | `json.hpp`: 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6 | Single header in `third_party/nlohmann/` |
| CLAP | 1.2.2 | https://github.com/free-audio/clap (git tag `1.2.2`, commit 27f20f81dec40b930d79ef429fd35dcc2d45db5b) | MIT | Plugin format headers (hosting + test plugins) | commit hash above | `include/` copied into `third_party/clap/` |
| Dear ImGui | 1.92.9 | https://github.com/ocornut/imgui (git tag `v1.92.9`, commit 01380c579715e62fb9a8d6ec0502c4ea83bfde6e) | MIT | GUI toolkit (core + Win32/DX11 + GLFW/OpenGL3 backends) | `imgui.cpp`: 91fb4ad056cdb9127e827a68342143a81ec08ea6797ca27a5c13142c62c6a324, `imgui.h`: 74c114582c8dafd7063b9d96c6dc0abebdcae2b807f67be18c6235ef9c136b57 | Source files copied into `third_party/imgui/` (unmodified) |

| Steinberg VST3 SDK | 3.8.1 (tag `v3.8.1_build_84`, commit 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96; base fcf9da0b, pluginterfaces 4f547e8e, public.sdk 586dc5e6) | https://github.com/steinbergmedia/vst3sdk (+ official submodules vst3_base, vst3_pluginterfaces, vst3_public_sdk) | MIT (LICENSE.txt included in third_party/vst3sdk) | VST3 hosting in RoYPluginHost + RoY VST3 test plugins | git commits above | Sources copied into `third_party/vst3sdk/` (AAX/AU/AUv3/InterAppAudio wrappers, samples, testsuite and VSTGUI not included); static libraries |

| LAME (libmp3lame) | 3.100 | https://sourceforge.net/projects/lame/files/lame/3.100/lame-3.100.tar.gz (verified identical to Ubuntu lame_3.100.orig.tar.gz) | LGPL-2.0 (COPYING/LICENSE in third_party/lame) | MP3 export | tarball: ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e | libmp3lame sources copied unmodified into `third_party/lame/`; built as separate shared library `roy_mp3lame`, loaded at runtime (LGPL: replaceable) |

## Toolchain (not shipped)
| Tool | Version | Source |
|---|---|---|
| GCC | 13.3.0 | Ubuntu 24.04 package |
| CMake | 3.28.3 | Ubuntu 24.04 package |
| Ninja | system | Ubuntu 24.04 package |
| mingw-w64 (g++-mingw-w64-x86-64-posix) | 13.2.0 (Ubuntu 26.1) | Ubuntu 24.04 package (apt) | GPL toolchain / runtime exceptions; binaries are linked statically | Windows x64 cross-build check |
| Wine (wine64) | 9.0 | Ubuntu 24.04 package (apt) | LGPL | Runs the Windows test suite and GUI self-test on Linux (test only) |
| GLFW (libglfw3-dev) | 3.3.10 | Ubuntu 24.04 package (apt) | zlib/libpng | Linux GUI window/input (development + CI screenshots); not used on Windows |
| Xvfb | system | Ubuntu 24.04 package | MIT/X11 | Headless display for GUI screenshots |
| zip | 3.0 | Ubuntu 24.04 package `zip` (apt) | Info-ZIP licence (BSD-style) | Packs the portable test ZIP (build time only, not shipped; `python3 -m zipfile` as fallback) | apt-verified package | `apt-get install zip` |
| NSIS (makensis) | 3.09-4ubuntu1 | Ubuntu 24.04 package `nsis` (apt, official archive) | zlib/libpng (installer stub; its LZMA code is CPL with the NSIS installer exception, bzip2 BSD-style) | Builds the Windows installer `RoYStudio-<version>-win64.exe` (64-bit stub via `packaging/makensis-amd64.sh`); the stub is part of the shipped installer | package verified by apt (signed Ubuntu archive) | `apt-get install nsis` |

## Not used / pending decisions
- **MP3 encoding**: approved by the owner on 2026-09-28 → LAME 3.100 added (see table and third_party/THIRD_PARTY_NOTICES.md).
- **RoY Studio licence** (owner decision): the repository root `LICENSE` belongs to the original repository
  template, not to RoY Studio. The installer shows `packaging/INSTALLER_NOTICE.txt` ("BETA test build,
  licence not yet published") until the owner decides.
- **Code signing certificate** (owner decision, costs money): NOT used for alpha/beta tests - the test packages
  are labelled "UNSIGNED DEVELOPMENT/TEST BUILD"; README_TEST.txt explains the SmartScreen warning and how to
  verify the files with SHA256SUMS.txt. Windows security is never switched off by RoY.
- **ASIO**: not included. The free Steinberg ASIO SDK is licensed GPLv3 (or a proprietary licence) - using it
  would decide RoY's licence, which is the owner's decision. WASAPI shared/exclusive needs no extra driver.
- **Runtime dependencies of the Windows build**: none besides Windows system DLLs (static mingw runtime;
  d3d11, d3dcompiler_47, dwmapi, gdi32, kernel32, msvcrt, ole32, shell32, user32, winmm) and our own
  roy_mp3lame.dll - no Visual C++ redistributable needed (checked with objdump).
- **Stem separation models** (e.g. Demucs, MIT code / model weights with their own terms): not added.
