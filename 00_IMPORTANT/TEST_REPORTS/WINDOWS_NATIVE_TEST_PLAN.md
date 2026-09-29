# RoY Studio – Windows Native & Real Audio Hardware Test Plan

Purpose: validate on a real Windows 10/11 x64 PC with a real audio interface what the build
environment (Linux + Wine + null audio backend) cannot test. Record every result as
PASS / FAIL / BLOCKED with notes, and put the filled copy into `00_IMPORTANT/TEST_REPORTS/`.

## 0. Preparation
- Easiest: the test kit ZIP (`cmake --build build-win --target package` → `RoYStudio-0.2.0-win64.zip`):
  programs + LAME DLL + licences + `roy_tests.exe` with `test_plugins\` (CLAP/VST3 test plugins, MOCK stem engine).
  Unzip anywhere; `README_TESTKIT.txt` lists the quick check. The tests find their resources next to `roy_tests.exe`.
- Or the installer `RoYStudio-0.2.0-win64.exe` (same build, `cmake --build build-win --target package`): 64-bit,
  installs into `C:\Program Files\RoY Studio`, start-menu entries, optional "Test kit" component (off by default),
  uninstaller. It is **not code-signed** yet → Windows SmartScreen shows "Windows protected your PC" →
  "More info" → "Run anyway" (only for this test build from a trusted source).
- Or build: Visual Studio 2022 (x64, Release) – `cmake -S RoYStudio -B build -G "Visual Studio 17 2022" -A x64` then build `ALL_BUILD`.
- Keep `roy_plugin_host.exe` and `roy_mp3lame.dll` next to `roy_studio.exe`.
- Note: CPU, RAM, GPU, Windows build, audio interface + driver version, buffer/sample rate defaults.
- Logs: `%APPDATA%\RoYStudio\roy_studio.log`, crash reports: `%APPDATA%\RoYStudio\CrashReports\`.

### 0.1 Installer
| # | Step | Expected |
|---|---|---|
| 0.1a | Run the installer, accept the notice, keep "RoY Studio", tick "Test kit" only if you run section 1 from the install folder | installs without errors; start menu "RoY Studio 0.2.0 BETA" with RoY Studio + notes; finish page starts RoY Studio |
| 0.1b | Run the installer again (upgrade/repair) | the old version is uninstalled first, settings + projects untouched |
| 0.1c | Settings > Apps > RoY Studio > Uninstall | program folder + start menu gone; `%APPDATA%\RoYStudio` (settings, plugin list, logs, crash reports) and all projects still there |

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
| 4.6 | Record the same 8 bars three times (stop/start between passes) | after the 2nd pass the take lanes open (header button `T2`/`T3`); the newest take plays (orange "comp" block) |
| 4.7 | In a take lane drag over 2 bars of Take 1; double-click Take 2; right-click → Rename/Delete take | dragged range plays from Take 1 (orange in its lane); double-click = whole Take 2; every step Ctrl+Z-able; deleted take's WAV stays in `Audio\` |
| 4.8 | Right-click a take → "Flatten comp to clips", play | sound identical to before, the comp became normal clips with short crossfades; takes remain for re-comping |

## 5. Playback, beat, MIDI, vocals
| # | Step | Expected |
|---|---|---|
| 5.1 | Demo project: play, loop, seek, metronome | correct timing, no clicks |
| 5.2 | Build a beat in CHANNELS, 808 in PIANO ROLL | audible immediately while playing |
| 5.3 | Connect a MIDI keyboard, start RoY; `roy_cli.exe midi-devices --monitor 10` first | device listed; monitor prints Note On/Off, CC, pitch bend |
| 5.3b | Select a MIDI track (RoY Synth / 808 / a VST3 instrument), play keys with the transport STOPPED | sound immediately, velocity works, sustain pedal holds notes, status bar shows "MIDI ... -> track" |
| 5.3c | Arm the MIDI track, press REC, play 4 bars, stop | one new clip on bar boundaries with the played notes; Ctrl+Z removes it in one step |
| 5.3d | Audio menu → MIDI panic while notes hang | all notes stop |
| 5.3e | Unplug the keyboard while holding notes, wait 3 s, plug it back in | no crash, no hanging notes ("MIDI input disconnected"); after replugging "MIDI input connected" within ~2 s and it plays again – no menu action needed. An input switched off in the Audio menu stays off |
| 5.3f | Play fast 1/16 notes at 120 BPM with buffer 1024 while recording, open the clip in PIANO ROLL | notes keep their played spacing (no grid-like 21 ms steps); constant latency of one buffer |
| 5.3g | MIXER: right-click a fader → MIDI Learn, turn a knob on the controller; same for pan, a send, a plugin parameter (right-click in the parameter list) | status bar shows MIDI LEARN, after the move the control follows the knob, "CC n" tag appears; the knob no longer changes the synth sound; Audio menu → MIDI controller mappings lists it; save/reopen keeps it; Esc cancels learning |
| 5.4 | VOCALS: Pitch analysis + Pitch Guardian on a real sung take | corrections audible and natural; original file unchanged |
| 5.5 | VOCALS: Preview → pitch editor shows waveform, pitch curve, notes with IN SCALE / OFF KEY / UNCERTAIN / CORRECTED | statuses plausible for your take |
| 5.6 | Apply, then A ORIGINAL / B CORRECTED while playing | instant switch, both play in sync; Apply again with other settings starts from the original |
| 5.7 | BEATS: Generate (Trap/Drill/...), groove templates, note repeat 1/16T, pattern chain | timing feel audible, one undo per action |
| 5.8 | BEATS: 808 Start Phase / Phase Reset, "Analyze kick vs 808" | plots appear, suggestions sensible, nothing changed in the project |

## 6. Plugins
| # | Step | Expected |
|---|---|---|
| 6.0 | `roy_cli.exe plugin-compat "C:\Program Files\Common Files\VST3" --editor --out C:\RoYTest\compat.md` | report with one row per plugin (load, audio, state, editor, unload); send the report back |
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
| 8.3 | `roy_cli.exe crash-test C:\RoYCrash segv` (test-only crash) | `C:\RoYCrash` contains `roy_cli_crash_*.txt` (ACCESS_VIOLATION, module roy_cli.exe) and a `.dmp` that opens in WinDbg / Visual Studio |
| 8.4 | Help > Create diagnostics report (also `roy_cli.exe diagnostics`) | a `.md` file in `%APPDATA%\RoYStudio\Diagnostics\` with Windows version, audio/MIDI devices, plugin failures, crash reports, log tail; your user name does not appear. **Attach this file to every bug report.** |

## 9. Long session (soak)
| # | Step | Expected |
|---|---|---|
| 9.1 | `roy_soak.exe --cycles 240 --plugin <folder>\test_plugins\roy_test_gain.clap --out C:\RoYSoak` (≈ 4 simulated hours, ~20 min) | `C:\RoYSoak\soak.md` says **Result: PASS** (RSS trend < 20 MB/simulated hour, handles and threads flat, 0 failed operations) |
| 9.2 | Real use: work 2–3 h in RoY Studio (record, comp, plugins, export), watch Task Manager | memory levels off, no growing handle count, no slowdown |
