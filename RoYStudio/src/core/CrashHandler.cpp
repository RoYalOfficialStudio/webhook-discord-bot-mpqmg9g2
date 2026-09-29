#include "core/CrashHandler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace roy::crash {

namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_installed{false};
std::atomic<bool> g_inHandler{false};
char g_prefix[1024] = {}; // "<dir>/<app>_crash_<pid>_" - built at install time
char g_app[64] = {};

// --- tiny async-signal-safe formatting helpers ---------------------------------------------
size_t cat(char* dst, size_t cap, size_t len, const char* s) {
    while (*s && len + 1 < cap) dst[len++] = *s++;
    dst[len] = 0;
    return len;
}
size_t catNum(char* dst, size_t cap, size_t len, unsigned long long v, int base = 10) {
    char tmp[32];
    int n = 0;
    do {
        const int d = static_cast<int>(v % static_cast<unsigned>(base));
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v /= static_cast<unsigned>(base);
    } while (v && n < 31);
    if (base == 16) len = cat(dst, cap, len, "0x");
    while (n > 0 && len + 1 < cap) dst[len++] = tmp[--n];
    dst[len] = 0;
    return len;
}

#ifdef _WIN32
using FileHandle = HANDLE;
FileHandle openOut(const char* path) {
    return CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
bool valid(FileHandle h) { return h != INVALID_HANDLE_VALUE; }
void put(FileHandle h, const char* s) {
    DWORD w = 0;
    WriteFile(h, s, static_cast<DWORD>(std::strlen(s)), &w, nullptr);
}
void closeOut(FileHandle h) { CloseHandle(h); }
unsigned long long nowSeconds() { return static_cast<unsigned long long>(std::time(nullptr)); }
unsigned long pid() { return GetCurrentProcessId(); }
#else
using FileHandle = int;
FileHandle openOut(const char* path) { return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
bool valid(FileHandle h) { return h >= 0; }
void put(FileHandle h, const char* s) {
    const size_t n = std::strlen(s);
    [[maybe_unused]] auto r = ::write(h, s, n);
}
void closeOut(FileHandle h) { ::close(h); }
unsigned long long nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts); // async-signal-safe
    return static_cast<unsigned long long>(ts.tv_sec);
}
unsigned long pid() { return static_cast<unsigned long>(::getpid()); }
#endif

// Writes "<prefix><time>.txt" + marker; returns the base path (without extension) in `base`.
FileHandle beginReport(char* base, size_t cap) {
    size_t n = cat(base, cap, 0, g_prefix);
    n = catNum(base, cap, n, nowSeconds());
    char path[1100];
    size_t p = cat(path, sizeof(path), 0, base);
    cat(path, sizeof(path), p, ".txt.unseen");
    if (FileHandle m = openOut(path); valid(m)) closeOut(m); // marker for the next start
    p = cat(path, sizeof(path), 0, base);
    cat(path, sizeof(path), p, ".txt");
    FileHandle f = openOut(path);
    if (valid(f)) {
        put(f, g_app);
        put(f, " crash report\nversion: " ROY_VERSION_STRING "\nprocess id: ");
        char num[32];
        catNum(num, sizeof(num), 0, pid());
        put(f, num);
        put(f, "\nunix time: ");
        catNum(num, sizeof(num), 0, nowSeconds());
        put(f, num);
        put(f, "\n");
    }
    return f;
}

#ifdef _WIN32
const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION (invalid memory access)";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INTEGER_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR (file/disk could not be read)";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIVILEGED_INSTRUCTION";
    default: return "unhandled exception";
    }
}

using MiniDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                 PMINIDUMP_CALLBACK_INFORMATION);

void writeMinidump(const char* base, EXCEPTION_POINTERS* e) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll"); // Windows system library
    if (!dbg) return;
    auto fn = reinterpret_cast<MiniDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbg, "MiniDumpWriteDump")));
    if (!fn) return;
    char path[1100];
    size_t p = cat(path, sizeof(path), 0, base);
    cat(path, sizeof(path), p, ".dmp");
    HANDLE f = openOut(path);
    if (!valid(f)) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), e ? &info : nullptr,
       nullptr, nullptr);
    closeOut(f);
}

