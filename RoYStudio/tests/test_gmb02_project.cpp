// GMB 02 - Project engine: format, save/load roundtrip, migration, backups,
// autosave, crash recovery, commands and undo/redo.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "arrange/ClipOps.h"
#include "commands/Commands.h"
#include "core/Process.h"
#include "project/ProjectIO.h"
#include "project/Session.h"

#include <fstream>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {

Project richProject() {
    Project p = makeNewProject("Rich Song", 48000.0, 140.0);
    p.author = "RoY";
    p.key = *parseKey("F# Minor");
    p.tempo.addTempoEvent(64.0, 150.0);
    p.tempo.addTimeSignature(8, 6, 8);
    p.settings.wrongNoteMode = WrongNoteMode::Block;
    p.settings.countInBars = 1;
    AudioAsset a;
    a.id = files::newId();
    a.path = "Audio/vox.wav";
    a.originalName = "vox.wav";
    a.frames = 480000;
    p.assets.push_back(a);
    const std::string vocalsBus = p.channels[1].id;
    std::string vId = addTrack(p, TrackType::Audio, "Lead Vocal", vocalsBus).id;
    Track* v = p.findTrack(vId);
    v->role = "vocal";
    AudioClip c;
    c.id = files::newId();
    c.assetId = a.id;
    c.startBeat = 4;
    c.lengthBeats = 8;
    c.fadeInBeats = 0.25;
    c.gainDb = -2.5f;
    c.stretch = 1.1;
    v->audioClips.push_back(c);
    v->takes.push_back({files::newId(), a.id, "Take 1", 0, 4.0, 8.0, "now", 0});
    v->comp.push_back({4.0, 6.0, v->takes[0].id});
    std::string mId = addTrack(p, TrackType::Midi, "Keys").id;
    MidiClip mc;
    mc.id = files::newId();
    mc.startBeat = 0;
    mc.lengthBeats = 8;
    mc.notes = {{60, 100, 0.0, 1.0}, {64, 90, 1.0, 0.5}, {67, 80, 2.0, 2.0, 1, false, true}};
    p.findTrack(mId)->midiClips.push_back(mc);
    Pattern pat = makeDefaultPattern("Beat A", 32);
    pat.rows[0].steps[0].on = true;
    pat.rows[0].steps[5].probability = 0.5f;
    pat.swing = 0.3f;
    p.patterns.push_back(pat);
    std::string bId = addTrack(p, TrackType::Beat, "Drums", p.channels[2].id).id;
    p.findTrack(bId)->patternClips.push_back({files::newId(), pat.id, 0.0, 16.0, false, false, 0x2E8B57});
    v = p.findTrack(vId); // re-lookup: adding tracks may reallocate the vector
    PluginSlot eq;
    eq.id = files::newId();
    eq.typeId = "roy.eq";
    eq.state = {{"params", {{"lowCut", 80.0}}}};
    p.findChannel(v->channelId)->inserts.push_back(eq);
    p.findChannel(v->channelId)->sends.push_back({files::newId(), p.channels[4].id, -8.0f, true, true});
    p.automation.push_back({files::newId(), v->channelId, "", "gain", true, {{0.0, -3.0f}, {8.0, 0.0f}}});
    p.markers.push_back({files::newId(), 16.0, "Hook", 0xFF0000});
    p.sections.push_back({files::newId(), "Verse 1", "verse", 0.0, 32.0, 0xFF8C00});
    p.loop = {true, 4.0, 12.0};
    p.presets = {{"vocalChain", "default"}};
    p.producerMemory = {{"vocalForward", true}};
    p.vocalSettings = {{v->id, {{"pitchGuardian", "assist"}}}};
    p.metadata = {{"genre", "trap"}};
    return p;
}

std::string canonical(const Project& p) {
    json j = projectToJson(p);
    j.erase("modifiedAt");
    return j.dump();
}

} // namespace

TEST_CASE("gmb02", "save close load verify roundtrip") {
    auto dir = tempDir("roundtrip");
    Project p = richProject();
    const std::string before = canonical(p);
    ProjectSession s;
    std::string err;
    REQUIRE(s.create(dir, p, &err));
    const fs::path file = s.file();
    CHECK(fs::exists(file));
    CHECK(fs::exists(s.folder() / "Audio"));
    CHECK(fs::exists(s.folder() / ".roy_session.lock"));
    s.close();
    CHECK(!fs::exists(file.parent_path() / ".roy_session.lock"));

    Project q;
    ProjectSession s2;
    REQUIRE(s2.open(file, q, OpenMode::Normal, &err));
    CHECK(canonical(q) == before);
    CHECK(q.tempo.tempoAt(70.0) == 150.0);
    CHECK(q.key.name() == "F# Minor");
    CHECK(q.settings.wrongNoteMode == WrongNoteMode::Block);
    REQUIRE(q.tracks.size() == 3);
    CHECK(q.tracks[1].midiClips[0].notes[2].slide);
    CHECK(q.patterns[0].rows[0].steps.size() == 32);
    CHECK(q.patterns[0].rows[0].steps[5].probability == 0.5f);
    // save again -> identical content, numbered backup created
    REQUIRE(s2.save(q, &err));
    auto backups = listBackups(file);
    CHECK(backups.size() == 1);
    Project r;
    REQUIRE(loadProject(file, r).ok);
    CHECK(canonical(r) == before);
}

