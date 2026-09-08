// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#define SHAD_TEST_SUPPORT_ONLY
#define SHAD_GPU_TIMING_TEST
#include "scheduler_submission.cpp"
#include "scratch_shader.inc"
#include <filesystem>

namespace {
PFN_vkGetQueryPoolResults real_results;
unsigned result_reads{};
VKAPI_ATTR VkResult VKAPI_CALL ObserveResults(VkDevice device, VkQueryPool pool, uint32_t first,
    uint32_t count, size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    Require(!(flags & VK_QUERY_RESULT_WAIT_BIT), "diagnostic used blocking query readback");
    ++result_reads;
    return real_results(device, pool, first, count, size, data, stride, flags);
}
}

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    Instance instance;
    const auto device = instance.GetDevice();
    auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
    real_results = dispatch.vkGetQueryPoolResults;
    dispatch.vkGetQueryPoolResults = ObserveResults;
    const char* path = std::getenv("SHADPS4_GPU_TIMING_DIR");
    Require(path != nullptr, "missing test output directory");
    const std::string directory{path};
    unsetenv("SHADPS4_GPU_TIMING_DIR");
    Scheduler scheduler{instance, "disabled"};
    setenv("SHADPS4_GPU_TIMING_DIR", directory.c_str(), 1);
    GpuTiming timing{instance, "test", 1};
    constexpr u64 bytes = 16 * 1024 * 1024;
    Buffer src{instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, bytes};
    Buffer dst{instance, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, bytes};
    Buffer readback{instance, scheduler, MemoryUsage::Download, 0, AllFlags, 4096};
    const auto barrier = [&](vk::CommandBuffer command) {
        const vk::MemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
        command.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &b});
    };
    const auto work = [&](vk::CommandBuffer command, u32 times, u64 size = bytes) {
        for (u32 i = 0; i < times; ++i) {
            command.fillBuffer(src.Handle(), 0, size, 0x12340000 + i);
            barrier(command);
            command.copyBuffer(src.Handle(), dst.Handle(), vk::BufferCopy{.size = size});
            barrier(command);
        }
    };
    // A real render encoder between compute timestamp scopes checks ordering
    // across the driver's pre-render / render / post-render encoder handling.
    auto img = Check(device.createImageUnique({.imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, .extent = {2048, 2048, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal, .usage = vk::ImageUsageFlagBits::eColorAttachment}));
    const auto req = device.getImageMemoryRequirements(*img);
    u32 type = std::countr_zero(req.memoryTypeBits);
    auto memory = Check(device.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = type}));
    Check(device.bindImageMemory(*img, *memory, 0));
    auto view = Check(device.createImageViewUnique({.image = *img, .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
    const auto command = scheduler.CommandBuffer();
    timing.Begin(command, scheduler.CurrentTick());
    const auto short_scope = timing.BeginWork(command, {.kind = GpuTiming::Kind::Detile, .bytes = bytes});
    work(command, 1);
    timing.EndWork(command, short_scope);
    const vk::ImageMemoryBarrier2 image_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *img, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    command.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &image_barrier});
    const vk::RenderingAttachmentInfo attachment{.imageView = *view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore};
    command.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {2048, 2048}},
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment});
    timing.RenderPass();
    command.endRendering();
    barrier(command);
    const auto long_scope = timing.BeginWork(command, {.kind = GpuTiming::Kind::Tile, .bytes = 32 * bytes});
    work(command, 32);
    timing.EndWork(command, long_scope);
    command.copyBuffer(dst.Handle(), readback.Handle(), vk::BufferCopy{.size = 4096});
    barrier(command);
    const u64 first_tick = scheduler.CurrentTick();
    timing.End(command, first_tick);
    const vk::SemaphoreTypeCreateInfo gate_type{.semaphoreType = vk::SemaphoreType::eTimeline};
    auto gate = Check(device.createSemaphoreUnique({.pNext = &gate_type}));
    SubmitInfo info{};
    info.AddWait(*gate, 1);
    scheduler.Flush(info);
    const unsigned waits = gpu_waits;
    auto begin = std::chrono::steady_clock::now();
    for (u32 i = 0; i < 1000; ++i) timing.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
    const auto collect_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count();
    Require(!result_reads && gpu_waits == waits, "read/wait before GPU completed");
    Check(device.signalSemaphore({.semaphore = *gate, .value = 1}));
    scheduler.Wait(first_tick);
    timing.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
    Require(result_reads == 1, "completed bank not read");
    Require(readback.is_coherent, "test requires coherent readback");
    const auto* values = reinterpret_cast<const u32*>(readback.mapped_data.data());
    for (u32 i = 0; i < 1024; ++i) Require(values[i] == 0x1234001f, "GPU output corrupted");

    // Real clear/store render passes; synthetic draw metadata exercises grouping
    // independently of shader compilation. Every ordinal is selected once over
    // eight submissions, and timestamps stay outside begin/endRendering.
    {
        GpuTiming render_timing{instance, "render", 1};
        for (u32 phase = 0; phase < GpuTiming::RenderSampleStride; ++phase) {
            auto cmd = scheduler.CommandBuffer();
            render_timing.Begin(cmd, scheduler.CurrentTick());
            for (u32 pass = 0; pass < GpuTiming::RenderSampleStride; ++pass) {
                barrier(cmd);
                render_timing.BeginRenderPass(cmd, 2048, 2048, 1);
                Require(render_timing.IsTimingRenderPass() == (pass == phase), "bad render rotation");
                cmd.beginRendering(vk::RenderingInfo{.renderArea = {{0, 0}, {2048, 2048}},
                    .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment});
                render_timing.RecordDraw(100, 200, 6);
                render_timing.RecordDraw(100, 201, 0, true);
                cmd.endRendering();
                render_timing.EndRenderPass(cmd);
                Require(!render_timing.IsTimingRenderPass(), "render token survived its pass");
            }
            const auto tick = scheduler.CurrentTick();
            render_timing.End(cmd, tick);
            scheduler.Flush();
            scheduler.Wait(tick);
            render_timing.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
            std::this_thread::sleep_for(2ms);
        }
    }

    // Withhold collection: all four banks must remain reserved even if the GPU
    // finishes, and the fifth submission must simply skip diagnostics.
    GpuTiming::Token stale;
    for (u32 i = 0; i < 5; ++i) {
        const auto cmd = scheduler.CommandBuffer();
        timing.Begin(cmd, scheduler.CurrentTick());
        timing.EndWork(cmd, short_scope); // stale token from a previous submission
        if (i == 0) {
            for (u32 n = 0; n < GpuTiming::MaxScopes + 3; ++n) {
                auto token = timing.BeginWork(cmd, {.kind = GpuTiming::Kind::Detile, .bytes = 4096});
                work(cmd, 1, 4096);
                timing.EndWork(cmd, token);
            }
        } else if (i == 1) {
            stale = timing.BeginWork(cmd, {.kind = GpuTiming::Kind::Tile, .bytes = 4096});
            work(cmd, 1, 4096); // deliberately not closed before Flush
        } else {
            timing.EndWork(cmd, stale);
            work(cmd, 1, 4096);
        }
        timing.End(cmd, scheduler.CurrentTick());
        scheduler.Flush();
    }
    scheduler.Finish();
    timing.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
    // Reuse the banks after completion. Keep scopes small to limit test duration.
    for (u32 i = 0; i < 9; ++i) {
        auto cmd = scheduler.CommandBuffer();
        timing.Begin(cmd, scheduler.CurrentTick());
        work(cmd, 1, 4096);
        timing.End(cmd, scheduler.CurrentTick());
        scheduler.Finish();
        timing.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
        std::this_thread::sleep_for(2ms); // give the asynchronous test file sink time to drain
    }
    // Exercise production Scheduler hooks with the normal period, including
    // semaphore-only flushes (must not allocate timestamp command buffers).
    {
        Scheduler integrated{instance, "integrated"};
        for (u32 i = 0; i < 65; ++i) {
            auto cmd = integrated.CommandBuffer();
            const auto token = integrated.BeginGpuWork({.kind = GpuTiming::Kind::Detile, .bytes = 4096});
            work(cmd, 1, 4096);
            integrated.EndGpuWork(token);
            RenderState state{};
            state.width = state.height = 2048;
            state.num_layers = state.num_color_attachments = 1;
            state.color_attachments[0].image_view = *view;
            state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
            state.color_attachments[0].is_clear = true;
            barrier(cmd);
            integrated.BeginRendering(state);
            integrated.RecordTimedDraw(123, 456, 6);
            integrated.EndRendering();
            integrated.Flush();
            integrated.Flush();
        }
        integrated.Finish();
    }
    scheduler.Finish();
    // Real compute dispatch, with no artificial memory barrier between the
    // dispatch and its end timestamp (the production detiler has this shape).
    {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {.binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
             .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute}}};
        auto set_layout = Check(device.createDescriptorSetLayoutUnique(
            {.bindingCount = 2, .pBindings = bindings.data()}));
        const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 2};
        auto pool = Check(device.createDescriptorPoolUnique({.maxSets = 1,
            .poolSizeCount = 1, .pPoolSizes = &size}));
        auto sets = Check(device.allocateDescriptorSets({.descriptorPool = *pool,
            .descriptorSetCount = 1, .pSetLayouts = &*set_layout}));
        const std::array<vk::DescriptorBufferInfo, 2> buffers{{
            {.buffer = src.Handle(), .range = bytes}, {.buffer = dst.Handle(), .range = bytes}}};
        for (u32 i = 0; i < 2; ++i) device.updateDescriptorSets(vk::WriteDescriptorSet{
            .dstSet = sets[0], .dstBinding = i, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffers[i]}, {});
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute, .size = 12};
        auto layout = Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
            .pSetLayouts = &*set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        auto shader = Check(device.createShaderModuleUnique({
            .codeSize = sizeof(ScratchCopyShader), .pCode = ScratchCopyShader}));
        auto pipeline = Check(device.createComputePipelineUnique({}, {
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *shader, .pName = "main"},
            .layout = *layout}));
        GpuTiming compute{instance, "compute", 1};
        auto cmd = scheduler.CommandBuffer();
        compute.Begin(cmd, scheduler.CurrentTick());
        cmd.fillBuffer(src.Handle(), 0, bytes, 123);
        barrier(cmd);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, sets, {});
        const std::array<u32, 3> params{0, 0, 7};
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params.data());
        for (u32 groups : {64U, 16384U}) {
            const auto token = compute.BeginWork(cmd, {.kind = GpuTiming::Kind::Detile, .bytes = groups * 256ULL});
            cmd.dispatch(groups, 1, 1);
            compute.EndWork(cmd, token);
            barrier(cmd);
        }
        cmd.copyBuffer(dst.Handle(), readback.Handle(), vk::BufferCopy{.size = 4096});
        barrier(cmd);
        compute.End(cmd, scheduler.CurrentTick());
        scheduler.Finish();
        compute.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
        for (u32 i = 0; i < 1024; ++i) Require(values[i] == 130, "compute data corrupted");
    }
    {
        GpuTiming limited{instance, "limited", 1, 512};
        for (u32 i = 0; i < 4; ++i) {
            auto cmd = scheduler.CommandBuffer();
            limited.Begin(cmd, scheduler.CurrentTick());
            auto token = limited.BeginWork(cmd, {.kind = GpuTiming::Kind::Tile, .bytes = 4096});
            work(cmd, 1, 4096);
            limited.EndWork(cmd, token);
            limited.End(cmd, scheduler.CurrentTick());
            scheduler.Finish();
            limited.Collect(scheduler.GetMasterSemaphore()->KnownGpuTick());
            std::this_thread::sleep_for(5ms);
        }
    }
    std::filesystem::remove(std::filesystem::path{directory} / "enable");
    const auto reads_before_idle = result_reads;
    {
        Scheduler idle{instance, "idle"};
        for (u32 i = 0; i < 64; ++i) {
            work(idle.CommandBuffer(), 1, 4096);
            idle.Flush();
        }
        idle.Finish();
    }
    Require(result_reads == reads_before_idle, "inactive diagnostic stage still issued queries");
    std::printf("PASS: delayed GPU, 1000 nonblocking collections in %lld us; 2048 output words; "
                "bounded banks, overflow, truncated/stale scopes, reuse, scheduler integration\n", collect_us);
}
