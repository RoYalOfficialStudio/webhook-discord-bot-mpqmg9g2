// LIVE MIDI INPUT: byte parser, engine MIDI thru (transport stopped), sustain pedal, target
// switching (all notes off), recording into a clip (one undo), queue overflow, offline renders
// unaffected, and the Linux raw-MIDI reader (a FIFO stands in for /dev/snd/midiC*D*).
#include "TestFramework.h"
#include "TestHelpers.h"

#include "audio/OfflineRender.h"
#include "commands/Commands.h"
#include "midi/MidiInput.h"

#include <fstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

// Drives the engine like the audio device callback does (realtime path; offline renders ignore live MIDI).
std::vector<std::vector<float>> play(AudioEngine& e, int64_t frames, int block = 256) {
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(frames)));
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    float* outs[2] = {l.data(), r.data()};
    for (int64_t pos = 0; pos < frames; pos += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, frames - pos));
        e.process(nullptr, 0, outs, 2, n);
        std::copy(l.begin(), l.begin() + n, out[0].begin() + pos);
        std::copy(r.begin(), r.begin() + n, out[1].begin() + pos);
    }
    return out;
}

struct Live {
    AudioEngine engine;
    ProjectRuntime rt{engine};
    Project p = makeNewProject("live", SR, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    std::unique_ptr<CommandContext> ctx;
    midi::MidiInputManager in{engine};
    std::string synth, synth2;
    Live() {
        engine.prepare(SR, 256);
        registerBuiltinProcessors();
        registerCoreCommands(reg);
        ctx = std::make_unique<CommandContext>(CommandContext{p, undo, &rt});
        REQUIRE(reg.execute(*ctx, "AddTrack", {{"type", "midi"}, {"name", "Keys"}, {"instrument", "roy.synth"}}));
        synth = ctx->result["id"];
        REQUIRE(reg.execute(*ctx, "AddTrack", {{"type", "midi"}, {"name", "Keys 2"}, {"instrument", "roy.synth"}}));
        synth2 = ctx->result["id"];
        REQUIRE(rt.rebuild(p));
        rt.setLiveMidiTrack(p, synth);
    }
    void send(std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> v(bytes);
        in.inject(v.data(), v.size());
    }
    float blockPeak(int blocks = 20) {
        auto out = play(engine, 256 * blocks, 256);
        return peak(out[0]);
    }
};
} // namespace

TEST_CASE("live-midi", "parser: running status, real-time bytes, SysEx, 1-byte messages") {
    midi::MidiParser parser;
    std::vector<std::array<int, 3>> got;
    auto emit = [&](uint8_t s, uint8_t a, uint8_t b) { got.push_back({s, a, b}); };
    const uint8_t stream[] = {0x90, 60, 100, 62, 90,     // running status: two note-ons
                              0xF8,                      // clock between messages
                              64, 0xFE, 80,              // running status note with active sensing inside
                              0xF0, 0x7E, 0x01, 0xF7,    // SysEx skipped (cancels running status)
                              0xC0, 5,                   // program change, 1 data byte
                              0xB0, 64, 127, 0x80, 60, 0};
    parser.feed(stream, sizeof(stream), emit);
    REQUIRE(got.size() == 6);
    CHECK((got[0] == std::array<int, 3>{0x90, 60, 100}));
    CHECK((got[1] == std::array<int, 3>{0x90, 62, 90}));
    CHECK((got[2] == std::array<int, 3>{0x90, 64, 80}));
    CHECK((got[3] == std::array<int, 3>{0xC0, 5, 0}));
    CHECK((got[4] == std::array<int, 3>{0xB0, 64, 127}));
    CHECK((got[5] == std::array<int, 3>{0x80, 60, 0}));
    // split across feed() calls
    got.clear();
    const uint8_t a[] = {0x91, 48}, b[] = {70};
    parser.feed(a, 2, emit);
    CHECK(got.empty());
    parser.feed(b, 1, emit);
    REQUIRE(got.size() == 1);
    CHECK(got[0][0] == 0x91);
    CHECK(midi::describeMessage(0x90, 60, 100).find("C4") != std::string::npos);
}