TEST_CASE("gmb02", "unknown keys from newer versions are preserved") {
    auto dir = tempDir("forward");
    Project p = richProject();
    json j = projectToJson(p);
    j["formatVersion"] = kProjectFormatVersion + 5;
    j["futureFeature"] = {{"x", 1}, {"y", "keep me"}};
    const fs::path f = dir / "future.roy";
    { std::ofstream o(f); o << j.dump(); }
    Project q;
    auto lr = loadProject(f, q);
    REQUIRE(lr.ok);
    CHECK(lr.newerThanSupported);
    CHECK(q.unknown.contains("futureFeature"));
    CHECK(projectToJson(q)["futureFeature"]["y"] == "keep me");
    // Saving over a newer-format file is refused (no data destruction)
    auto sr = saveProject(q, f);
    CHECK(!sr.ok);
    auto after = files::readAll(f);
    CHECK(after && after->find("futureFeature") != std::string::npos);
}

TEST_CASE("gmb02", "migration from v0 keeps a backup") {
    auto dir = tempDir("migrate");
    fs::create_directories(dir / "BACKUPS");
    const fs::path f = dir / "old.roy";
    json old = {{"name", "Legacy"}, {"bpm", 93.0}, {"timeSignatureNum", 3}, {"timeSignatureDen", 4},
                {"mixer", json::array({{{"id", "m"}, {"name", "MASTER"}, {"kind", "master"}}})},
                {"tracks", json::array({{{"id", "t1"}, {"name", "Vox"}, {"channelId", "c1"},
                                         {"clips", json::array({{{"id", "k"}, {"assetId", "a"}, {"startBeat", 2.0}, {"lengthBeats", 1.0}}})}}})}};
    { std::ofstream o(f); o << old.dump(); }
    Project p;
    auto r = loadProject(f, p);
    REQUIRE(r.ok);
    CHECK(r.migrated);
    CHECK(fs::exists(r.migrationBackup));
    CHECK_NEAR(p.tempo.tempoAt(0), 93.0, 1e-9);
    CHECK(p.tempo.timeSignatures()[0].numerator == 3);
    REQUIRE(p.tracks.size() == 1);
    CHECK(p.tracks[0].audioClips.size() == 1);
    CHECK(p.findChannel("c1") != nullptr); // repaired missing channel
    CHECK(r.migrationLog.size() >= 3);
    // original untouched
    auto orig = files::readAll(r.migrationBackup);
    CHECK(orig && orig->find("\"bpm\"") != std::string::npos);
}

TEST_CASE("gmb02", "numbered backups and retention") {
    auto dir = tempDir("backups");
    Project p = makeNewProject("Backup Test");
    ProjectSession s;
    s.backupRetention = 3;
    REQUIRE(s.create(dir, p));
    for (int i = 0; i < 6; ++i) {
        p.name = "Backup Test " + std::to_string(i);
        REQUIRE(s.save(p));
    }
    auto b = listBackups(s.file());
    CHECK(b.size() == 3);
    CHECK(b.back().filename().string().find("_0006.roy") != std::string::npos);
    CHECK(b.front().filename().string().find("_0004.roy") != std::string::npos);
}

TEST_CASE("gmb02", "autosave never touches the project file") {
    auto dir = tempDir("autosave");
    Project p = makeNewProject("Auto");
    ProjectSession s;
    REQUIRE(s.create(dir, p));
    const auto original = *files::readAll(s.file());
    CHECK(!s.autosave(p)); // not dirty
    p.name = "Changed";
    s.markDirty();
    s.autosaveIntervalSec = 3600;
    CHECK(!s.autosave(p)); // interval not elapsed
    CHECK(s.autosave(p, true));
    CHECK(fs::exists(s.recoveryFolder() / "autosave.roy"));
    CHECK(*files::readAll(s.file()) == original);
    s.close();
}

