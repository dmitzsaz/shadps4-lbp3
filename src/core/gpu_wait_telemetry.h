// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <source_location>
#include "common/types.h"

namespace Core::PerfTelemetry {

enum class GpuWaitSource : u8 {
    Unspecified,
    StreamReuse,
    SchedulerFinish,
    CommandPool,
    DescriptorRetirement,
    FaultBufferReuse,
    PriorityCallback,
};

enum class GpuWaitResource : u8 {
    Unspecified,
    UploadStaging,
    UniformStream,
    QuadIndex,
    Download,
    DeviceUtility,
    TileScratch,
};

enum class GpuWaitContext : u32 {
    None = 0,
    TextureCacheLock = 1 << 0,
    ImageRefresh = 1 << 1,
    BufferUpload = 1 << 2,
    UploadTrackerLocks = 1 << 3,
    ImageDownload = 1 << 4,
};

struct GpuWaitInfo {
    GpuWaitSource source{};
    GpuWaitResource resource{};
    u64 resource_id{}; // Host object identity; never dereferenced by the writer.
    u64 capacity_bytes{};
    u64 request_bytes{};
    u64 offset_bytes{};
};

struct GpuWaitEvent {
    using Clock = std::chrono::steady_clock;
    Clock::time_point begin{};
    Clock::time_point end{};
    u64 thread_id{};
    u64 timeline_id{}; // Ticks from different timelines must not be compared.
    u64 target_tick{};
    u64 known_gpu_tick{};
    u64 current_tick{};
    GpuWaitInfo info{};
    u32 context_bits{};
    u64 image_address{};
    u64 image_bytes{};
    std::source_location caller{};
    std::source_location texture_lock_site{};
};

[[nodiscard]] bool IsEnabled() noexcept;
void RecordGpuWait(const GpuWaitEvent& event) noexcept;

// A thread-local scope chain is inspected only on the actual blocking semaphore path.
// It owns no resource and does not change locking, submission or dirty-state protocols.
class ScopedGpuWaitContext {
public:
    explicit ScopedGpuWaitContext(GpuWaitContext flag_, u64 image_address_ = 0,
                                  u64 image_bytes_ = 0,
                                  std::source_location location_ = std::source_location::current())
        : flag{flag_}, image_address{image_address_}, image_bytes{image_bytes_},
          location{location_}, active{flag_ != GpuWaitContext::None && IsEnabled()} {
        if (active) {
            previous = current;
            current = this;
        }
    }

    ~ScopedGpuWaitContext() {
        if (active) {
            current = previous;
        }
    }

    ScopedGpuWaitContext(const ScopedGpuWaitContext&) = delete;
    ScopedGpuWaitContext& operator=(const ScopedGpuWaitContext&) = delete;

    static void Capture(GpuWaitEvent& event) noexcept {
        for (auto* scope = current; scope; scope = scope->previous) {
            event.context_bits |= static_cast<u32>(scope->flag);
            if (scope->flag == GpuWaitContext::TextureCacheLock) {
                event.texture_lock_site = scope->location;
            }
            if (event.image_address == 0 && scope->image_address != 0) {
                event.image_address = scope->image_address;
                event.image_bytes = scope->image_bytes;
            }
        }
    }

private:
    inline static thread_local const ScopedGpuWaitContext* current{};
    const ScopedGpuWaitContext* previous{};
    GpuWaitContext flag;
    u64 image_address;
    u64 image_bytes;
    std::source_location location;
    bool active;
};

// Construct only after the cached tick and refreshed tick checks have both failed.
class ScopedGpuWait {
public:
    ScopedGpuWait(u64 timeline_id, u64 target_tick, u64 known_gpu_tick, u64 current_tick,
                  const GpuWaitInfo& info, std::source_location caller) noexcept;
    ~ScopedGpuWait();
    ScopedGpuWait(const ScopedGpuWait&) = delete;
    ScopedGpuWait& operator=(const ScopedGpuWait&) = delete;

private:
    bool active;
    GpuWaitEvent event{};
};

} // namespace Core::PerfTelemetry
