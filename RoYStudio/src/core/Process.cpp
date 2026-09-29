#include "core/Process.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace roy {

namespace {
using Clock = std::chrono::steady_clock;
int remainingMs(Clock::time_point deadline) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(ms);
}
} // namespace

#ifdef _WIN32
// ============================================================================ Windows
int currentProcessId() { return static_cast<int>(GetCurrentProcessId()); }

bool isProcessAlive(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

std::string hostName() {
    char buf[256];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return std::string(buf, n);
    return "unknown";
}

std::string executableDirectory() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    const auto p = w.find_last_of(L"\\/");
    if (p != std::wstring::npos) w = w.substr(0, p);
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

struct ChildProcess::Impl {
    PROCESS_INFORMATION pi{};
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    bool started = false;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    kill();
    if (impl_->stdinWrite) CloseHandle(impl_->stdinWrite);
    if (impl_->stdoutRead) CloseHandle(impl_->stdoutRead);
    if (impl_->started) {
        CloseHandle(impl_->pi.hProcess);
        CloseHandle(impl_->pi.hThread);
    }
}

static std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    int bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { q.append(static_cast<size_t>(bs * 2 + 1), L'\\'); bs = 0; q += c; continue; }
        q.append(static_cast<size_t>(bs), L'\\');
        bs = 0;
        q += c;
    }
    q.append(static_cast<size_t>(bs * 2), L'\\');
    q += L'"';
    return q;
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& args, std::string* error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring cmd = quoteArg(widen(executable));
    for (auto& a : args) cmd += L" " + quoteArg(widen(a));
    // Suppress the Windows error dialog if the child crashes.
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const BOOL ok = CreateProcessW(widen(executable).c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &impl_->pi);
    SetErrorMode(oldMode);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        if (error) *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    impl_->stdinWrite = inW;
    impl_->stdoutRead = outR;
    impl_->started = true;
    return true;
}

int ChildProcess::pid() const { return impl_->started ? static_cast<int>(impl_->pi.dwProcessId) : -1; }

void ChildProcess::reap(bool block, int timeoutMs) {
    if (!impl_->started || exited_) return;
    if (WaitForSingleObject(impl_->pi.hProcess, block ? static_cast<DWORD>(timeoutMs) : 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(impl_->pi.hProcess, &code);
        exited_ = true;
        exitCode_ = static_cast<int>(code);
        crashed_ = code >= 0xC0000000u;
        reason_ = crashed_ ? std::format("exception 0x{:08X}", static_cast<unsigned>(code)) : "exit " + std::to_string(code);
    }
}

bool ChildProcess::isRunning() {
    reap(false, 0);
    return impl_->started && !exited_;
}

bool ChildProcess::writeAll(const void* data, size_t size) {
    if (!impl_->stdinWrite) return false;
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD w = 0;
        if (!WriteFile(impl_->stdinWrite, p, static_cast<DWORD>(size), &w, nullptr)) return false;
        p += w;
        size -= w;
    }
    return true;
}

void ChildProcess::closeStdin() {
    if (impl_->stdinWrite) CloseHandle(impl_->stdinWrite);
    impl_->stdinWrite = nullptr;
}

long ChildProcess::readSome(char* dst, size_t max, int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(impl_->stdoutRead, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(impl_->stdoutRead, dst, static_cast<DWORD>(std::min<size_t>(max, avail)), &got, nullptr)) return -1;
            return static_cast<long>(got);
        }
        if (Clock::now() >= deadline) return 0;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

bool ChildProcess::wait(int timeoutMs) {
    reap(true, timeoutMs);
    return exited_;
}

void ChildProcess::kill() {
    if (!impl_->started || exited_) return;
    TerminateProcess(impl_->pi.hProcess, 0xDEAD);
    WaitForSingleObject(impl_->pi.hProcess, 2000);
    reap(false, 0);
    reason_ = "killed";
}

#else
// ============================================================================ POSIX
int currentProcessId() { return static_cast<int>(::getpid()); }

bool isProcessAlive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

std::string hostName() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return "unknown";
}

std::string executableDirectory() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    std::string s(buf, static_cast<size_t>(n));
    const auto p = s.find_last_of('/');
    return p == std::string::npos ? "." : s.substr(0, p);
}

