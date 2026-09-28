# RoY Studio test report

126 tests, 0 failed, 99441 checks

| Suite | Test | Result | Time (s) |
|---|---|---|---|
| gmb01 | tempo map conversions | PASS | 0.000 |
| gmb01 | transport play pause stop seek | PASS | 0.000 |
| gmb01 | transport loop splits blocks | PASS | 0.000 |
| gmb01 | count-in and pre-roll | PASS | 0.000 |
| gmb01 | metronome clicks on every beat | PASS | 0.001 |
| gmb01 | engine plays a clip bit-exact at unity | PASS | 0.003 |
| gmb01 | sample rates and buffer sizes give identical output | PASS | 0.032 |
| gmb01 | routing busses sends mute solo | PASS | 0.004 |
| gmb01 | pre-fader send ignores fader | PASS | 0.002 |
| gmb01 | plugin delay compensation aligns paths | PASS | 0.001 |
| gmb01 | automation drives channel gain | PASS | 0.004 |
| gmb01 | audio callback does not allocate | PASS | 0.008 |
| gmb01 | non-finite samples never reach the output | PASS | 0.000 |
| gmb01 | graph hot swap while playing | PASS | 0.002 |
| gmb01 | null audio device runs the engine | PASS | 0.042 |
| gmb02 | save close load verify roundtrip | PASS | 0.018 |
| gmb02 | unknown keys from newer versions are preserved | PASS | 0.004 |
| gmb02 | migration from v0 keeps a backup | PASS | 0.000 |
| gmb02 | numbered backups and retention | PASS | 0.006 |
| gmb02 | autosave never touches the project file | PASS | 0.002 |
| gmb02 | crash recovery: recover, last stable, discard | PASS | 0.007 |
| gmb02 | second instance cannot open a live project | PASS | 0.005 |
| gmb02 | commands with undo redo and macros | PASS | 0.002 |
| gmb02 | command palette search and shortcuts | PASS | 0.000 |
| gmb02 | routing command rejects feedback loops | PASS | 0.000 |
| gmb03 | split clip renders sample-identical | PASS | 0.016 |
| gmb03 | move, trim, slip and duplicate | PASS | 0.030 |
| gmb03 | locked clips and groups | PASS | 0.003 |
| gmb03 | fades and crossfade are smooth | PASS | 0.010 |
| gmb03 | clip gain, mute, reverse | PASS | 0.008 |
| gmb03 | time stretch keeps pitch, changes length | PASS | 0.046 |
| gmb03 | stretched clip plays through the engine | PASS | 0.026 |
| gmb03 | waveform cache levels, persistence, speed | PASS | 0.721 |
| gmb03 | snap, time formats, markers, sections, loop | PASS | 0.000 |
| gmb04 | record a take into its own file | PASS | 0.365 |
| gmb04 | recorded take plays back at the right position | PASS | 0.366 |
| gmb04 | loop recording creates one take per pass | PASS | 0.370 |
| gmb04 | punch in and out | PASS | 0.368 |
| gmb04 | existing files are never overwritten | PASS | 0.697 |
| gmb04 | comping selects ranges across takes | PASS | 0.000 |
| gmb04 | flatten comp keeps audio identical | PASS | 0.372 |
| gmb04 | input clipping warning and meter | PASS | 0.031 |
| gmb04 | never-lose: recover a performance without pressing record | PASS | 0.045 |
| gmb04 | never-lose memory is bounded | PASS | 0.450 |
| gmb04 | recording path does not allocate on the audio thread | PASS | 0.368 |
| gmb05 | keys and scales | PASS | 0.000 |
| gmb05 | wrong note blocker modes | PASS | 0.000 |
| gmb05 | AddNote command obeys the blocker and can be disabled | PASS | 0.001 |
| gmb05 | quantize humanize transpose | PASS | 0.000 |
| gmb05 | duplicate legato strum arpeggiate | PASS | 0.000 |
| gmb05 | chord detection | PASS | 0.000 |
| gmb05 | standard MIDI file roundtrip | PASS | 0.001 |
| gmb05 | ghost notes from other clips | PASS | 0.000 |
| gmb05 | synth renders MIDI with correct pitch | PASS | 0.004 |
| gmb05 | synth polyphony, voice stealing, no allocation | PASS | 0.008 |
| gmb05 | MIDI track plays through the engine | PASS | 0.017 |
| gmb06 | Pitch Guardian command is non-destructive and undoable | PASS | 0.221 |
| gmb06 | Vocal Doctor command returns executable fixes | PASS | 0.184 |
| gmb06 | breath reduction writes undoable automation | PASS | 0.277 |
| gmb06 | Double Magnet command aligns a double track | PASS | 0.233 |
| gmb06 | pitch detection accuracy | PASS | 0.263 |
| gmb06 | analysis distinguishes stable, vibrato, slide | PASS | 0.337 |
| gmb06 | ASSIST corrects a sharp note and leaves in-tune notes untouched | PASS | 0.545 |
| gmb06 | vibrato is preserved while the centre is corrected | PASS | 0.383 |
| gmb06 | OFF-KEY filter and ALLOW CHROMATIC decide targets | PASS | 0.129 |
| gmb06 | low confidence is not corrected aggressively | PASS | 0.121 |
| gmb06 | formant preservation | PASS | 0.272 |
| gmb06 | slide is preserved and transitions stay continuous | PASS | 0.248 |
| gmb06 | vocal doctor finds technical problems | PASS | 0.454 |
| gmb06 | vocal doctor detects breaths and plosives | PASS | 0.245 |
| gmb06 | vocal microscope inspects a region | PASS | 0.232 |
| gmb06 | double magnet tightens a sloppy double | PASS | 0.734 |
| gmb06 | ghost take compares timing and pitch | PASS | 0.245 |
| gmb06 | flow analyzer classifies early, on beat, late | PASS | 0.122 |
| gmb06 | onsets, bpm and key estimation | PASS | 0.107 |
| gmb07 | pattern length, duplicate, variation, swing | PASS | 0.000 |
| gmb07 | pattern expansion: probability, flam, roll, micro timing, mute/solo | PASS | 0.000 |
| gmb07 | drum voices have the expected spectra | PASS | 0.005 |
| gmb07 | drum sample pads play assigned samples | PASS | 0.001 |
| gmb07 | beat track plays a pattern through the engine | PASS | 0.031 |
| gmb07 | 808: pitch, glide/slide, key lock, saturation | PASS | 0.009 |
| gmb07 | instruments do not allocate on the audio thread | PASS | 0.006 |
| gmb07 | kick/808 collision analyzer | PASS | 0.030 |
| gmb08 | sampler key mapping, pitch, velocity layers | PASS | 0.003 |
| gmb08 | sampler loop, one-shot, reverse, choke | PASS | 0.000 |
| gmb08 | sample editing tools | PASS | 0.000 |
| gmb08 | transient slicing and SLICE TO PADS | PASS | 0.006 |
| gmb08 | root note, bpm and key detection | PASS | 0.873 |
| gmb08 | stem separation: exact reconstruction, measured quality | PASS | 0.260 |
| gmb08 | import, slice-to-pads and stems commands | PASS | 0.154 |
| gmb09 | every processor survives all test signals at all sample rates | PASS | 2.680 |
| gmb09 | RoY EQ matches its response curve | PASS | 0.006 |
| gmb09 | compressor static curve, makeup and sidechain | PASS | 0.007 |
| gmb09 | limiter: ceiling, true peak, latency | PASS | 0.094 |
| gmb09 | gate, de-esser, transient, clipper | PASS | 0.012 |
| gmb09 | reverb decay and delay timing | PASS | 0.005 |
| gmb09 | saturation harmonics and oversampling alias suppression | PASS | 0.063 |
| gmb09 | modulation, stereo and pitch | PASS | 0.016 |
| gmb09 | VocalTune corrects a sharp note in realtime | PASS | 0.037 |
| gmb09 | noise cleaner improves SNR, analyzer, dynamic space | PASS | 0.107 |
| gmb09 | BS.1770 loudness, gating, true peak | PASS | 1.046 |
| gmb10 | mix analysis finds masking, collisions, phase, headroom, stereo | PASS | 0.146 |
| gmb10 | engine capture and suggested Dynamic Space fix work in the mix | PASS | 0.065 |
| gmb10 | what-if A/B, commit as one undo step, discard | PASS | 0.043 |
| gmb10 | producer memory: learn, hints, reset, global file | PASS | 0.063 |
| gmb11 | FLAC encoder is lossless (decoded by an independent decoder) | PASS | 0.031 |
| gmb11 | dither: TPDF statistics and noise shaping | PASS | 0.010 |
| gmb11 | export mixdown WAV/FLAC, SRC, normalisation, selection | PASS | 0.315 |
| gmb11 | export tails, plugin delay compensation, stems, instrumental | PASS | 0.800 |
| gmb11 | master assistant reaches targets without extreme limiting | PASS | 6.892 |
| gmb11 | reference comparison and master chain command | PASS | 0.104 |
| gmb12 | energy map and section suggestions | PASS | 1.219 |
| gmb12 | vocal DNA learns the own voice and flags deviations | PASS | 1.004 |
| gmb12 | project assistant findings | PASS | 0.000 |
| gmb12 | intelligence commands through the engine | PASS | 1.430 |
| integration | definition of done: full production workflow survives save, close and reopen | PASS | 6.172 |
| parallel | multi-core mixing is bit-identical to single-threaded | PASS | 2.691 |
| parallel | graph levels respect routing and sidechains | PASS | 0.065 |
| parallel | worker threads start and stop cleanly while graphs change | PASS | 0.135 |
| plugins | scanner: CLAP detection, categories, database views, rescan, duplicates | PASS | 0.024 |
| plugins | scanner: crash and hang are quarantined, VST3 detected, wrong architecture rejected | PASS | 1.621 |
| plugins | sandboxed CLAP effect processes audio in a separate process, state round-trips | PASS | 0.043 |
| plugins | sandboxed CLAP instrument plays MIDI notes | PASS | 0.016 |
| plugins | plugin crash during playback: PLUGIN CRASHED, project continues, restart recovers | PASS | 0.037 |
| plugins | hung plugin host is detected by timeout and the audio continues | PASS | 0.516 |
| plugins | missing plugin module is reported, project still builds | PASS | 0.010 |
