// GMB 05 - MIDI + piano roll + Wrong Note Blocker + SMF + synth.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "dsp/FFT.h"
#include "instruments/Synth.h"
#include "midi/MidiFile.h"
#include "midi/MidiOps.h"
#include "midi/Scale.h"

using namespace roy;
using namespace roytest;

namespace {
double dominantHz(const std::vector<float>& x, size_t from, int n, double sr) {
    auto mag = dsp::magnitudeSpectrum(x.data() + from, n);
    size_t best = 1;
    for (size_t i = 2; i + 1 < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    const double a = mag[best - 1], b = mag[best], c = mag[best + 1];
    return (static_cast<double>(best) + 0.5 * (a - c) / (a - 2 * b + c)) * sr / n;
}
MidiClip clipWith(std::vector<MidiNote> notes, double len = 8.0) {
    MidiClip c;
    c.id = files::newId();
    c.lengthBeats = len;
    c.notes = std::move(notes);
    return c;
}
} // namespace

TEST_CASE("gmb05", "keys and scales") {
    auto c = *parseKey("C Major");
    auto a = *parseKey("A Minor");
    auto fs = *parseKey("F# Minor");
    auto d = *parseKey("D Dorian");
    CHECK(c.contains(60) && c.contains(64) && !c.contains(61) && !c.contains(66));
    CHECK(a.pitchClasses() == std::vector<int>({9, 11, 0, 2, 4, 5, 7}));
    CHECK(fs.contains(66) && fs.contains(69) && !fs.contains(65));
    CHECK(d.contains(71) && !d.contains(70)); // B natural in D Dorian
    CHECK(fs.name() == "F# Minor");
    CHECK(parseKey("Bb major")->root == 10);
    CHECK(!parseKey("H Major"));
    CHECK(c.nearest(61) == 60);
    CHECK(c.nearest(61, true) == 62);
    CHECK_NEAR(c.nearestPitch(61.4), 62.0, 1e-9);
    CHECK(noteName(60) == "C4");
    CHECK(noteName(69) == "A4");
}

TEST_CASE("gmb05", "wrong note blocker modes") {
    const Key c{0, ScaleType::Major};
    auto off = applyWrongNoteBlocker(c, WrongNoteMode::Off, 61);
    CHECK(off.allowed && off.note == 61 && off.outOfScale);
    auto hl = applyWrongNoteBlocker(c, WrongNoteMode::Highlight, 61);
    CHECK(hl.allowed && hl.note == 61 && hl.outOfScale);
    auto sn = applyWrongNoteBlocker(c, WrongNoteMode::Snap, 61);
    CHECK(sn.allowed && sn.note == 60);
    auto bl = applyWrongNoteBlocker(c, WrongNoteMode::Block, 61);
    CHECK(!bl.allowed);
    auto ok = applyWrongNoteBlocker(c, WrongNoteMode::Block, 62);
    CHECK(ok.allowed && !ok.outOfScale);
}

TEST_CASE("gmb05", "AddNote command obeys the blocker and can be disabled") {
    Project p = makeNewProject("m");
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo};
    REQUIRE(reg.execute(ctx, "AddTrack", {{"type", "midi"}, {"name", "Keys"}}));
    const std::string tid = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "AddMidiClip", {{"trackId", tid}, {"lengthBeats", 4.0}}));
    const std::string cid = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "SetKey", {{"key", "C Major"}}));
    REQUIRE(reg.execute(ctx, "SetWrongNoteMode", {{"mode", "block"}}));
    CHECK(!reg.execute(ctx, "AddNote", {{"clipId", cid}, {"pitch", 66}}));
    CHECK(ctx.error.find("not in C Major") != std::string::npos);
    REQUIRE(reg.execute(ctx, "AddNote", {{"clipId", cid}, {"pitch", 67}}));
    REQUIRE(reg.execute(ctx, "SetWrongNoteMode", {{"mode", "snap"}}));
    REQUIRE(reg.execute(ctx, "AddNote", {{"clipId", cid}, {"pitch", 66}, {"startBeat", 1.0}}));
    CHECK(ctx.result["pitch"] == 65);
    REQUIRE(reg.execute(ctx, "SetWrongNoteMode", {{"mode", "off"}}));
    REQUIRE(reg.execute(ctx, "AddNote", {{"clipId", cid}, {"pitch", 66}, {"startBeat", 2.0}}));
    CHECK(ctx.result["outOfKey"] == true);
    CHECK(p.findMidiClip(cid)->notes.size() == 3);
    REQUIRE(undo.undo());
    CHECK(p.findMidiClip(cid)->notes.size() == 2);
}

