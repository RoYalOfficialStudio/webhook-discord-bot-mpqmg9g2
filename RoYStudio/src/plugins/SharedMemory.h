#pragma once
// Cross-process shared memory and wake-up signals for the plugin sandbox.
//  * POSIX: shm_open/mmap + process-shared unnamed semaphores placed inside the region.
//  * Windows: named file mapping + named auto-reset events.
// wait()/post() are realtime-friendly (no allocation, one syscall).
#include <cstddef>
#include <string>

namespace roy::ipc {

class SharedRegion {
public:
    SharedRegion() = default;
    ~SharedRegion();
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;

    // Creates a new zero-filled region (owner unlinks it on destruction).
    bool create(const std::string& name, size_t size, std::string* error = nullptr);
    bool open(const std::string& name, size_t size, std::string* error = nullptr);
    void close();
    void* data() const { return data_; }
    size_t size() const { return size_; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    void* data_ = nullptr;
    size_t size_ = 0;
    bool owner_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

class Signal {
public:
    Signal() = default;
    ~Signal();
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    // `storage` must be >= 64 bytes inside a SharedRegion (used on POSIX);
    // `name` identifies the signal across processes (used on Windows).
    bool create(const std::string& name, void* storage);
    bool open(const std::string& name, void* storage);
    void close();
    void post() noexcept;
    // Returns true if signalled, false on timeout.
    bool wait(int timeoutMicros) noexcept;

private:
    void* sem_ = nullptr;
    bool owner_ = false;
};

// A reasonably unique name for a new region ("roy_<pid>_<counter>_<random>").
std::string uniqueIpcName(const std::string& tag);

} // namespace roy::ipc
