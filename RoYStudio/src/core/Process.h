#pragma once
// Portable child-process handling with stdin/stdout pipes and timeouts.
// Used by the plugin scanner/sandbox (RoYPluginHost) and session locking.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace roy {

int currentProcessId();
bool isProcessAlive(int pid);
std::string hostName();
// Directory containing the running executable.
std::string executableDirectory();

class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess(); // kills the child if still running
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& executable, const std::vector<std::string>& args, std::string* error = nullptr);
    bool isRunning();
    int pid() const;

    bool writeAll(const void* data, size_t size);
    void closeStdin();
    // Reads exactly `size` bytes. Returns false on timeout, EOF or error.
    bool readExact(void* dst, size_t size, int timeoutMs);
    // Reads one '\n'-terminated line (without the newline).
    bool readLine(std::string& line, int timeoutMs);
    // Reads until EOF or timeout.
    std::string readAll(int timeoutMs);

    // Waits for exit. Returns true if the process exited within the timeout.
    bool wait(int timeoutMs);
    void kill();
    int exitCode() const { return exitCode_; }
    // True if the child terminated abnormally (signal / access violation).
    bool crashed() const { return crashed_; }
    std::string terminationReason() const { return reason_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string pending_;
    int exitCode_ = -1;
    bool crashed_ = false;
    bool exited_ = false;
    std::string reason_;
    // Reads whatever is available (up to max) within timeout; returns bytes read, 0 on timeout, -1 on EOF/error.
    long readSome(char* dst, size_t max, int timeoutMs);
    void reap(bool block, int timeoutMs);
};

} // namespace roy