TEST_CASE("gmb05", "quantize humanize transpose") {
    auto c = clipWith({{60, 100, 0.07, 0.4}, {62, 100, 0.52, 0.2}, {64, 100, 1.23, 0.5}});
    auto q = c;
    midi::quantize(q, {}, 0.25);
    CHECK_NEAR(q.notes[0].startBeat, 0.0, 1e-12);
    CHECK_NEAR(q.notes[1].startBeat, 0.5, 1e-12);
    CHECK_NEAR(q.notes[2].startBeat, 1.25, 1e-12);
    CHECK_NEAR(q.notes[0].endBeat(), 0.47, 1e-12); // ends kept
    auto half = c;
    midi::quantize(half, {}, 0.25, 0.5);
    CHECK_NEAR(half.notes[0].startBeat, 0.035, 1e-12);
    auto h1 = q, h2 = q;
    midi::humanize(h1, {}, 0.02, 10, 42);
    midi::humanize(h2, {}, 0.02, 10, 42);
    CHECK(h1.notes[1].startBeat == h2.notes[1].startBeat); // deterministic
    CHECK(std::fabs(h1.notes[1].startBeat - 0.5) <= 0.02 + 1e-12);
    auto t = q;
    const Key cmaj{0, ScaleType::Major};
    midi::transpose(t, {}, 2, &cmaj); // diatonic: C->E, D->F, E->G
    CHECK(t.notes[0].pitch == 64);
    CHECK(t.notes[1].pitch == 65);
    CHECK(t.notes[2].pitch == 67);
    midi::transpose(t, {0}, -12);
    CHECK(t.notes[0].pitch == 52);
}

TEST_CASE("gmb05", "duplicate legato strum arpeggiate") {
    auto c = clipWith({{60, 100, 0.0, 0.5}, {64, 100, 0.0, 0.5}, {67, 100, 0.0, 0.5}, {65, 90, 2.0, 0.5}}, 4.0);
    auto d = c;
    auto created = midi::duplicate(d, {0, 1, 2});
    CHECK(created.size() == 3);
    CHECK_NEAR(d.notes[4].startBeat, 1.0, 1e-12);
    auto l = c;
    midi::legato(l, {});
    CHECK_NEAR(l.notes[0].lengthBeats, 2.0, 1e-12);
    CHECK_NEAR(l.notes[2].lengthBeats, 2.0, 1e-12);
    auto s = c;
    midi::strum(s, {}, 0.05, true);
    CHECK_NEAR(s.notes[0].startBeat, 0.0, 1e-12);
    CHECK_NEAR(s.notes[1].startBeat, 0.05, 1e-12);
    CHECK_NEAR(s.notes[2].startBeat, 0.10, 1e-12);
    CHECK_NEAR(s.notes[2].endBeat(), 0.5, 1e-12);
    auto a = clipWith({{60, 100, 0.0, 2.0}, {64, 100, 0.0, 2.0}, {67, 100, 0.0, 2.0}});
    midi::arpeggiate(a, {}, 0.25, midi::ArpMode::Up, 0.5);
    REQUIRE(a.notes.size() == 8);
    CHECK(a.notes[0].pitch == 60 && a.notes[1].pitch == 64 && a.notes[2].pitch == 67 && a.notes[3].pitch == 60);
    CHECK_NEAR(a.notes[3].startBeat, 0.75, 1e-12);
    CHECK_NEAR(a.notes[0].lengthBeats, 0.125, 1e-12);
}

