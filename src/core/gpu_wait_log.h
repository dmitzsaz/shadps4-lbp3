// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "core/gpu_wait_telemetry.h"

namespace Core::PerfTelemetry {

// GPU/guest threads only try to enqueue a fixed-size event. Formatting, file writes
// and rotation happen on a separate thread, including when no frame can finish.
class GpuWaitLog {
public:
    static constexpr size_t QueueCapacity = 1024;
    static constexpr size_t FileLimit = 4 * 1024 * 1024;

    GpuWaitLog();
    ~GpuWaitLog();
    bool Open(const std::filesystem::path& csv, GpuWaitEvent::Clock::time_point start,
              size_t file_limit = FileLimit);
    void Close();
    void Record(const GpuWaitEvent& event) noexcept;

private:
    void Run(std::stop_token token);
    void Drain();
    bool Rotate();
    void WriteStatus();

    std::mutex queue_mutex;
    std::vector<GpuWaitEvent> queued;
    std::vector<GpuWaitEvent> draining;
    std::atomic_bool accepting{};
    std::atomic_bool io_failed{};
    std::atomic<u64> dropped_busy{};
    std::atomic<u64> dropped_full{};
    std::atomic<u64> dropped_io{};
    GpuWaitEvent::Clock::time_point start_time{};
    std::ofstream stream;
    std::filesystem::path csv_path;
    std::filesystem::path previous_path;
    std::filesystem::path status_path;
    size_t file_limit{};
    size_t written_bytes{};
    u64 written_events{};
    u64 current_events{};
    u64 previous_events{};
    u64 evicted_events{};
    u64 rotations{};
    std::mutex wake_mutex;
    std::condition_variable_any wake_cv;
    std::jthread writer;
};

} // namespace Core::PerfTelemetry
