// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string_view>

#include "common/elf_info.h"

namespace Core::Lbp3Online {

inline constexpr std::string_view SupportedSerial = "CUSA00063";
inline constexpr std::string_view SupportedAppVersion = "01.26";

inline std::atomic_bool g_enabled{false};

inline void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

[[nodiscard]] inline bool IsEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// Call only after the title metadata has been loaded by Core::Emulator.
[[nodiscard]] inline bool IsSupportedTitleVersion() {
    const auto& info = Common::ElfInfo::Instance();
    return info.IsInitialized() && info.GameSerial() == SupportedSerial &&
           info.AppVer() == SupportedAppVersion;
}

[[nodiscard]] inline bool IsSupportedTitle() {
    return IsEnabled() && IsSupportedTitleVersion();
}

} // namespace Core::Lbp3Online
