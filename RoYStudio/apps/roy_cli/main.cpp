// roy_cli - headless RoY Studio command line.
// Every project operation goes through the same command system as the GUI,
// so scripted work is undoable-by-design, validated and logged the same way.
#include "audio/AudioEngine.h"
#include "audio/DeviceManager.h"
#include "audio/Processor.h"
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "core/Files.h"
#include "core/Log.h"
#include "plugins/Sandbox.h"
#include "plugins/Scanner.h"
#include "project/ProjectIO.h"
#include "project/Session.h"
#include "midi/Scale.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace roy;

namespace {

int usage(int code) {
    std::printf(
        "RoY Studio %s - command line\n"
        "usage:\n"
        "  roy_cli version\n"
        "  roy_cli devices [backend]\n"
        "  roy_cli new <parentFolder> <name> [bpm] [key]     create a project (folder + .roy file)\n"
        "  roy_cli info <project.roy>                         summary as JSON\n"
        "  roy_cli commands [search]                          list commands\n"
        "  roy_cli run <project.roy> <Command> [jsonArgs]     execute a command and save\n"
        "  roy_cli export <project.roy> [wav|flac] [folder]   render the song\n"
        "  roy_cli check <project.roy>                        Project Assistant findings\n"
        "  roy_cli recovery <project.roy>                     crash-recovery status\n"
        "  roy_cli scan-plugins [--db file] [--force] [--retry] [--timeout ms] [paths...]\n"
        "  roy_cli plugins <INSTALLED|AVAILABLE|FAILED|BLACKLISTED|FAVORITES|RECENT|INSTRUMENTS|EFFECTS|DUPLICATES> [--db file]\n",
        ROY_VERSION_STRING);
    return code;
}

fs::path defaultPluginDb() { return files::userDataDirectory() / "plugins.json"; }

void setupProcessors() {
    registerBuiltinProcessors();
    registerPluginProcessors();
}

// Opens a project with session lock + runtime, runs `fn`, closes cleanly.
struct Opened {
    Project project;
    ProjectSession session;
    AudioEngine engine;
    std::unique_ptr<ProjectRuntime> runtime;
    std::unique_ptr<UndoManager> undo;
    CommandRegistry registry;
    std::unique_ptr<CommandContext> ctx;

    bool open(const fs::path& file, std::string& err) {
        auto info = ProjectSession::inspect(file);
        if (info.lockOwnerAlive) {
            err = std::format("project is open in another RoY Studio instance (pid {} on {})", info.lockPid, info.lockHost);
            return false;
        }
        if (!session.open(file, project, OpenMode::Normal, &err)) return false;
        setupProcessors();
        engine.prepare(project.sampleRate > 0 ? project.sampleRate : 48000.0, 512);
        engine.setWorkerThreads(AudioEngine::defaultWorkerThreads());
        runtime = std::make_unique<ProjectRuntime>(engine);
        runtime->setProjectDirectory(session.folder());
        undo = std::make_unique<UndoManager>(project);
        registerCoreCommands(registry);
        ctx = std::make_unique<CommandContext>(CommandContext{project, *undo, runtime.get(), session.folder()});
        ctx->changed = [this](bool) { runtime->rebuild(project); };
        if (!runtime->rebuild(project)) {
            err = "project could not be compiled for playback";
            return false;
        }
        for (auto& w : runtime->lastWarnings()) std::fprintf(stderr, "warning: %s\n", w.c_str());
        return true;
    }
    bool save(std::string& err) {
        runtime->captureProcessorStates(project);
        return session.save(project, &err);
    }
    ~Opened() {
        runtime.reset();
        session.close();
    }
};

void print(const json& j) { std::printf("%s\n", j.dump(2).c_str()); }

} // namespace

