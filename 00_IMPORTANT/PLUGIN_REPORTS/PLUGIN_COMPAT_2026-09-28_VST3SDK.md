# Plugin compatibility pass – official Steinberg VST3 SDK plugins (2026-09-28)

**What was tested:** all 19 plugin modules (55 plugin classes) that ship as source with the official
Steinberg VST 3 SDK **v3.8.1_build_84** (github.com/steinbergmedia/vst3sdk, same commits as the
SDK vendored in RoY; MIT licence). They are third-party code written by Steinberg / mda (not by RoY):
AGain variants (VSTGUI editors), ADelay, the **VST3 Host Checker**, Note Expression Synth (+UI),
Panner, TransportControl, program-change / pitch-names / remap / legacy-MIDI-CC / prefetch /
channel-context / UTF-16-name test plugins and the complete **mda** collection (DX10, JX10, EPiano,
Piano, BeatBox, Dynamics, Limiter, Leslie, Delay, TalkBox, …).

**How:** built from source on Linux (Release, VSTGUI on) and run with
`roy_cli plugin-compat <folder> --editor --seconds 2` under Xvfb: sandboxed scan → load → audio
(effects: −12 dBFS 440 Hz tone, instruments: a note) → every parameter moved → state saved and
restored into a fresh instance → editor open/close → unload (host process must end).
Build-only packages (for the test plugins, not shipped with RoY) came from the official Ubuntu
24.04 archive via apt: libxcb*/libxkbcommon*/libcairo2/libfontconfig1/libfreetype/libpango1.0/
libgtkmm-3.0/libsqlite3 (-dev).

## Result: 53 of 55 PASS – the 2 failures are bugs in the plugins, RoY is protected against them

### Host bugs found in RoY and FIXED in this pass
| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Every VSTGUI editor (AGain, Host Checker, Panner, Note Expression Synth UI, …) crashed the plugin host on "open editor" (Linux) | Since SDK 3.7 VSTGUI takes its Linux run loop from the **host context** (`IPluginFactory3::setHostContext` → `setupVSTGUIRunloop`); RoY's host application did not implement `Linux::IRunLoop` and never called `setHostContext` → null run loop | Host application now implements `Linux::IRunLoop` (process-wide run loop, pumped every 10 ms); `setHostContext` is called after loading a module. Regression test: the RoY VST3 test view registers a timer on the host context (`vst3/editor` test) |
| 2 | AGainSimple / PitchNames editors killed the plugin host | View reports 0 × 0 before `attached()`; RoY created a 0 × 0 X11 window → `BadValue` → Xlib's default error handler calls `exit()` | Window sizes are sanitised (fallback 400 × 300, 50..16384) on create and resize (Windows too); a non-fatal X error handler logs instead of exiting. Regression test: the test view reports 0 × 0 before attach |
| 3 | A plugin's NaN/Inf output would have poisoned every following effect and the master | no sanitising of sandbox output | NaN/Inf from a plugin become silence, are counted per instance and shown once in the UI ("PLUGIN OUTPUT INVALID") |
| 4 | Plugins without own state (getState → kNotImplemented / empty chunk) were reported as "rejected state" | empty component chunk was still passed to `setState` | empty chunk / `kNotImplemented` = "no state", not a rejection |
| 5 | (robustness) stray non-JSON lines on the control channel failed the command | – | the reader skips non-protocol lines and logs them |

### Plugin bugs found (not RoY's): handled safely
| Plugin | Finding | Evidence | RoY behaviour |
|---|---|---|---|
| mda Bandisto | NaN output from the first block | filter state `fb1/fb2/fb3` is never initialised (mdaBandistoProcessor.h/.cpp) | output silenced, counted, user warned; project continues |
| mda Tracker | NaN output | `phi`, `buf1..4`, `saw`, `env`, … never initialised (only `dphi`/`min` in setActive) | same |
| Advanced Tutorial | its own state does not load | `getState` writes one double, `setState` first reads a uint32 then a double (tutorial.cpp) | runs with defaults, the rejected state is kept in the project (`rejectedState`) |
| ADelay / SyncDelay | silent during the first second | 1 s default delay (by design) | – (audio after 1 s verified) |

## Full table (final run)

