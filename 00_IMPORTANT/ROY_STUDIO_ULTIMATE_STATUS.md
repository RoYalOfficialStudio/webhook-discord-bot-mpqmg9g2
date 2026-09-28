# ROY STUDIO ULTIMATE STATUS

| | |
|---|---|
| VERSION | 0.2.0 – release stage **BETA** (gate below passed; BETA HARDENING next) |
| BUILD | Linux x86_64 GCC 13.3 RelWithDebInfo; Windows x64 mingw-w64 13.2 cross build (static) |
| COMMIT | 2db4c3f + this report commit (branch claude/bold-franklin-fybkj5) |
| DATUM | 2026-09-28 |

Statuses: PASS / PARTIAL / UNTESTED / BLOCKED / FAIL only. "PASS" means: implemented and verified by
automated tests in this environment (Linux + Windows build under Wine, null audio device, Xvfb display).
Nothing here is "fertig": real Windows machines, real audio interfaces and third-party plugins are
still to be validated (see TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md).

## Verification run of this report
| Check | Result |
|---|---|
| Linux full suite | PASS – 196 tests, 0 failed, 101 291 checks (after the live-MIDI block) |
| Windows full suite under Wine 9.0 | PASS – 194 tests, 0 failed, 101 298 checks (SIGSTOP + FIFO tests are POSIX-only) |
| Windows GUI self-test (roy_studio.exe --selftest, Wine, null audio) | PASS – 17/17 steps |
| Windows + Linux CLI self-test (roy_cli selftest) | PASS |
| ASan/UBSan full suite | PASS – 190 tests; only findings are the deliberate crash test plugins. One test expectation failed first (scanner error text when ASan turns the crash into exit code 1) → message fixed, plugin suite re-run under ASan: 8/8 PASS |
| TSan (parallel, mixer, faults, automation) | PASS – 0 warnings |
| Golden save/reload (WAV bit-identical, MP3 byte-identical, VST3 + CLAP state) | PASS |
| Fault injection (9 scenarios, see below) | PASS (Linux + Wine) |
| Performance regression check | PASS – no code regression (same-machine A/B), see BENCHMARKS/REGRESSION_CHECK_2026-09-28.md |
| UI benchmark 100 tracks (llvmpipe) | PASS – 36..67 FPS per area |

## BETA gate
| Gate item | Status | Evidence |
|---|---|---|
| Linux | PASS | full suite |
| Wine | PASS | full suite + GUI/CLI self-tests |
| Project roundtrip | PASS | golden DoD test: save → close → reopen → identical project, bit-identical WAV, byte-identical MP3 |
| WAV | PASS | export/import, atomic `.partial` + replace, disk-full safe |
| FLAC | PASS | native encoder, decode-verified |
| MP3 | PASS | LAME 3.100 (LGPL, runtime-loaded), 128–320 CBR/VBR, ID3, gapless decode length = WAV length |
| CLAP | PASS | scan, audio, notes, params, state, GUI, crash/hang isolation |
| VST3 | PASS | SDK 3.8.1: scan → load → audio → MIDI → automation → state (component+controller) → editor → unload, tested with a VST3 module built from the official SDK classes; third-party VST3 plugins UNTESTED |
| Crash isolation | PASS | crash + hang (CLAP and VST3) → PLUGIN CRASHED, pass-through, restart/disable/remove, safe mode |
| State restore | PASS | plugin states survive save/reload; a rejected state is kept (`rejectedState`) |
| Plugin GUI | PASS | editor open/close/resize/always-on-top/state on X11 (Xvfb) and Win32 (Wine); real DPI/multi-monitor UNTESTED |
| Audio preview | PASS | click/stop, auto preview, tempo sync, volume, excluded from renders |
| Pitch Guardian | PASS | synthetic material incl. jitter/noise/rap; UNCERTAIN notes not corrected; real recordings UNTESTED |
| Off-Key filter | PASS | Major, Minor, Dorian, Phrygian, Major/Minor Pentatonic, Allow Chromatic |
| 100-track performance | PASS | LARGE 99 tracks: 0/3750 blocks over budget with 4 threads (256 samples) |
| Recovery | PASS | autosave, crash snapshot, last stable, disk-full / device-loss / corrupt-project fault tests |
| Undo | PASS | memento undo; macros: 100 tracks, pattern generation, multi-clip move, pattern chains = one step each |
| Windows native | UNTESTED HARDWARE VALIDATION | allowed by the gate |
| Real audio hardware | UNTESTED HARDWARE VALIDATION | allowed by the gate |

**Gate result: all required items PASS → BETA.** Version raised to 0.2.0.