struct ChildProcess::Impl {
    pid_t pid = -1;
    int inFd = -1;
    int outFd = -1;
};

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
ChildProcess::~ChildProcess() {
    kill();
    if (impl_->inFd >= 0) ::close(impl_->inFd);
    if (impl_->outFd >= 0) ::close(impl_->outFd);
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& args, std::string* error) {
    int inPipe[2], outPipe[2];
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
        if (error) *error = "pipe failed";
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, executable.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    if (rc != 0) {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        if (error) *error = std::string("posix_spawn failed: ") + std::strerror(rc);
        return false;
    }
    ::signal(SIGPIPE, SIG_IGN); // a crashed child must not kill the DAW through a broken pipe
    impl_->pid = pid;
    impl_->inFd = inPipe[1];
    impl_->outFd = outPipe[0];
    ::fcntl(impl_->outFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(impl_->inFd, F_SETFD, FD_CLOEXEC);
    return true;
}

int ChildProcess::pid() const { return impl_->pid; }

void ChildProcess::reap(bool block, int timeoutMs) {
    if (impl_->pid <= 0 || exited_) return;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(impl_->pid, &status, WNOHANG);
        if (r == impl_->pid) {
            exited_ = true;
            if (WIFEXITED(status)) {
                exitCode_ = WEXITSTATUS(status);
                reason_ = "exit " + std::to_string(exitCode_);
            } else if (WIFSIGNALED(status)) {
                exitCode_ = 128 + WTERMSIG(status);
                crashed_ = WTERMSIG(status) != SIGKILL || reason_ != "killed";
                reason_ = std::string("signal ") + std::to_string(WTERMSIG(status));
            }
            return;
        }
        if (r < 0 || !block || Clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool ChildProcess::isRunning() {
    reap(false, 0);
    return impl_->pid > 0 && !exited_;
}

bool ChildProcess::writeAll(const void* data, size_t size) {
    if (impl_->inFd < 0) return false;
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t w = ::write(impl_->inFd, p, size);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        p += w;
        size -= static_cast<size_t>(w);
    }
    return true;
}

void ChildProcess::closeStdin() {
    if (impl_->inFd >= 0) ::close(impl_->inFd);
    impl_->inFd = -1;
}

long ChildProcess::readSome(char* dst, size_t max, int timeoutMs) {
    if (impl_->outFd < 0) return -1;
    pollfd pfd{impl_->outFd, POLLIN, 0};
    int pr;
    do {
        pr = ::poll(&pfd, 1, timeoutMs);
    } while (pr < 0 && errno == EINTR);
    if (pr == 0) return 0;
    if (pr < 0) return -1;
    const ssize_t n = ::read(impl_->outFd, dst, max);
    if (n <= 0) return -1;
    return static_cast<long>(n);
}

bool ChildProcess::wait(int timeoutMs) {
    reap(true, timeoutMs);
    return exited_;
}

void ChildProcess::kill() {
    if (impl_->pid <= 0 || exited_) return;
    reason_ = "killed";
    ::kill(impl_->pid, SIGKILL);
    reap(true, 2000);
    crashed_ = false;
    reason_ = "killed";
}
#endif

bool openInFileBrowser(const std::string& folder) {
#ifdef _WIN32
    const int len = MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len > 0 ? len : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, w.data(), len);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"explore", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp("xdg-open", "xdg-open", folder.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid > 0;
#endif
}

// ============================================================================ common
bool ChildProcess::readExact(void* dst, size_t size, int timeoutMs) {
    char* out = static_cast<char*>(dst);
    size_t got = 0;
    const size_t fromPending = std::min(size, pending_.size());
    if (fromPending) {
        std::memcpy(out, pending_.data(), fromPending);
        pending_.erase(0, fromPending);
        got = fromPending;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < size) {
        const long n = readSome(out + got, size - got, remainingMs(deadline));
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool ChildProcess::readLine(std::string& line, int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto nl = pending_.find('\n');
        if (nl != std::string::npos) {
            line = pending_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, nl + 1);
            return true;
        }
        char buf[4096];
        const long n = readSome(buf, sizeof(buf), remainingMs(deadline));
        if (n <= 0) return false;
        pending_.append(buf, static_cast<size_t>(n));
    }
}

std::string ChildProcess::readAll(int timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string out = std::move(pending_);
    pending_.clear();
    for (;;) {
        char buf[4096];
        const long n = readSome(buf, sizeof(buf), remainingMs(deadline));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace roy
