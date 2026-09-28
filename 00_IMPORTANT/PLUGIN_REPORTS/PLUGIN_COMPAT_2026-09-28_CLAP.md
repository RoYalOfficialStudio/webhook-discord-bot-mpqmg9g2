# Plugin compatibility pass – official CLAP example plugins (2026-09-28)

**What was tested:** the 20 example plugins of **free-audio/clap-plugins** (official CLAP organisation,
MIT, commit 81791d0, submodules clap 195b42a / clap-helpers 37f40f5) – gain, DC offset (+latency), latency,
offline latency, SVF filter, CLAP Synth (polyphonic, modulation), gain metering, mini curve display,
transport / track info, project location, character check (UTF-8 names), realtime requirement,
scratch memory, three undo tests. Built headless from source (no Qt GUI) in two variants:
Debug (all plugin assertions + clap-helpers host checks active) and Release.

**How:** `roy_cli plugin-compat <folder> --editor --seconds 2` (sandboxed load → audio → parameters →
state into a fresh instance → editor → unload).

## Result
| Build | PASS | Notes |
|---|---|---|
| Release | **20 / 20** | all process audio, all state round trips exact, clean unload |
| Debug (assertions on) | 19 / 20 | CLAP SVF aborts on a plugin-internal assertion (below) – RoY isolates it: PLUGIN CRASHED, pass-through, the host process is cleaned up |

No clap-helpers host-conformance violation was reported for RoY in the Debug build.

### Plugin bug found
| Plugin | Finding | Evidence | RoY behaviour |
|---|---|---|---|
| CLAP SVF (Debug) | `Assertion b.channelCount() == 1 || b.channelCount() == _channelCount` | svf-module.cc: without an FM input the module computes `_fmBuffer (1 ch).product(fmAmount, *_input (2 ch))` – happens with any stereo host | crash isolated in the sandbox, project continues; Release build (assertion off) processes normally |

### Tool fix
`plugin-compat` now recognises "no embeddable editor" (headless plugins) as "no editor" instead of an editor failure.

## Full table (Release build)

| Plugin | Vendor | Format | Kind | Params | Load | Audio | Out RMS | State | Editor | Unload | Result | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CLAP ADSR | clap | clap | instrument | 5 | 3 ms | PASS | -5 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Character Check 🌶 | clap 🌶 | clap | effect | 15 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| DC Offset | clap | clap | effect | 1 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| DC Offset (with latency) | clap | clap | effect | 1 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Gain | clap | clap | effect | 1 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Gain Adjustment Metering | clap | clap | effect | 0 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Latency | clap | clap | effect | 0 | 3 ms | PASS | -16 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Mini Curve Display | clap | clap | effect | 0 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Mini Curve Display (Dynamic) | clap | clap | effect | 0 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Location Test | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| Offline Latency | clap | clap | effect | 0 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Realtime Requirement | clap | clap | effect | 0 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Scratch Memory Test | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| CLAP SVF | clap | clap | effect | 6 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| CLAP Synth | clap | clap | instrument | 25 | 5 ms | PASS | -2 dB | PASS | none | PASS | **PASS** | editor: plugin has no embeddable editor for this platform |
| Track Info | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| Transport Info | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| Undo Test (no deltas) | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| Undo Test (deltas not persistent) | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
| Undo Test | clap | clap | effect | 0 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin has no embeddable editor for this platform |
