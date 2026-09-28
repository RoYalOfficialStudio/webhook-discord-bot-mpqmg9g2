# RoY Studio test report

122 tests, 0 failed, 99105 checks

| Suite | Test | Result | Time (s) |
|---|---|---|---|
| gmb01 | tempo map conversions | PASS | 0.000 |
| gmb01 | transport play pause stop seek | PASS | 0.000 |
| gmb01 | transport loop splits blocks | PASS | 0.000 |
| gmb01 | count-in and pre-roll | PASS | 0.000 |
| gmb01 | metronome clicks on every beat | PASS | 0.001 |
| gmb01 | engine plays a clip bit-exact at unity | PASS | 0.003 |
| gmb01 | sample rates and buffer sizes give identical output | PASS | 0.034 |
| gmb01 | routing busses sends mute solo | PASS | 0.004 |
| gmb01 | pre-fader send ignores fader | PASS | 0.002 |
| gmb01 | plugin delay compensation aligns paths | PASS | 0.001 |
| gmb01 | automation drives channel gain | PASS | 0.004 |
| gmb01 | audio callback does not allocate | PASS | 0.008 |
| gmb01 | non-finite samples never reach the output | PASS | 0.000 |
| gmb01 | graph hot swap while playing | PASS | 0.002 |
| gmb01 | null audio device runs the engine | PASS | 0.042 |
| gmb02 | save close load verify roundtrip | PASS | 0.021 |
| gmb02 | unknown keys from newer versions are preserved | PASS | 0.004 |
| gmb02 | migration from v0 keeps a backup | PASS | 0.000 |
| gmb02 | numbered backups and retention | PASS | 0.006 |
| gmb02 | autosave never touches the project file | PASS | 0.002 |
| gmb02 | crash recovery: recover, last stable, discard | PASS | 0.006 |
| gmb02 | second instance cannot open a live project | PASS | 0.001 |
| gmb02 | commands with undo redo and macros | PASS | 0.001 |
| gmb02 | command palette search and shortcuts | PASS | 0.000 |
| gmb02 | routing command rejects feedback loops | PASS | 0.000 |
| gmb03 | split clip renders sample-identical | PASS | 0.016 |
| gmb03 | move, trim, slip and duplicate | PASS | 0.029 |
| gmb03 | locked clips and groups | PASS | 0.004 |
| gmb03 | fades and crossfade are smooth | PASS | 0.010 |
| gmb03 | clip gain, mute, reverse | PASS | 0.009 |
| gmb03 | time stretch keeps pitch, changes length | PASS | 0.041 |
| gmb03 | stretched clip plays through the engine | PASS | 0.028 |
| gmb03 | waveform cache levels, persistence, speed | PASS | 0.669 |
| gmb03 | snap, time formats, markers, sections, loop | PASS | 0.000 |
| gmb04 | record a take into its own file | PASS | 0.359 |
| gmb04 | recorded take plays back at the right position | PASS | 0.364 |
| gmb04 | loop recording creates one take per pass | PASS | 0.363 |
| gmb04 | punch in and out | PASS | 0.365 |
| gmb04 | existing files are never overwritten | PASS | 0.689 |
| gmb04 | comping selects ranges across takes | PASS | 0.000 |
| gmb04 | flatten comp keeps audio identical | PASS | 0.369 |
| gmb04 | input clipping warning and meter | PASS | 0.029 |
| gmb04 | never-lose: recover a performance without pressing record | PASS | 0.049 |
| gmb04 | never-lose memory is bounded | PASS | 0.401 |
| gmb04 | recording path does not allocate on the audio thread | PASS | 0.359 |
| gmb05 | keys and scales | PASS | 0.000 |
| gmb05 | wrong note blocker modes | PASS | 0.000 |
| gmb05 | AddNote command obeys the blocker and can be disabled | PASS | 0.002 |
| gmb05 | quantize humanize transpose | PASS | 0.000 |
| gmb05 | duplicate legato strum arpeggiate | PASS | 0.000 |
| gmb05 | chord detection | PASS | 0.001 |
| gmb05 | standard MIDI file roundtrip | PASS | 0.001 |
| gmb05 | ghost notes from other clips | PASS | 0.000 |
| gmb05 | synth renders MIDI with correct pitch | PASS | 0.004 |
| gmb05 | synth polyphony, voice stealing, no allocation | PASS | 0.009 |
| gmb05 | MIDI track plays through the engine | PASS | 0.019 |
| gmb06 | Pitch Guardian command is non-destructive and undoable | PASS | 0.246 |
| gmb06 | Vocal Doctor command returns executable fixes | PASS | 0.192 |
| gmb06 | breath reduction writes undoable automation | PASS | 0.267 |
| gmb06 | Double Magnet command aligns a double track | PASS | 0.228 |
| gmb06 | pitch detection accuracy | PASS | 0.238 |
| gmb06 | analysis distinguishes stable, vibrato, slide | PASS | 0.335 |
| gmb06 | ASSIST corrects a sharp note and leaves in-tune notes untouched | PASS | 0.556 |
| gmb06 | vibrato is preserved while the centre is corrected | PASS | 0.368 |
| gmb06 | OFF-KEY filter and ALLOW CHROMATIC decide targets | PASS | 0.123 |
| gmb06 | low confidence is not corrected aggressively | PASS | 0.124 |
| gmb06 | formant preservation | PASS | 0.270 |
| gmb06 | slide is preserved and transitions stay continuous | PASS | 0.255 |
| gmb06 | vocal doctor finds technical problems | PASS | 0.465 |
| gmb06 | vocal doctor detects breaths and plosives | PASS | 0.251 |
| gmb06 | vocal microscope inspects a region | PASS | 0.232 |
| gmb06 | double magnet tightens a sloppy double | PASS | 0.884 |
| gmb06 | ghost take compares timing and pitch | PASS | 0.238 |
| gmb06 | flow analyzer classifies early, on beat, late | PASS | 0.119 |
| gmb06 | onsets, bpm and key estimation | PASS | 0.108 |
| gmb07 | pattern length, duplicate, variation, swing | PASS | 0.000 |
| gmb07 | pattern expansion: probability, flam, roll, micro timing, mute/solo | PASS | 0.000 |
| gmb07 | drum voices have the expected spectra | PASS | 0.005 |
| gmb07 | drum sample pads play assigned samples | PASS | 0.001 |
| gmb07 | beat track plays a pattern through the engine | PASS | 0.032 |
| gmb07 | 808: pitch, glide/slide, key lock, saturation | PASS | 0.010 |
| gmb07 | instruments do not allocate on the audio thread | PASS | 0.007 |
| gmb07 | kick/808 collision analyzer | PASS | 0.031 |
| gmb08 | sampler key mapping, pitch, velocity layers | PASS | 0.003 |
| gmb08 | sampler loop, one-shot, reverse, choke | PASS | 0.000 |
| gmb08 | sample editing tools | PASS | 0.000 |
| gmb08 | transient slicing and SLICE TO PADS | PASS | 0.006 |
| gmb08 | root note, bpm and key detection | PASS | 0.836 |
| gmb08 | stem separation: exact reconstruction, measured quality | PASS | 0.249 |
| gmb08 | import, slice-to-pads and stems commands | PASS | 0.150 |
| gmb09 | every processor survives all test signals at all sample rates | PASS | 2.565 |
| gmb09 | RoY EQ matches its response curve | PASS | 0.006 |
| gmb09 | compressor static curve, makeup and sidechain | PASS | 0.007 |
| gmb09 | limiter: ceiling, true peak, latency | PASS | 0.083 |
| gmb09 | gate, de-esser, transient, clipper | PASS | 0.012 |
| gmb09 | reverb decay and delay timing | PASS | 0.004 |
| gmb09 | saturation harmonics and oversampling alias suppression | PASS | 0.061 |
| gmb09 | modulation, stereo and pitch | PASS | 0.016 |
| gmb09 | VocalTune corrects a sharp note in realtime | PASS | 0.041 |
| gmb09 | noise cleaner improves SNR, analyzer, dynamic space | PASS | 0.123 |
| gmb09 | BS.1770 loudness, gating, true peak | PASS | 1.001 |
| gmb10 | mix analysis finds masking, collisions, phase, headroom, stereo | PASS | 0.133 |
| gmb10 | engine capture and suggested Dynamic Space fix work in the mix | PASS | 0.058 |
| gmb10 | what-if A/B, commit as one undo step, discard | PASS | 0.039 |
| gmb10 | producer memory: learn, hints, reset, global file | PASS | 0.001 |
| gmb11 | FLAC encoder is lossless (decoded by an independent decoder) | PASS | 0.032 |
| gmb11 | dither: TPDF statistics and noise shaping | PASS | 0.010 |
| gmb11 | export mixdown WAV/FLAC, SRC, normalisation, selection | PASS | 0.359 |
| gmb11 | export tails, plugin delay compensation, stems, instrumental | PASS | 0.811 |
| gmb11 | master assistant reaches targets without extreme limiting | PASS | 6.868 |
| gmb11 | reference comparison and master chain command | PASS | 0.098 |
| gmb12 | energy map and section suggestions | PASS | 1.286 |
| gmb12 | vocal DNA learns the own voice and flags deviations | PASS | 0.918 |
| gmb12 | project assistant findings | PASS | 0.000 |
| gmb12 | intelligence commands through the engine | PASS | 1.452 |
| plugins | scanner: CLAP detection, categories, database views, rescan, duplicates | PASS | 0.016 |
| plugins | scanner: crash and hang are quarantined, VST3 detected, wrong architecture rejected | PASS | 1.620 |
| plugins | sandboxed CLAP effect processes audio in a separate process, state round-trips | PASS | 0.044 |
| plugins | sandboxed CLAP instrument plays MIDI notes | PASS | 0.015 |
| plugins | plugin crash during playback: PLUGIN CRASHED, project continues, restart recovers | PASS | 0.035 |
| plugins | hung plugin host is detected by timeout and the audio continues | PASS | 0.127 |
| plugins | missing plugin module is reported, project still builds | PASS | 0.007 |
