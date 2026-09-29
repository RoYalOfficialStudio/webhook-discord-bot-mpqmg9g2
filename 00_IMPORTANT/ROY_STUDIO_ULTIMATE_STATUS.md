# ROY STUDIO ULTIMATE STATUS

| | |
|---|---|
| VERSION | 0.2.0 – release stage **BETA** (gate below passed; BETA HARDENING next) |
| BUILD | Linux x86_64 GCC 13.3 RelWithDebInfo; Windows x64 mingw-w64 13.2 cross build (static) |
| COMMIT | see git log of branch claude/bold-franklin-fybkj5 (snapshot 0002) |
| DATUM | 2026-09-29 |

Statuses: PASS / PARTIAL / UNTESTED / BLOCKED / FAIL only. "PASS" means: implemented and verified by
automated tests in this environment (Linux + Windows build under Wine, null audio device, Xvfb display).
Nothing here is "fertig": real Windows machines, real audio interfaces and third-party plugins are
still to be validated (see TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md).

## Verification run of this report
| Check | Result |
|---|---|
| Linux full suite | PASS – 212 tests, 0 failed, 101 666 checks (after the crash-report + diagnostics block) |
| Windows full suite under Wine 9.0 | PASS – 208 tests, 0 failed, 101 620 checks (SIGSTOP + FIFO tests are POSIX-only) |
| Windows GUI self-test (roy_studio.exe --audio null --selftest, Wine) | PASS – 19/19 steps (new: MIDI learn, diagnostics report) |
| Windows + Linux CLI self-test (roy_cli selftest) | PASS |
| ASan/UBSan full suite | PASS – 190 tests; only findings are the deliberate crash test plugins. One test expectation failed first (scanner error text when ASan turns the crash into exit code 1) → message fixed, plugin suite re-run under ASan: 8/8 PASS |
| TSan (parallel, mixer, faults, automation) | PASS – 0 warnings |
| ASan/UBSan on the last blocks (takes, undo, recording, commands, vocal, live MIDI, MIDI learn) | PASS – 0 findings |
| TSan live MIDI + MIDI learn | PASS – 0 warnings |
| Soak test (roy_soak, 2 simulated hours) | PASS – RSS flat (−12.7 MB/h), fds/threads flat, 0 failures; before the fix +118 MB/h FAIL |
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
| RECORDING | PARTIAL | takes, loop, punch, comp, never-lose, disk-full handling PASS headless; **take lanes + comping in the playlist** (lanes open after the 2nd take, swipe-comp by dragging over a take, double-click = whole take, rename / delete take / clear / flatten comp – all undoable commands, comp audibly verified); real inputs UNTESTED |
| MIDI | PARTIAL | editing, SMF import/export PASS; LIVE INPUT PASS (parser, MIDI thru with stopped transport, sustain pedal, target follows the selected track, recording → clip in one undo, panic, overflow-safe); **sample-accurate timing** (arrival time stamps, constant one-callback latency instead of block jitter, verified with a deterministic clock); **hot-plug** (inputs appear/disappear automatically, stable WinMM ids by device name, all-notes-off on unplug, switched-off inputs stay off); **MIDI LEARN** (CC → volume / pan / width / sends / any effect or instrument parameter, saved in the project, undoable, mapped CCs no longer reach the instrument, mapping list in the Audio menu, removed with its target) – WinMM / ALSA raw-MIDI backends verified only without real devices (Linux via FIFO, Wine enumeration) → real keyboards/controllers UNTESTED |
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
| CLAP | PASS | third-party pass: official free-audio/clap-plugins 20/20 PASS (Release); Debug build: 19/20, the SVF abort is a plugin assertion RoY isolates – see PLUGIN_REPORTS/PLUGIN_COMPAT_2026-09-28_CLAP.md; commercial plugins UNTESTED |
| VST3 | PASS | see gate; **third-party compatibility pass: 53/55 official Steinberg SDK plugins PASS** (the 2 failures are plugin bugs RoY now contains) – 2 real host bugs found and fixed (VSTGUI Linux run loop via host context, 0 × 0 editor size / fatal X errors), see PLUGIN_REPORTS/PLUGIN_COMPAT_2026-09-28_VST3SDK.md; commercial plugins UNTESTED |
| PLUGIN SANDBOX | PASS | one RoYPluginHost per instance; crash/hang/exit-without-result quarantined; NaN/Inf plugin output silenced + reported; X11 errors of plugins non-fatal |
| PLUGIN GUI | PASS | see gate |
| BROWSER | PASS | categories, search, preview, drag & drop to playlist / sampler / drum pad / channel / mixer slot |
| AUDIO PREVIEW | PASS | |
| STEM SEPARATION | PARTIAL | IStemSeparator: BasicStemSeparator PASS (DSP, honest bleed warnings); AdvancedStemSeparator adapter PASS with a MOCK engine – a real high-quality engine (e.g. Demucs) must be installed + configured by the user (no model bundled) → quality UNTESTED |
| MIX INTELLIGENCE | PASS | |
| MASTERING | PASS | |
| WAV / FLAC / MP3 | PASS | |
| BACKUP | PASS | |
| RECOVERY | PASS | |
| UNDO/REDO | PASS | consecutive steps share their state (history memory halved), 512 MB byte budget besides the 500-step limit (oldest steps dropped, newest always kept) |
| LONG SESSION (SOAK) | PASS | `roy_soak`: 2 simulated hours (120 takes, 24 sandboxed plugin loads, 12 exports) – found and fixed an unbounded runtime audio cache (+118 MB/h → flat), fds/threads flat, 0 failed operations; see BENCHMARKS/SOAK_2026-09-29.md |
| CRASH REPORTS (RoY itself) | PASS | fatal errors of roy_studio / roy_cli (invalid memory access, abort, std::terminate) → text report (error, address, module / backtrace, version) + Windows minidump (system dbghelp.dll) in `CrashReports\`; next start shows "closed unexpectedly" once; verified with real crashes in a child process on Linux and Wine; real Windows UNTESTED |
| PACKAGING | PASS | ZIP test kit + 64-bit NSIS installer (start menu, optional "Test kit" component, upgrade = uninstall first, uninstaller keeps settings/logs/projects) verified by silent install → self-test → uninstall under Wine; found + fixed: LAME DLL missing from component installs; not code-signed (owner decision) |
| DIAGNOSTICS | PASS | Help > Create diagnostics report / `roy_cli diagnostics`: system (incl. Wine detection), audio + MIDI devices, engine overloads, plugin scan failures with reasons, quarantine, crash reports, log tail – home folder / user name anonymised, nothing sent anywhere |
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
* (2026-09-29) Audio no longer used by the project (deleted takes/clips, old stretch variants) is released
  from RAM after each graph rebuild and re-read from its file if an undo brings it back (the first
  playback after such an undo may load the file again).
* (2026-09-29) Undo history is additionally limited to 512 MB; with very large projects the oldest steps
  can drop out before the 500-step limit.
* (2026-09-29) A comp now shows in the playlist even when the track also has normal clips (it always played).
* (2026-09-29) Live MIDI keeps the played timing: one audio buffer of constant latency instead of 0..1
  buffer of jitter (at 256 samples: 5.3 ms constant instead of 0–5.3 ms varying).
* (2026-09-29) Windows MIDI input ids are now "winmm:<device name>" (were "winmm:<index>").
* (2026-09-29) RoY Studio / roy_cli install a crash handler: a fatal error now leaves a report (+ minidump on
  Windows) in CrashReports\ and the next start says so once. New Help menu (diagnostics report).
* (2026-09-29) Right-click on a mixer fader / pan now opens a menu (Reset / MIDI Learn) instead of
  resetting directly; same for plugin parameters (Default value / MIDI Learn).

## Next blocks (BETA HARDENING → RELEASE CANDIDATE)
1. Windows-native validation package: ZIP test kit, installer, crash reports, diagnostics report done; next: first-run audio/MIDI check wizard. Owner decisions pending: RoY Studio licence text, code-signing certificate.
2. ~~Live MIDI: timestamps, MIDI learn, hot-plug~~ done 2026-09-29; next: validation with real keyboards/controllers (TEST_PLAN 5.3–5.3g), NRPN/14-bit CC and relative encoders for MIDI learn.
3. Real-recording vocal material in the regression suite (needs user-provided takes).
4. Third-party plugin compatibility: VST3 SDK plugins 53/55 and CLAP example plugins 20/20 done; next: the user's own plugins via `roy_cli plugin-compat` on Windows (TEST_PLAN 6.0).
5. ~~Long-session soak test~~ done (2026-09-29, PASS after cache fix); repeat `roy_soak.exe --cycles 240` on a real Windows PC (TEST_PLAN 9.1).
6. Take comping on real vocal takes (TEST_PLAN 4.6–4.8).
