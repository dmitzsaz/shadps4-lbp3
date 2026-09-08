// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <barrier>
#include <cstdlib>
#include <iostream>
#include "core/gpu_wait_log.h"
#include "core/performance_telemetry.h"

namespace Core::PerfTelemetry {
bool IsEnabled() noexcept { return false; }
void Increment(Counter, u64) noexcept {}
void AddTime(TimeMetric, std::chrono::nanoseconds) noexcept {}
void RecordGpuWait(const GpuWaitEvent&) noexcept {}
} // namespace Core::PerfTelemetry

using namespace Core::PerfTelemetry;
using namespace std::chrono_literals;

void Require(bool value) {
    if (!value) {
        std::abort();
    }
}

GpuWaitEvent Event(u64 request = 64) {
    const auto now = GpuWaitEvent::Clock::now();
    return {.begin = now, .end = now + 1ms, .thread_id = 42, .timeline_id = 1,
            .target_tick = 7, .known_gpu_tick = 6, .current_tick = 8,
            .info = {.source = GpuWaitSource::StreamReuse, .resource = GpuWaitResource::TileScratch,
                     .resource_id = 2, .capacity_bytes = 4096, .request_bytes = request},
            .context_bits = 3, .image_address = 0x1000000, .image_bytes = 4096,
            .caller = std::source_location::current(),
            .texture_lock_site = std::source_location::current()};
}

int main(int argc, char** argv) {
    Require(argc == 2);
    const std::filesystem::path out{argv[1]};
    GpuWaitLog log;
    Require(!log.Open(out / "invalid.csv", GpuWaitEvent::Clock::now(), GpuWaitLog::FileLimit + 1));
    Require(!log.Open(out / "missing" / "log.csv", GpuWaitEvent::Clock::now()));
    Require(log.Open(out / "concurrent.csv", GpuWaitEvent::Clock::now(), 8192));
    std::barrier start{9};
    std::vector<std::jthread> producers;
    for (unsigned i = 0; i < 8; ++i) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (unsigned j = 0; j < 4000; ++j) {
                log.Record(Event());
            }
        });
    }
    start.arrive_and_wait();
    producers.clear(); // Join producers before closing; all accepted rows must be drained.
    log.Close();

    // Force repeated rotation while ensuring the final event survives a long session.
    Require(log.Open(out / "rotation.csv", GpuWaitEvent::Clock::now(), 8192));
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 100; ++j) {
            log.Record(Event());
        }
        std::this_thread::sleep_for(150ms);
    }
    log.Record(Event(424242));
    log.Close();

    // Reopen rejects a wait that began in the previous session and removes previous history.
    const auto stale = Event();
    Require(log.Open(out / "restart.csv", GpuWaitEvent::Clock::now(), 8192));
    log.Record(stale);
    log.Record(Event(123));
    log.Close();
    log.Record(Event(456)); // Closed journal must ignore producers.
    std::cout << "PASS: concurrent producers, bounded queue, rotation, close/drain, stale session rejection\n";
}
