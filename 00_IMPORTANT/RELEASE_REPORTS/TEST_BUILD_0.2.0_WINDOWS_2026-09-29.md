# RoY Studio 0.2.0 – Windows test build (free, UNSIGNED DEVELOPMENT/TEST BUILD)

Built from commit a25848a (branch claude/bold-franklin-fybkj5) on 2026-09-29 with free tools only
(mingw-w64 13 static, CMake 3.28, NSIS 3.09, zip). Licence of RoY Studio: LICENSE DECISION PENDING.

| File | SHA-256 | Size |
|---|---|---|
| RoYStudio-0.2.0-PORTABLE-win64.zip | e693d0d6b8aadfc42eba9e20b4e53eef1dcb37f809a7b76e760272a3da8e5806 | 24.8 MB |
| RoYStudio-0.2.0-win64.exe (installer) | 795ba4aedaf11cb831929f36d499cb12879af34b4190eef2920f92c53b1f90ff | 15.7 MB |
| RoYStudio.exe (inside) | 1227551e8b89cb73640685cc25b36ba210917623c5760c0eb43e37c089ce7c7d | |

The files are build artifacts (not stored in git). Rebuild: `RoYStudio/packaging/build_windows_test_package.sh`
(Ubuntu/WSL) or `RoYStudio\packaging\BUILD_TEST_PACKAGE.bat` (Windows with WSL).

## Portable layout
```
RoYStudio-0.2.0-PORTABLE-win64/
  RoYStudio.exe  RoYPluginHost.exe  roy_cli.exe  roy_mp3lame.dll
  START_ROY_STUDIO.bat  README_TEST.txt  RoYStudio.portable  LICENSE_DECISION_PENDING.txt
  BUILD_INFO.txt  SHA256SUMS.txt  licenses/
  Plugins/   RoYTestPlugins.vst3 (Gain + Synth), roy_test_gain.clap (Gain + Sine)
  Samples/Drums/  kick, snare, clap, hat, openhat, 808 (self-generated)
  Projects/  (default save location)       UserData/ (created on first start)
  TestKit/   Projects/RoY Test Beat, Audio/ (tones, synthetic vocal), MIDI/, TEST_RESULTS.md,
             RUN_SYSTEM_CHECK.bat, RUN_SESSION_TEST.bat, RUN_AUTOMATED_TESTS.bat,
             CREATE_DIAGNOSTIC_PACKAGE.bat, AutomatedTests/ (roy_tests + crash/hang test plugins)
```
Runtime dependencies: Windows system DLLs only (checked with objdump) – no VC++ redistributable.

## Verification in this environment (Linux + Wine 9, null audio device, Xvfb)
| Check | Result |
|---|---|
| Linux full suite | 218/218 PASS |
| ASan/UBSan (support, live-midi, midi-learn, takes, recording, audio) | PASS (only the deliberate crash-test write) |
| Portable ZIP unzipped under Wine: `RoYStudio.exe --session-test` | SESSION TEST PASSED – 41 PASS, 2 SKIPPED (microphone level, opening Explorer) |
| Portable ZIP: GUI self-test | 21/21 PASS |
| Portable ZIP: `TestKit\RUN_AUTOMATED_TESTS.bat` | 214 tests, 0 failed |
| Portable ZIP: RUN_SYSTEM_CHECK.bat / diagnostic package | works; zip valid, no user/computer name |
| Portable mode isolation | nothing written to %APPDATA% or %TEMP% |
| Installer: silent install → self-test → uninstall | PASS; Plugins\ installed; user data kept |

## Bugs found and fixed while making it testable
* The CLICK button / metronome setting never switched the engine metronome on – no click in the app.
* The count-in setting was never passed to the transport – no count-in before recording.
* A failing audio input (no microphone, Windows privacy setting) made the whole device fail – now playback-only fallback with a clear message.
* A dangling reference while iterating a temporary JSON object in the music session (found by a compiler warning).
* (earlier block) the LAME DLL was missing from component installs.

## Not verifiable here – needs the owner's real Windows PC
Real WASAPI devices (shared/exclusive), a real microphone, real latency/xruns, SmartScreen behaviour, DPI.
