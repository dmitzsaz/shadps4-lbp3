// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_lbp3_ng_cpu_hle.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/memory.h"
#include "core/performance_telemetry.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Vulkan {
namespace {

constexpr std::array Lbp3NgHashes{
    0x15f3a569593c4c58ULL, 0x39392c783089119fULL, 0x744d4d82942b9961ULL,
    0x0020a4d78a49461cULL, 0xcdd355b7331679a5ULL, 0x01e2c7ba10806334ULL,
};

struct CpuOutput {
    VAddr address{};
    std::vector<u32> words;
};

struct CpuResult {
    std::vector<CpuOutput> outputs;
};

std::array<bool, Lbp3NgHashes.size()> logged_failures{};

[[nodiscard]] bool EnvEnabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

[[nodiscard]] bool IsLbp3V126(bool async_compute) noexcept {
    return async_compute && Common::ElfInfo::Instance().GameSerial() == "CUSA00063" &&
           Common::ElfInfo::Instance().AppVer() == "01.26";
}

[[nodiscard]] size_t HashIndex(u64 hash) noexcept {
    const auto it = std::ranges::find(Lbp3NgHashes, hash);
    return it == Lbp3NgHashes.end() ? Lbp3NgHashes.size()
                                    : static_cast<size_t>(it - Lbp3NgHashes.begin());
}

[[nodiscard]] u32 InvocationCount(const AmdGpu::ComputeProgram& cs_program) noexcept {
    // All six v1.26 kernels are one-dimensional (local_size=64, dispatch Y/Z=1). Keep the
    // observed ABI explicit instead of pretending the recompiler's flattened lane formula is a
    // generic multidimensional CPU ABI.
    if (cs_program.dim_y != 1 || cs_program.dim_z != 1 || cs_program.num_thread_y.full != 1 ||
        cs_program.num_thread_z.full != 1) {
        return 0;
    }
    const u64 count = static_cast<u64>(cs_program.dim_x) * cs_program.num_thread_x.full;
    return count <= std::numeric_limits<u32>::max() ? static_cast<u32>(count) : 0;
}

[[nodiscard]] u32 PushedUserData(const Shader::Info& info, u32 pushed_index) noexcept {
    u32 mask = info.ud_mask.mask;
    while (mask) {
        const u32 register_index = std::countr_zero(mask);
        mask &= ~(1U << register_index);
        if (pushed_index-- == 0) {
            return register_index < info.user_data.size() ? info.user_data[register_index]
                                                          : std::numeric_limits<u32>::max();
        }
    }
    return std::numeric_limits<u32>::max();
}

[[nodiscard]] u32 FlatBound(const Shader::Info& info, u32 index, u32 fallback) noexcept {
    return index < info.flattened_ud_buf.size() ? info.flattened_ud_buf[index] : fallback;
}

[[nodiscard]] u32 WordCount(const AmdGpu::Buffer& sharp) noexcept {
    return sharp.GetSize() / static_cast<u32>(sizeof(u32));
}

[[nodiscard]] std::optional<std::vector<u32>> ReadWords(Rasterizer& rasterizer,
                                                        const AmdGpu::Buffer& sharp,
                                                        u32 word_count) {
    if (!sharp || sharp.base_address <= 1 || word_count > WordCount(sharp)) {
        return std::nullopt;
    }
    const u64 byte_count = static_cast<u64>(word_count) * sizeof(u32);
    auto* memory = Core::Memory::Instance();
    if (!memory->IsValidMapping(sharp.base_address, byte_count)) {
        return std::nullopt;
    }

    auto& cache = rasterizer.GetBufferCache();
    if (cache.IsRegionGpuModified(sharp.base_address, byte_count)) {
        cache.ReadMemory(sharp.base_address, byte_count);
    }

    std::vector<u32> words(word_count);
    memory->CopySparseMemory(sharp.base_address, reinterpret_cast<u8*>(words.data()), byte_count);
    return words;
}

[[nodiscard]] float AsFloat(u32 value) noexcept {
    return std::bit_cast<float>(value);
}

[[nodiscard]] u32 AsBits(float value) noexcept {
    return std::bit_cast<u32>(value);
}

[[nodiscard]] std::optional<CpuResult> Run15f3(const Shader::Info& info,
                                               const AmdGpu::ComputeProgram& cs_program,
                                               Rasterizer& rasterizer) {
    if (info.buffers.size() != 3) {
        return std::nullopt;
    }
    const auto control_sharp = info.buffers[0].GetSharp(info);
    const auto state_sharp = info.buffers[1].GetSharp(info);
    const auto output_sharp = info.buffers[2].GetSharp(info);
    auto control = ReadWords(rasterizer, control_sharp, 4);
    if (!control) {
        return std::nullopt;
    }
    const u32 count = std::min({(*control)[0], InvocationCount(cs_program), WordCount(state_sharp),
                                WordCount(output_sharp)});
    auto state = ReadWords(rasterizer, state_sharp, count);
    auto output = ReadWords(rasterizer, output_sharp, count);
    if (!state || !output) {
        return std::nullopt;
    }

    const u32 output_bound = PushedUserData(info, 0);
    const u32 state_bound = PushedUserData(info, 1);
    const float bias = AsFloat((*control)[1]);
    const float scale = AsFloat((*control)[2]);
    for (u32 i = 0; i < count; ++i) {
        const float x = AsFloat((*state)[i]);
        const float biased = bias + AsFloat((*output)[i]);
        if (i < output_bound) {
            (*output)[i] = AsBits(std::fma(scale, x, biased));
        }

        const float x2 = x * x;
        const float x4 = x2 * x2;
        const float limited = 2.9200000762939453125f * x4;
        const float updated = x - std::fmin(x, limited);
        if (i < state_bound) {
            (*state)[i] = AsBits(updated);
        }
    }

    CpuResult result;
    result.outputs.push_back({state_sharp.base_address, std::move(*state)});
    result.outputs.push_back({output_sharp.base_address, std::move(*output)});
    return result;
}

[[nodiscard]] std::optional<CpuResult> Run3939(const Shader::Info& info,
                                               const AmdGpu::ComputeProgram& cs_program,
                                               Rasterizer& rasterizer) {
    if (info.buffers.size() != 2) {
        return std::nullopt;
    }
    const auto control_sharp = info.buffers[0].GetSharp(info);
    const auto output_sharp = info.buffers[1].GetSharp(info);
    auto control = ReadWords(rasterizer, control_sharp, 1);
    if (!control) {
        return std::nullopt;
    }
    const u32 count = std::min({(*control)[0], InvocationCount(cs_program), PushedUserData(info, 0),
                                WordCount(output_sharp)});
    CpuResult result;
    result.outputs.push_back({output_sharp.base_address, std::vector<u32>(count, 0)});
    return result;
}

[[nodiscard]] std::optional<CpuResult> Run744d(const Shader::Info& info,
                                               const AmdGpu::ComputeProgram& cs_program,
                                               Rasterizer& rasterizer) {
    if (info.buffers.size() != 6) {
        return std::nullopt;
    }
    const auto control_sharp = info.buffers[0].GetSharp(info);
    const auto flags_sharp = info.buffers[1].GetSharp(info);
    const auto field_sharp = info.buffers[2].GetSharp(info);
    const auto coefficient_sharp = info.buffers[3].GetSharp(info);
    const auto output_sharp = info.buffers[4].GetSharp(info);
    auto control = ReadWords(rasterizer, control_sharp, 4);
    if (!control) {
        return std::nullopt;
    }
    const u32 dim_x = (*control)[1];
    const u32 dim_y = (*control)[2];
    const u32 dim_z = (*control)[3];
    const u64 cell_count_64 = static_cast<u64>(dim_x) * dim_y * dim_z;
    if (dim_x < 3 || dim_y < 3 || dim_z < 3 || cell_count_64 > std::numeric_limits<u32>::max()) {
        return std::nullopt;
    }
    const u32 cell_count = static_cast<u32>(cell_count_64);
    const u32 output_bound = FlatBound(info, 18, WordCount(output_sharp));
    const u32 count = std::min({(*control)[0], InvocationCount(cs_program), cell_count,
                                output_bound, WordCount(coefficient_sharp), WordCount(output_sharp),
                                WordCount(field_sharp) / 4});
    auto flags = ReadWords(rasterizer, flags_sharp, (count + 3) / 4);
    auto field = ReadWords(rasterizer, field_sharp, count * 4);
    auto coefficient = ReadWords(rasterizer, coefficient_sharp, count);
    auto output = ReadWords(rasterizer, output_sharp, count);
    if (!flags || !field || !coefficient || !output) {
        return std::nullopt;
    }

    const u32 plane = dim_x * dim_y;
    for (u32 i = 0; i < count; ++i) {
        const u32 x = i % dim_x;
        const u32 y = (i / dim_x) % dim_y;
        const u32 z = i / plane;
        if (x == 0 || x + 1 >= dim_x || y == 0 || y + 1 >= dim_y || z == 0 || z + 1 >= dim_z ||
            (((*flags)[i >> 2] >> 24) & 0xff) != 1) {
            continue;
        }

        float sum = AsFloat((*field)[4 * (i - 1) + 1]) * AsFloat((*coefficient)[i - 1]);
        sum = std::fma(AsFloat((*field)[4 * i]), AsFloat((*coefficient)[i]), sum);
        sum = std::fma(AsFloat((*field)[4 * i + 1]), AsFloat((*coefficient)[i + 1]), sum);
        sum = std::fma(AsFloat((*field)[4 * (i - dim_x) + 2]), AsFloat((*coefficient)[i - dim_x]),
                       sum);
        sum = std::fma(AsFloat((*field)[4 * i + 2]), AsFloat((*coefficient)[i + dim_x]), sum);
        sum = std::fma(AsFloat((*field)[4 * (i - plane) + 3]), AsFloat((*coefficient)[i - plane]),
                       sum);
        sum = std::fma(AsFloat((*field)[4 * i + 3]), AsFloat((*coefficient)[i + plane]), sum);
        (*output)[i] = AsBits(sum);
    }

    CpuResult result;
    result.outputs.push_back({output_sharp.base_address, std::move(*output)});
    return result;
}

[[nodiscard]] std::optional<CpuResult> Run0020(const Shader::Info& info,
                                               const AmdGpu::ComputeProgram& cs_program,
                                               Rasterizer& rasterizer) {
    if (info.buffers.size() != 3) {
        return std::nullopt;
    }
    const auto control_sharp = info.buffers[0].GetSharp(info);
    const auto source_sharp = info.buffers[1].GetSharp(info);
    const auto output_sharp = info.buffers[2].GetSharp(info);
    auto control = ReadWords(rasterizer, control_sharp, 8);
    if (!control) {
        return std::nullopt;
    }
    const u32 axis = (*control)[1];
    const u32 out_x = (*control)[2];
    const u32 out_y = (*control)[3];
    const u32 out_z = (*control)[4];
    const u32 src_x = (*control)[5];
    const u32 src_y = (*control)[6];
    const u32 src_z = (*control)[7];
    const u64 out_count_64 = static_cast<u64>(out_x) * out_y * out_z;
    const u64 src_count_64 = static_cast<u64>(src_x) * src_y * src_z;
    if (axis > 2 || out_x == 0 || out_y == 0 || out_z == 0 ||
        out_count_64 > std::numeric_limits<u32>::max() || src_count_64 > WordCount(source_sharp)) {
        return std::nullopt;
    }
    const u32 count =
        std::min({(*control)[0], InvocationCount(cs_program), static_cast<u32>(out_count_64),
                  PushedUserData(info, 0), WordCount(output_sharp)});
    auto source = ReadWords(rasterizer, source_sharp, static_cast<u32>(src_count_64));
    if (!source) {
        return std::nullopt;
    }
    std::vector<u32> output(count);
    const u32 out_plane = out_x * out_y;
    for (u32 i = 0; i < count; ++i) {
        const u32 x = i % out_x;
        const u32 y = (i / out_x) % out_y;
        const u32 z = i / out_plane;
        const u32 first = x + src_x * (y + src_y * z);
        const u32 second =
            x + static_cast<u32>(axis == 0) +
            src_x * (y + static_cast<u32>(axis == 1) + src_y * (z + static_cast<u32>(axis == 2)));
        if (first >= source->size() || second >= source->size()) {
            return std::nullopt;
        }
        const float sum = AsFloat((*source)[first]) + AsFloat((*source)[second]);
        output[i] = AsBits(sum * 0.5f);
    }

    CpuResult result;
    result.outputs.push_back({output_sharp.base_address, std::move(output)});
    return result;
}

[[nodiscard]] std::optional<CpuResult> RunTrilinear(const Shader::Info& info,
                                                    const AmdGpu::ComputeProgram& cs_program,
                                                    Rasterizer& rasterizer, bool cdd_variant) {
    if (info.buffers.size() != 11) {
        return std::nullopt;
    }
    std::array<AmdGpu::Buffer, 10> sharps;
    for (u32 i = 0; i < sharps.size(); ++i) {
        sharps[i] = info.buffers[i].GetSharp(info);
    }
    auto control = ReadWords(rasterizer, sharps[0], 8);
    if (!control) {
        return std::nullopt;
    }
    const u32 output_bound = FlatBound(info, 38, WordCount(sharps[9]));
    u32 count =
        std::min({(*control)[0], InvocationCount(cs_program), output_bound, WordCount(sharps[9])});
    for (u32 i : {1U, 2U, 3U, 4U, 5U, 7U, 8U}) {
        count = std::min(count, WordCount(sharps[i]));
    }
    if (count == 0 || (*control)[2] == 0 || (*control)[3] == 0) {
        return std::nullopt;
    }

    std::array<std::optional<std::vector<u32>>, 9> input;
    for (u32 i = 1; i <= 5; ++i) {
        input[i - 1] = ReadWords(rasterizer, sharps[i], count);
    }
    input[5] = ReadWords(rasterizer, sharps[6], WordCount(sharps[6]));
    input[6] = ReadWords(rasterizer, sharps[7], count);
    input[7] = ReadWords(rasterizer, sharps[8], count);
    input[8] = ReadWords(rasterizer, sharps[9], count);
    if (std::ranges::any_of(input, [](const auto& words) { return !words.has_value(); })) {
        return std::nullopt;
    }

    const auto& tags = *input[0];
    const auto& coord0 = *input[1];
    const auto& coord1 = *input[2];
    const auto& coord2 = *input[3];
    const auto& weight0 = *input[4];
    const auto& volume = *input[5];
    const auto& weight1 = *input[6];
    const auto& weight2 = *input[7];
    auto output = std::move(*input[8]);
    const u32 dim0 = (*control)[2];
    const u32 dim1 = (*control)[3];
    const u32 tag_match = (*control)[5];
    const u32 mode = (*control)[6];
    const bool negative_control = std::bit_cast<s32>((*control)[1]) < 0;

    for (u32 i = 0; i < count; ++i) {
        const u32 tag = (tags[i] >> 16) & 0xf;
        const bool selected = cdd_variant ? tag != mode && !(negative_control != (tag == tag_match))
                                          : !(negative_control != (tag_match == tag));
        if (!selected) {
            continue;
        }

        const u32 q0 = coord0[i];
        const u32 q1 = coord1[i];
        const u32 q2 = coord2[i];
        const u32 q0_next = q0 + 1;
        const u32 q1_base = dim1 * q1;
        const u32 q1_next = dim1 * (q1 + 1);
        const u32 base00 = dim0 * (q0 + q1_base);
        const u32 base10 = dim0 * (q0_next + q1_base);
        const u32 base01 = dim0 * (q0 + q1_next);
        const u32 base11 = dim0 * (q0_next + q1_next);
        const std::array indices{
            q2 + base00,     q2 + base10,     q2 + base01,     q2 + base11,
            q2 + 1 + base00, q2 + 1 + base10, q2 + 1 + base01, q2 + 1 + base11,
        };
        if (std::ranges::any_of(indices, [&](u32 index) { return index >= volume.size(); })) {
            return std::nullopt;
        }

        const float w0 = AsFloat(weight0[i]);
        const float w1 = AsFloat(weight1[i]);
        const float w2 = AsFloat(weight2[i]);
        const float one_minus_w0 = 1.0f - w0;
        const float w0_v101 = w0 * AsFloat(volume[indices[5]]);
        const float w0_v100 = w0 * AsFloat(volume[indices[1]]);
        const float w0_v111 = w0 * AsFloat(volume[indices[7]]);
        const float one_minus_w1 = 1.0f - w1;
        const float w0_v110 = w0 * AsFloat(volume[indices[3]]);
        const float y0_z1 = w1 * std::fma(one_minus_w0, AsFloat(volume[indices[4]]), w0_v101);
        const float low = std::fma(
            one_minus_w1, std::fma(one_minus_w0, AsFloat(volume[indices[0]]), w0_v100), y0_z1);
        const float y1_z1 = w1 * std::fma(one_minus_w0, AsFloat(volume[indices[6]]), w0_v111);
        const float high = std::fma(
            one_minus_w1, std::fma(one_minus_w0, AsFloat(volume[indices[2]]), w0_v110), y1_z1);
        const float interpolated = std::fma(w2, high, std::fma(-w2, low, low));

        if (cdd_variant) {
            output[i] = ((*control)[1] & 0x40000000U) != 0
                            ? AsBits(interpolated)
                            : AsBits(interpolated + AsFloat(output[i]));
        } else {
            output[i] = mode == 3 ? AsBits((interpolated + AsFloat(output[i])) * 0.5f)
                                  : AsBits(interpolated);
        }
    }

    CpuResult result;
    result.outputs.push_back({sharps[9].base_address, std::move(output)});
    return result;
}

[[nodiscard]] std::optional<CpuResult> BuildCpuResult(const Shader::Info& info,
                                                      const AmdGpu::ComputeProgram& cs_program,
                                                      Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case 0x15f3a569593c4c58ULL:
        return Run15f3(info, cs_program, rasterizer);
    case 0x39392c783089119fULL:
        return Run3939(info, cs_program, rasterizer);
    case 0x744d4d82942b9961ULL:
        return Run744d(info, cs_program, rasterizer);
    case 0x0020a4d78a49461cULL:
        return Run0020(info, cs_program, rasterizer);
    case 0xcdd355b7331679a5ULL:
        return RunTrilinear(info, cs_program, rasterizer, true);
    case 0x01e2c7ba10806334ULL:
        return RunTrilinear(info, cs_program, rasterizer, false);
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool WriteCpuResult(Rasterizer& rasterizer, const CpuResult& result) {
    auto* memory = Core::Memory::Instance();
    for (const auto& output : result.outputs) {
        const u64 size = output.words.size() * sizeof(u32);
        if (size == 0 || !memory->IsValidMapping(output.address, size)) {
            return false;
        }
    }
    for (const auto& output : result.outputs) {
        const u64 size = output.words.size() * sizeof(u32);
        if (!memory->TryWriteBacking(std::bit_cast<void*>(output.address), output.words.data(),
                                     size)) {
            return false;
        }
        rasterizer.GetBufferCache().NotifyCpuWrite(output.address, size);
    }
    return true;
}

void LogFailureOnce(u64 hash, std::string_view reason) {
    const size_t index = HashIndex(hash);
    if (index < logged_failures.size() && !std::exchange(logged_failures[index], true)) {
        LOG_ERROR(Render_Vulkan, "[LBP3_NG_CPU_HLE] hash={:#018x} failed: {}", hash, reason);
    }
}

} // namespace

bool ExecuteLbp3NgCpuHle(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                         Rasterizer& rasterizer, bool async_compute) {
    // This replacement is deliberately bounded to the only title revision whose six kernels
    // and runtime ABI were validated byte-for-byte. Keep an escape hatch for diagnosis and for
    // unexpected future platform regressions; all other cases use the native GPU path.
    if (EnvEnabled("SHADPS4_LBP3_DISABLE_NG_CPU_HLE") || !IsLbp3V126(async_compute) ||
        HashIndex(info.pgm_hash) >= Lbp3NgHashes.size()) {
        return false;
    }
    auto result = BuildCpuResult(info, cs_program, rasterizer);
    if (!result) {
        LogFailureOnce(info.pgm_hash, "unsupported runtime ABI or inaccessible guest buffer");
        return false;
    }
    if (!WriteCpuResult(rasterizer, *result)) {
        LogFailureOnce(info.pgm_hash, "could not publish CPU output to guest backing");
        return false;
    }

    rasterizer.MarkLbp3NgCpuHleDispatch();
    Core::PerfTelemetry::Increment(Core::PerfTelemetry::Counter::Lbp3NgCpuHleDispatches);
    return true;
}

} // namespace Vulkan
