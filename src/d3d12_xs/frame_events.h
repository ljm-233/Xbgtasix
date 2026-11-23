// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace xbgtasix::d3d12::detail {

// A desktop clock supplies the display cadence. The public Microsoft
// Xbox-GDK-Samples FramePacing sample uses microseconds for interval and offset,
// and the SimpleTriangle sample describes the period as frame pipeline latency.
class FrameEvents {
public:
    void set_interval(UINT interval_us) {
        {
            std::lock_guard lock(mutex_);
            interval_ = std::chrono::microseconds(interval_us);
            const auto now = Clock::now();
            epoch_ = now;
            if (interval_us == 0) ++reset_generation_;
            for (auto& event : events_) {
                if (interval_us == 0) event = {};
                else if (event.scheduled) event.deadline = first_deadline(event.offset, now);
            }
        }
        changed_.notify_all();
    }

    HRESULT schedule(UINT type, UINT offset_us) {
        if (type >= events_.size()) return E_INVALIDARG;
        {
            std::lock_guard lock(mutex_);
            if (interval_.count() == 0) return E_INVALIDARG;
            const auto offset = std::chrono::microseconds(offset_us);
            events_[type] = {true, offset, first_deadline(offset, Clock::now())};
        }
        changed_.notify_all();
        return S_OK;
    }

    HRESULT wait(UINT type, UINT timeout_ms, UINT64* token) {
        if (type >= events_.size()) return E_INVALIDARG;
        if (!token) return E_POINTER;
        std::unique_lock lock(mutex_);
        const auto limit = timeout_ms == INFINITE
            ? Clock::time_point::max()
            : Clock::now() + std::chrono::milliseconds(timeout_ms);
        const UINT64 generation = reset_generation_;
        auto& event = events_[type];
        while (event.scheduled && generation == reset_generation_) {
            const auto now = Clock::now();
            if (event.deadline <= now && event.deadline <= limit) {
                const auto step = (now - event.deadline) / interval_ + 1;
                event.deadline += interval_ * step;
                *token = next_token_++;
                if (next_token_ == 0) ++next_token_;
                return S_OK;
            }
            if (now >= limit) return D3DERR_DEVICEREMOVED;
            changed_.wait_until(lock, std::min(event.deadline, limit));
        }
        return E_INVALIDARG;
    }

private:
    using Clock = std::chrono::steady_clock;
    using Microseconds = std::chrono::microseconds;

    struct Event {
        bool scheduled = false;
        Microseconds offset{};
        Clock::time_point deadline{};
    };

    Clock::time_point first_deadline(Microseconds offset, Clock::time_point now) const {
        const auto first = epoch_ + interval_ + offset;
        if (now < first) return first;
        return first + interval_ * ((now - first) / interval_ + 1);
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    Microseconds interval_{};
    Clock::time_point epoch_{};
    std::array<Event, 3> events_{};
    UINT64 next_token_ = 1;
    UINT64 reset_generation_ = 0;
};

}  // namespace xbgtasix::d3d12::detail
