# ROY STUDIO ULTIMATE STATUS

| | |
|---|---|
| VERSION | 0.1.0 (release stage: **ALPHA** → ALPHA HARDENING in progress) |
| BUILD | Linux x86_64 GCC 13.3 RelWithDebInfo; Windows x64 mingw-w64 13.2 cross build (static) |
| COMMIT | 1cf0521 + this report commit (branch claude/bold-franklin-fybkj5) |
| DATUM | 2026-09-28 |

## Verification run of this report
| Check | Result |
|---|---|
| Linux full suite | PASS – 126 tests, 0 failed, 99 441 checks |
| Windows full suite under Wine 9.0 | PASS – 125 tests, 0 failed (the SIGSTOP hang test is POSIX-only) |
| Windows GUI self-test (roy_studio.exe --selftest, Wine) | PASS – 17/17 steps |
| Windows CLI self-test (roy_cli.exe selftest, Wine) | PASS |
| Linux GUI/CLI self-tests | PASS |
| Multi-core tests (0/1/3/7 workers bit-identical, level graph, start/stop stress ×20) | PASS |
| Determinism (export twice, save/close/reopen/export bit-identical) | PASS |
| Plugin sandbox tests (scan, quarantine, crash, hang, restart) | PASS |
| Recovery tests (autosave, crash snapshot, last stable) | PASS |
| Benchmark smoke test | PASS |
| ASan/UBSan full suite | PASS (only findings: the deliberate crash test plugin) |
| TSan multi-core tests | PASS (0 warnings) |

Bug found and fixed during this run: a new mixing worker thread could miss its first job and the audio thread
waited forever (start-ticket race; showed up under Wine). Fixed + 20× stress test.

## Components
| Area | Status | Notes |
|---|---|---|
| CORE | PASS | logging, files, processes, command system |
| PROJECT | PASS | versioned JSON, migration, forward-compatible keys |
| AUDIO | PARTIAL | engine PASS; only null backend exercised (no audio hardware here) |
| PLAYLIST | PASS | engine + GUI (clips, waveforms, drag move, split, sections, markers) |
| RECORDING | PARTIAL | takes, loop, punch, comp, never-lose PASS headless; real inputs UNTESTED |
| MIDI | PARTIAL | editing, SMF import/export PASS; live MIDI device input NOT IMPLEMENTED |
| PIANO ROLL | PASS | GUI add/delete/select/velocity/quantize/scale highlight + Wrong Note Blocker |
| VOCALS | PARTIAL | analysis/repair PASS on synthetic voices; real recordings UNTESTED; editor UX basic |
| PITCH GUARDIAN | PARTIAL | PASS on synthetic material; needs broader test material + curve editor |
| OFF-KEY FILTER | PARTIAL | tested Major/Minor; other modes not yet covered by tests |
| BEAT LAB | PARTIAL | steps, swing, probability, rolls, variations; groove templates/chaining open |
| 808 LAB | PARTIAL | glide/slide/key lock/collision analyzer; visual analyzer open |
| SAMPLER | PASS | zones, loops, slicing |
| MIXER | PASS | busses, sends pre/post, sidechain, PDC, solo-in-place, multi-core |
| CLAP | PASS | scan, load, process audio+notes, params, state, crash isolation |
| VST3 | PARTIAL | SDK 3.8.1 vendored and building; host implementation in progress (scan = detection only) |
| PLUGIN SANDBOX | PASS | one RoYPluginHost process per instance, crash/hang → PLUGIN CRASHED, restart |
| PLUGIN GUI | FAIL | plugin editor windows not implemented yet |
| BROWSER | PARTIAL | file browsing, search, import, drag to playlist; categories/tags open |
| AUDIO PREVIEW | FAIL | not implemented yet |
| STEM SEPARATION | PARTIAL | basic DSP separator only |
| MIX INTELLIGENCE | PASS | analysis + suggestions with executable commands |
| MASTERING | PASS | presets, assistant, BS.1770 loudness / true peak |
| WAV | PASS | |
| FLAC | PASS | native encoder, decode-verified |
| MP3 | BLOCKED → approved | not implemented yet (licence approved 2026-09-28; next block) |
| BACKUP | PASS | numbered backups + retention |
| RECOVERY | PASS | autosave, crash detection, recover/last stable/discard |
| UNDO/REDO | PASS | memento undo, macros (one step for bulk operations) |
| PERFORMANCE | PASS | LARGE 99 tracks: 0 / 3750 blocks over budget (4 threads, 256 samples); XL: 21 / 3750 |
| LINUX | PASS | |
| WINDOWS/WINE | PASS | |
| WINDOWS NATIVE | UNTESTED | see TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md |
| REAL AUDIO HARDWARE | UNTESTED | see TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md (ASIO: no backend → BLOCKED) |

## Next blocks (in order)
VST3 host → plugin GUI editors → plugin parameter UX → MP3 → browser preview + drag & drop + plugin browser →
vocal hardening + vocal editor UX → IStemSeparator → Beat/808 features → mixer/automation hardening →
crash injection → performance regression → BETA gate.
