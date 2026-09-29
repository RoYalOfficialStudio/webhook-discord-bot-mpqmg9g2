# ROY STUDIO ULTIMATE STATUS

| | |
|---|---|
| VERSION | 0.2.0 – release stage **BETA** (BETA gate passed; BETA HARDENING done as far as possible without real hardware; RC gate blocked by owner-only items, see below) |
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
| Linux full suite | PASS – 218 tests, 0 failed, 101 753 checks (after the portable Windows test build block) |
| Windows full suite under Wine 9.0 | PASS – 210 tests, 0 failed, 101 649 checks (SIGSTOP + FIFO tests are POSIX-only) |
| Windows GUI self-test (RoYStudio.exe --audio null --selftest, Wine) | PASS – 21/21 steps (new: metronome audible, count-in) – also run from the unpacked PORTABLE zip and from the installed program |
| PORTABLE package under Wine (unzipped like a user) | PASS – FIRST REAL MUSIC SESSION automated 41/43 PASS + 2 SKIPPED (microphone level, opening Explorer need a person), TestKit\\RUN_AUTOMATED_TESTS.bat 214/214, system check + diagnostic package from the package, nothing written to %APPDATA% or %TEMP% |
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

## WINDOWS TEST BUILD (free, 2026-09-29)
| Item | Status | Notes |
|---|---|---|
| Portable ZIP `RoYStudio-0.2.0-PORTABLE-win64.zip` | PASS (Wine) | double-click START_ROY_STUDIO.bat; RoYStudio.exe, RoYPluginHost.exe, roy_cli.exe, roy_mp3lame.dll; Plugins\ (safe RoY TEST VST3 + CLAP), Samples\, Projects\, TestKit\, README_TEST.txt, SHA256SUMS.txt; only Windows system DLLs needed; portable marker keeps everything in the folder |
| Installer `RoYStudio-0.2.0-win64.exe` | PASS (Wine) | NSIS 64-bit, unsigned; now also installs Plugins\ |
| First start: WELCOME + STEP 1–5 + SYSTEM CHECK | PASS | output list + PLAY TEST TONE, WASAPI shared (default) / exclusive option, input list + LIVE INPUT LEVEL + TEST MICROPHONE (3 s record + playback), buffer 64–1024 with dropout warning, native sample rates, MIDI with SKIP, PASS/WARNING/FAIL system check |
| FIRST REAL MUSIC SESSION | PASS (automated) | 43 guided steps A beat … F plugins, DO IT / done by hand, results file for the diagnostic package |
| HELP > CREATE DIAGNOSTIC PACKAGE | PASS | RoYStudio_Diagnostics_<time>.zip (report, system check, log, crash reports text-only, plugin scan, settings, session results); user name / home / computer name replaced; no minidumps, no projects |
| Owner feedback round 1 (real PC screenshots) | FIXED | playlist: take block deletable (right-click / Del), track menu on the name; BEATS: patterns deletable (x button, right-click Rename / Duplicate / Delete, Del key; also "Delete" in CHANNELS; its playlist clips go with it, one Ctrl+Z restores both); ImGui "conflicting ID" error in QUICK GROOVES vs GENERATE fixed (own ID scopes); tempo field was frozen while dragging (re-read every frame) – now drag / double-click-to-type / mouse wheel / − + buttons, undoable, also in PROJECT; 808 preview: PLAY 808 + one-octave note keys + "preview on change" in 808 LAB, piano roll plays clicked / added notes and piano keys (live-MIDI path, never recorded) – self-test step "808 preview audible (transport stopped)" PASS on Linux + Wine |
| Owner feedback round 2 | DONE | PLAYLIST pattern strip + drag a pattern from PLAYLIST / BEATS / CHANNELS onto a track (non-beat lane → beat track); LIVE button on audio tracks = input monitoring through the channel with RoY VocalTune in the song key (recording stays dry, playback through the same chain; verified: E+40 cents in → E out with the transport stopped), right-click: retune speed / strength / on-off; CHANNEL PRESETS: MIXER > PRESETS save/load a channel's chain + fader/pan (`<user data>/Presets/Channel/*.roychain`, replace keeps a backup), 3 free RoY vocal chains, VocalTune follows the new song's key, one Ctrl+Z; real microphone latency/feel UNTESTED (owner) |
| Owner feedback round 3: bought MP3/WAV beat | DONE | IMPORT BEAT: PLAYLIST button / Ctrl+B / File menu (Windows file dialog, starts in Downloads, multi-select), drag from the Explorer into the window (WM_DROPFILES; on an audio lane = placed there), BROWSER "Downloads" tab + right-click "Import as BEAT"; window with LISTEN, tempo + key from the file name or estimated (x2 / ÷2), "set song tempo/key"; ImportBeat command = own track from bar 1 + tempo + key in ONE undo step, file copied into the project; MP3 length now gapless-exact (probe bug fixed); umlaut file names verified on the Windows build (Wine, UTF-8 locale) |
| Owner feedback round 4: mixer + crackles | FIXED (crackle cause = likely clipping, to be confirmed on the PC) | owner screenshot showed red CLIP on the imported beat channel: loud mastered MP3s exceed 0 dBFS, with vocals the master overloads the sound card. Now: live OUTPUT PROTECTION (soft knee from -1 dBFS, never above 0 dBFS, transparent below, renders/exports untouched, bit-identical for normal levels), status bar TOO LOUD (→ MIXER) and DROPOUTS (one click → buffer 512) hints, IMPORT BEAT puts the beat fader at -6 dB and shows the file peak. Measured: VocalTune produces no clicks even at 0 ms retune; the rage vocal chain costs ~12 % of one core at 256 samples. MIXER: insert/send rack scrolls, draggable splitter, "more below" arrow |
| Owner feedback round 5: crash + fader | FIXED | (1) IMPORT BEAT froze/crashed on Windows: miniaudio's WASAPI init puts the main thread in the COM multi-threaded apartment, where IFileOpenDialog must not run (reproduced under Wine: UI hung, no dialog). Native dialogs now run on their own STA thread, asynchronously, owned by the main window; verified under Wine end-to-end (dialog → file → IMPORT BEAT window → track). Same fix for the export "Browse...". (2) Mixer fader jumped back to the old value on release (widget re-reads the project value; on the release frame the slider no longer writes it, so the OLD value was committed) - same bug in pan, sends, swing, groove amount, drum row volume, step probability/ratchet/micro timing: shared `editFinished()` keeps the last edited value; verified by drag tests (Linux + Windows build) |
| Owner feedback round 6: "cannot export" | FIXED | export itself worked, but it ran on the UI thread: a 3-min song with the rage vocal chain + master chain takes ~37 s here (effects cost the same offline as live: distortion 3.1 %, VocalTune 2.9 %, saturation 2.5 %, limiter 2.4 % of real time each) - the window froze, Windows shows "not responding". Now: export runs in a background thread with a progress bar + Cancel, the UI draws only the progress window meanwhile (project untouched), result window EXPORT DONE / FAILED with file names, LUFS/dBTP and OPEN EXPORT FOLDER; EXPORT button in the transport bar. WAV + MP3 verified in the Windows build under Wine. Next: speed up distortion/saturation/limiter oversampling (see backlog) |
| Real Windows PC | **BLOCKED – owner** | run the portable ZIP (README_TEST.txt), return TEST_RESULTS.md + diagnostic package |
| ASIO | not in this build | WASAPI shared/exclusive needs no driver; the free ASIO SDK is GPLv3 (would force a licence decision) |

