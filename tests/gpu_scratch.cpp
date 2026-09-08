// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reuse the headless Instance/logging/observation support; its main is not compiled.
#define SHAD_TEST_SUPPORT_ONLY
#include "scheduler_submission.cpp"
#include "scratch_shader.inc"

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    using namespace Core::PerfTelemetry;
    Instance instance;
    const auto device = instance.GetDevice();
    Scheduler scheduler{instance};
#ifdef SHAD_SCRATCH_OLD_CONTROL
    StreamBuffer scratch{instance, scheduler, MemoryUsage::DeviceLocal, 4096,
                         GpuWaitResource::TileScratch};
#else
    GpuScratchBuffer scratch{instance, scheduler, 4096};
    Require(!scratch.Reserve(0) && !scratch.Reserve(4097), "invalid reservation changed ring state");
#endif
    const auto reserve = [&](u64 size) {
        const auto offset = scratch.Reserve(size, 256);
        Require(offset && *offset % 256 == 0 && *offset + size <= scratch.SizeBytes(),
                "scratch alignment/bounds");
#ifdef SHAD_SCRATCH_OLD_CONTROL
        scratch.Commit(false);
#endif
        return *offset;
    };

    constexpr u32 iterations = 512;
    constexpr std::array<u32, 5> sizes{256, 768, 512, 1024, 1280};
    constexpr u64 output_size = 4096 + iterations * 1280;
    Buffer input{instance, scheduler, MemoryUsage::Upload, 0, AllFlags, 4096};
    Buffer output{instance, scheduler, MemoryUsage::Download, 0, AllFlags, output_size};
    Require(input.is_coherent && output.is_coherent, "test requires coherent host buffers");
    for (u32 i = 0; i < 1024; ++i) {
        reinterpret_cast<u32*>(input.mapped_data.data())[i] = i * 13 + 3;
    }
    std::memset(output.mapped_data.data(), 0, output_size);
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{.binding = 0, .descriptorType = vk::DescriptorType::eStorageBuffer,
                                       .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{.binding = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                                       .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
    };
    auto desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .bindingCount = 2, .pBindings = bindings.data()}));
    const vk::DescriptorPoolSize pool_size{.type = vk::DescriptorType::eStorageBuffer,
                                           .descriptorCount = 4};
    auto desc_pool = Check(device.createDescriptorPoolUnique({
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &pool_size}));
    const std::array layouts{*desc_layout, *desc_layout};
    const auto sets = Check(device.allocateDescriptorSets({
        .descriptorPool = *desc_pool, .descriptorSetCount = 2, .pSetLayouts = layouts.data()}));
    const std::array infos{
        vk::DescriptorBufferInfo{.buffer = input.Handle(), .range = 4096},
        vk::DescriptorBufferInfo{.buffer = scratch.Handle(), .range = 4096},
        vk::DescriptorBufferInfo{.buffer = scratch.Handle(), .range = 4096},
        vk::DescriptorBufferInfo{.buffer = output.Handle(), .range = output_size},
    };
    for (u32 i = 0; i < 4; ++i) {
        device.updateDescriptorSets(vk::WriteDescriptorSet{
            .dstSet = sets[i / 2], .dstBinding = i % 2, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &infos[i]}, {});
    }
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute, .size = 12};
    auto layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1, .pSetLayouts = &layouts[0],
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range}));
    auto shader = Check(device.createShaderModuleUnique({
        .codeSize = sizeof(ScratchCopyShader), .pCode = ScratchCopyShader}));
    auto pipeline = Check(device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *shader, .pName = "main"},
        .layout = *layout}));

    const auto barrier = [&](vk::CommandBuffer cmd, vk::Buffer buffer, u64 offset, u64 size,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::BufferMemoryBarrier2 memory{
            .srcStageMask = src_stage, .srcAccessMask = src_access,
            .dstStageMask = dst_stage, .dstAccessMask = dst_access,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer, .offset = offset, .size = size};
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &memory});
    };
    std::vector<u32> expected;
    u64 output_offset{};
    const auto record = [&](u32 size, bool compute_writes_scratch, u32 bias) {
        const u64 offset = reserve(size);
        const auto cmd = scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        const std::array<u32, 3> params = compute_writes_scratch
            ? std::array<u32, 3>{0, static_cast<u32>(offset / 4), bias}
            : std::array<u32, 3>{static_cast<u32>(offset / 4), static_cast<u32>(output_offset / 4), bias};
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0,
                               sets[compute_writes_scratch ? 0 : 1], {});
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params.data());
        if (!compute_writes_scratch) {
            cmd.copyBuffer(input.Handle(), scratch.Handle(), vk::BufferCopy{.dstOffset = offset, .size = size});
            barrier(cmd, scratch.Handle(), offset, size,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
        }
        cmd.dispatch(size / 256, 1, 1);
        if (compute_writes_scratch) {
            barrier(cmd, scratch.Handle(), offset, size,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            cmd.copyBuffer(scratch.Handle(), output.Handle(),
                           vk::BufferCopy{.srcOffset = offset, .dstOffset = output_offset, .size = size});
        }
        // No post-read barrier here: only production scratch reuse protects its next overwrite.
        for (u32 word = 0; word < size / 4; ++word) {
            expected.push_back(word * 13 + 3 + bias);
        }
        output_offset += size;
    };
    record(4096, true, 100);
    const vk::SemaphoreTypeCreateInfo gate_type{.semaphoreType = vk::SemaphoreType::eTimeline};
    auto gate = Check(device.createSemaphoreUnique({.pNext = &gate_type}));
    auto first_done = Check(device.createFenceUnique({}));
    SubmitInfo first{};
    first.AddWait(*gate, 1);
    first.AddSignal(*first_done);
    const auto gate_begin = std::chrono::steady_clock::now();
    scheduler.Flush(first);
    const unsigned waits_before = gpu_waits;
    const unsigned submits_before = submission_count;
    const unsigned faults_before = texture_lock_waits;
    std::mutex texture_mutex;
    std::promise<void> locked;
    auto locked_future = locked.get_future();
    auto producer = std::async(std::launch::async, [&] {
        std::scoped_lock lock{texture_mutex};
        ScopedGpuWaitContext lock_context{GpuWaitContext::TextureCacheLock};
        locked.set_value();
        for (u32 i = 0; i < iterations; ++i) {
            record(sizes[i % sizes.size()], i % 2 == 0, 1000 + i * 4099);
            if (i == iterations / 2 - 1) {
                scheduler.Flush();
            }
        }
        barrier(scheduler.CommandBuffer(), output.Handle(), 0, output_offset,
                vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        scheduler.Flush();
    });
    Require(locked_future.wait_for(5s) == std::future_status::ready, "producer not scheduled");
    std::array<std::future<void>, 6> writers;
    for (auto& writer : writers) {
        writer = std::async(std::launch::async, [&] {
            ScopedFaultLock lock{texture_mutex, Counter::TextureFaultLockWaits,
                                 TimeMetric::TextureFaultLockWait};
        });
    }
    // Observe progress or an actual production wait promptly, then verify the
    // first GPU submission is still pending. Do not rely on a multi-second gate.
    const auto observation_deadline = std::chrono::steady_clock::now() + 500ms;
    while (producer.wait_for(0ms) != std::future_status::ready && gpu_waits == waits_before &&
           std::chrono::steady_clock::now() < observation_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    auto writer_deadline = std::chrono::steady_clock::now() + 500ms;
    unsigned writers_before_gpu{};
    do {
        writers_before_gpu = 0;
        for (auto& writer : writers) {
            writers_before_gpu += writer.wait_for(0ms) == std::future_status::ready;
        }
        if (writers_before_gpu == writers.size() ||
            texture_lock_waits == faults_before + writers.size()) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < writer_deadline);
    const bool recorded_before_gpu = producer.wait_for(0ms) == std::future_status::ready;
    const bool gate_unreleased = Check(device.getSemaphoreCounterValue(*gate)) == 0;
    const bool gpu_blocked = device.getFenceStatus(*first_done) == vk::Result::eNotReady;
    const unsigned waits_while_gated = gpu_waits - waits_before;
    const unsigned submits_while_gated = submission_count - submits_before;
    const auto gate_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - gate_begin).count();
    Check(device.signalSemaphore({.semaphore = *gate, .value = 1}));
    producer.get();
    for (auto& writer : writers) {
        writer.get();
    }
    scheduler.Finish();
    Require(gate_unreleased && gpu_blocked, "GPU bypassed unsignalled gate");
    for (size_t i = 0; i < expected.size(); ++i) {
        Require(reinterpret_cast<const u32*>(output.mapped_data.data())[i] == expected[i],
                "scratch data corrupted across compute/transfer reuse");
    }
    std::printf("Scratch recorded before GPU gate: %s; writers before gate: %u/6; waits: %u; "
                "queued submissions: %u; verified GPU words: %zu; capacity: %llu bytes\n",
                recorded_before_gpu ? "yes" : "NO", writers_before_gpu, waits_while_gated,
                submits_while_gated, expected.size(), static_cast<unsigned long long>(scratch.SizeBytes()));
    std::printf("Gate observation: %.3f ms; first submission still pending: yes\n", gate_ms);
    Require(recorded_before_gpu && writers_before_gpu == 6 && waits_while_gated == 0 &&
                submits_while_gated == 2, "CPU blocked on GPU scratch reuse");
    std::puts("PASS: GPU scratch reuse across queued submissions and repeated wraps, mixed compute/transfer, bounded memory, guest writers progress");
}
