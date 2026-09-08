// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Links the production Scheduler, CommandPool and MasterSemaphore objects against a real ICD.
// Only instance setup (headless), UI uploads, logging and telemetry storage are test support.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <set>
#include "core/gpu_wait_telemetry.h"
#include "core/performance_telemetry.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

namespace {
using namespace std::chrono_literals;
std::atomic<unsigned> gpu_waits{};
std::atomic_bool telemetry_enabled{true};
std::atomic<unsigned> texture_lock_waits{};
std::atomic<s64> texture_lock_ns{};
std::mutex events_mutex;
std::vector<Core::PerfTelemetry::GpuWaitEvent> wait_events;
std::atomic<unsigned> active_submits{};
std::atomic<unsigned> submission_count{};
std::atomic<unsigned> empty_submissions{};
std::atomic<unsigned> submitted_command_buffers{};
PFN_vkQueueSubmit real_submit{};

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(3);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL ObservedSubmit(VkQueue queue, uint32_t count,
                                              const VkSubmitInfo* info, VkFence fence) {
    Require(active_submits.fetch_add(1) == 0, "concurrent host access to VkQueue");
    std::this_thread::yield();
    const auto result = real_submit(queue, count, info, fence);
    for (uint32_t i = 0; i < count; ++i) {
        empty_submissions.fetch_add(info[i].commandBufferCount == 0);
        submitted_command_buffers.fetch_add(info[i].commandBufferCount);
    }
    submission_count.fetch_add(count);
    active_submits.fetch_sub(1);
    return result;
}
} // namespace

void assert_fail_impl() {
    Require(false, "production assertion");
}

namespace Common {
void SetCurrentThreadName(const char* name) {
    pthread_setname_np(name);
}
std::string GetCurrentThreadName() {
    return "scheduler-regression";
}
namespace Log {
// Prepopulate the only keys used by these objects before starting threads.
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS{
    {Class::Debug, {}}, {Class::Render_Vulkan, {}}};
} // namespace Log
} // namespace Common

namespace Core::PerfTelemetry {
bool IsEnabled() noexcept {
    return telemetry_enabled;
}
void Increment(Counter counter, u64 amount) noexcept {
    if (counter == Counter::GpuWaits) {
        gpu_waits.fetch_add(amount);
    } else if (counter == Counter::TextureFaultLockWaits) {
        texture_lock_waits.fetch_add(amount);
    }
}
void AddTime(TimeMetric metric, std::chrono::nanoseconds duration) noexcept {
    if (metric == TimeMetric::TextureFaultLockWait) {
        texture_lock_ns.fetch_add(duration.count());
    }
}
void RecordGpuWait(const GpuWaitEvent& event) noexcept {
    // Test-only observation sink; production uses the bounded asynchronous journal.
    std::scoped_lock lock{events_mutex};
    wait_events.push_back(event);
}
} // namespace Core::PerfTelemetry

namespace ImGui::Core::TextureManager {
void Submit() {}
} // namespace ImGui::Core::TextureManager

