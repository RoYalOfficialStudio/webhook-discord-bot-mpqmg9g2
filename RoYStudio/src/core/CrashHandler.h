#pragma once
// CRASH REPORTS for RoY's own programs (third-party plugins crash in their sandbox process and
// are reported by the sandbox). On a fatal error - access violation, illegal instruction,
// division by zero, abort()/std::terminate - the handler writes
//   <dir>/<app>_crash_<pid>_<unix time>.txt   (error, address, module, version, backtrace on Linux)
//   <dir>/<app>_crash_<pid>_<unix time>.dmp   (Windows minidump via the system's dbghelp.dll)
// plus a marker so the next start can tell the user. The report contains no project content,
// no passwords and no tokens. The process then ends with the original error.
// The handler only uses pre-built paths and raw OS calls (no allocation, no locks).
#include <string>
#include <vector>

namespace roy::crash {

void install(const std::string& directory, const std::string& appName);
bool installed();
// Reports written by earlier runs that were not shown yet; clears their markers.
std::vector<std::string> takeUnseenReports(const std::string& directory);

// TEST ONLY: crashes the process on purpose ("segv" = invalid write, "abort" = std::abort,
// "terminate" = uncaught exception). Used by roy_cli crash-test.
[[noreturn]] void crashForTesting(const std::string& kind);

} // namespace roy::crash
