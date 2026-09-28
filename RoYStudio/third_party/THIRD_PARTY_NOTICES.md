# Third-party software in RoY Studio

| Component | Version | Licence | How it is used |
|---|---|---|---|
| miniaudio | 0.11.22 | Public Domain / MIT-0 | compiled in (audio I/O, decoding) |
| nlohmann/json | 3.11.3 | MIT | compiled in |
| CLAP | 1.2.2 | MIT | headers (plugin API) |
| Dear ImGui | 1.92.9 | MIT (`imgui/LICENSE.txt`) | compiled into the GUI |
| Steinberg VST3 SDK | 3.8.1 | MIT (`vst3sdk/LICENSE.txt`) | compiled into RoYPluginHost (hosting) and the VST3 test plugin |
| LAME (libmp3lame) | 3.100 | **LGPL-2.0** (`lame/COPYING`, `lame/LICENSE`) | built as the **separate shared library** `roy_mp3lame` (`.dll`/`.so`), loaded at runtime; not linked into RoY Studio |

## LAME / LGPL notes
- The library is built from the unmodified upstream sources in `third_party/lame/` (only a RoY-specific `roy_config/config.h`
  replaces the autoconf output). Source: https://sourceforge.net/projects/lame/files/lame/3.100/ , SHA-256
  ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e (identical to Ubuntu's `lame_3.100.orig.tar.gz`).
- Users may replace `roy_mp3lame.dll` / `roy_mp3lame.so` with their own build of libmp3lame (same C API);
  the environment variable `ROY_MP3LAME` can point to any compatible library.
- When distributing binaries, ship `third_party/lame/COPYING` with them and offer the LAME sources.
- "MPEG Layer-3 audio coding technology" patents expired (2017); no licence fee applies.