TEST_CASE("live-midi", "MIDI thru plays the target instrument with the transport stopped; switching releases notes") {
    Live l;
    CHECK(l.blockPeak() < 1e-6f);
    l.send({0x90, 60, 110});
    CHECK(l.blockPeak() > 0.01f);
    CHECK(!l.engine.transport().isPlaying());
    l.send({0x80, 60, 0});
    l.blockPeak(200); // release tail
    CHECK(l.blockPeak() < 1e-3f);
    // a held note on track 1, then the target moves to track 2: track 1 must go silent
    l.send({0x90, 64, 110});
    l.blockPeak(4);
    l.rt.setLiveMidiTrack(l.p, l.synth2);
    l.blockPeak(200);
    CHECK(l.blockPeak() < 1e-3f);
    l.send({0x90, 67, 110}); // now plays on track 2
    CHECK(l.blockPeak() > 0.01f);
    CHECK(l.engine.liveMidiReceived() >= 4);
    // a track without an instrument is no target
    Track& audio = addTrack(l.p, TrackType::Audio, "Vox");
    REQUIRE(l.rt.rebuild(l.p));
    l.rt.setLiveMidiTrack(l.p, audio.id);
    CHECK(l.engine.liveMidiTarget() == -1);
}

TEST_CASE("live-midi", "sustain pedal holds notes until it is released") {
    Live l;
    l.send({0xB0, 64, 127, 0x90, 60, 110});
    l.blockPeak(4);
    l.send({0x80, 60, 0}); // key up, pedal down -> keeps sounding
    l.blockPeak(100);
    CHECK(l.blockPeak() > 0.01f);
    l.send({0xB0, 64, 0}); // pedal up -> released
    l.blockPeak(200);
    CHECK(l.blockPeak() < 1e-3f);
}

TEST_CASE("live-midi", "recording: notes land on the timeline as played, one undo step, pedal extends") {
    Live l;
    l.engine.transport().play();
    l.engine.setLiveMidiRecording(true);
    auto framesFor = [&](double beats) { return static_cast<int64_t>(std::llround(beats * 0.5 * SR)); }; // 120 bpm: 0.5 s per beat
    play(l.engine, framesFor(1.0), 256);          // beat 1: C4 on
    l.send({0x90, 60, 100});
    play(l.engine, framesFor(1.0), 256);          // beat 2: C4 off, E4 on (running status)
    l.send({0x80, 60, 0, 0x90, 64, 90});
    play(l.engine, framesFor(0.5), 256);          // beat 2.5: pedal down, E4 off (held)
    l.send({0xB0, 64, 127, 0x80, 64, 0});
    play(l.engine, framesFor(1.5), 256);          // beat 4: pedal up -> E4 ends
    l.send({0xB0, 64, 0});
    play(l.engine, framesFor(0.5), 256);          // beat 4.5: G4 on, never released
    l.send({0x90, 67, 80});
    play(l.engine, framesFor(1.0), 256);
    l.engine.setLiveMidiRecording(false);
    const int64_t end = l.engine.transport().position();
    std::vector<RecordedMidi> rec;
    l.engine.drainRecordedMidi(rec);
    REQUIRE(rec.size() >= 7);
    json ev = json::array();
    for (auto& r : rec) ev.push_back({r.timeline, r.msg.status, r.msg.data1, r.msg.data2});
    const size_t undoBefore = l.undo.undoCount();
    REQUIRE(l.reg.execute(*l.ctx, "AddMidiRecording", {{"trackId", l.synth}, {"events", ev}, {"sampleRate", SR}, {"endTimeline", end}}));
    CHECK(l.undo.undoCount() == undoBefore + 1);
    const MidiClip& c = l.p.findTrack(l.synth)->midiClips.back();
    REQUIRE(c.notes.size() == 3);
    const double tol = 256.0 / SR * 2.0 + 1e-9; // one audio block (in beats at 120 bpm)
    CHECK_NEAR(c.startBeat, 0.0, 1e-9);
    CHECK_NEAR(c.notes[0].startBeat, 1.0, tol);
    CHECK_NEAR(c.notes[0].lengthBeats, 1.0, tol);
    CHECK(c.notes[0].velocity == 100);
    CHECK_NEAR(c.notes[1].startBeat, 2.0, tol);
    CHECK_NEAR(c.notes[1].lengthBeats, 2.0, tol);        // held by the pedal until beat 4
    CHECK_NEAR(c.notes[2].startBeat, 4.5, tol);
    CHECK_NEAR(c.notes[2].endBeat(), 5.5, tol);          // closed at the stop position
    CHECK_NEAR(c.lengthBeats, 8.0, 1e-9);                // whole bars
    REQUIRE(l.undo.undo());
    CHECK(l.p.findTrack(l.synth)->midiClips.empty());
    // nothing played -> clear error, nothing added
    CHECK(!l.reg.execute(*l.ctx, "AddMidiRecording", {{"trackId", l.synth}, {"events", json::array({{0, 0xB0, 1, 5}})}}));
    // not recording / transport stopped: nothing is captured
    l.engine.transport().stop();
    l.engine.setLiveMidiRecording(true);
    l.send({0x90, 60, 100});
    play(l.engine, 256 * 4, 256);
    rec.clear();
    l.engine.drainRecordedMidi(rec);
    CHECK(rec.empty());
}