TEST_CASE("gmb05", "chord detection") {
    CHECK(midi::detectChord({60, 64, 67}).name == "C");
    CHECK(midi::detectChord({57, 60, 64, 67}).name == "Am7");
    CHECK(midi::detectChord({59, 62, 67}).name == "G/B");
    CHECK(midi::detectChord({54, 57, 61}).name == "F#m");
    CHECK(midi::detectChord({62, 65, 69, 72}).name == "Dm7");
    CHECK(midi::detectChord({60, 65, 67}).name == "Csus4");
    CHECK(midi::detectChord({60}).root == -1);
    auto clip = clipWith({{60, 100, 0.0, 2.0}, {64, 100, 0.0, 2.0}, {67, 100, 0.0, 2.0},
                          {57, 100, 2.0, 2.0}, {60, 100, 2.0, 2.0}, {64, 100, 2.0, 2.0}}, 4.0);
    auto chords = midi::detectChords(clip, 0.5);
    REQUIRE(chords.size() == 2);
    CHECK(chords[0].chord.name == "C");
    CHECK(chords[1].chord.name == "Am");
    CHECK_NEAR(chords[1].beat, 2.0, 1e-12);
}

TEST_CASE("gmb05", "standard MIDI file roundtrip") {
    midi::SmfData d;
    d.tempo = TempoMap(97.0, 3, 4);
    d.tempo.addTempoEvent(12.0, 140.0);
    midi::SmfTrack t;
    t.name = "Lead";
    t.notes = {{60, 100, 0.0, 1.0}, {67, 64, 1.5, 0.25}, {72, 127, 3.0, 2.0, 2}};
    d.tracks.push_back(t);
    auto bytes = midi::buildMidi(d);
    midi::SmfData back;
    std::string err;
    REQUIRE(midi::parseMidi(bytes, back, &err));
    REQUIRE(back.tracks.size() == 1);
    CHECK(back.tracks[0].name == "Lead");
    REQUIRE(back.tracks[0].notes.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(back.tracks[0].notes[i].pitch == t.notes[i].pitch);
        CHECK(back.tracks[0].notes[i].velocity == t.notes[i].velocity);
        CHECK_NEAR(back.tracks[0].notes[i].startBeat, t.notes[i].startBeat, 1e-9);
        CHECK_NEAR(back.tracks[0].notes[i].lengthBeats, t.notes[i].lengthBeats, 1e-9);
    }
    CHECK(back.tracks[0].notes[2].channel == 2);
    CHECK_NEAR(back.tempo.tempoAt(0), 97.0, 0.01);
    CHECK_NEAR(back.tempo.tempoAt(13.0), 140.0, 0.01);
    CHECK(back.tempo.timeSignatures()[0].numerator == 3);
    // file IO + import into a project
    auto dir = tempDir("smf");
    REQUIRE(midi::writeMidiFile(dir / "a.mid", d));
    CHECK(!midi::writeMidiFile(dir / "a.mid", d)); // never overwrite
    midi::SmfData f;
    REQUIRE(midi::readMidiFile(dir / "a.mid", f));
    Project p = makeNewProject("imp");
    auto ids = midi::importSmfIntoProject(p, f, 4.0, true);
    REQUIRE(ids.size() == 1);
    CHECK(p.findTrack(ids[0])->midiClips[0].notes.size() == 3);
    CHECK_NEAR(p.tempo.tempoAt(0), 97.0, 0.01);
    CHECK(!midi::parseMidi({'M', 'T', 'h', 'd'}, f, &err));
}

TEST_CASE("gmb05", "ghost notes from other clips") {
    Project p = makeNewProject("g");
    const std::string a = addTrack(p, TrackType::Midi, "A").id;
    const std::string b = addTrack(p, TrackType::Midi, "B").id;
    auto c1 = clipWith({{60, 100, 0.0, 1.0}}, 4.0);
    c1.startBeat = 4.0;
    auto c2 = clipWith({{48, 100, 1.0, 1.0}, {50, 100, 3.0, 1.0}}, 4.0);
    c2.startBeat = 2.0;
    p.findTrack(a)->midiClips.push_back(c1);
    p.findTrack(b)->midiClips.push_back(c2);
    auto g = midi::ghostNotes(p, c1.id);
    REQUIRE(g.size() == 1);
    CHECK(g[0].pitch == 50);
    CHECK_NEAR(g[0].startBeat, 1.0, 1e-12);
}