namespace Vulkan {
Instance::Instance(bool, bool) {
    const char* loader_path = std::getenv("SHAD_TEST_VULKAN_LOADER");
    Require(loader_path != nullptr, "missing explicit Vulkan loader");
    static vk::detail::DynamicLoader loader{loader_path};
    auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    const vk::ApplicationInfo app_info{.pApplicationName = "shadPS4 scheduler regression",
                                       .apiVersion = VK_API_VERSION_1_3};
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const vk::InstanceCreateInfo create_info{
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &extension,
    };
    instance = Check(vk::createInstanceUnique(create_info));
    dispatch.init(*instance);
    physical_devices = Check(instance->enumeratePhysicalDevices());
    Require(physical_devices.size() == 1, "expected one KosmicKrisp physical device");
    physical_device = physical_devices.front();
    properties = physical_device.getProperties();
    const auto props =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    driver_id = props.get<vk::PhysicalDeviceDriverProperties>().driverID;
    Require(driver_id == vk::DriverId::eMesaKosmickrisp, "wrong driver; pool limit is KK-specific");
    const auto families = physical_device.getQueueFamilyProperties();
    Require(!families.empty() && bool(families[0].queueFlags & vk::QueueFlagBits::eGraphics),
            "queue 0 must support graphics");
    const float priority = 1;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    vk::PhysicalDeviceSynchronization2Features sync2_features{.synchronization2 = true};
#ifdef SHAD_GPU_TIMING_TEST
    vk::PhysicalDeviceDynamicRenderingFeatures rendering_features{.dynamicRendering = true};
    sync2_features.pNext = &rendering_features;
#endif
    const vk::PhysicalDeviceTimelineSemaphoreFeatures timeline_features{
        .pNext = &sync2_features, .timelineSemaphore = true};
#ifdef SHAD_ATTACHMENTLESS_TEST
    Require(physical_device.getFeatures().vertexPipelineStoresAndAtomics,
            "vertex storage writes are required for the side-effect regression");
    const vk::PhysicalDeviceFeatures features{.vertexPipelineStoresAndAtomics = true};
#endif
    const vk::DeviceCreateInfo device_info{
        .pNext = &timeline_features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
#ifdef SHAD_ATTACHMENTLESS_TEST
        .pEnabledFeatures = &features,
#endif
    };
    device = Check(physical_device.createDeviceUnique(device_info));
    dispatch.init(*device);
    graphics_queue = device->getQueue(0, 0);
    present_queue = graphics_queue;
    real_submit = dispatch.vkQueueSubmit;
    dispatch.vkQueueSubmit = ObservedSubmit;
    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = dispatch.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = dispatch.vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = physical_device, .device = *device,
        .pVulkanFunctions = &functions, .instance = *instance, .vulkanApiVersion = VK_API_VERSION_1_3};
    Require(vmaCreateAllocator(&allocator_info, &allocator) == VK_SUCCESS, "VMA initialization");
}
Instance::~Instance() {
    vmaDestroyAllocator(allocator);
}
} // namespace Vulkan

void TestResourceWaits(Vulkan::Instance& instance);

