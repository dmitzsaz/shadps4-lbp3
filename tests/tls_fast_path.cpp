// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>
#include "core/tls.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
void Require(bool condition) {
    if (!condition) {
        std::fputs("TLS fast path regression failed\n", stderr);
        std::abort();
    }
}

void LiveThreadState() {
    std::array<u8, 128> block{};
    std::array<Core::DtvEntry, 4> dtv{};
    dtv[0].counter = 7;
    dtv[1].counter = 2;
    dtv[2].pointer = block.data();
    Require(Core::TryGetTlsAddress(dtv.data(), 7, 1, 0) == block.data());
    auto* address = Core::TryGetTlsAddress(dtv.data(), 7, 1, 63);
    Require(address == &block[63]);
    *address = 123;
    Require(block[63] == 123);
    // An unallocated slot and a new module must go through the existing slow path.
    Require(Core::TryGetTlsAddress(dtv.data(), 7, 2, 0) == nullptr);
    Require(Core::TryGetTlsAddress(dtv.data(), 7, 3, 0) == nullptr);
    Require(Core::TryGetTlsAddress(dtv.data(), 8, 1, 0) == nullptr);
    // Switching guest thread/fiber state must use the supplied DTV, not a host-thread cache.
    std::array<u8, 128> second{};
    auto second_dtv = dtv;
    second_dtv[2].pointer = second.data();
    Require(Core::TryGetTlsAddress(second_dtv.data(), 7, 1, 63) == &second[63]);
    Require(Core::TryGetTlsAddress(dtv.data(), 7, 1, 63) == &block[63]);
    std::puts("PASS live TLS, unallocated/new slots, generation invalidation and DTV switching");
}

void BoundsBeforeAccess() {
#ifndef _WIN32
    const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* mapping = static_cast<u8*>(
        mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Require(mapping != MAP_FAILED);
    Require(mprotect(mapping + page, page, PROT_NONE) == 0);
    auto* dtv = reinterpret_cast<Core::DtvEntry*>(mapping + page) - 3;
    dtv[0].counter = 1;
    dtv[1].counter = 1;
    u8 value{};
    dtv[2].pointer = &value;
    Require(Core::TryGetTlsAddress(dtv, 1, 1, 0) == &value);
    Require(Core::TryGetTlsAddress(dtv, 1, 2, 0) == nullptr);
    Require(Core::TryGetTlsAddress(dtv, 1, 0, 0) == nullptr);
    Require(Core::TryGetTlsAddress(dtv, 1, std::numeric_limits<u64>::max(), 0) == nullptr);
    Require(Core::TryGetTlsAddress(dtv, 2, 2, 0) == nullptr);
    Require(munmap(mapping, page * 2) == 0);
    std::puts("PASS DTV bounds against an inaccessible page");
#endif
}

void ConcurrentThreads() {
    constexpr int Workers = 8;
    constexpr int Generations = 32;
    std::atomic<u32> generation{1};
    std::barrier ready(Workers + 1);
    std::vector<std::jthread> threads;
    for (int id = 0; id < Workers; ++id) {
        threads.emplace_back([&, id] {
            std::array<u8, 64> block{};
            std::array<Core::DtvEntry, 3> dtv{};
            dtv[0].counter = 0;
            dtv[1].counter = 1;
            dtv[2].pointer = block.data();
            for (int phase = 1; phase <= Generations; ++phase) {
                ready.arrive_and_wait();
                const auto published = generation.load(std::memory_order_acquire);
                Require(published == phase);
                Require(Core::TryGetTlsAddress(dtv.data(), published, 1, id) == nullptr);
                // Model completion of the locked DTV refresh; the real slow path is unchanged.
                dtv[0].counter = published;
                for (int n = 0; n < 20000; ++n) {
                    auto* p = Core::TryGetTlsAddress(
                        dtv.data(), generation.load(std::memory_order_acquire), 1, id);
                    Require(p == &block[id]);
                    *p = static_cast<u8>(id + phase);
                    Require(block[id] == id + phase);
                }
                ready.arrive_and_wait();
            }
        });
    }
    for (u32 phase = 1; phase <= Generations; ++phase) {
        generation.store(phase, std::memory_order_release);
        ready.arrive_and_wait();
        ready.arrive_and_wait();
    }
    threads.clear();
    std::puts("PASS 8 independent guest DTVs, 32 published generations, 5,120,000 lookups");
}
} // namespace

int main() {
    LiveThreadState();
    BoundsBeforeAccess();
    ConcurrentThreads();
}
