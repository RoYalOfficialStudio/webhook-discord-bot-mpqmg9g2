# RoY Studio – Status 2026-09-28 (GMB 06 – 12 + Plugin system)

Build: CMake/Ninja, GCC 13.3 (Linux container, headless). Full suite: **122 tests, 0 failed**
(see `TEST_REPORTS/latest/roy_tests.md`). No GUI yet, no real audio hardware (null backend only).

## GMB 06 – Vocal Lab: PASS (headless, synthetic voices)
FFT-YIN pitch detection, note/slide/vibrato segmentation, Pitch Guardian (modes, Off-Key filter,
chromatic allow, formant-preserving TD-PSOLA), Vocal Doctor, microscope, region edits, Double Magnet
(DTW alignment), take comparison, Flow Analyzer.
Limitation: validated on synthetic test voices (clearly labelled mock generator), not yet on real recordings.

## GMB 07 – Beat Lab / 808 Lab: PASS (headless)
Step sequencer (probability, swing, variations), RoY Drums, RoY 808 (glide/slide, key lock), Kick/808 collision analyzer.

## GMB 08 – Sampler / Stems: PASS / PARTIAL
Sampler zones, loop-aware playback, slicing to pads, sample analysis (BPM/key/onsets).
Stem separation: basic DSP separator only (synthetic SDR bass 31.8 dB, vocals 31.4 dB, drums 16.1 dB);
ML models not included (licence decision open) -> PARTIAL for real-world material.

## GMB 09 – Mixer & RoY effects: PASS
20 effects (EQ, compressor, limiter (provably safe), gate, de-esser, transient, clipper, reverb, delay,
chorus/flanger, phaser, stereo, saturation, noise cleaner, analyzer, pitch, VocalTune, Dynamic Space …),
BS.1770 loudness + true peak (4x oversampling).

## GMB 10 – Mix Intelligence: PASS (analysis/suggestions, never auto-applied without command)
## GMB 11 – Mastering / Export: PASS / PARTIAL
WAV + native FLAC export, stems export aligned with PDC, master presets, assistant, reference compare.
MP3: BLOCKED (encoder licence decision required).
## GMB 12 – Intelligence: PASS
Energy map, section suggestions, Vocal DNA (local, not for voice imitation), Project Assistant.

## Plugin system: PASS for CLAP (sandboxed) / PARTIAL for VST3
Architecture: `RoYStudio -> PluginHostManager (Sandbox.cpp) -> roy_plugin_host (one process per instance) -> plugin`.
- Scanner out of process with timeout; crash/hang -> QUARANTINE (file never moved/deleted); rescan skips
  unchanged/blacklisted/quarantined; retry; duplicates; arch detection (ELF/PE/Mach-O); DB JSON;
  views INSTALLED / AVAILABLE / FAILED / BLACKLISTED / FAVORITES / RECENT / INSTRUMENTS / EFFECTS / DUPLICATES.
- Sandboxed CLAP host: audio + events + parameter changes via shared memory and process-shared semaphores,
  control (activate, state save/load, params) via JSON lines; plugin stdout cannot corrupt the protocol.
- Crash during playback: "PLUGIN CRASHED" logged, JSON crash report, effect -> pass-through,
  instrument -> silence, project keeps rendering; `PluginStatus`, `RestartPlugin` (restores last good state).
- Hang: audio thread waits at most `processTimeoutMs` (default 250 ms) once, then bypasses; watchdog kills the host.
- DAW-side `process()` verified allocation-free.
- Tested with REAL CLAP plugins built from `RoYStudio/plugins_test/` (test plugins: gain effect, sine
  instrument, deliberate crash, deliberate hang).
- VST3: detected and described from `moduleinfo.json`, listed as "unsupported" – loading needs the
  Steinberg VST3 SDK (not vendored yet). No third-party plugin binaries were downloaded.
Limitations: no plugin GUIs (editor windows) yet; output events (param gestures) from plugins ignored;
Windows code paths (named events / file mapping / CreateProcess) written but not yet compiled/tested on Windows.

## Changed / new files (this step)
`src/plugins/{PluginProtocol.h,SharedMemory.*,ClapHost.*,HostService.*,Sandbox.*,Scanner.*}`, `src/core/Base64.h`,
`src/commands/PluginCommands.cpp`, `apps/roy_plugin_host/main.cpp`, `plugins_test/*.cpp`, `tests/test_plugins.cpp`,
`src/audio/ProjectRuntime.*` (capture keeps instance, forgetProcessor, allProcessors).

## Next
Integration: Definition-of-Done end-to-end test, CLI expansion, GUI, Windows cross-build check, benchmarks, release report.