TEST_CASE("gmb02", "crash recovery: recover, last stable, discard") {
    auto dir = tempDir("crash");
    Project p = makeNewProject("Crashy");
    fs::path file;
    {
        ProjectSession s;
        REQUIRE(s.create(dir, p));
        file = s.file();
        REQUIRE(s.save(p)); // creates a backup-able file
        addTrack(p, TrackType::Audio, "Unsaved Track");
        s.markDirty();
        REQUIRE(s.autosave(p, true));
        // simulate a crash: the lock stays behind with a dead pid
        json lock = {{"pid", 999999}, {"host", hostName()}};
        files::atomicWrite(file.parent_path() / ".roy_session.lock", lock.dump());
        // (no close)
        std::error_code ec;
        fs::last_write_time(file, fs::last_write_time(file) - std::chrono::seconds(10), ec);
    }
    json lock = {{"pid", 999999}, {"host", hostName()}};
    files::atomicWrite(file.parent_path() / ".roy_session.lock", lock.dump());
    auto info = ProjectSession::inspect(file);
    CHECK(info.lockFound);
    CHECK(info.crashed);
    CHECK(info.snapshotAvailable);
    CHECK(info.snapshotNewer);

    const auto originalText = *files::readAll(file);
    {
        Project r;
        ProjectSession s;
        REQUIRE(s.open(file, r, OpenMode::RecoverProject));
        CHECK(r.tracks.size() == 1);
        CHECK(s.openedFromRecovery());
        CHECK(s.isDirty());
        CHECK(*files::readAll(file) == originalText); // original never overwritten automatically
        s.close();
    }
    {
        // corrupt the main file -> OPEN LAST STABLE picks the newest readable backup
        REQUIRE(!listBackups(file).empty());
        files::atomicWrite(file, "{ this is not json");
        Project r;
        ProjectSession s;
        std::string err;
        CHECK(!s.open(file, r, OpenMode::Normal, &err));
        REQUIRE(s.open(file, r, OpenMode::OpenLastStable, &err));
        CHECK(r.name == "Crashy");
        s.close();
    }
    {
        Project r;
        ProjectSession s;
        files::atomicWrite(file, originalText);
        REQUIRE(s.open(file, r, OpenMode::DiscardRecovery));
        CHECK(!fs::exists(file.parent_path() / "RECOVERY" / "autosave.roy"));
        bool discardedKept = false;
        for (auto& e : fs::directory_iterator(file.parent_path() / "RECOVERY"))
            discardedKept |= e.path().filename().string().rfind("discarded_", 0) == 0;
        CHECK(discardedKept); // moved aside, not deleted
        s.close();
    }
}

TEST_CASE("gmb02", "second instance cannot open a live project") {
    auto dir = tempDir("lock");
    Project p = makeNewProject("Locked");
    ProjectSession s;
    REQUIRE(s.create(dir, p));
    // another live process holds it: a real child process that stays alive
    // (the plugin host scanning the hanging test plugin never finishes)
    ChildProcess other;
    REQUIRE(other.start(ROY_PLUGIN_HOST_EXE, {"--scan", (std::filesystem::path(ROY_TEST_PLUGIN_DIR) / "roy_test_hang.clap").string()}));
    json lock = {{"pid", other.pid()}, {"host", hostName()}};
    files::atomicWrite(s.folder() / ".roy_session.lock", lock.dump());
    Project q;
    ProjectSession s2;
    std::string err;
    CHECK(!s2.open(s.file(), q, OpenMode::Normal, &err));
    CHECK(err.find("already open") != std::string::npos);
    other.kill();
}

TEST_CASE("gmb02", "commands with undo redo and macros") {
    Project p = makeNewProject("Cmd");
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo};
    const std::string initial = canonical(p);

    REQUIRE(reg.execute(ctx, "AddTrack", {{"type", "audio"}, {"name", "Vox"}}));
    const std::string trackId = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "SetChannelGain", {{"trackId", trackId}, {"gainDb", -4.5}}));
    REQUIRE(reg.execute(ctx, "SetKey", {{"key", "A Minor"}}));
    const std::string afterAll = canonical(p);
    CHECK(undo.undoCount() == 3);
    CHECK(undo.undoName() == "Set Key");

    // a failing command leaves no trace
    CHECK(!reg.execute(ctx, "SetChannelGain", {{"trackId", "nope"}, {"gainDb", 1}}));
    CHECK(undo.undoCount() == 3);
    CHECK(canonical(p) == afterAll);

    REQUIRE(undo.undo());
    REQUIRE(undo.undo());
    CHECK(p.findChannel(p.findTrack(trackId)->channelId)->gainDb == 0.0f);
    REQUIRE(undo.undo());
    CHECK(canonical(p) == initial);
    CHECK(!undo.canUndo());
    REQUIRE(undo.redo());
    REQUIRE(undo.redo());
    REQUIRE(undo.redo());
    CHECK(canonical(p) == afterAll);

    // Macro = one undo step, all-or-nothing
    const size_t n = undo.undoCount();
    REQUIRE(reg.executeMacro(ctx, "Vocal setup", {{"AddBus", {{"name", "ADLIBS"}}},
                                                  {"AddMarker", {{"beat", 8.0}, {"name", "Drop"}}},
                                                  {"SetTempo", {{"bpm", 95.0}}}}));
    CHECK(undo.undoCount() == n + 1);
    CHECK(!reg.executeMacro(ctx, "Broken", {{"AddMarker", {{"beat", 1.0}}}, {"DeleteTrack", {{"trackId", "missing"}}}}));
    CHECK(p.markers.size() == 1); // failed macro rolled back its first step
    REQUIRE(undo.undo());
    CHECK(p.markers.empty());
    CHECK_NEAR(p.tempo.tempoAt(0), 120.0, 1e-9);
}

