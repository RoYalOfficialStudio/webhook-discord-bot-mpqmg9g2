# RoY Studio – Windows Native & Real Audio Hardware Test Plan

Purpose: validate on a real Windows 10/11 x64 PC with a real audio interface what the build
environment (Linux + Wine + null audio backend) cannot test. Record every result as
PASS / FAIL / BLOCKED with notes, and put the filled copy into `00_IMPORTANT/TEST_REPORTS/`.

## 0. Preparation
- Easiest: the test kit ZIP (`cmake --build build-win --target package` → `RoYStudio-0.2.0-win64.zip`):
  programs + LAME DLL + licences + `roy_tests.exe` with `test_plugins\` (CLAP/VST3 test plugins, MOCK stem engine).
  Unzip anywhere; `README_TESTKIT.txt` lists the quick check. The tests find their resources next to `roy_tests.exe`.
- Or build: Visual Studio 2022 (x64, Release) – `cmake -S RoYStudio -B build -G "Visual Studio 17 2022" -A x64` then build `ALL_BUILD`.
- Keep `roy_plugin_host.exe` and `roy_mp3lame.dll` next to `roy_studio.exe`.
- Note: CPU, RAM, GPU, Windows build, audio interface + driver version, buffer/sample rate defaults.
- Logs: `%APPDATA%\RoYStudio\roy_studio.log`, crash reports: `%APPDATA%\RoYStudio\CrashReports\`.

## 1. Automated checks first (5 min)
| # | Step | Expected |
|---|---|---|
| 1.1 | `roy_tests.exe` | `... tests, 0 failed` |
| 1.2 | `roy_cli.exe selftest C:\RoYTest\cli` | `CLI SELFTEST PASSED` |
| 1.3 | `roy_studio.exe --selftest C:\RoYTest\gui` (uses the default WASAPI device) | report `C:\RoYTest\gui\selftest_report.txt` = `SELFTEST PASSED` |
| 1.4 | `roy_cli.exe devices` and `roy_cli.exe devices wasapi` | lists your interface inputs/outputs |

## 2. App start & GUI (Direct3D 11)
| # | Step | Expected |
|---|---|---|
| 2.1 | Start `roy_studio.exe` | window maximized, gold/obsidian theme, no errors in status bar |
| 2.2 | 100 % / 150 % / 200 % display scaling, move window between monitors | text sharp, layout not clipped |
| 2.3 | `roy_studio.exe --benchui 100` is Linux-only (GLFW); on Windows: open demo, add 100 tracks via palette, scroll/zoom playlist | smooth (≥ 50 FPS perceived) |

## 3. Audio devices (WASAPI; ASIO if available)
| # | Step | Expected |
|---|---|---|
| 3.1 | Audio menu: 44.1 / 48 / 96 kHz | status bar shows the actual rate, playback correct pitch |
| 3.2 | Buffer 64 / 128 / 256 / 512 / 1024 | stable playback; note the smallest buffer without xruns (status bar "xruns") |
| 3.3 | Unplug the USB interface during playback | no crash; message "AUDIO DEVICE LOST"; transport stops; a running recording is stopped and its take kept |
| 3.3b | Plug it back in | RoY reconnects automatically within a few seconds ("audio device reconnected"); if the device stays gone it falls back to the Windows default device |
| 3.3c | Start another app that takes the device exclusively | same as 3.3 (loss detected, reconnect when released) |
| 3.4 | ASIO | CURRENTLY NOT SUPPORTED (miniaudio has no ASIO backend) – verify message/behaviour, mark BLOCKED |

## 4. Recording & monitoring
| # | Step | Expected |
|---|---|---|
| 4.1 | Add Vocal track, arm (R), enable IN monitoring, speak | input meter moves, monitor audible with low latency |
| 4.2 | Record 30 s, stop | take appears, file in `<project>\Audio\`, plays back in sync |
| 4.3 | Roundtrip latency: loop output→input with a cable, record a click (metronome on) | measured offset ≤ 1 ms after latency compensation; note raw value |
| 4.4 | Record 20 min with 256 buffer | no dropouts (xruns counter 0), file complete |
| 4.5 | Kill `roy_studio.exe` via Task Manager while recording, restart, open project | RECOVER PROJECT offered, "recover last performance" works |

## 5. Playback, beat, MIDI, vocals
| # | Step | Expected |
|---|---|---|
| 5.1 | Demo project: play, loop, seek, metronome | correct timing, no clicks |
| 5.2 | Build a beat in CHANNELS, 808 in PIANO ROLL | audible immediately while playing |
| 5.3 | MIDI keyboard input (if supported yet) | currently NOT IMPLEMENTED for live input – mark BLOCKED/UNTESTED |
| 5.4 | VOCALS: Pitch analysis + Pitch Guardian on a real sung take | corrections audible and natural; original file unchanged |
| 5.5 | VOCALS: Preview → pitch editor shows waveform, pitch curve, notes with IN SCALE / OFF KEY / UNCERTAIN / CORRECTED | statuses plausible for your take |
| 5.6 | Apply, then A ORIGINAL / B CORRECTED while playing | instant switch, both play in sync; Apply again with other settings starts from the original |
| 5.7 | BEATS: Generate (Trap/Drill/...), groove templates, note repeat 1/16T, pattern chain | timing feel audible, one undo per action |
| 5.8 | BEATS: 808 Start Phase / Phase Reset, "Analyze kick vs 808" | plots appear, suggestions sensible, nothing changed in the project |

## 6. Plugins
| # | Step | Expected |
|---|---|---|
| 6.1 | PLUGINS → Scan (with your installed CLAP/VST3) | list INSTALLED/FAILED; nothing crashes RoY |
| 6.2 | Insert a CLAP effect, a VST3 effect, a VST3 instrument | audio processed, parameters visible |
| 6.3 | Open plugin editor window, resize, close, reopen | plugin keeps state, RoY keeps running |
| 6.4 | Save, close, reopen project | plugin state restored |
| 6.5 | Kill `roy_plugin_host.exe` in Task Manager while playing | "PLUGIN CRASHED", audio continues, RESTART works |

## 7. Export & files
| # | Step | Expected |
|---|---|---|
| 7.1 | Export WAV 16/24/32f, FLAC, MP3 128/320 | files play in Windows Media Player / VLC, loudness as reported |
| 7.2 | Export into a read-only folder | clear error, project untouched |
| 7.2b | Export/record onto an almost full USB stick | clear "disk full" error, previous files intact, recorded take keeps what was written, "Recover last performance" rescues the rest |
| 7.3 | Save/Reload, Backups folder, Undo/Redo across 50 edits | identical state |

## 8. Crash recovery
| # | Step | Expected |
|---|---|---|
| 8.1 | Make edits, wait > 60 s (autosave), kill process | on reopen: RECOVER PROJECT restores the edits |
| 8.2 | Corrupt `.roy` file (truncate) and open | OPEN LAST STABLE uses the newest backup |
