// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;

// Optional diagnostic, enabled only by the capture launcher. All timestamps are
// outside rendering. Results are read only after the scheduler's GPU tick completes.
class GpuTiming {
public:
    static constexpr u32 SamplePeriod = 31; // Odd: avoid always sampling the same of two submits.
    static constexpr u32 BankCount = 4;
    static constexpr u32 MaxScopes = 64;
    static constexpr u32 RenderSampleStride = 8;
    static constexpr u32 QueryCount = 2 + 2 * MaxScopes;
    static constexpr size_t FileLimit = 2 * 1024 * 1024;

    enum class Kind : u32 { Submission, Detile, Tile, Render };
    struct Work {
        Kind kind{};
        u64 address{};
        u64 bytes{};
        u32 width{}, height{}, depth{}, pitch{}, bits{}, tile_mode{}, mips{}, layers{};
        u32 render_pass{}, draw_calls{}, indirect_calls{}, shader_changes{};
        u64 vertices{}, first_vs{}, first_ps{}, last_vs{}, last_ps{};
    };
    struct Token {
        u64 tick{};
        u32 index{};
    };

    GpuTiming(const Instance& instance, const char* role, u32 period = SamplePeriod,
              size_t file_limit = FileLimit);
    ~GpuTiming();
    void Begin(vk::CommandBuffer command, u64 tick);
    void End(vk::CommandBuffer command, u64 tick);
    void Collect(u64 completed_tick);
    void RenderPass();
    void BeginRenderPass(vk::CommandBuffer command, u32 width, u32 height, u32 layers);
    void EndRenderPass(vk::CommandBuffer command);
    bool IsTimingRenderPass() const;
    void RecordDraw(u64 vs, u64 ps, u64 vertices, bool indirect = false);
    Token BeginWork(vk::CommandBuffer command, const Work& work);
    void EndWork(vk::CommandBuffer command, Token token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