TEST_CASE("gmb05", "synth renders MIDI with correct pitch") {
    RoySynth s;
    s.setParam("osc1Wave", 3); // sine
    s.setParam("oscMix", 0.0f);
    s.setParam("cutoff", 20000.0f);
    s.setParam("filterEnv", 0.0f);
    s.prepare(48000.0, 512);
    AudioBuffer buf(2, 512);
    std::vector<float> out;
    NoteEvent on;
    on.type = NoteEvent::NoteOn;
    on.note = 69;
    on.velocity = 1.0f;
    on.offset = 100;
    NoteEvent off = on;
    off.type = NoteEvent::NoteOff;
    for (int b = 0; b < 120; ++b) {
        buf.clear();
        auto blk = buf.block();
        if (b == 0) s.process(blk, nullptr, &on, 1);
        else if (b == 60) s.process(blk, nullptr, &off, 1);
        else s.process(blk, nullptr, nullptr, 0);
        out.insert(out.end(), buf.channel(0), buf.channel(0) + 512);
    }
    CHECK(peak(out, 0, 100) == 0.0f); // sample accurate start
    CHECK(std::fabs(out[101]) > 0.0f);
    CHECK_NEAR(dominantHz(out, 8000, 16384, 48000.0), 440.0, 1.0);
    CHECK(peak(out, 60 * 512 + 48000 * 1) < 1e-4f); // released
    CHECK(allFinite(out));
    CHECK(s.activeVoices() == 0);
}

TEST_CASE("gmb05", "synth polyphony, voice stealing, no allocation") {
    RoySynth s;
    s.prepare(48000.0, 256);
    AudioBuffer buf(2, 256);
    std::vector<NoteEvent> evs;
    for (int i = 0; i < 20; ++i) {
        NoteEvent e;
        e.type = NoteEvent::NoteOn;
        e.note = static_cast<int16_t>(40 + i);
        e.offset = i;
        evs.push_back(e);
    }
    long allocs = 0;
    {
        AllocationCounter c;
        auto blk = buf.block();
        s.process(blk, nullptr, evs.data(), static_cast<int>(evs.size()));
        for (int k = 0; k < 50; ++k) s.process(blk, nullptr, nullptr, 0);
        allocs = c.count();
    }
    CHECK(allocs == 0);
    CHECK(s.activeVoices() == 16);
    CHECK(allFinite(std::vector<float>(buf.channel(0), buf.channel(0) + 256)));
}

TEST_CASE("gmb05", "MIDI track plays through the engine") {
    registerBuiltinProcessors();
    AudioEngine engine;
    engine.prepare(48000.0, 256);
    ProjectRuntime rt(engine);
    Project p = makeNewProject("midi", 48000.0, 120.0);
    const std::string tid = addTrack(p, TrackType::Midi, "Keys").id;
    auto clip = clipWith({{57, 110, 1.0, 1.0}, {64, 90, 2.0, 0.5}}, 4.0);
    p.findTrack(tid)->midiClips.push_back(clip);
    REQUIRE(rt.rebuild(p));
    auto out = render(engine, 48000 * 2);
    CHECK(peak(out[0], 0, 23990) < 1e-6f);   // silent before beat 1
    CHECK(peak(out[0], 24000, 36000) > 0.05f); // first note
    CHECK(allFinite(out[0]));
    // looping clip content repeats
    p.findTrack(tid)->midiClips[0].loopLengthBeats = 2.0;
    p.findTrack(tid)->midiClips[0].lengthBeats = 8.0;
    REQUIRE(rt.rebuild(p));
    auto looped = render(engine, 48000 * 4);
    CHECK(peak(looped[0], 72000, 84000) > 0.05f); // beat 3 = repeat of beat 1
}
