# RoY Studio – External Dependencies

All dependencies are vendored as source in `RoYStudio/third_party/`. No binaries, no DLLs, no plugins
from unknown sources. Hashes below were computed after download.

| DEPENDENCY | VERSION | SOURCE | LICENSE | PURPOSE | HASH (SHA-256) | INSTALLATION METHOD |
|---|---|---|---|---|---|---|
| miniaudio | 0.11.22 (2025-02-24) | https://github.com/mackron/miniaudio (tag `0.11.22`, raw file) | Public Domain or MIT-0 | Audio device I/O (WASAPI/DirectSound/ALSA/Pulse/JACK/CoreAudio/null), WAV/FLAC/MP3 decoding | `miniaudio.h`: 9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410 | Single header copied into `third_party/miniaudio/` |
| nlohmann/json | 3.11.3 | https://github.com/nlohmann/json/releases/tag/v3.11.3 (release asset `json.hpp`) | MIT | Project file format, state serialization | `json.hpp`: 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6 | Single header in `third_party/nlohmann/` |
| CLAP | 1.2.2 | https://github.com/free-audio/clap (git tag `1.2.2`, commit 27f20f81dec40b930d79ef429fd35dcc2d45db5b) | MIT | Plugin format headers (hosting + test plugins) | commit hash above | `include/` copied into `third_party/clap/` |

## Toolchain (not shipped)
| Tool | Version | Source |
|---|---|---|
| GCC | 13.3.0 | Ubuntu 24.04 package |
| CMake | 3.28.3 | Ubuntu 24.04 package |
| Ninja | system | Ubuntu 24.04 package |

## Not used / pending decisions
- **VST3 SDK** (Steinberg, MIT since 3.8): not yet vendored. Needed for VST3 *loading*; the scanner reads VST3 bundles without loading them.
- **MP3 encoding**: requires an encoder (e.g. LAME, LGPL). Not added – needs an explicit licensing decision.
- **Stem separation models** (e.g. Demucs, MIT code / model weights with their own terms): not added.