TEST_CASE("gmb02", "undo history: consecutive steps share states, byte budget drops the oldest steps") {
    Project p = makeNewProject("UndoMem");
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo};
    // a big model: a MIDI clip with many notes makes every snapshot large
    REQUIRE(reg.execute(ctx, "AddTrack", {{"type", "midi"}, {"name", "Keys"}}));
    const std::string track = ctx.result["id"];
    REQUIRE(reg.execute(ctx, "AddMidiClip", {{"trackId", track}, {"lengthBeats", 256.0}}));
    const std::string clip = ctx.result["id"];
    auto* c = p.findMidiClip(clip);
    for (int i = 0; i < 3000; ++i) {
        MidiNote n;
        n.pitch = 36 + i % 48;
        n.startBeat = i * 0.08;
        n.lengthBeats = 0.07;
        c->notes.push_back(n);
    }
    undo.clear();
    const size_t state = canonical(p).size();
    const std::string start = canonical(p);
    for (int i = 0; i < 20; ++i) REQUIRE(reg.execute(ctx, "SetChannelGain", {{"trackId", track}, {"gainDb", -0.5 * (i + 1)}}));
    CHECK(undo.undoCount() == 20);
    // 20 steps = 21 distinct states (not 40)
    CHECK(undo.memoryBytes() < state * 22);
    CHECK(undo.memoryBytes() > state * 19);
    // undo all / redo all still exact
    while (undo.undo()) {}
    CHECK(canonical(p) == start);
    while (undo.redo()) {}
    CHECK_NEAR(p.findChannel(p.findTrack(track)->channelId)->gainDb, -10.0f, 1e-6);
    // budget: about 5 states -> only the newest steps remain, and they still undo correctly
    undo.setMemoryLimit(state * 5 + state / 2);
    REQUIRE(reg.execute(ctx, "SetChannelGain", {{"trackId", track}, {"gainDb", -11.0}}));
    CHECK(undo.memoryBytes() <= state * 5 + state / 2);
    CHECK(undo.undoCount() >= 3);
    CHECK(undo.undoCount() <= 5);
    REQUIRE(undo.undo());
    CHECK_NEAR(p.findChannel(p.findTrack(track)->channelId)->gainDb, -10.0f, 1e-6);
    // the newest step always survives even if a single state is over budget
    undo.setMemoryLimit(1);
    REQUIRE(reg.execute(ctx, "SetChannelGain", {{"trackId", track}, {"gainDb", -3.0}}));
    CHECK(undo.undoCount() == 1);
    REQUIRE(undo.undo());
    CHECK_NEAR(p.findChannel(p.findTrack(track)->channelId)->gainDb, -10.0f, 1e-6);
}

TEST_CASE("gmb02", "command palette search and shortcuts") {
    CommandRegistry reg;
    registerCoreCommands(reg);
    CHECK(reg.list().size() > 40);
    auto hits = reg.search("splt clp");
    REQUIRE(!hits.empty());
    CHECK(hits[0]->id == "SplitClip");
    CHECK(reg.byShortcut("Ctrl+D")->id == "DuplicateClip");
    CHECK(reg.setShortcut("SplitClip", "Ctrl+D"));
    CHECK(reg.byShortcut("Ctrl+D")->id == "SplitClip");
    CHECK(reg.find("DuplicateClip")->shortcut.empty());
}

TEST_CASE("gmb02", "routing command rejects feedback loops") {
    Project p = makeNewProject("Route");
    UndoManager undo(p);
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo};
    const std::string a = p.channels[1].id, b = p.channels[2].id;
    REQUIRE(reg.execute(ctx, "RouteChannel", {{"channelId", a}, {"output", b}}));
    CHECK(!reg.execute(ctx, "RouteChannel", {{"channelId", b}, {"output", a}}));
    CHECK(ctx.error.find("feedback") != std::string::npos);
}
