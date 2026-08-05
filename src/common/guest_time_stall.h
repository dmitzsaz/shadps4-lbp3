// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <mutex>

#include "common/types.h"

namespace Common {

// Tracks host-only work which has no guest-hardware equivalent. Kernel timeouts that overlap one
// of these stalls can exclude it from their deadline instead of exposing shader/pipeline compiler
// latency to the guest as if the emulated device had stopped responding.
class GuestTimeStallTracker {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    struct Snapshot {
        Duration elapsed{};
        bool active{};
    };

    void Begin() {
        const auto now = Clock::now();
        std::scoped_lock lock{mutex};
        if (active_scopes++ == 0) {
            active_since = now;
        }
    }

    void End() {
        const auto now = Clock::now();
        std::scoped_lock lock{mutex};
        if (--active_scopes == 0) {
            elapsed += now - active_since;
        }
    }

    [[nodiscard]] Snapshot GetSnapshot() const {
        const auto now = Clock::now();
        std::scoped_lock lock{mutex};
        return {
            .elapsed = elapsed + (active_scopes != 0 ? now - active_since : Duration{}),
            .active = active_scopes != 0,
        };
    }

private:
    mutable std::mutex mutex;
    Duration elapsed{};
    Clock::time_point active_since{};
    u32 active_scopes{};
};

inline GuestTimeStallTracker& GetGuestTimeStallTracker() {
    static GuestTimeStallTracker tracker;
    return tracker;
}

class GuestTimeStallScope {
public:
    GuestTimeStallScope() {
        GetGuestTimeStallTracker().Begin();
    }

    ~GuestTimeStallScope() {
        GetGuestTimeStallTracker().End();
    }

    GuestTimeStallScope(const GuestTimeStallScope&) = delete;
    GuestTimeStallScope& operator=(const GuestTimeStallScope&) = delete;
};

} // namespace Common
