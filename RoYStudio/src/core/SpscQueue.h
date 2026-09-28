#pragma once
// Wait-free single-producer / single-consumer ring buffer.
// Capacity is rounded up to a power of two; storage is allocated once in the
// constructor so push/pop never allocate (safe on the audio thread).
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace roy {

template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_trivially_copyable_v<T>);
public:
    explicit SpscQueue(size_t capacity) {
        size_t c = 2;
        while (c < capacity + 1) c <<= 1;
        mask_ = c - 1;
        slots_ = std::make_unique<T[]>(c);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    bool push(const T& v) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) return false; // full
        slots_[h] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& out) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false; // empty
        out = std::move(slots_[t]);
        tail_.store((t + 1) & mask_, std::memory_order_release);
        return true;
    }
    size_t sizeApprox() const noexcept {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }
    size_t capacity() const noexcept { return mask_; }

private:
    size_t mask_ = 0;
    std::unique_ptr<T[]> slots_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

// Single-producer / single-consumer ring of raw samples (floats), used for
// audio-thread -> disk-thread streaming (recording).
class SampleFifo {
public:
    explicit SampleFifo(size_t capacitySamples) {
        size_t c = 2;
        while (c < capacitySamples + 1) c <<= 1;
        mask_ = c - 1;
        data_ = std::make_unique<float[]>(c);
    }
    size_t freeSpace() const noexcept {
        return mask_ - available();
    }
    size_t available() const noexcept {
        return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)) & mask_;
    }
    // Writes up to n samples; returns the number actually written.
    size_t write(const float* src, size_t n) noexcept {
        const size_t space = freeSpace();
        if (n > space) n = space;
        size_t h = head_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) data_[(h + i) & mask_] = src[i];
        head_.store((h + n) & mask_, std::memory_order_release);
        return n;
    }
    size_t read(float* dst, size_t n) noexcept {
        const size_t avail = available();
        if (n > avail) n = avail;
        size_t t = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) dst[i] = data_[(t + i) & mask_];
        tail_.store((t + n) & mask_, std::memory_order_release);
        return n;
    }
private:
    size_t mask_ = 0;
    std::unique_ptr<float[]> data_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace roy