TEST_CASE("live-midi", "flooding the input never blocks or crashes; offline renders ignore live MIDI") {
    Live l;
    std::vector<uint8_t> flood;
    for (int i = 0; i < 5000; ++i) {
        flood.push_back(0x90);
        flood.push_back(static_cast<uint8_t>(40 + i % 40));
        flood.push_back(i % 2 ? 0 : 100);
    }
    l.in.inject(flood.data(), flood.size());
    CHECK(l.engine.liveMidiDropped() > 0);                 // queue bounded (1024), extra messages dropped and counted
    auto out = play(l.engine, 256 * 50, 256);
    CHECK(allFinite(out[0]));
    // offline render (export): a note played meanwhile must not end up in the file
    l.send({0xB0, 123, 0});
    play(l.engine, 256 * 200, 256);
    AudioEngine ref;
    ref.prepare(SR, 256);
    ProjectRuntime rrt(ref);
    REQUIRE(rrt.rebuild(l.p));
    OfflineRenderOptions o;
    o.numFrames = 4800;
    std::string err;
    auto want = renderOffline(ref, o, &err);
    l.send({0x90, 72, 127});
    auto got = renderOffline(l.engine, o, &err);
    REQUIRE(!got.empty());
    REQUIRE(!want.empty());
    CHECK(got[0] == want[0]);
}

#ifndef _WIN32
namespace {
int64_t g_fakeNow = 0;
int64_t fakeClock() { return g_fakeNow; }
} // namespace

TEST_CASE("live-midi", "timestamps: sample-accurate placement with constant latency, late messages wait, chunked callbacks") {
    AudioEngine e;
    e.prepare(SR, 240); // 240 frames @ 48 kHz = exactly 5 ms per callback
    e.setLiveMidiClock(&fakeClock);
    e.setLiveMidiRecording(true);
    e.transport().play();
    const int64_t T0 = 1'000'000'000, period = 5'000'000;
    std::vector<float> l(1024), r(1024);
    float* outs[2] = {l.data(), r.data()};
    auto callback = [&](int k, int frames = 240) {
        g_fakeNow = T0 + k * period;
        e.process(nullptr, 0, outs, 2, frames);
    };
    // 1) notes played every 1000 samples (20.833 ms) at arbitrary moments between callbacks
    callback(0);
    std::vector<int64_t> sent;
    int k = 0;
    for (int n = 0; n < 12; ++n) {
        const int64_t t = T0 + 7'777'777 + static_cast<int64_t>(n) * 20'833'333; // not aligned to callbacks
        while (T0 + (k + 1) * period <= t) callback(++k);                        // run callbacks up to the arrival
        e.pushLiveMidi(LiveMidiMessage{0x90, static_cast<uint8_t>(60 + n), 100, 0, t});
        sent.push_back(t);
    }
    for (int i = 0; i < 3; ++i) callback(++k);
    std::vector<RecordedMidi> rec;
    e.drainRecordedMidi(rec);
    REQUIRE(rec.size() == sent.size());
    for (size_t i = 0; i < rec.size(); ++i) {
        // arrival time in samples + exactly one callback (240) of latency, +-1 sample rounding
        const double expect = static_cast<double>(sent[i] - T0) * SR / 1e9 + 240.0;
        CHECK_NEAR(static_cast<double>(rec[i].timeline), expect, 1.01);
    }
    // spacing is kept (block-start quantisation would give multiples of 240)
    for (size_t i = 1; i < rec.size(); ++i) CHECK_NEAR(static_cast<double>(rec[i].timeline - rec[i - 1].timeline), 1000.0, 1.01);

    // 2) a message stamped after the current callback began waits for the next callback
    const int64_t late = T0 + (k + 1) * period + 100; // arrives while callback k+1 runs
    e.pushLiveMidi(LiveMidiMessage{0x90, 90, 100, 0, late});
    callback(++k);
    rec.clear();
    e.drainRecordedMidi(rec);
    CHECK(rec.empty());
    callback(++k);
    e.drainRecordedMidi(rec);
    REQUIRE(rec.size() == 1);
    CHECK_NEAR(static_cast<double>(rec[0].timeline), static_cast<double>(late - T0) * SR / 1e9 + 240.0, 1.01);

    // 3) unstamped messages (on-screen keyboard, tests) start the next block as before
    e.pushLiveMidi(LiveMidiMessage{0x90, 91, 100, 0, 0});
    const int64_t before = e.transport().position();
    callback(++k);
    rec.clear();
    e.drainRecordedMidi(rec);
    REQUIRE(rec.size() == 1);
    CHECK(rec[0].timeline == before);

    // 4) one 960-frame callback is processed in 4 chunks of 240: an event due at 700 lands in chunk 3
    e.prepare(SR, 240);
    e.transport().seek(0);
    e.transport().play();
    g_fakeNow = T0 + 100 * period;
    e.process(nullptr, 0, outs, 2, 960);
    const int64_t prevStart = g_fakeNow, longPeriod = 20'000'000; // 960 frames = 20 ms
    const int64_t t = prevStart + longPeriod * 700 / 960;
    e.pushLiveMidi(LiveMidiMessage{0x90, 92, 100, 0, t});
    rec.clear();
    g_fakeNow = prevStart + longPeriod;
    const int64_t cbTimeline = e.transport().position();
    e.process(nullptr, 0, outs, 2, 960);
    e.drainRecordedMidi(rec);
    REQUIRE(rec.size() == 1);
    CHECK_NEAR(static_cast<double>(rec[0].timeline - cbTimeline), 700.0, 1.01);
    e.setLiveMidiClock(nullptr);
}

