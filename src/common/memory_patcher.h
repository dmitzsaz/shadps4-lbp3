// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

namespace MemoryPatcher {

extern EXPORT uintptr_t g_eboot_address;
extern uint64_t g_eboot_image_size;
extern std::string g_game_serial;
extern std::string patch_file;
extern bool g_lbp3_patch_prize_bubbles;
extern bool g_lbp3_disable_sprite_lights;
extern bool g_lbp3_disable_tone_map;

struct Lbp3DirectLevelTarget {
    uint32_t slot_type{};
    uint32_t slot_id{};
    uint32_t adventure_type{};
    uint32_t adventure_id{};
};

extern std::optional<Lbp3DirectLevelTarget> g_lbp3_direct_level;

bool ConfigureLbp3DirectLevel(std::string_view spec);
std::string GetLbp3DirectLevelSpec();

enum PatchMask : uint8_t {
    None,
    Mask,
    Mask_Jump32,
};

struct patchInfo {
    std::string gameSerial;
    std::string modNameStr;
    std::string offsetStr;
    std::string valueStr;
    std::string targetStr;
    std::string sizeStr;
    bool isOffset;
    bool littleEndian;
    PatchMask patchMask;
    int maskOffset;
};

std::string convertValueToHex(const std::string type, const std::string valueStr);

void OnGameLoaded();
void AddPatchToQueue(const patchInfo& patchToAdd);

void PatchMemory(const patchInfo& patch);

static std::vector<int32_t> PatternToByte(const std::string& pattern);
uintptr_t PatternScan(const std::string& signature);

} // namespace MemoryPatcher
