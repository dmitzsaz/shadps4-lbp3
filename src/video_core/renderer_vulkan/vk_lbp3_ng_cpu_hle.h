// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace AmdGpu {
struct ComputeProgram;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// Executes the six byte-validated LBP3 1.26 NG simulation kernels against guest backing.
/// Returns true only when the native dispatch was fully replaced. Set
/// SHADPS4_LBP3_DISABLE_NG_CPU_HLE=1 to force the native fallback for diagnosis.
[[nodiscard]] bool ExecuteLbp3NgCpuHle(const Shader::Info& info,
                                       const AmdGpu::ComputeProgram& cs_program,
                                       Rasterizer& rasterizer, bool async_compute);

} // namespace Vulkan