LONG WINAPI onException(EXCEPTION_POINTERS* e) {
    if (g_inHandler.exchange(true)) TerminateProcess(GetCurrentProcess(), 0xC0000409u);
    const DWORD code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
    void* addr = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr;
    char base[1100];
    FileHandle f = beginReport(base, sizeof(base));
    if (valid(f)) {
        char line[1400];
        size_t n = cat(line, sizeof(line), 0, "error: ");
        n = cat(line, sizeof(line), n, exceptionName(code));
        n = cat(line, sizeof(line), n, " code ");
        n = catNum(line, sizeof(line), n, code, 16);
        n = cat(line, sizeof(line), n, "\naddress: ");
        n = catNum(line, sizeof(line), n, reinterpret_cast<uintptr_t>(addr), 16);
        HMODULE mod = nullptr;
        char modName[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(addr), &mod) && mod) {
            GetModuleFileNameA(mod, modName, sizeof(modName));
            n = cat(line, sizeof(line), n, "\nmodule: ");
            n = cat(line, sizeof(line), n, std::strrchr(modName, '\\') ? std::strrchr(modName, '\\') + 1 : modName);
            n = cat(line, sizeof(line), n, " + ");
            n = catNum(line, sizeof(line), n, reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod), 16);
        }
        cat(line, sizeof(line), n, "\nminidump: same name, .dmp (open with WinDbg / Visual Studio)\n");
        put(f, line);
        closeOut(f);
    }
    writeMinidump(base, e);
    TerminateProcess(GetCurrentProcess(), code ? code : 0xC0000001u);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onAbort(int) {
    if (g_inHandler.exchange(true)) TerminateProcess(GetCurrentProcess(), 3);
    char base[1100];
    FileHandle f = beginReport(base, sizeof(base));
    if (valid(f)) {
        put(f, "error: abort() / std::terminate (fatal internal error)\n");
        closeOut(f);
    }
    TerminateProcess(GetCurrentProcess(), 3);
}
#else
const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort / std::terminate)";
    default: return "fatal signal";
    }
}

void onSignal(int sig, siginfo_t* info, void*) {
    if (!g_inHandler.exchange(true)) {
        char base[1100];
        FileHandle f = beginReport(base, sizeof(base));
        if (valid(f)) {
            char line[256];
            size_t n = cat(line, sizeof(line), 0, "error: ");
            n = cat(line, sizeof(line), n, signalName(sig));
            n = cat(line, sizeof(line), n, "\naddress: ");
            n = catNum(line, sizeof(line), n, reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr), 16);
            cat(line, sizeof(line), n, "\nbacktrace:\n");
            put(f, line);
            void* frames[64];
            const int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, f);
            closeOut(f);
        }
    }
    ::signal(sig, SIG_DFL); // end with the original signal (the parent sees a crash)
    ::raise(sig);
}

alignas(16) char g_altStack[64 * 1024]; // handler still runs after a stack overflow
#endif

std::terminate_handler g_prevTerminate = nullptr;
} // namespace

void install(const std::string& directory, const std::string& appName) {
    std::error_code ec;
    fs::create_directories(directory, ec);
    std::string prefix = (fs::path(directory) / (appName + "_crash_")).string();
    prefix += std::to_string(pid()) + "_";
    std::snprintf(g_prefix, sizeof(g_prefix), "%s", prefix.c_str());
    std::snprintf(g_app, sizeof(g_app), "%s", appName.c_str());
    if (g_installed.exchange(true)) return;
    g_prevTerminate = std::set_terminate([] {
        std::abort(); // -> SIGABRT handler writes the report
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
    std::signal(SIGABRT, onAbort);
#else
    stack_t ss{};
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
    void* warm[1];
    backtrace(warm, 1); // loads libgcc now: the first backtrace() call may allocate
#endif
}

bool installed() { return g_installed.load(); }

std::vector<std::string> takeUnseenReports(const std::string& directory) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(directory, ec)) {
        const std::string name = e.path().filename().string();
        const std::string suffix = ".txt.unseen";
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const fs::path report = e.path().parent_path() / name.substr(0, name.size() - 7); // strip ".unseen"
        if (fs::exists(report, ec)) out.push_back(report.string());
        fs::remove(e.path(), ec);
    }
    return out;
}

void crashForTesting(const std::string& kind) {
    if (kind == "abort") std::abort();
    if (kind == "terminate") std::terminate(); // what an uncaught exception ends in
    volatile int* p = nullptr;
    *p = 42; // invalid write
    std::abort();
}

} // namespace roy::crash
