#pragma once
// VST3 hosting inside RoYPluginHost (Steinberg VST3 SDK 3.8.1, MIT).
// Module load, component + controller, connection, bus setup, processing with
// parameter changes / note events / MIDI-CC mapping, state (component + controller),
// editor (IPlugView in a native window, IPlugFrame + Linux IRunLoop), unload.
#include "pluginhost/HostedPlugin.h"

#include <memory>
#include <string>

namespace roy::vst3 {

using json = nlohmann::json;

// Scans one module (.vst3 bundle or file): {"ok":true,"plugins":[{id,name,vendor,version,subCategories,instrument,instantiates,paramCount,...}]}
json scanModule(const std::string& path);

class Module; // opaque (VST3::Hosting::Module)

// Loads the module and instantiates class `classId` (UID string as produced by scanModule).
std::unique_ptr<pluginhost::HostedPlugin> createInstance(const std::string& path, const std::string& classId, std::string* error);

} // namespace roy::vst3
