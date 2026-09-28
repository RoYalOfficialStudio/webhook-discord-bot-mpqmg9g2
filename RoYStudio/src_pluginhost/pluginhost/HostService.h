#pragma once
// Entry points of the sandbox process RoYPluginHost (apps/roy_plugin_host).
//   roy_plugin_host --scan <module>                                    -> one JSON line, exit 0
//   roy_plugin_host --host <clap|vst3> <module> <pluginId> <shmName>   -> serves one instance
// Format is detected from the extension for --scan (.clap / .vst3).
// The DAW never calls these in-process.
#include <string>

namespace roy::pluginhost {

int runScan(const std::string& modulePath);
int runHost(const std::string& format, const std::string& modulePath, const std::string& pluginId, const std::string& shmName);

} // namespace roy::pluginhost