int main(int argc, char** argv) {
    log::setLevel(log::Level::Warn);
    std::vector<std::string> a(argv + 1, argv + argc);
    const std::string cmd = a.empty() ? "help" : a[0];
    std::string err;

    if (cmd == "--version" || cmd == "version") {
        std::printf("RoY Studio %s\n", ROY_VERSION_STRING);
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return usage(0);

    if (cmd == "devices") {
        DeviceManager dm;
        if (!dm.initialise(a.size() > 1 ? a[1] : "auto", &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("backend: %s\n", dm.backendName().c_str());
        for (auto& d : dm.outputDevices()) std::printf("  out %s%s\n", d.name.c_str(), d.isDefault ? " (default)" : "");
        for (auto& d : dm.inputDevices()) std::printf("  in  %s%s\n", d.name.c_str(), d.isDefault ? " (default)" : "");
        return 0;
    }

    if (cmd == "commands") {
        CommandRegistry r;
        registerCoreCommands(r);
        const auto list = a.size() > 1 ? r.search(a[1]) : r.list();
        for (auto* c : list)
            std::printf("%-22s %-10s %-28s %s%s\n", c->id.c_str(), c->category.c_str(), c->title.c_str(), c->shortcut.c_str(),
                        c->undoable ? "" : "  (not undoable)");
        return 0;
    }

    if (cmd == "new" && a.size() >= 3) {
        const double bpm = a.size() > 3 ? std::atof(a[3].c_str()) : 120.0;
        Project p = makeNewProject(a[2], 48000.0, bpm > 0 ? bpm : 120.0);
        if (a.size() > 4)
            if (auto k = parseKey(a[4])) p.key = *k;
        ProjectSession s;
        if (!s.create(a[1], p, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("%s\n", s.file().string().c_str());
        s.close();
        return 0;
    }

    if ((cmd == "info" || cmd == "check" || cmd == "export" || cmd == "run") && a.size() >= 2) {
        Opened o;
        if (!o.open(a[1], err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (cmd == "info") {
            json tracks = json::array();
            for (auto& t : o.project.tracks)
                tracks.push_back({{"id", t.id}, {"name", t.name}, {"type", t.type == TrackType::Audio ? "audio" : t.type == TrackType::Midi ? "midi" : "beat"},
                                  {"clips", t.audioClips.size() + t.midiClips.size() + t.patternClips.size()},
                                  {"instrument", t.instrument ? t.instrument->typeId : ""}});
            print({{"name", o.project.name}, {"file", o.session.file().string()}, {"tempo", o.project.tempo.tempoAt(0)},
                   {"key", o.project.key.name()}, {"sampleRate", o.project.sampleRate}, {"lengthBeats", o.project.endBeat()},
                   {"tracks", tracks}, {"channels", o.project.channels.size()}, {"assets", o.project.assets.size()},
                   {"latencySamples", o.runtime->graphLatencySamples()}});
            return 0;
        }
        if (cmd == "check") {
            if (!o.registry.execute(*o.ctx, "ProjectCheck", {{"backups", listBackups(o.session.file()).size()}})) {
                std::fprintf(stderr, "error: %s\n", o.ctx->error.c_str());
                return 1;
            }
            print(o.ctx->result);
            return 0;
        }
        if (cmd == "export") {
            json args = {{"format", a.size() > 2 ? a[2] : "wav"}};
            if (a.size() > 3) args["folder"] = a[3];
            if (!o.registry.execute(*o.ctx, "Export", args)) {
                std::fprintf(stderr, "error: %s\n", o.ctx->error.c_str());
                return 1;
            }
            print(o.ctx->result);
            return 0;
        }
        // run
        if (a.size() < 3) return usage(2);
        json args = json::object();
        if (a.size() > 3) {
            args = json::parse(a[3], nullptr, false);
            if (args.is_discarded() || !args.is_object()) {
                std::fprintf(stderr, "error: arguments must be a JSON object\n");
                return 2;
            }
        }
        const CommandInfo* info = o.registry.find(a[2]);
        if (!info) {
            std::fprintf(stderr, "error: unknown command %s (see roy_cli commands)\n", a[2].c_str());
            return 2;
        }
        if (!o.registry.execute(*o.ctx, a[2], args)) {
            std::fprintf(stderr, "error: %s\n", o.ctx->error.c_str());
            return 1;
        }
        if (info->undoable && !o.save(err)) {
            std::fprintf(stderr, "error: save failed: %s\n", err.c_str());
            return 1;
        }
        print(o.ctx->result);
        return 0;
    }

    if (cmd == "recovery" && a.size() >= 2) {
        auto i = ProjectSession::inspect(a[1]);
        print({{"lockFound", i.lockFound}, {"lockOwnerAlive", i.lockOwnerAlive}, {"crashed", i.crashed},
               {"snapshotAvailable", i.snapshotAvailable}, {"snapshotNewer", i.snapshotNewer}, {"snapshotTime", i.snapshotTime},
               {"projectFileReadable", i.projectFileReadable}, {"lastGoodBackup", i.lastGoodBackup.string()}});
        return 0;
    }

    if (cmd == "scan-plugins" || cmd == "plugins") {
        fs::path db = defaultPluginDb();
        plugins::ScanOptions opt;
        std::string view;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "--db" && i + 1 < a.size()) db = a[++i];
            else if (a[i] == "--force") opt.force = true;
            else if (a[i] == "--retry") opt.retryQuarantined = true;
            else if (a[i] == "--timeout" && i + 1 < a.size()) opt.timeoutMs = std::atoi(a[++i].c_str());
            else if (cmd == "plugins") view = a[i];
            else opt.paths.push_back(a[i]);
        }
        plugins::PluginDatabase d;
        if (fs::exists(db) && !d.load(db, &err)) {
            // Keep the unreadable file for inspection, start a fresh database.
            files::safeCopy(db, files::uniquePath(db.string() + ".corrupt"));
            std::fprintf(stderr, "warning: %s - starting a new plugin database\n", err.c_str());
        }
        if (cmd == "plugins") {
            print(d.view(view.empty() ? "AVAILABLE" : view));
            return 0;
        }
        if (opt.paths.empty()) opt.paths = plugins::defaultPluginPaths();
        auto rep = plugins::scanPlugins(d, opt);
        if (!d.save(db, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        json j = rep.toJson();
        j["database"] = db.string();
        print(j);
        return 0;
    }
    return usage(2);
}
