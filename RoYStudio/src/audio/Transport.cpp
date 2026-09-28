#include "audio/Transport.h"

#include <algorithm>

namespace roy {

Transport::Transport() = default;

void Transport::post(const Request& r) {
    // The queue is large; if it is ever full the request is retried by
    // spinning briefly - this only happens if the audio thread is not running,
    // in which case the requests are applied on the next advance().
    for (int i = 0; i < 1000 && !requests_.push(r); ++i) {}
}

void Transport::play() { post({Req::Play}); }
void Transport::pause() { post({Req::Pause}); }
void Transport::stop() { post({Req::Stop}); }
void Transport::seek(int64_t sample) { post({Req::Seek, sample}); }
void Transport::setLoop(bool enabled, int64_t s, int64_t e) {
    if (e < s) std::swap(s, e);
    post({Req::Loop, s, e, enabled});
    // Published immediately so the UI reflects the setting even when stopped.
    loopEnabled_.store(enabled);
    loopStart_.store(s);
    loopEnd_.store(e);
}
void Transport::setCountInSamples(int64_t samples) {
    countInSetting_.store(std::max<int64_t>(0, samples));
    post({Req::CountIn, std::max<int64_t>(0, samples)});
}
void Transport::setPreRollSamples(int64_t samples) {
    preRollSetting_.store(std::max<int64_t>(0, samples));
    post({Req::PreRoll, std::max<int64_t>(0, samples)});
}

void Transport::apply(const Request& r) noexcept {
    switch (r.type) {
    case Req::Play:
        if (st_ != State::Playing) {
            if (st_ == State::Stopped) playStart_ = pos_;
            st_ = State::Playing;
            if (countInSamples_ > 0) {
                countInRemaining_ = countInSamples_;
            } else if (preRollSamples_ > 0) {
                pos_ -= preRollSamples_;
                pendingJump_ = true;
            }
        }
        break;
    case Req::Pause:
        if (st_ == State::Playing) st_ = State::Paused;
        countInRemaining_ = 0;
        break;
    case Req::Stop:
        st_ = State::Stopped;
        countInRemaining_ = 0;
        pos_ = playStart_;
        pendingJump_ = true;
        break;
    case Req::Seek:
        pos_ = r.a;
        if (st_ != State::Playing) playStart_ = r.a;
        pendingJump_ = true;
        break;
    case Req::Loop:
        loopOn_ = r.flag && r.b > r.a;
        lStart_ = r.a;
        lEnd_ = r.b;
        break;
    case Req::CountIn:
        countInSamples_ = r.a;
        break;
    case Req::PreRoll:
        preRollSamples_ = r.a;
        break;
    }
}

int Transport::advance(int numFrames, Segment* out, int maxSegments) noexcept {
    Request r;
    while (requests_.pop(r)) apply(r);

    int n = 0;
    int offset = 0;
    while (offset < numFrames && n < maxSegments) {
        Segment& s = out[n];
        s = Segment{};
        s.blockOffset = offset;
        s.jumped = pendingJump_;
        pendingJump_ = false;
        int remaining = numFrames - offset;

        if (st_ != State::Playing) {
            s.timelineStart = pos_;
            s.numFrames = remaining;
            ++n;
            break;
        }
        if (countInRemaining_ > 0) {
            const int take = static_cast<int>(std::min<int64_t>(remaining, countInRemaining_));
            s.countIn = true;
            s.timelineStart = pos_ - countInRemaining_;
            s.numFrames = take;
            countInRemaining_ -= take;
            if (countInRemaining_ == 0) pendingJump_ = true;
            offset += take;
            ++n;
            continue;
        }
        s.rolling = true;
        s.timelineStart = pos_;
        int take = remaining;
        bool wrap = false;
        if (loopOn_ && pos_ < lEnd_ && pos_ + take >= lEnd_) {
            take = static_cast<int>(lEnd_ - pos_);
            wrap = true;
        }
        s.numFrames = take;
        pos_ += take;
        offset += take;
        if (take > 0) ++n;
        if (wrap) {
            pos_ = lStart_;
            pendingJump_ = true;
            loopCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // If we ran out of segment slots (pathologically short loop), pad silently.
    if (offset < numFrames && n > 0) out[n - 1].numFrames += numFrames - offset;

    position_.store(pos_, std::memory_order_release);
    state_.store(static_cast<int>(st_), std::memory_order_release);
    countingIn_.store(countInRemaining_ > 0, std::memory_order_release);
    loopEnabled_.store(loopOn_, std::memory_order_release);
    return n;
}

} // namespace roy