#ifndef SHAD_TEST_SUPPORT_ONLY
int main() {
    using namespace Vulkan;
    Instance instance;
    const vk::Device device = instance.GetDevice();
    constexpr unsigned writes_per_scheduler = 128;
    constexpr unsigned total_words = writes_per_scheduler * 2;
    vk::UniqueDeviceMemory memory;
    auto buffer = Check(device.createBufferUnique({
        .size = total_words * sizeof(u32),
        .usage = vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    }));
    const auto requirements = device.getBufferMemoryRequirements(*buffer);
    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    unsigned memory_type = 0;
    const auto flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    for (; memory_type < memory_properties.memoryTypeCount; ++memory_type) {
        if ((requirements.memoryTypeBits & (1u << memory_type)) &&
            (memory_properties.memoryTypes[memory_type].propertyFlags & flags) == flags) {
            break;
        }
    }
    Require(memory_type < memory_properties.memoryTypeCount, "no coherent readback memory");
    memory = Check(device.allocateMemoryUnique(
        {.allocationSize = requirements.size, .memoryTypeIndex = memory_type}));
    Check(device.bindBufferMemory(*buffer, *memory, 0));
    auto* mapped = static_cast<u32*>(Check(device.mapMemory(*memory, 0, VK_WHOLE_SIZE)));
    std::fill_n(mapped, total_words, 0);
    const vk::SemaphoreTypeCreateInfo gate_type{.semaphoreType = vk::SemaphoreType::eTimeline};
    auto gate = Check(device.createSemaphoreUnique({.pNext = &gate_type}));
    auto binary = Check(device.createSemaphoreUnique({}));
    auto empty_fence = Check(device.createFenceUnique({}));
    Frame presented_frame{};
    presented_frame.present_done = *empty_fence;

    bool four_frames_submitted_before_gate = false;
    bool fifth_acquire_blocked = false;
    bool second_submitted_while_blocked = false;
    bool reuse_query_returned_before_gate = false;
    bool reuse_query_reported_ready = false;
    {
        Scheduler first{instance};
        Scheduler second{instance};
        std::set<VkCommandBuffer> first_buffers, second_buffers;
        std::atomic<unsigned> callbacks{};
        std::atomic<unsigned> empty_callbacks{};
        const auto record = [&](Scheduler& scheduler, std::set<VkCommandBuffer>& buffers,
                                unsigned word) {
            auto command = scheduler.CommandBuffer();
            buffers.insert(static_cast<VkCommandBuffer>(command));
            command.fillBuffer(*buffer, word * sizeof(u32), sizeof(u32), 0x12340000 + word);
            const vk::MemoryBarrier barrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eHostRead,
            };
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                    vk::PipelineStageFlagBits::eHost, {}, barrier, {}, {});
            scheduler.DeferOperation([&] { callbacks.fetch_add(1); });
        };
        auto first_four = std::async(std::launch::async, [&] {
            for (unsigned i = 0; i < 4; ++i) {
                record(first, first_buffers, i);
                SubmitInfo info{};
                if (i == 0) {
                    info.AddWait(*gate, 1);
                }
                first.Flush(info);
                // Model PrepareFrame's real submit followed by Liverpool's terminal flush.
                SubmitInfo end_frame{};
                if (i == 0) {
                    end_frame.AddSignal(*binary);
                } else if (i == 1) {
                    end_frame.AddWait(*binary);
                } else if (i == 3) {
                    end_frame.AddSignal(*empty_fence);
                }
                first.DeferOperation([&] { empty_callbacks.fetch_add(1); });
                first.Flush(end_frame);
            }
        });
        four_frames_submitted_before_gate = first_four.wait_for(500ms) == std::future_status::ready;
        std::future<void> fifth;
        if (four_frames_submitted_before_gate) {
            first_four.get();
            const auto waits_before = gpu_waits.load();
            const auto submits_before = submission_count.load();
            fifth = std::async(std::launch::async, [&] {
                record(first, first_buffers, 4);
                first.Flush();
            });
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (gpu_waits.load() == waits_before &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            fifth_acquire_blocked = gpu_waits.load() > waits_before &&
                                    fifth.wait_for(0ms) == std::future_status::timeout &&
                                    submission_count.load() == submits_before;
        }
        Require(Check(device.getSemaphoreCounterValue(*gate)) == 0, "gate signalled too early");
        Require(device.getFenceStatus(*empty_fence) == vk::Result::eNotReady,
                "empty submission fence bypassed preceding GPU work");
        Require(callbacks.load() == 0 && empty_callbacks.load() == 0,
                "deferred callbacks bypassed preceding GPU work");
        auto reuse_query = std::async(std::launch::async,
                                      [&] { return presented_frame.IsPresentComplete(device); });
        reuse_query_returned_before_gate = reuse_query.wait_for(500ms) == std::future_status::ready;
        if (reuse_query_returned_before_gate) {
            reuse_query_reported_ready = reuse_query.get();
        }
        auto second_one = std::async(std::launch::async, [&] {
            record(second, second_buffers, writes_per_scheduler);
            second.Flush();
        });
        second_submitted_while_blocked = second_one.wait_for(500ms) == std::future_status::ready;
        // Always release the GPU gate and join, including the old-code negative control.
        Check(device.signalSemaphore({.semaphore = *gate, .value = 1}));
        if (reuse_query.valid()) {
            reuse_query_reported_ready = reuse_query.get();
        }
        if (first_four.valid()) {
            first_four.get();
        }
        if (fifth.valid()) {
            fifth.get();
        }
        second_one.get();
        std::printf("Four real frames plus empty terminal submits returned before gate: %s\n",
                    four_frames_submitted_before_gate ? "yes" : "NO");
        std::printf("Pool wait deferred to fifth recording request: %s\n",
                    fifth_acquire_blocked ? "yes" : "NO");
        std::printf("Second scheduler submitted before gate release: %s\n",
                    second_submitted_while_blocked ? "yes" : "NO");
        std::printf(
            "Busy frame reuse query returned before gate without reporting completion: %s\n",
            reuse_query_returned_before_gate && !reuse_query_reported_ready ? "yes" : "NO");

        auto fill_remaining = [&](Scheduler& scheduler, std::set<VkCommandBuffer>& buffers,
                                  unsigned begin, unsigned end) {
            for (unsigned i = begin; i < end; ++i) {
                record(scheduler, buffers, i);
                scheduler.Flush();
            }
            scheduler.Finish();
            scheduler.PopPendingOperations();
        };
        auto worker = std::async(std::launch::async, [&] {
            fill_remaining(first, first_buffers, four_frames_submitted_before_gate ? 5 : 4,
                           writes_per_scheduler);
        });
        fill_remaining(second, second_buffers, writes_per_scheduler + 1, total_words);
        worker.get();
        Require(first_buffers.size() == 4 && second_buffers.size() <= 4,
                "command pool grew beyond four buffers");
        Require(callbacks.load() == total_words, "deferred operations not completed");
        Require(empty_callbacks.load() == 4, "empty submission callbacks not completed");
        Require(device.getFenceStatus(*empty_fence) == vk::Result::eSuccess,
                "empty submission fence was not signalled");
        Require(presented_frame.IsPresentComplete(device),
                "completed presentation was not made available for reuse");
        for (unsigned i = 0; i < total_words; ++i) {
            Require(mapped[i] == 0x12340000 + i, "GPU readback mismatch");
        }
        std::printf(
            "GPU readback: %u words correct; deferred operations: %u; pool sizes: %zu/%zu\n",
            total_words, callbacks.load(), first_buffers.size(), second_buffers.size());
    }
    device.unmapMemory(*memory);
    Require(reuse_query_returned_before_gate && !reuse_query_reported_ready,
            "optional frame reuse blocked or bypassed pending GPU completion");
    Require(four_frames_submitted_before_gate,
            "terminal flush eagerly waited for an unused next command buffer");
    Require(fifth_acquire_blocked, "command pool did not wait at the fifth recording request");
    Require(second_submitted_while_blocked,
            "global submission mutex blocked another scheduler during command-pool wait");
    Require(empty_submissions.load() == 6 && submitted_command_buffers.load() == total_words,
            "empty terminal/Finish submissions consumed command buffers");
    std::printf("PASS: %u serialized submissions (%u without command buffers), bounded pools, "
                "deferred acquisition, semaphore/fence ordering, cross-scheduler progress\n",
                submission_count.load(), empty_submissions.load());
    TestResourceWaits(instance);
}
#endif

