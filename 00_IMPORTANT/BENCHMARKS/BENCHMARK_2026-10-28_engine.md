# RoY Studio benchmark 2026-10-28T17:38:39Z

Host: vm | buffer 256 @ 48 kHz (budget 5.33 ms) | 20 s per size

Machine: 4 hardware threads -> 3 mixing worker threads + the audio thread in multi-core mode.

| Size | Tracks | Threads | Graph build | CPU mean | CPU p99 | CPU max | Blocks over budget | Offline render | RAM | Save | Load | .roy size | PDC |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SMALL | 9 (+4 busses) | 1 | 3 ms | 20 % | 31 % | 123 % | 1 / 3750 | 5.2x realtime | 38 MB | 5 ms | 4 ms | 83 KB | 112 smp |
| SMALL | 9 (+4 busses) | 4 | 2 ms | 15 % | 34 % | 86 % | 0 / 3750 | 6.5x realtime | 42 MB | 7 ms | 4 ms | 83 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 1 | 4 ms | 62 % | 99 % | 146 % | 32 / 3750 | 1.6x realtime | 81 MB | 12 ms | 11 ms | 239 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 4 | 3 ms | 30 % | 48 % | 929 % | 5 / 3750 | 3.3x realtime | 89 MB | 13 ms | 12 ms | 239 KB | 112 smp |
| LARGE | 99 (+4 busses) | 1 | 7 ms | 116 % | 199 % | 355 % | 2260 / 3750 | 0.9x realtime | 140 MB | 22 ms | 20 ms | 444 KB | 112 smp |
| LARGE | 99 (+4 busses) | 4 | 5 ms | 50 % | 85 % | 213 % | 24 / 3750 | 2.1x realtime | 154 MB | 23 ms | 23 ms | 444 KB | 112 smp |
| XL | 150 (+4 busses) | 1 | 7 ms | 161 % | 279 % | 349 % | 3746 / 3750 | 0.7x realtime | 224 MB | 34 ms | 31 ms | 648 KB | 112 smp |
| XL | 150 (+4 busses) | 4 | 7 ms | 67 % | 115 % | 217 % | 62 / 3750 | 1.6x realtime | 245 MB | 34 ms | 31 ms | 648 KB | 112 smp |

## Processor cost (one instance, per block)

| Processor | Kind | Time/block | % of budget |
|---|---|---|---|
| roy.vocaltune2 | effect | 198.9 us | 3.73 % |
| roy.vocaltune | effect | 195.8 us | 3.67 % |
| roy.distortion | effect | 179.1 us | 3.36 % |
| roy.saturation | effect | 149.6 us | 2.81 % |
| roy.limiter | effect | 129.3 us | 2.42 % |
| roy.clipper | effect | 120.5 us | 2.26 % |
| roy.analyzer | effect | 71.3 us | 1.34 % |
| roy.noisecleaner | effect | 62.0 us | 1.16 % |
| roy.synth | instrument | 52.0 us | 0.97 % |
| roy.drums | instrument | 51.5 us | 0.96 % |
| roy.phaser | effect | 36.1 us | 0.68 % |
| roy.808 | instrument | 29.6 us | 0.56 % |
| roy.compressor | effect | 16.6 us | 0.31 % |
| roy.pitch | effect | 16.1 us | 0.30 % |
| roy.deesser | effect | 11.5 us | 0.22 % |
| roy.chorus | effect | 11.2 us | 0.21 % |
| roy.transient | effect | 10.8 us | 0.20 % |
| roy.flanger | effect | 10.5 us | 0.20 % |
| roy.delay | effect | 10.4 us | 0.20 % |
| roy.dynamicspace | effect | 9.4 us | 0.18 % |
| roy.reverb | effect | 8.4 us | 0.16 % |
| roy.eq | effect | 2.2 us | 0.04 % |
| roy.gate | effect | 1.8 us | 0.03 % |
| roy.stereo | effect | 0.8 us | 0.02 % |
| roy.sampler | instrument | 0.8 us | 0.02 % |

CPU = callback wall time / buffer duration. Threads = audio thread + mixing workers (channels of one routing level run
in parallel; output is bit-identical to single-threaded). Blocks over budget would be audible dropouts (xruns) on a
real device with this buffer size. Measured on a shared cloud VM (no realtime scheduling), so p99/max include VM jitter.
