// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The runner includes the exact production Flip method. Only its collaborators
// are replaced: host presentation can be held while a guest waiter tries to run.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>
#include "core/performance_telemetry.h"

using namespace std::chrono_literals;

static void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Notifications {
    std::mutex mutex;
    std::condition_variable cv;
    bool event{};
    bool label{};

    void Notify(bool is_label) {
        std::scoped_lock lock{mutex};
        (is_label ? label : event) = true;
        cv.notify_all();
    }

    bool Wait(bool needs_label) {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, 5s, [&] { return event && (!needs_label || label); });
    }
};

static Notifications* notifications;

namespace Libraries::Kernel {
namespace OrbisKernelEvent {
enum class Filter { VideoOut };
}
struct EventQueue {
    int calls{};
    u64 id{};
    u64 payload{};
    void TriggerEvent(u64 event_id, OrbisKernelEvent::Filter, void* data) {
        ++calls;
        id = event_id;
        payload = reinterpret_cast<uintptr_t>(data);
        notifications->Notify(false);
    }
};
static EventQueue event_queue;
static EventQueue* GetEqueue(int id) {
    return id == 1 ? &event_queue : nullptr;
}
static u64 sceKernelGetProcessTime() { return 123456; }
static u64 sceKernelReadTsc() { return 987654; }
} // namespace Libraries::Kernel

namespace Vulkan {
struct Frame { int id{}; };
class Presenter {
public:
    bool hdr{};
    Frame* shown{};
    void SetHDR(bool value) { hdr = value; }
    void Present(Frame* frame) {
        std::unique_lock lock{mutex};
        shown = frame;
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool WaitEntered() {
        std::unique_lock lock{mutex};
        return cv.wait_for(lock, 5s, [&] { return entered; });
    }
    void Release() {
        std::scoped_lock lock{mutex};
        released = true;
        cv.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{};
    bool released{};
};
} // namespace Vulkan

static std::unique_ptr<Vulkan::Presenter> presenter;
static struct {
    std::pair<u32, u32> game_resolution{1920, 1080};
    std::pair<u32, u32> output_resolution{1920, 1080};
} DebugState;

struct FrameRecord {
    std::atomic<int> count{};
    u32 pending{};
    u32 depth{};
    Core::PerfTelemetry::FlipTiming timing{};
};
static FrameRecord* frame_record;

namespace Core::PerfTelemetry {
bool IsEnabled() noexcept { return true; }
void RecordFrame(u32 pending, u32 depth, u32, u32, u32, u32, const FlipTiming& timing) {
    frame_record->pending = pending;
    frame_record->depth = depth;
    frame_record->timing = timing;
    ++frame_record->count;
}
} // namespace Core::PerfTelemetry

namespace Libraries::VideoOut {
enum class OrbisVideoOutInternalEventId : u64 { Flip = 1 };
struct Port {
    struct {
        u64 count{40};
        u64 process_time{};
        u64 tsc{};
        s64 flip_arg{};
        s32 current_buffer{};
        u32 gc_queue_num{1};
        u32 flip_pending_num{1};
    } flip_status;
    struct { u64 count{100}; } vblank_status;
    std::mutex port_mutex;
    int flip_rate{1};
    bool is_hdr{true};
    int prev_index{};
    std::array<u64, 16> buffer_labels{};
    std::vector<int> flip_events{0, 1}; // An expired equeue must be skipped.
    void SignalVoLabel() { notifications->Notify(true); }
};
class VideoOutDriver {
public:
    struct Request {
        Vulkan::Frame* frame{};
        Port* port{};
        s64 flip_arg{};
        s32 index{};
        bool eop{};
        std::chrono::steady_clock::time_point prepare_begin{};
        std::chrono::steady_clock::time_point ready{};
    };
    void Flip(const Request& req);
    std::mutex mutex;
    std::queue<Request> requests;
};

#include "videoout_flip_under_test.inc"

static void Scenario(int previous_index, int current_index, bool eop) {
    Notifications notices;
    notifications = &notices;
    Kernel::event_queue = {};
    FrameRecord record;
    frame_record = &record;
    presenter = std::make_unique<Vulkan::Presenter>();
    Port port;
    port.prev_index = previous_index;
    port.buffer_labels.fill(1);
    VideoOutDriver driver;
    Vulkan::Frame snapshot{7};
    const auto now = std::chrono::steady_clock::now();
    const VideoOutDriver::Request request{
        .frame = &snapshot, .port = &port, .flip_arg = 0x1234, .index = current_index,
        .eop = eop, .prepare_begin = now - 2ms, .ready = now - 1ms,
    };
    auto next_guest_frame = std::async(std::launch::async, [&] {
        Require(notices.Wait(previous_index != -1), "guest notification never arrived");
        Require(port.flip_status.count == 41 && port.flip_status.current_buffer == current_index,
                "notification exposed stale flip status");
        Require(previous_index == -1 || port.buffer_labels[previous_index] == 0,
                "previous guest output was not released");
        Require(current_index == -1 || port.buffer_labels[current_index] == 1,
                "current guest output was released prematurely");
        {
            std::scoped_lock lock{port.port_mutex};
            ++port.flip_status.flip_pending_num;
        }
        std::scoped_lock lock{driver.mutex};
        driver.requests.push(request); // Model arrival of the next prepared request.
    });
    auto flip = std::async(std::launch::async, [&] { driver.Flip(request); });
    const bool entered = presenter->WaitEntered();
    const bool guest_progress = next_guest_frame.wait_for(250ms) == std::future_status::ready;
    const bool host_still_blocked = flip.wait_for(0ms) == std::future_status::timeout;
    const bool no_early_frame_count = record.count.load() == 0;
    // Release and join even when testing the old implementation.
    presenter->Release();
    flip.get();
    next_guest_frame.get();

    Require(entered && host_still_blocked, "host gate failed to hold presentation");
    Require(guest_progress, "guest work waited for host presentation to return");
    Require(no_early_frame_count, "FPS telemetry counted a still-blocked host Present");
    Require(record.count == 1 && record.pending == 1 && record.depth == 1,
            "next request or post-Present queue snapshot was lost");
    Require(port.flip_status.gc_queue_num == (eop ? 0u : 1u), "wrong EOP queue count");
    Require(port.flip_status.flip_arg == 0x1234 && port.prev_index == current_index,
            "wrong flip argument or retained output index");
    Require(Kernel::event_queue.calls == 1 && Kernel::event_queue.id == 1 &&
            Kernel::event_queue.payload == (1 | (0x1234ULL << 16)),
            "flip event was lost, duplicated or changed");
    Require(presenter->hdr && presenter->shown == &snapshot, "wrong host frame or HDR state");
    Require(record.timing.feedback_end > request.ready &&
            record.timing.feedback_end <= record.timing.present_begin &&
            record.timing.present_begin <= record.timing.present_end,
            "feedback/presentation timestamp ordering is incorrect");
}
} // namespace Libraries::VideoOut

int main() {
    try {
        Libraries::VideoOut::Scenario(0, 1, true);
        Libraries::VideoOut::Scenario(-1, 0, true);
        Libraries::VideoOut::Scenario(1, -1, false);
        Libraries::VideoOut::Scenario(0, 1, false);
        std::puts("PASS: production Flip releases guest work before blocked host Present; "
                  "previous/current labels, first/blank/EOP flips, event payload, queue accounting "
                  "and host-return FPS boundary preserved");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