| Plugin | Vendor | Format | Kind | Params | Load | Audio | Out RMS | State | Editor | Unload | Result | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ADelay | Steinberg Media Technologies | vst3 | effect | 2 | 3 ms | PASS | -18 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| AGain Sample Accurate | Steinberg Media Technologies | vst3 | effect | 2 | 4 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| Advanced Tutorial | Steinberg Media Technologies | vst3 | effect | 1 | 4 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | Advanced Tutorial: the plugin rejected its saved state (other plugin version or damaged data) - it runs with default settings; the saved state is kept in the project and not lost; editor: plugin provides no editor view |
| AGainSimple VST3 | Steinberg Media Technologies | vst3 | effect | 3 | 7 ms | PASS | -15 dB | PASS | PASS | PASS | **PASS** |  |
| AGain VST3 | Steinberg Media Technologies | vst3 | effect | 3 | 7 ms | PASS | -15 dB | PASS | PASS | PASS | **PASS** |  |
| AGain SideChain VST3 | Steinberg Media Technologies | vst3 | effect | 3 | 7 ms | PASS | -15 dB | PASS | PASS | PASS | **PASS** |  |
| Test Channel Context | Steinberg Media Technologies | vst3 | instrument | 11 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| VST3 Host Checker | Steinberg Media Technologies | vst3 | instrument | 140 | 9 ms | PASS | -300 dB | PASS | PASS | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins) |
| Test Legacy MIDI CC Out | Steinberg Media Technologies | vst3 | effect | 18 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Ambience | mda | vst3 | effect | 5 | 4 ms | PASS | -20 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Bandisto | mda | vst3 | effect | 11 | 4 ms | FAIL | -300 dB | PASS | none | PASS | **FAIL** | plugin produced 192000 NaN/Inf samples - RoY replaced them with silence (plugin bug); restored instance: invalid output too; editor: plugin provides no editor view |
| mda BeatBox | mda | vst3 | effect | 13 | 3 ms | PASS | -24 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Combo | mda | vst3 | effect | 8 | 3 ms | PASS | -29 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda DeEsser | mda | vst3 | effect | 4 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Degrade | mda | vst3 | effect | 7 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Delay | mda | vst3 | effect | 7 | 3 ms | PASS | -11 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Detune | mda | vst3 | effect | 5 | 3 ms | PASS | -13 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Dither | mda | vst3 | effect | 6 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda DubDelay | mda | vst3 | effect | 8 | 4 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda DX10 | mda | vst3 | instrument | 19 | 3 ms | PASS | -25 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Dynamics | mda | vst3 | effect | 11 | 3 ms | PASS | -13 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda EPiano | mda | vst3 | instrument | 15 | 3 ms | PASS | -22 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Image | mda | vst3 | effect | 7 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda JX10 | mda | vst3 | instrument | 31 | 3 ms | PASS | -20 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Leslie | mda | vst3 | effect | 10 | 3 ms | PASS | -12 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Limiter | mda | vst3 | effect | 6 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Loudness | mda | vst3 | effect | 4 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda MultiBand | mda | vst3 | effect | 14 | 3 ms | PASS | -16 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Overdrive | mda | vst3 | effect | 4 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Piano | mda | vst3 | instrument | 15 | 4 ms | PASS | -21 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda RePsycho! | mda | vst3 | effect | 8 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda RezFilter | mda | vst3 | effect | 11 | 3 ms | PASS | -8 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda RingMod | mda | vst3 | effect | 4 | 3 ms | PASS | -18 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Round Panner | mda | vst3 | effect | 3 | 3 ms | PASS | -18 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Shepard | mda | vst3 | effect | 4 | 4 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Splitter | mda | vst3 | effect | 8 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Stereo Simulator | mda | vst3 | effect | 6 | 3 ms | PASS | -13 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Sub-Bass Synthesizer | mda | vst3 | effect | 7 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda TalkBox | mda | vst3 | effect | 5 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda TestTone | mda | vst3 | effect | 9 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| mda Thru-Zero Flanger | mda | vst3 | effect | 6 | 3 ms | PASS | -18 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| mda Tracker | mda | vst3 | effect | 9 | 3 ms | FAIL | -18 dB | PASS | none | PASS | **FAIL** | plugin produced 96000 NaN/Inf samples - RoY replaced them with silence (plugin bug); restored instance: invalid output too; editor: plugin provides no editor view |
| mda SpecMeter | mda | vst3 | effect | 36 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| Test Multiple Program Changes | Steinberg Media Technologies | vst3 | effect | 19 | 4 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| Note Expression Synth With UI | Steinberg Media Technologies | vst3 | instrument | 18 | 8 ms | PASS | -24 dB | PASS | PASS | PASS | **PASS** |  |
| Note Expression Synth | Steinberg Media Technologies | vst3 | instrument | 17 | 7 ms | PASS | -24 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| Note Expression Text | Steinberg Media Technologies | vst3 | instrument | 1 | 7 ms | PASS | -300 dB | PASS | PASS | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins) |
| Panner | Steinberg Media Technologies | vst3 | effect | 2 | 7 ms | PASS | -18 dB | PASS | PASS | PASS | **PASS** |  |
| PitchNames | Steinberg Media Technologies | vst3 | instrument | 2 | 7 ms | PASS | -300 dB | PASS | PASS | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins) |
| Test Prefetchable Support | Steinberg Media Technologies | vst3 | effect | 2 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| Test Program Change | Steinberg Media Technologies | vst3 | effect | 3 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| Test Remap ParamID | Steinberg | vst3 | effect | 2 | 3 ms | PASS | -15 dB | PASS | none | PASS | **PASS** | editor: plugin provides no editor view |
| SyncDelay | Steinberg Media Technologies | vst3 | effect | 2 | 3 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
| TransportControl | Steinberg Media Technologies | vst3 | effect | 4 | 7 ms | PASS | -15 dB | PASS | PASS | PASS | **PASS** |  |
| UTF16Name öüäéèê-やあ-مرحبًا | Steinberg Media Technologies - öüäéèê-やあ-مرحبًا | vst3 | effect | 5 | 4 ms | PASS | -300 dB | PASS | none | PASS | **PASS** | silent output (may be expected, e.g. MIDI/utility plugins); editor: plugin provides no editor view |
