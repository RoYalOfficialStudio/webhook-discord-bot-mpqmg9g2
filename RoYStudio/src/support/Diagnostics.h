#pragma once
// DIAGNOSTICS REPORT for testers: one Markdown file with everything needed to understand a
// problem on a real machine - system, audio + MIDI devices, plugin scan results (failures with
// reasons), crash reports of RoY's own programs, and the end of the log. Written only when the
// user asks for it (Help menu / `roy_cli diagnostics`), never sent anywhere automatically.
// Privacy: the user's home folder and user name are replaced ("~", "<user>"); no project
// content, no audio, no credentials are included (project names / paths can appear in the log
// section). The user reviews the file before sharing.
#include <filesystem>
#include <string>
#include <vector>

namespace roy::support {

struct DiagnosticsInput {
    std::string program = "RoY Studio";
    std::filesystem::path userDataDir;     // logs, CrashReports/, plugins.json
    std::string logFileName = "roy_studio.log";
    std::vector<std::string> audioLines;   // backend, devices, current settings (caller formats)
    std::vector<std::string> midiLines;    // MIDI inputs
    std::vector<std::string> sessionLines; // e.g. open project summary (names/counts only)
    int logTailLines = 300;
    int crashReportsIncluded = 5;          // newest crash reports copied in full
};

// OS, Wine (if any), CPU threads, memory.
std::vector<std::string> systemInfo();
// Replaces the home directory (both slash styles) and the user name.
std::string sanitize(const std::string& text);
std::string buildReport(const DiagnosticsInput& in);
// Writes <userData>/Diagnostics/RoY_Diagnostics_<UTC time>.md (or `file` if given) atomically.
std::filesystem::path writeReport(const DiagnosticsInput& in, const std::filesystem::path& file = {}, std::string* error = nullptr);

} // namespace roy::support
