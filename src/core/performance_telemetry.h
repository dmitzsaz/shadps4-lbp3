// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include "common/types.h"

namespace Core::PerfTelemetry {

enum class Counter : u8 {
    DrawCalls,
    DrawsEmitted,
    IndirectDrawCalls,
    DispatchCalls,
    DispatchesEmitted,
    DescriptorWrites,
    RenderPassBegins,
    RenderPassEnds,
    VkSubmits,
    GpuWaits,
    GpuFrames,
    GfxSubmits,
    AscSubmits,
    GfxDwords,
    AscDwords,
    GraphicsPipelineCompiles,
    ComputePipelineCompiles,
    GuestShaderCompiles,
    HostShaderCompiles,
    GuestWriteFaults,
    GuestReadFaults,
    PresentCalls,
    DirectImportAttempts,
    DirectImportSuccesses,
    DirectImportFailures,
    DirectBufferBinds,
    DirectReadbackBytes,
    DirectUploadBytes,
    DirectFaultSubmits,
    DirectFaultFinishes,
    DirectVisibilityBarriers,
    OrderedGuestReleases,
    GuestFenceSubmits,
    Lbp3NgCpuHleDispatches,
    Lbp3NgCpuHleReleases,
    VkEmptySubmits,
    CommandBufferAcquires,
    ReusedPresents,
    EmptyFlipSlots,
    ReuseSkippedBusy,
    TextureFaultLockWaits,
    BufferFaultLockWaits,
    TileScratchReservations,
    TileScratchBytes,
    TileScratchReuseBarriers,
    IndexedQuadDraws,
    ExpandedIndexedQuadDraws,
    IndexedQuadFallbacks,
    ExpandedQuadIndexBytes,
    Count,
};

enum class TimeMetric : u8 {
    GpuFrameCpu,
    DrawCpu,
    DispatchCpu,
    ResourceBindCpu,
    OnSubmitCpu,
    RasterFlushCpu,
    PrepareFrameCpu,
    PresentCpu,
    FramePoolWait,
    PresentFenceWait,
    VkSubmitCpu,
    GpuWait,
    GraphicsPipelineCompile,
    ComputePipelineCompile,
    GuestShaderCompile,
    HostShaderCompile,
    FaultService,
    SamplerOverhead,
    SubmitMutexWait,
    QueueSubmitCpu,
    CommandPoolWait,
    CommandBufferAcquireCpu,
    SubmitPendingOpsCpu,
    SwapchainAcquireCpu,
    SwapchainPresentCpu,
    ReusePrepareCpu,
    ReuseFenceWait,
    GnmSubmitWait,
    TextureFaultLockWait,
    BufferFaultLockWait,
    QuadIndexExpandCpu,
    Count,
};

// Timestamps travel with one VideoOut request, unlike the interval-wide atomic totals.
// "ready" means CPU preparation finished before the queue mutex, not GPU completion or scanout.
struct FlipTiming {
    std::chrono::steady_clock::time_point prepare_begin{};
    std::chrono::steady_clock::time_point ready{};
    std::chrono::steady_clock::time_point present_begin{};
    std::chrono::steady_clock::time_point present_end{};
    s32 videoout_flip_rate{-1};
    u64 videoout_vblank_count{};
    std::chrono::steady_clock::time_point feedback_end{};
};

void Start();
void Stop();

void SetStartRequested(bool requested) noexcept;
// Configure before Start(), for an isolated diagnostic session.
void SetOutputDirectory(std::string directory);
[[nodiscard]] bool IsStartRequested() noexcept;

[[nodiscard]] bool IsEnabled() noexcept;
[[nodiscard]] u64 GetRecordedFrameCount() noexcept;

void Increment(Counter counter, u64 amount = 1) noexcept;
void AddTime(TimeMetric metric, std::chrono::nanoseconds duration) noexcept;

void RecordFrame(u32 pending_flips, u32 request_depth, u32 game_width, u32 game_height,
                 u32 output_width, u32 output_height, const FlipTiming& flip_timing);

class ScopedTimer {
public:
    explicit ScopedTimer(TimeMetric metric_) noexcept
        : metric{metric_}, active{IsEnabled()}, start{active ? Clock::now() : Clock::time_point{}} {
    }

    ~ScopedTimer() {
        if (active) {
            AddTime(metric,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start));
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    TimeMetric metric;
    bool active;
    Clock::time_point start;
};

// Sample only contended fault-side lock acquisitions. Uncontended acquisitions
// perform no clock reads; when telemetry is disabled the original lock() path is used.
template <typename Mutex>
class ScopedFaultLock {
public:
    ScopedFaultLock(Mutex& mutex, Counter counter, TimeMetric metric)
        : lock{mutex, std::defer_lock} {
        if (!IsEnabled()) {
            lock.lock();
        } else if (!lock.try_lock()) {
            Increment(counter);
            ScopedTimer timer{metric};
            lock.lock();
        }
    }

private:
    std::unique_lock<Mutex> lock;
};

} // namespace Core::PerfTelemetry