## Components
| Area | Status | Notes |
|---|---|---|
| CORE | PASS | logging, files (atomic writes, TEST-ONLY disk-fault injection), processes, command system |
| PROJECT | PASS | versioned JSON, migration, forward-compatible keys; corrupt-file fuzzing (truncated / bit flips / wrong types) never crashes |
| AUDIO ENGINE | PASS | multi-core mixing (bit-identical to single thread), PDC, automation |
| AUDIO DEVICES | PARTIAL | device-loss detection (stop notification + stall watchdog) and auto-reconnect with default-device fallback PASS on the null backend; real WASAPI/ALSA devices UNTESTED |
| PLAYLIST | PASS | clips, waveforms, drag, split, sections, markers, Ctrl+click multi-select → MoveClips (one undo) |
| RECORDING | PARTIAL | takes, loop, punch, comp, never-lose, disk-full handling PASS headless; real inputs UNTESTED |
| MIDI | PARTIAL | editing, SMF import/export PASS; LIVE INPUT PASS (parser, MIDI thru with stopped transport, sustain pedal, target follows the selected track, recording → clip in one undo, panic, overflow-safe) – WinMM / ALSA raw-MIDI backends verified only without real devices (Linux via FIFO, Wine enumeration) → real keyboards UNTESTED; recorded timing resolution = one audio block (5.3 ms at 256) |
| PIANO ROLL | PASS | |
| AUTOMATION | PASS | volume, pan, width, sends, plugin + effect params, tempo; curves Linear / Hold / Smooth / Bezier(tension) |
| VOCALS | PASS | pitch editor: waveform, pitch curve, detected/target notes, cents, confidence, IN SCALE / OFF KEY / UNCERTAIN / CORRECTED; Strength, Speed, Humanize, Formant, Vibrato + Slide preserve; A/B ORIGINAL/CORRECTED; original never modified; real recordings UNTESTED |
| PITCH GUARDIAN | PASS | always tunes from the original take (no stacked corrections) |
| OFF-KEY FILTER | PASS | 6 scale modes + chromatic allow |
| BEAT LAB | PASS | swing, groove templates (MPC 54–66 %, triplet, boom bap, trap, drill, humanize), velocity curves, probability, ratchets, flams, note repeat incl. triplets, variations, generator (5 styles), pattern chains |
| 808 LAB | PASS | root detection (note + cents), tune, glide, slides, mono/legato, AHDSR, distortion/saturation/soft clip, start phase + phase reset (de-clicked) |
| KICK ↔ 808 ANALYZER | PASS | visual: low-band envelopes, spectrum overlap, running phase correlation; suggestions only, nothing changed |
| SAMPLER | PASS | zones, loops, slicing, auto root on drop |
| MIXER | PASS | busses, sends pre/post (switch live), send UI, sidechain, solo / solo safe, mute, PDC, loop refusal across outputs+sends+sidechains, delete bus with re-routing, random graph tests |
| CLAP | PASS | |
| VST3 | PASS | see gate; third-party plugins UNTESTED |
| PLUGIN SANDBOX | PASS | one RoYPluginHost per instance; crash/hang/exit-without-result quarantined |
| PLUGIN GUI | PASS | see gate |
| BROWSER | PASS | categories, search, preview, drag & drop to playlist / sampler / drum pad / channel / mixer slot |
| AUDIO PREVIEW | PASS | |
| STEM SEPARATION | PARTIAL | IStemSeparator: BasicStemSeparator PASS (DSP, honest bleed warnings); AdvancedStemSeparator adapter PASS with a MOCK engine – a real high-quality engine (e.g. Demucs) must be installed + configured by the user (no model bundled) → quality UNTESTED |
| MIX INTELLIGENCE | PASS | |
| MASTERING | PASS | |
| WAV / FLAC / MP3 | PASS | |
| BACKUP | PASS | |
| RECOVERY | PASS | |
| UNDO/REDO | PASS | |
| FAULT INJECTION | PASS | plugin crash, plugin hang, missing audio file, corrupt project, invalid plugin state, device loss, callback stall, export failure (disk full, folder is a file), disk full while saving and recording |
| PERFORMANCE | PASS | SMALL..XL + UI, regression check with `roy_bench --baseline` |
| LINUX | PASS | |
| WINDOWS/WINE | PASS | |
| WINDOWS NATIVE | UNTESTED | TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md |
| REAL AUDIO HARDWARE | UNTESTED | ASIO: no backend in miniaudio → BLOCKED |

## Behaviour changes in this block (for testers)
* RoY 808: new params Start Phase / Phase Reset (default on) – every hit now starts at the same phase.
  Older projects load with Phase Reset on, so an 808 may sound very slightly different at note starts.
* `SetRowPattern` defines the whole row (clears leftover ratchets/flams/probabilities on that row).
* A plugin that ends the scan process without a result is quarantined as crashed (was "failed").

## Next blocks (BETA HARDENING → RELEASE CANDIDATE)
1. Windows-native validation package (installer/zip, first-run checks, crash-report collection).
2. Live MIDI: sub-block timestamps (driver time stamps), MIDI learn for plugin/mixer parameters, hot-plug rescan.
3. Real-recording vocal material in the regression suite (needs user-provided takes).
4. Third-party plugin compatibility pass (free CLAP/VST3 plugins from official sources).
5. Long-session soak test (multi-hour playback/record cycles), memory growth check.
