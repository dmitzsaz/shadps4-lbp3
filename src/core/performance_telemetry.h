// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

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
    Count,
};

enum class PhaseEventKind : u8 {
    ReleaseQueued,
    ReleaseFlushBegin,
    ReleaseFlushEnd,
    ReleaseVisible,
    ComputeWait,
};

struct PhaseEvent {
    PhaseEventKind kind{};
    u32 queue_id{~0u};
    u64 timestamp_ns{};
    u64 start_ns{};
    u64 sequence{};
    u64 related_sequence{};
    VAddr address{};
    u64 value{};
    u64 tick{};
    u64 yields{};
    u32 reference{};
    u32 observed_begin{};
    u32 observed_end{};
};

void Start();
void Stop();

void SetStartRequested(bool requested) noexcept;
[[nodiscard]] bool IsStartRequested() noexcept;

[[nodiscard]] bool IsEnabled() noexcept;
[[nodiscard]] u64 GetRecordedFrameCount() noexcept;

[[nodiscard]] u64 TimestampNs() noexcept;
void RecordPhaseEvent(const PhaseEvent& event) noexcept;

void Increment(Counter counter, u64 amount = 1) noexcept;
void AddTime(TimeMetric metric, std::chrono::nanoseconds duration) noexcept;

void RecordFrame(u32 pending_flips, u32 request_depth, u32 game_width, u32 game_height,
                 u32 output_width, u32 output_height);

class ScopedTimer {
public:
    explicit ScopedTimer(TimeMetric metric_) noexcept
        : metric{metric_}, active{IsEnabled()}, start{active ? Clock::now() : Clock::time_point{}} {}

    ~ScopedTimer() {
        if (active) {
            AddTime(metric, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start));
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

} // namespace Core::PerfTelemetry