## RELEASE CANDIDATE gate (evaluated 2026-09-29) – NOT PASSED, blocked by owner-only items
| RC item | Status | What is needed |
|---|---|---|
| All automated suites green (Linux, Wine, ASan/UBSan, TSan) | PASS | – |
| Long-session stability (soak) | PASS | repeat 9.1 on a real PC |
| Installer / test kit / crash reports / diagnostics | PASS | – |
| Windows native validation (TEST_PLAN sections 0–9) | **BLOCKED – owner** | run the test kit / installer on a real Windows 10/11 PC and return the filled test plan + diagnostics reports |
| Real audio interface (latency, xruns, device loss) | **BLOCKED – owner** | TEST_PLAN 3.x, 4.x with your interface |
| Real MIDI keyboard / controller | **BLOCKED – owner** | TEST_PLAN 5.3–5.3g |
| Your own third-party plugins | **BLOCKED – owner** | `roy_cli plugin-compat <your plugin folders> --editor --out report.md` (TEST_PLAN 6.0) |
| Real vocal takes for Pitch Guardian / comping | **BLOCKED – owner** | a few dry vocal recordings (TEST_PLAN 4.6–4.8, 5.4–5.6) |
| RoY Studio licence text | **BLOCKED – owner decision** | choose the licence (proprietary / open source) |
| Code signing | **BLOCKED – owner decision** | optional certificate purchase; without it SmartScreen warns |

