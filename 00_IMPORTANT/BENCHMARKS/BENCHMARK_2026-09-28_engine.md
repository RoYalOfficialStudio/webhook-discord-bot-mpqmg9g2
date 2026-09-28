# RoY Studio benchmark 2026-09-28T18:57:17Z

Host: vm | buffer 256 @ 48 kHz (budget 5.33 ms) | 20 s per size

Machine: 4 hardware threads -> 3 mixing worker threads + the audio thread in multi-core mode.

| Size | Tracks | Threads | Graph build | CPU mean | CPU p99 | CPU max | Blocks over budget | Offline render | RAM | Save | Load | .roy size | PDC |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SMALL | 9 (+4 busses) | 1 | 2 ms | 13 % | 17 % | 23 % | 0 / 3750 | 7.5x realtime | 41 MB | 5 ms | 4 ms | 83 KB | 112 smp |
| SMALL | 9 (+4 busses) | 4 | 1 ms | 10 % | 15 % | 34 % | 0 / 3750 | 10.2x realtime | 47 MB | 5 ms | 4 ms | 83 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 1 | 3 ms | 38 % | 51 % | 76 % | 0 / 3750 | 2.5x realtime | 117 MB | 14 ms | 11 ms | 239 KB | 112 smp |
| MEDIUM | 48 (+4 busses) | 4 | 2 ms | 21 % | 37 % | 96 % | 0 / 3750 | 3.6x realtime | 140 MB | 54 ms | 10 ms | 239 KB | 112 smp |
| LARGE | 99 (+4 busses) | 1 | 5 ms | 77 % | 104 % | 132 % | 50 / 3750 | 1.3x realtime | 247 MB | 24 ms | 19 ms | 444 KB | 112 smp |
| LARGE | 99 (+4 busses) | 4 | 4 ms | 44 % | 66 % | 97 % | 0 / 3750 | 2.4x realtime | 283 MB | 16 ms | 19 ms | 444 KB | 112 smp |
| XL | 150 (+4 busses) | 1 | 6 ms | 115 % | 160 % | 274 % | 2942 / 3750 | 0.9x realtime | 399 MB | 28 ms | 27 ms | 648 KB | 112 smp |
| XL | 150 (+4 busses) | 4 | 6 ms | 55 % | 92 % | 149 % | 21 / 3750 | 1.5x realtime | 426 MB | 31 ms | 29 ms | 648 KB | 112 smp |

Plugin scan: 3 modules, 3 plugins OK, 0 crashed, 1 timeouts (quarantined) in 3.11 s (out of process, the hang test plugin costs the full 3000 ms timeout)

## Processor cost (one instance, per block)

| Processor | Kind | Time/block | % of budget |
|---|---|---|---|
| roy.distortion | effect | 155.4 us | 2.91 % |
| roy.vocaltune | effect | 109.1 us | 2.05 % |
| roy.limiter | effect | 96.2 us | 1.80 % |
| roy.saturation | effect | 95.9 us | 1.80 % |
| roy.clipper | effect | 95.4 us | 1.79 % |
| roy.analyzer | effect | 55.6 us | 1.04 % |
| roy.drums | instrument | 43.4 us | 0.81 % |
| roy.noisecleaner | effect | 42.4 us | 0.79 % |
| roy.808 | instrument | 30.0 us | 0.56 % |
| roy.synth | instrument | 29.2 us | 0.55 % |
| roy.phaser | effect | 26.0 us | 0.49 % |
| roy.pitch | effect | 12.6 us | 0.24 % |
| roy.compressor | effect | 11.7 us | 0.22 % |
| roy.delay | effect | 11.4 us | 0.21 % |
| roy.chorus | effect | 10.4 us | 0.19 % |
| roy.flanger | effect | 8.9 us | 0.17 % |
| roy.deesser | effect | 8.4 us | 0.16 % |
| roy.dynamicspace | effect | 7.6 us | 0.14 % |
| roy.reverb | effect | 6.2 us | 0.12 % |
| roy.transient | effect | 5.2 us | 0.10 % |
| roy.gate | effect | 1.5 us | 0.03 % |
| roy.eq | effect | 1.1 us | 0.02 % |
| roy.sampler | instrument | 0.6 us | 0.01 % |
| roy.stereo | effect | 0.5 us | 0.01 % |

CPU = callback wall time / buffer duration. Threads = audio thread + mixing workers (channels of one routing level run
in parallel; output is bit-identical to single-threaded). Blocks over budget would be audible dropouts (xruns) on a
real device with this buffer size. Measured on a shared cloud VM (no realtime scheduling), so p99/max include VM jitter.
