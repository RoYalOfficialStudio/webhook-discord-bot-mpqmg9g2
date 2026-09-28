// Plugin health and crash recovery commands.
#include "audio/ProjectRuntime.h"
#include "commands/Commands.h"
#include "plugins/Sandbox.h"

namespace roy {

namespace {
bool fail(CommandContext& ctx, const std::string& e) {
    ctx.error = e;
    return false;
}
std::string slotName(const Project& p, const std::string& slotId) {
    for (auto& c : p.channels)
        for (auto& s : c.inserts)
            if (s.id == slotId) return s.name + " @ " + c.name;
    for (auto& t : p.tracks)
        if (t.instrument && t.instrument->id == slotId) return t.instrument->name + " @ " + t.name;
    return slotId;
}
} // namespace

void registerPluginCommands(CommandRegistry& r) {
    r.add({"PluginStatus", "Plugin Status", "Plugins", "", false, false, [](CommandContext& ctx, const json&) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               json list = json::array();
               for (auto& [slot, proc] : ctx.runtime->allProcessors()) {
                   auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get());
                   if (!sp) continue;
                   list.push_back({{"slotId", slot}, {"where", slotName(ctx.project, slot)}, {"typeId", sp->typeId()},
                                   {"name", sp->displayName()}, {"status", sp->alive() ? "ok" : "PLUGIN CRASHED"},
                                   {"problem", sp->problem()}, {"latency", sp->latencySamples()}, {"instrument", sp->isInstrument()}});
               }
               json crashes = json::array();
               for (auto& e : plugins::takeCrashEvents())
                   crashes.push_back({{"typeId", e.typeId}, {"plugin", e.pluginName}, {"reason", e.reason}, {"time", e.time}});
               ctx.result = {{"plugins", list}, {"newCrashes", crashes}};
               return true;
           }});
    // Recreates a (crashed) plugin instance from its last good state. Not an undo step:
    // it changes the running instance, not the project.
    r.add({"RestartPlugin", "Restart Plugin", "Plugins", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               const std::string slot = a.value("slotId", "");
               if (!ctx.runtime->processorForSlot(slot)) return fail(ctx, "no plugin instance for slot " + slot);
               ctx.runtime->captureProcessorStates(ctx.project); // crashed instances return their last good state
               ctx.runtime->forgetProcessor(slot);
               if (!ctx.runtime->rebuild(ctx.project)) return fail(ctx, "graph rebuild failed");
               auto proc = ctx.runtime->processorForSlot(slot);
               auto* sp = dynamic_cast<SandboxedPluginProcessor*>(proc.get());
               ctx.result = {{"slotId", slot}, {"restarted", sp && sp->alive()}};
               if (!sp || !sp->alive()) return fail(ctx, "plugin could not be restarted (see log)");
               return true;
           }});
}

} // namespace roy
