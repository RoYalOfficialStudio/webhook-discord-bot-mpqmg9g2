#include "plugins/SharedMemory.h"
#include "core/Files.h"
#include "core/Process.h"

#include <atomic>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace roy::ipc {

std::string uniqueIpcName(const std::string& tag) {
    static std::atomic<int> counter{0};
    return std::format("roy_{}_{}_{}_{}", tag, currentProcessId(), counter.fetch_add(1), files::newId().substr(0, 8));
}

#ifdef _WIN32
namespace {
std::wstring wname(const std::string& n, const char* suffix) {
    const std::string s = "Local\\" + n + suffix;
    return std::wstring(s.begin(), s.end());
}
} // namespace

SharedRegion::~SharedRegion() { close(); }

bool SharedRegion::create(const std::string& name, size_t size, std::string* error) {
    close();
    const auto w = wname(name, "_shm");
    handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<uint64_t>(size) >> 32),
                                 static_cast<DWORD>(size & 0xffffffffu), w.c_str());
    if (!handle_) {
        if (error) *error = std::format("CreateFileMapping failed ({})", GetLastError());
        return false;
    }
    data_ = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!data_) {
        if (error) *error = "MapViewOfFile failed";
        close();
        return false;
    }
    std::memset(data_, 0, size);
    name_ = name;
    size_ = size;
    owner_ = true;
    return true;
}

bool SharedRegion::open(const std::string& name, size_t size, std::string* error) {
    close();
    const auto w = wname(name, "_shm");
    handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
    if (!handle_) {
        if (error) *error = "OpenFileMapping failed";
        return false;
    }
    data_ = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!data_) {
        if (error) *error = "MapViewOfFile failed";
        close();
        return false;
    }
    name_ = name;
    size_ = size;
    return true;
}

void SharedRegion::close() {
    if (data_) UnmapViewOfFile(data_);
    if (handle_) CloseHandle(handle_);
    data_ = nullptr;
    handle_ = nullptr;
    size_ = 0;
    owner_ = false;
}

Signal::~Signal() { close(); }
bool Signal::create(const std::string& name, void*) {
    close();
    sem_ = CreateEventW(nullptr, FALSE, FALSE, wname(name, "_evt").c_str());
    owner_ = true;
    return sem_ != nullptr;
}
bool Signal::open(const std::string& name, void*) {
    close();
    sem_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wname(name, "_evt").c_str());
    return sem_ != nullptr;
}
void Signal::close() {
    if (sem_) CloseHandle(sem_);
    sem_ = nullptr;
}
void Signal::post() noexcept {
    if (sem_) SetEvent(sem_);
}
bool Signal::wait(int timeoutMicros) noexcept {
    if (!sem_) return false;
    const DWORD ms = static_cast<DWORD>((timeoutMicros + 999) / 1000);
    return WaitForSingleObject(sem_, ms) == WAIT_OBJECT_0;
}

#else
// ============================================================================ POSIX
SharedRegion::~SharedRegion() { close(); }

bool SharedRegion::create(const std::string& name, size_t size, std::string* error) {
    close();
    const std::string n = "/" + name;
    fd_ = shm_open(n.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ < 0) {
        if (error) *error = std::format("shm_open({}) failed: {}", n, std::strerror(errno));
        return false;
    }
    if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        if (error) *error = std::format("ftruncate failed: {}", std::strerror(errno));
        ::close(fd_);
        shm_unlink(n.c_str());
        fd_ = -1;
        return false;
    }
    data_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        if (error) *error = "mmap failed";
        ::close(fd_);
        shm_unlink(n.c_str());
        fd_ = -1;
        return false;
    }
    name_ = name;
    size_ = size;
    owner_ = true;
    return true;
}

bool SharedRegion::open(const std::string& name, size_t size, std::string* error) {
    close();
    const std::string n = "/" + name;
    fd_ = shm_open(n.c_str(), O_RDWR, 0600);
    if (fd_ < 0) {
        if (error) *error = std::format("shm_open({}) failed: {}", n, std::strerror(errno));
        return false;
    }
    data_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        if (error) *error = "mmap failed";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    name_ = name;
    size_ = size;
    return true;
}

void SharedRegion::close() {
    if (data_) munmap(data_, size_);
    if (fd_ >= 0) ::close(fd_);
    if (owner_ && !name_.empty()) shm_unlink(("/" + name_).c_str());
    data_ = nullptr;
    fd_ = -1;
    size_ = 0;
    owner_ = false;
}

static_assert(sizeof(sem_t) <= 64, "sem_t must fit into SignalStorage");

Signal::~Signal() { close(); }
bool Signal::create(const std::string&, void* storage) {
    close();
    if (sem_init(static_cast<sem_t*>(storage), 1, 0) != 0) return false;
    sem_ = storage;
    owner_ = true;
    return true;
}
bool Signal::open(const std::string&, void* storage) {
    close();
    sem_ = storage;
    return true;
}
void Signal::close() {
    if (sem_ && owner_) sem_destroy(static_cast<sem_t*>(sem_));
    sem_ = nullptr;
    owner_ = false;
}
void Signal::post() noexcept {
    if (sem_) sem_post(static_cast<sem_t*>(sem_));
}
bool Signal::wait(int timeoutMicros) noexcept {
    if (!sem_) return false;
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    const long long ns = static_cast<long long>(ts.tv_nsec) + static_cast<long long>(timeoutMicros) * 1000LL;
    ts.tv_sec += static_cast<time_t>(ns / 1000000000LL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000LL);
    for (;;) {
        if (sem_timedwait(static_cast<sem_t*>(sem_), &ts) == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}
#endif

} // namespace roy::ipc