TEST_CASE("live-midi", "raw MIDI device reader (FIFO in place of /dev/snd/midiC*D*)") {
    Live l;
    const auto dir = tempDir("live_midi_fifo");
    const std::string fifo = (dir / "midiC9D0").string();
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    std::string err;
    REQUIRE_MSG_OK(l.in.open(fifo, &err), err);
    CHECK(l.in.isOpen(fifo));
    const int w = ::open(fifo.c_str(), O_WRONLY);
    REQUIRE(w >= 0);
    const uint8_t bytes[] = {0x90, 60, 100, 0xF8, 0x80, 60, 0};
    REQUIRE(::write(w, bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes)));
    for (int i = 0; i < 200 && l.in.messageCount() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(l.in.messageCount() == 2);
    CHECK(l.in.lastMessageText().find("Note Off") != std::string::npos);
    ::close(w);                                             // writer gone (like unplugging): reader keeps running
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    l.in.close(fifo);
    CHECK(!l.in.isOpen(fifo));
    CHECK(!l.in.open((dir / "missing").string(), &err));
    CHECK(!err.empty());
}

TEST_CASE("live-midi", "hot-plug: rescan opens new inputs, closes unplugged ones, respects switched-off inputs") {
    Live l;
    const auto dir = tempDir("live_midi_hotplug");
    l.in.setDeviceDirectory(dir.string());
    CHECK(l.in.devices().empty());
    auto r = l.in.rescan();
    CHECK(r.added.empty());
    CHECK(r.removed.empty());
    // plug in two devices (+ an unrelated file that is not a MIDI device)
    const std::string a = (dir / "midiC5D0").string(), b = (dir / "midiC6D0").string();
    REQUIRE(::mkfifo(a.c_str(), 0600) == 0);
    REQUIRE(::mkfifo(b.c_str(), 0600) == 0);
    { std::ofstream((dir / "pcmC0D0p").string()) << "x"; }
    r = l.in.rescan();
    CHECK(r.added.size() == 2);
    CHECK(l.in.isOpen(a));
    CHECK(l.in.isOpen(b));
    CHECK(l.in.rescan().added.empty()); // nothing new
    // the new input works right away
    const int w = ::open(a.c_str(), O_WRONLY);
    REQUIRE(w >= 0);
    const uint8_t on[] = {0x90, 64, 90};
    REQUIRE(::write(w, on, sizeof(on)) == 3);
    for (int i = 0; i < 200 && l.in.messageCount() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(l.in.messageCount() == 1);
    ::close(w);
    // unplug one: its reader is stopped and the input closed
    ::unlink(b.c_str());
    r = l.in.rescan();
    REQUIRE(r.removed.size() == 1);
    CHECK(r.removed[0] == b);
    CHECK(!l.in.isOpen(b));
    CHECK(l.in.isOpen(a));
    // an input the user switched off is not reopened; re-plugging b opens it again
    l.in.close(a);
    REQUIRE(::mkfifo(b.c_str(), 0600) == 0);
    r = l.in.rescan({a});
    CHECK(r.added.size() == 1);
    CHECK(!l.in.isOpen(a));
    CHECK(l.in.isOpen(b));
    // a device that cannot be opened is reported, not retried silently into a crash
    const std::string c = (dir / "midiC7D0").string();
    { std::ofstream(c) << ""; }
    ::chmod(c.c_str(), 0);
    r = l.in.rescan({a});
    if (::geteuid() != 0) CHECK(r.failed.size() == 1); // root can open anything
    l.in.closeAll();
    CHECK(l.in.openDevices().empty());
}
#endif
