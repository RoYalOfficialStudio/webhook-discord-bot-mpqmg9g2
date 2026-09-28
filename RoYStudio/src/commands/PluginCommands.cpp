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
    r.add({"OpenPluginEditor", "Open Plugin Editor", "Plugins", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               auto* sp = dynamic_cast<SandboxedPluginProcessor*>(ctx.runtime->processorForSlot(a.value("slotId", "")).get());
               if (!sp) return fail(ctx, "not a plugin slot (or plugin not loaded)");
               if (!sp->alive()) return fail(ctx, "PLUGIN CRASHED - restart it first");
               std::string err;
               if (!sp->openEditor(a.value("alwaysOnTop", false), &err)) return fail(ctx, err);
               ctx.result = sp->editorState();
               return true;
           }});
    r.add({"ClosePluginEditor", "Close Plugin Editor", "Plugins", "", false, false, [](CommandContext& ctx, const json& a) {
               if (!ctx.runtime) return fail(ctx, "audio runtime not available");
               auto* sp = dynamic_cast<SandboxedPluginProcessor*>(ctx.runtime->processorForSlot(a.value("slotId", "")).get());
               if (!sp) return fail(ctx, "not a plugin slot");
               sp->closeEditor(); // the plugin instance keeps running
               ctx.result = sp->editorState();
               return true;
           }});
    // UI-only slot data (favourite parameters, editor size). Undoable, never recreates the plugin.
    r.add({"SetSlotUi", "Set Plugin UI Data", "Plugins", "", true, false, [](CommandContext& ctx, const json& a) {
               PluginSlot* s = ctx.project.findSlot(a.value("slotId", ""));
               if (!s) {
                   for (auto& t : ctx.project.tracks)
                       if (t.instrument && t.instrument->id == a.value("slotId", "")) s = &*t.instrument;
               }
               if (!s) return fail(ctx, "slot not found");
               const std::string key = a.value("key", "");
               if (key.empty()) return fail(ctx, "key required");
               if (!s->state.is_object()) s->state = json::object();
               s->state["ui"][key] = a.value("value", json());
               return true;
           }});
}

} // namespace roy
