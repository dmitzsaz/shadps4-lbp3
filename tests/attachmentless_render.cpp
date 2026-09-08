// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#define SHAD_TEST_SUPPORT_ONLY
#define SHAD_GPU_TIMING_TEST
#define SHAD_ATTACHMENTLESS_TEST
#include "scheduler_submission.cpp"
#include "attachmentless_shader.inc"

int main() {
    using namespace Vulkan;
    using namespace VideoCore;
    Instance instance;
    Scheduler scheduler{instance, "attachmentless-test"};
    const auto device = instance.GetDevice();
    Buffer output{instance, scheduler, MemoryUsage::Download, 0, AllFlags, 4096};
    Require(output.is_coherent, "test requires coherent output");
    const vk::DescriptorSetLayoutBinding binding{.binding = 0,
        .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eVertex};
    auto set_layout = Check(device.createDescriptorSetLayoutUnique(
        {.bindingCount = 1, .pBindings = &binding}));
    const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 1};
    auto pool = Check(device.createDescriptorPoolUnique({.maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &size}));
    auto sets = Check(device.allocateDescriptorSets({.descriptorPool = *pool,
        .descriptorSetCount = 1, .pSetLayouts = &*set_layout}));
    const vk::DescriptorBufferInfo info{.buffer = output.Handle(), .range = 4096};
    device.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = sets[0], .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
        .pBufferInfo = &info}, {});
    auto layout = Check(device.createPipelineLayoutUnique(
        {.setLayoutCount = 1, .pSetLayouts = &*set_layout}));
    auto shader = Check(device.createShaderModuleUnique(
        {.codeSize = sizeof(AttachmentlessShader), .pCode = AttachmentlessShader}));
    const vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eVertex,
        .module = *shader, .pName = "main"};
    const vk::PipelineVertexInputStateCreateInfo input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::Viewport viewport{0, 0, 16384, 16384, 0, 1};
    const vk::Rect2D scissor{{0, 0}, {16384, 16384}};
    const vk::PipelineViewportStateCreateInfo viewport_info{.viewportCount = 1,
        .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone, .lineWidth = 1};
    const vk::PipelineMultisampleStateCreateInfo samples{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendStateCreateInfo blend{};
    const vk::PipelineRenderingCreateInfo rendering{};
    auto pipeline = Check(device.createGraphicsPipelineUnique({}, {
        .pNext = &rendering, .stageCount = 1, .pStages = &stage, .pVertexInputState = &input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport_info,
        .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &blend, .layout = *layout}));

    for (const bool minimize : {false, true}) {
        std::memset(output.mapped_data.data(), 0, 4096);
        RenderState state{};
        state.width = state.height = 16384;
        state.num_layers = 1;
        if (minimize) state.MinimizeAttachmentlessArea(false);
        Require(state.width == (minimize ? 1 : 16384), "wrong attachmentless extent");
        const auto cmd = scheduler.CommandBuffer();
        vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        scheduler.BeginRendering(state);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, sets, {});
        cmd.draw(3, 4, 0, 0);
        scheduler.EndRendering();
        barrier = {.srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        scheduler.Finish();
        const auto* words = reinterpret_cast<const u32*>(output.mapped_data.data());
        for (u32 i = 0; i < 12; ++i) Require(words[i] == 0x12340000 + i, "vertex side effect was lost");
        Require(words[12] == 0, "unexpected shader output");
    }
    for (u32 mode = 0; mode < 3; ++mode) {
        RenderState state{};
        state.width = state.height = 16384;
        const vk::ImageView view{reinterpret_cast<VkImageView>(uintptr_t{1})};
        if (mode == 1) state.color_attachments[7].image_view = view;
        if (mode == 2) state.depth_stencil_attachment.image_view = view;
        state.MinimizeAttachmentlessArea(mode == 0);
        Require(state.width == 16384 && state.height == 16384, "observable rendering was constrained");
    }
    std::puts("PASS: 16384 and 1 pixel attachmentless areas preserve all 12 instanced vertex writes; fragment/color/depth guards hold");
}
