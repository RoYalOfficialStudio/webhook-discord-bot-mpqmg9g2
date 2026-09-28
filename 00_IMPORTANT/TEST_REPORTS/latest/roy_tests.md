# RoY Studio test report

56 tests, 0 failed, 531 checks

| Suite | Test | Result | Time (s) |
|---|---|---|---|
| gmb01 | tempo map conversions | PASS | 0.000 |
| gmb01 | transport play pause stop seek | PASS | 0.000 |
| gmb01 | transport loop splits blocks | PASS | 0.000 |
| gmb01 | count-in and pre-roll | PASS | 0.000 |
| gmb01 | metronome clicks on every beat | PASS | 0.001 |
| gmb01 | engine plays a clip bit-exact at unity | PASS | 0.003 |
| gmb01 | sample rates and buffer sizes give identical output | PASS | 0.033 |
| gmb01 | routing busses sends mute solo | PASS | 0.004 |
| gmb01 | pre-fader send ignores fader | PASS | 0.002 |
| gmb01 | plugin delay compensation aligns paths | PASS | 0.001 |
| gmb01 | automation drives channel gain | PASS | 0.004 |
| gmb01 | audio callback does not allocate | PASS | 0.008 |
| gmb01 | non-finite samples never reach the output | PASS | 0.000 |
| gmb01 | graph hot swap while playing | PASS | 0.002 |
| gmb01 | null audio device runs the engine | PASS | 0.042 |
| gmb02 | save close load verify roundtrip | PASS | 0.019 |
| gmb02 | unknown keys from newer versions are preserved | PASS | 0.004 |
| gmb02 | migration from v0 keeps a backup | PASS | 0.000 |
| gmb02 | numbered backups and retention | PASS | 0.007 |
| gmb02 | autosave never touches the project file | PASS | 0.002 |
| gmb02 | crash recovery: recover, last stable, discard | PASS | 0.007 |
| gmb02 | second instance cannot open a live project | PASS | 0.002 |
| gmb02 | commands with undo redo and macros | PASS | 0.002 |
| gmb02 | command palette search and shortcuts | PASS | 0.000 |
| gmb02 | routing command rejects feedback loops | PASS | 0.000 |
| gmb03 | split clip renders sample-identical | PASS | 0.015 |
| gmb03 | move, trim, slip and duplicate | PASS | 0.029 |
| gmb03 | locked clips and groups | PASS | 0.004 |
| gmb03 | fades and crossfade are smooth | PASS | 0.011 |
| gmb03 | clip gain, mute, reverse | PASS | 0.009 |
| gmb03 | time stretch keeps pitch, changes length | PASS | 0.041 |
| gmb03 | stretched clip plays through the engine | PASS | 0.027 |
| gmb03 | waveform cache levels, persistence, speed | PASS | 0.718 |
| gmb03 | snap, time formats, markers, sections, loop | PASS | 0.000 |
| gmb04 | record a take into its own file | PASS | 0.363 |
| gmb04 | recorded take plays back at the right position | PASS | 0.360 |
| gmb04 | loop recording creates one take per pass | PASS | 0.366 |
| gmb04 | punch in and out | PASS | 0.360 |
| gmb04 | existing files are never overwritten | PASS | 0.690 |
| gmb04 | comping selects ranges across takes | PASS | 0.000 |
| gmb04 | flatten comp keeps audio identical | PASS | 0.371 |
| gmb04 | input clipping warning and meter | PASS | 0.030 |
| gmb04 | never-lose: recover a performance without pressing record | PASS | 0.047 |
| gmb04 | never-lose memory is bounded | PASS | 0.391 |
| gmb04 | recording path does not allocate on the audio thread | PASS | 0.358 |
| gmb05 | keys and scales | PASS | 0.000 |
| gmb05 | wrong note blocker modes | PASS | 0.000 |
| gmb05 | AddNote command obeys the blocker and can be disabled | PASS | 0.002 |
| gmb05 | quantize humanize transpose | PASS | 0.000 |
| gmb05 | duplicate legato strum arpeggiate | PASS | 0.000 |
| gmb05 | chord detection | PASS | 0.001 |
| gmb05 | standard MIDI file roundtrip | PASS | 0.001 |
| gmb05 | ghost notes from other clips | PASS | 0.000 |
| gmb05 | synth renders MIDI with correct pitch | PASS | 0.005 |
| gmb05 | synth polyphony, voice stealing, no allocation | PASS | 0.010 |
| gmb05 | MIDI track plays through the engine | PASS | 0.021 |
