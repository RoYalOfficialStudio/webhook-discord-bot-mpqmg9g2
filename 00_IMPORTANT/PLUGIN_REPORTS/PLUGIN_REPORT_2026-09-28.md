# Plugin report 2026-09-28

Environment: Linux x86_64 container. No third-party plugins installed or downloaded (by design: only test
plugins built from our own source were used).

| Module | Format | Result | Notes |
|---|---|---|---|
| roy_test_gain.clap (RoY Test Gain, RoY Test Sine) | CLAP 1.2.2 | OK – scanned, loaded in sandbox, processed audio | gain exact to 1e-6, state round-trip, 440 Hz instrument |
| roy_test_crash.clap | CLAP | crash at scan -> QUARANTINED; crash during process -> PLUGIN CRASHED, bypass, restart OK | deliberate |
| roy_test_hang.clap | CLAP | hang at scan -> timeout 1.5 s -> QUARANTINED | deliberate |
| Fake Synth.vst3 (moduleinfo only) | VST3 | detected, UNSUPPORTED (SDK not bundled) | synthetic test bundle |
| old32.clap (PE i386 header) | CLAP | WRONG_ARCH | synthetic |

Scan time for the test folder: ~15 ms for 2 modules (4 plugins).
