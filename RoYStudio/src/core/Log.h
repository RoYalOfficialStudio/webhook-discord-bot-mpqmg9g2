#pragma once
// Structured logging for RoY Studio.
//
// * Normal (non-realtime) threads call log::write()/info()/... which format and
//   write synchronously to the console and/or a log file.
// * The audio thread must never format strings, allocate or take slow locks.
//   It calls log::audioEvent() with a string *literal* and a number. Events are
//   pushed into a lock-free ring and written by log::flushAudioEvents(), which
//   the message thread calls periodically. Audio events go to a separate
//   category ("audio") so dropouts/overloads are traceable.
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace roy::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Fatal };

const char* levelName(Level l);
void setLevel(Level l);
Level level();
void setConsole(bool enabled);
// Opens (appends) a log file. Returns false if the file cannot be opened.
bool setFile(const std::string& path);
void closeFile();

// Removes obvious secrets (token=..., password=..., Bearer ...) from a message.
std::string redact(std::string_view msg);

void write(Level l, std::string_view category, std::string_view msg);

template <typename... Args>
void writef(Level l, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (l < level()) return;
    write(l, category, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void trace(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Trace, c, f, std::forward<Args>(a)...); }
template <typename... Args> void debug(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Debug, c, f, std::forward<Args>(a)...); }
template <typename... Args> void info(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Info, c, f, std::forward<Args>(a)...); }
template <typename... Args> void warn(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Warn, c, f, std::forward<Args>(a)...); }
template <typename... Args> void error(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Error, c, f, std::forward<Args>(a)...); }
template <typename... Args> void fatal(std::string_view c, std::format_string<Args...> f, Args&&... a) { writef(Level::Fatal, c, f, std::forward<Args>(a)...); }

// ---- realtime-safe audio event channel -----------------------------------
// `staticMessage` MUST point to a string with static storage duration.
void audioEvent(Level l, const char* staticMessage, double value = 0.0);
// Drains pending audio events into the regular log. Message thread only.
int flushAudioEvents();
// Number of audio events dropped because the ring was full.
uint64_t droppedAudioEvents();

// In-memory capture of the last N lines, useful for tests and crash reports.
std::string recentLines(int maxLines = 200);

} // namespace roy::log