## Components
| Area | Status | Notes |
|---|---|---|
| CORE | PASS | logging, files (atomic writes, TEST-ONLY disk-fault injection), processes, command system |
| PROJECT | PASS | versioned JSON, migration, forward-compatible keys; corrupt-file fuzzing (truncated / bit flips / wrong types) never crashes |
| AUDIO ENGINE | PASS | multi-core mixing (bit-identical to single thread), PDC, automation |
| AUDIO DEVICES | PARTIAL | output/input device selection in the Audio menu + setup check (test tone, input meter), remembered in settings.json with default-device fallback + message; device-loss detection (stop notification + stall watchdog) and auto-reconnect with default-device fallback PASS on the null backend; real WASAPI/ALSA devices UNTESTED |
| PLAYLIST | PASS | clips, waveforms, drag, split, sections, markers, Ctrl+click multi-select → MoveClips (one undo) |
| RECORDING | PARTIAL | takes, loop, punch, comp, never-lose, disk-full handling PASS headless; **take lanes + comping in the playlist** (lanes open after the 2nd take, swipe-comp by dragging over a take, double-click = whole take, rename / delete take / clear / flatten comp – all undoable commands, comp audibly verified); real inputs UNTESTED |
| MIDI | PARTIAL | editing, SMF import/export PASS; LIVE INPUT PASS (parser, MIDI thru with stopped transport, sustain pedal, target follows the selected track, recording → clip in one undo, panic, overflow-safe); **sample-accurate timing** (arrival time stamps, constant one-callback latency instead of block jitter, verified with a deterministic clock); **hot-plug** (inputs appear/disappear automatically, stable WinMM ids by device name, all-notes-off on unplug, switched-off inputs stay off); **MIDI LEARN** (CC → volume / pan / width / sends / any effect or instrument parameter, saved in the project, undoable, mapped CCs no longer reach the instrument, mapping list in the Audio menu, removed with its target) – WinMM / ALSA raw-MIDI backends verified only without real devices (Linux via FIFO, Wine enumeration) → real keyboards/controllers UNTESTED |
| PIANO ROLL | PASS | |
| AUTOMATION | PASS | volume, pan, width, sends, plugin + effect params, tempo; curves Linear / Hold / Smooth / Bezier(tension) |
| VOCALS | PASS | pitch editor: waveform, pitch curve, detected/target notes, cents, confidence, IN SCALE / OFF KEY / UNCERTAIN / CORRECTED; Strength, Speed, Humanize, Formant, Vibrato + Slide preserve; A/B ORIGINAL/CORRECTED; original never modified; real recordings UNTESTED |
| PITCH GUARDIAN | PASS | always tunes from the original take (no stacked corrections) |
| OFF-KEY FILTER | PASS | 6 scale modes + chromatic allow |
| BEAT LAB | PASS | patterns: delete (with their playlist clips, undoable) / rename / duplicate; swing, groove templates (MPC 54–66 %, triplet, boom bap, trap, drill, humanize), velocity curves, probability, ratchets, flams, note repeat incl. triplets, variations, generator (5 styles), pattern chains |
| 808 LAB | PASS | PREVIEW (PLAY 808, note keys, preview on change – audible with the song stopped); root detection (note + cents), tune, glide, slides, mono/legato, AHDSR, distortion/saturation/soft clip, start phase + phase reset (de-clicked) |
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
| APP SETTINGS | PASS | `%APPDATA%\RoYStudio\settings.json`: audio device/buffer/rate, switched-off MIDI inputs, setup-check state; atomic write, damaged file kept as .corrupt + defaults, unknown keys preserved; automated runs never read/write it |
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
* (2026-09-29) Windows program files are now named RoYStudio.exe and RoYPluginHost.exe.
* (2026-09-29) FIX: the CLICK button / metronome setting never reached the engine (no click in the app) and the
  count-in setting was never applied - both now work (self-test steps added).
* (2026-09-29) If the audio input cannot be opened (no microphone, Windows privacy setting) RoY now opens playback
  only instead of having no sound at all; exclusive mode falls back to shared.
* (2026-09-29) Right-click menus: plugin parameters and faders; new Help menu entries (music session, system check,
  diagnostic package, user data folder).
* (2026-09-29) First start shows a "Setup check" (audio output + test tone, input meter, MIDI, plugin scan); audio device,
  buffer and sample rate are now remembered between starts (before: always system default, 256, 48 kHz).
* (2026-09-29) RoY Studio / roy_cli install a crash handler: a fatal error now leaves a report (+ minidump on
  Windows) in CrashReports\ and the next start says so once. New Help menu (diagnostics report).
* (2026-09-29) Right-click on a mixer fader / pan now opens a menu (Reset / MIDI Learn) instead of
  resetting directly; same for plugin parameters (Default value / MIDI Learn).

## Next blocks (BETA HARDENING → RELEASE CANDIDATE)
1. Windows-native validation package: ZIP test kit, installer, crash reports, diagnostics report, first-start setup check, remembered audio settings done. Owner decisions pending: RoY Studio licence text, code-signing certificate.
2. ~~Live MIDI: timestamps, MIDI learn, hot-plug~~ done 2026-09-29; next: validation with real keyboards/controllers (TEST_PLAN 5.3–5.3g), NRPN/14-bit CC and relative encoders for MIDI learn.
3. Real-recording vocal material in the regression suite (needs user-provided takes).
4. Third-party plugin compatibility: VST3 SDK plugins 53/55 and CLAP example plugins 20/20 done; next: the user's own plugins via `roy_cli plugin-compat` on Windows (TEST_PLAN 6.0).
5. ~~Long-session soak test~~ done (2026-09-29, PASS after cache fix); repeat `roy_soak.exe --cycles 240` on a real Windows PC (TEST_PLAN 9.1).
6. Take comping on real vocal takes (TEST_PLAN 4.6–4.8).
