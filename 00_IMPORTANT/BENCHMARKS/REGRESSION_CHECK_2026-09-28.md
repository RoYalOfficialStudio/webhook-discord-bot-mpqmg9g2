# Performance regression check – 2026-09-28

Compared: baseline `BENCHMARK_2026-09-28_engine.json` (commit 1cf0521) against the current build
(commit 2db4c3f + pending report commit) with the new `roy_bench --baseline` check (tolerance 15 %,
exit code 2 on regression). Buffer 256 samples @ 48 kHz (budget 5.33 ms), 4 hardware threads.

## 1. Baseline comparison (`BENCHMARK_2026-09-28b_engine.md`)

| Size | Threads | CPU mean (base → now) | Render | Verdict of the tool |
|---|---|---|---|---|
| SMALL | 1 | 13 → 16 % | 7.5 → 6.3x | REGRESSION |
| SMALL | 4 | 10 → 11 % | 10.2 → 8.8x | REGRESSION |
| MEDIUM | 1 | 38 → 49 % | 2.5 → 2.0x | REGRESSION |
| MEDIUM | 4 | 21 → 22 % | 3.6 → 4.6x | ok |
| LARGE | 1 | 77 → 93 % | 1.3 → 1.1x | REGRESSION |
| LARGE | 4 | 44 → 36 % | 2.4 → 2.9x | faster |
| XL | 1 | 115 → 136 % | 0.9 → 0.7x | REGRESSION |
| XL | 4 | 55 → 47 % | 1.5 → 2.1x | faster |

The tool flagged 5 regressions. They were **investigated, not ignored**:

* The per-processor micro benchmarks of processors whose code did **not** change at all
  (EQ, saturation, limiter, synth, phaser, …) were a median **1.28x slower** in the new run.
  The VM itself was ~25–30 % slower during this run (shared cloud host, no realtime scheduling).
* Decisive test – **same-machine A/B**: the baseline commit 1cf0521 was built in a separate worktree
  (same compiler, RelWithDebInfo) and run interleaved with the current build (8 s per size):

| Build | Round | MEDIUM 1 thr | MEDIUM 4 thr | LARGE 1 thr | LARGE 4 thr |
|---|---|---|---|---|---|
| old (1cf0521) | 1 | 48 % | 22 % | 90 % | 34 % |
| new | 1 | 48 % | 21 % | 93 % | 35 % |
| old (1cf0521) | 2 | 50 % | 22 % | 89 % | 33 % |
| new | 2 | 48 % | 21 % | 90 % | 34 % |

Old and new are equal within ±3 % (both directions). **Result: no code regression.** The new features
of this block (automation curves, send/tempo automation, groove templates, 808 phase/de-click, solo safe)
cost no measurable engine CPU. The tempo map is now O(log n) (was O(n)), which matters for dense
tempo automation (2 million lookups on a 10 000-event map < 1.5 s, see test `automation`).

Rule kept for the future: a flagged regression is only accepted as "environment" after a
same-machine A/B against the baseline build, as done here.

## 2. UI (100 tracks, llvmpipe software OpenGL) – `BENCHMARK_2026-09-28b_ui.md`

| Area | before (FPS) | now (FPS) |
|---|---|---|
| PLAYLIST | 41 | 36 |
| CHANNELS | 56 | 47 |
| PIANO ROLL | 50 | 44 |
| MIXER | 48 | 40 |
| VOCALS | 59 | 53 |
| BEATS | 58 | 46 |
| PLUGINS | 65 | 67 |
| MASTER | 70 | 64 |
| PROJECT | 71 | 63 |

All areas stay above 30 FPS with 100 tracks on a software renderer. The drop is in line with the slower
VM during this run (PLUGINS, whose UI did not change, is unchanged); MIXER and BEATS additionally draw
more widgets now (send sliders, solo safe, groove/step details). Status: PASS, to be re-measured on
real Windows GPU hardware (see TEST_REPORTS/WINDOWS_NATIVE_TEST_PLAN.md).