void TestResourceWaits(Vulkan::Instance& instance) {
    using namespace Vulkan;
    using namespace VideoCore;
    using namespace Core::PerfTelemetry;
    const auto device = instance.GetDevice();
    const std::array resources{
        std::pair{GpuWaitResource::UploadStaging, MemoryUsage::Upload},
        std::pair{GpuWaitResource::UniformStream, MemoryUsage::Stream},
        std::pair{GpuWaitResource::QuadIndex, MemoryUsage::Stream},
        std::pair{GpuWaitResource::Download, MemoryUsage::Download},
        std::pair{GpuWaitResource::DeviceUtility, MemoryUsage::DeviceLocal},
        std::pair{GpuWaitResource::TileScratch, MemoryUsage::DeviceLocal},
    };
    for (const auto [resource, usage] : resources) {
        Scheduler scheduler{instance};
        StreamBuffer ring{instance, scheduler, usage, 4096, resource};
        Buffer readback{instance, scheduler, MemoryUsage::Download, 0,
                        vk::BufferUsageFlagBits::eTransferDst, 4096};
        Require(readback.is_coherent && !readback.mapped_data.empty(), "coherent readback");
        const vk::SemaphoreTypeCreateInfo gate_type{.semaphoreType = vk::SemaphoreType::eTimeline};
        auto gate = Check(device.createSemaphoreUnique({.pNext = &gate_type}));
        Require(ring.Reserve(4096) == 0, "initial ring reservation");
        ring.Commit(false);
        const auto command = scheduler.CommandBuffer();
        const u32 pattern = 0x12340000 + static_cast<u32>(resource);
        command.fillBuffer(ring.Handle(), 0, 4096, pattern);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eTransfer, {},
                                vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                                  .dstAccessMask = vk::AccessFlagBits::eTransferRead},
                                {}, {});
        command.copyBuffer(ring.Handle(), readback.Handle(), vk::BufferCopy{.size = 4096});
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {},
                                vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                                  .dstAccessMask = vk::AccessFlagBits::eHostRead},
                                {}, {});
        bool consumed{};
        scheduler.DeferOperation([&] { consumed = true; });
        const u64 tick = scheduler.CurrentTick();
        SubmitInfo info{};
        info.AddWait(*gate, 1);
        scheduler.Flush(info);
        const auto waits_before = gpu_waits.load();
        Require(!ring.Reserve(64, 16, false), "busy nonblocking reservation must fail");
        Require(gpu_waits == waits_before, "nonblocking reservation was counted as a wait");

        std::mutex texture_mutex;
        const auto faults_before = texture_lock_waits.load();
        auto reserve = std::async(std::launch::async, [&] {
            std::scoped_lock lock{texture_mutex};
            ScopedGpuWaitContext lock_context{GpuWaitContext::TextureCacheLock};
            ScopedGpuWaitContext refresh_context{GpuWaitContext::ImageRefresh, 0x1000000, 4096};
            ScopedGpuWaitContext tracker_context{GpuWaitContext::UploadTrackerLocks};
            Require(ring.Reserve(64, 16) == 0, "reservation after GPU completion");
            if (usage == MemoryUsage::Download) {
                Require(consumed, "download ring reused bytes before CPU callback");
            }
            ring.Commit(false);
        });
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (gpu_waits == waits_before && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const bool waiter_started = gpu_waits > waits_before;
        std::array<std::future<void>, 6> writers;
        for (auto& writer : writers) {
            writer = std::async(std::launch::async, [&] {
                ScopedFaultLock lock{texture_mutex, Counter::TextureFaultLockWaits,
                                     TimeMetric::TextureFaultLockWait};
            });
        }
        deadline = std::chrono::steady_clock::now() + 5s;
        while (texture_lock_waits < faults_before + writers.size() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const bool all_writers_blocked = texture_lock_waits == faults_before + writers.size();
        const bool ring_still_blocked = reserve.wait_for(0ms) == std::future_status::timeout;
        // Release before asserting failures, so an error cannot strand the GPU/worker.
        Check(device.signalSemaphore({.semaphore = *gate, .value = 1}));
        reserve.get();
        for (auto& writer : writers) {
            writer.get();
        }
        Require(waiter_started && ring_still_blocked && all_writers_blocked,
                "expected ring wait and six contended texture writers");
        Require(texture_lock_ns > 0, "fault lock wall time was not collected");
        scheduler.PopPendingOperations();
        Require(consumed, "deferred readback not consumed");
        for (size_t i = 0; i < 1024; ++i) {
            Require(reinterpret_cast<const u32*>(readback.mapped_data.data())[i] == pattern,
                    "ring was reused before GPU copy completed");
        }
        {
            std::scoped_lock lock{events_mutex};
            const auto& event = wait_events.back();
            Require(event.info.source == GpuWaitSource::StreamReuse && event.info.resource == resource,
                    "wrong ring source/resource");
            Require(event.info.capacity_bytes == 4096 && event.info.request_bytes == 64 &&
                        event.info.offset_bytes == 0 && event.target_tick == tick &&
                        event.known_gpu_tick < tick && event.current_tick > tick,
                    "wrong requested range or timeline");
            Require(event.context_bits == 11 && event.image_address == 0x1000000 &&
                        event.image_bytes == 4096 && event.thread_id != 0 &&
                        event.timeline_id != 0 && event.end >= event.begin,
                    "context or thread/timing lost");
            Require(std::string_view{event.caller.function_name()}.contains("WaitPendingOperations") &&
                        std::string_view{event.texture_lock_site.function_name()}.contains("TestResourceWaits"),
                    "caller/lock site lost");
        }
        const auto count = gpu_waits.load();
        scheduler.Wait(tick); // Cached completion is not an actual wait.
        Require(gpu_waits == count, "completed timeline generated an event");
        GpuWaitEvent context_after{};
        ScopedGpuWaitContext::Capture(context_after);
        Require(context_after.context_bits == 0, "context leaked across threads/scopes");
    }
    // Disabled instrumentation must not inspect thread state, record events or count locks.
    telemetry_enabled = false;
    const auto count = gpu_waits.load();
    {
        ScopedGpuWaitContext context{GpuWaitContext::ImageRefresh};
        ScopedGpuWait wait{1, 2, 0, 3, {}, std::source_location::current()};
        GpuWaitEvent captured{};
        ScopedGpuWaitContext::Capture(captured);
        Require(captured.context_bits == 0, "disabled context was captured");
    }
    Require(gpu_waits == count, "disabled telemetry recorded a wait");
    telemetry_enabled = true;
    std::printf("PASS: six production StreamBuffer roles; delayed real GPU; 36 contended writers; "
                "range/tick/context attribution; 6144 readback words; download callback before reuse; "
                "nonblocking/completed/disabled paths emit no waits\n");
}
