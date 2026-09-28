# RoY Studio benchmark 2026-09-28T22:00:33Z

Host: vm | buffer 256 @ 48 kHz (budget 5.33 ms) | 20 s per size

Machine: 4 hardware threads -> 3 mixing worker threads + the audio thread in multi-core mode.

| Size | Tracks | Threads | Graph build | CPU mean | CPU p99 | CPU max | Blocks over budget | Offline render | RAM | Save | Load | .roy size | PDC |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SMALL | 9 (+4 busses) | 1 | 3 ms | 16 % | 23 % | 32 % | 0 / 3750 | 6.3x realtime | 41 MB | 6 ms | 5 ms | 83 KB | 112 smp |
| SMALL | 9 (+4 busses) | 4 | 1 ms | 11 % | 17 % | 21 % | 0 / 3750 | 8.8x realtime | 47 MB | 6 ms | 5 ms | 83 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 1 | 4 ms | 49 % | 66 % | 104 % | 1 / 3750 | 2.0x realtime | 118 MB | 12 ms | 11 ms | 239 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 4 | 5 ms | 22 % | 31 % | 34 % | 0 / 3750 | 4.6x realtime | 140 MB | 12 ms | 11 ms | 239 KB | 112 smp |
| LARGE | 99 (+4 busses) | 1 | 5 ms | 93 % | 117 % | 140 % | 1238 / 3750 | 1.1x realtime | 247 MB | 21 ms | 19 ms | 444 KB | 112 smp |
| LARGE | 99 (+4 busses) | 4 | 5 ms | 36 % | 55 % | 77 % | 0 / 3750 | 2.9x realtime | 283 MB | 21 ms | 20 ms | 444 KB | 112 smp |
| XL | 150 (+4 busses) | 1 | 7 ms | 136 % | 170 % | 295 % | 3741 / 3750 | 0.7x realtime | 397 MB | 33 ms | 29 ms | 648 KB | 112 smp |
| XL | 150 (+4 busses) | 4 | 7 ms | 47 % | 75 % | 101 % | 1 / 3750 | 2.1x realtime | 465 MB | 30 ms | 29 ms | 648 KB | 112 smp |

Plugin scan: 4 modules, 7 plugins OK, 0 crashed, 1 timeouts (quarantined) in 3.12 s (out of process, the hang test plugin costs the full 3000 ms timeout)

## Comparison with baseline BENCHMARK_2026-09-28_engine.json

Tolerance 15 %. CPU values in % of the buffer budget; render in x realtime.

| Size | Threads | CPU mean (base -> now) | CPU p99 | Render | Verdict |
|---|---|---|---|---|---|
| SMALL | 1 | 13 -> 16 % | 17 -> 23 % | 7.5 -> 6.3x | REGRESSION |
| SMALL | 4 | 10 -> 11 % | 15 -> 17 % | 10.2 -> 8.8x | REGRESSION |
| MEDIUM | 1 | 38 -> 49 % | 51 -> 66 % | 2.5 -> 2.0x | REGRESSION |
| MEDIUM | 4 | 21 -> 22 % | 37 -> 31 % | 3.6 -> 4.6x | ok |
| LARGE | 1 | 77 -> 93 % | 104 -> 117 % | 1.3 -> 1.1x | REGRESSION |
| LARGE | 4 | 44 -> 36 % | 66 -> 55 % | 2.4 -> 2.9x | faster |
| XL | 1 | 115 -> 136 % | 160 -> 170 % | 0.9 -> 0.7x | REGRESSION |
| XL | 4 | 55 -> 47 % | 92 -> 75 % | 1.5 -> 2.1x | faster |

**5 REGRESSION(S)** - investigate before release.

## Processor cost (one instance, per block)

| Processor | Kind | Time/block | % of budget |
|---|---|---|---|
| roy.distortion | effect | 173.5 us | 3.25 % |
| roy.vocaltune | effect | 142.7 us | 2.68 % |
| roy.saturation | effect | 133.4 us | 2.50 % |
| roy.limiter | effect | 121.7 us | 2.28 % |
| roy.clipper | effect | 107.2 us | 2.01 % |
| roy.noisecleaner | effect | 60.4 us | 1.13 % |
| roy.analyzer | effect | 57.6 us | 1.08 % |
| roy.drums | instrument | 52.2 us | 0.98 % |
| roy.synth | instrument | 40.4 us | 0.76 % |
| roy.phaser | effect | 36.4 us | 0.68 % |
| roy.808 | instrument | 28.6 us | 0.54 % |
| roy.pitch | effect | 16.2 us | 0.30 % |
| roy.compressor | effect | 13.9 us | 0.26 % |
| roy.chorus | effect | 11.3 us | 0.21 % |
| roy.flanger | effect | 10.8 us | 0.20 % |
| roy.delay | effect | 10.3 us | 0.19 % |
| roy.deesser | effect | 9.8 us | 0.18 % |
| roy.dynamicspace | effect | 9.7 us | 0.18 % |
| roy.reverb | effect | 8.2 us | 0.15 % |
| roy.transient | effect | 7.0 us | 0.13 % |
| roy.gate | effect | 1.9 us | 0.04 % |
| roy.eq | effect | 1.5 us | 0.03 % |
| roy.sampler | instrument | 0.8 us | 0.02 % |
| roy.stereo | effect | 0.7 us | 0.01 % |

CPU = callback wall time / buffer duration. Threads = audio thread + mixing workers (channels of one routing level run
in parallel; output is bit-identical to single-threaded). Blocks over budget would be audible dropouts (xruns) on a
real device with this buffer size. Measured on a shared cloud VM (no realtime scheduling), so p99/max include VM jitter.
