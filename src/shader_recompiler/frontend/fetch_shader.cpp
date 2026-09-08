// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include "common/assert.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"

namespace Shader::Gcn {

namespace {

constexpr size_t MaxCachedCodeWords = 1024;
constexpr size_t MaxCachedAttributes = 64;

class FetchShaderCache {
public:
    const FetchShaderData* Find(const u32* code) const {
        for (const auto& entry : sets[SetIndex(code)].entries) {
            if (entry.address != nullptr && entry.address == code &&
                MatchesCode(code, entry.code)) {
                return &entry.data;
            }
        }
        return nullptr;
    }

    void Insert(const u32* code, std::span<const u32> snapshot, const FetchShaderData& data) {
        auto& set = sets[SetIndex(code)];
        auto it = std::ranges::find(set.entries, code, &Entry::address);
        auto& entry = it != set.entries.end() ? *it : set.entries[set.next++ % Ways];
        entry.address = nullptr;
        entry.code.assign(snapshot.begin(), snapshot.end());
        entry.data = data;
        entry.address = code;
    }

private:
    static constexpr size_t NumSets = 32;
    static constexpr size_t Ways = 4;

    struct Entry {
        const u32* address{};
        std::vector<u32> code;
        FetchShaderData data;
    };
    struct Set {
        std::array<Entry, Ways> entries;
        size_t next{};
    };

    static size_t SetIndex(const u32* code) {
        // Mix aligned guest pointers instead of selecting only their low bits.
        const u64 address = reinterpret_cast<uintptr_t>(code) >> 2;
        return ((address ^ (address >> 16)) * 0x9e3779b97f4a7c15ULL) >> 59;
    }

    static bool MatchesCode(const u32* code, std::span<const u32> snapshot) {
        // A shader can be replaced by a shorter one and its following pages unmapped.
        // Compare one 4 KiB region at a time, stopping at the first changed prefix;
        // an unchanged prefix still contains the same instruction boundaries/terminator.
        constexpr size_t ComparePageBytes = 4096;
        auto* current = reinterpret_cast<const u8*>(code);
        auto* saved = reinterpret_cast<const u8*>(snapshot.data());
        size_t remaining = snapshot.size_bytes();
        while (remaining != 0) {
            const size_t page_remaining =
                ComparePageBytes - (reinterpret_cast<uintptr_t>(current) % ComparePageBytes);
            const size_t size = std::min(remaining, page_remaining);
            if (std::memcmp(current, saved, size) != 0) {
                return false;
            }
            current += size;
            saved += size;
            remaining -= size;
        }
        return true;
    }

    // At most 128 entries, each with <= 4 KiB of code and 64 attributes.
    std::array<Set, NumSets> sets;
};

} // namespace

/**
 * s_load_dwordx4 s[8:11], s[2:3], 0x00
 * s_load_dwordx4 s[12:15], s[2:3], 0x04
 * s_load_dwordx4 s[16:19], s[2:3], 0x08
 * s_waitcnt     lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
 * buffer_load_format_xyz v[8:10], v0, s[12:15], 0 idxen
 * buffer_load_format_xy v[12:13], v0, s[16:19], 0 idxen
 * s_waitcnt     0
 * s_setpc_b64   s[0:1]

 * s_load_dwordx4  s[4:7], s[2:3], 0x0
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[4:7], v0, s[4:7], 0 idxen
 * s_load_dwordx4  s[4:7], s[2:3], 0x8
 * s_waitcnt       lgkmcnt(0)
 * buffer_load_format_xyzw v[8:11], v0, s[4:7], 0 idxen
 * s_waitcnt       vmcnt(0) & expcnt(0) & lgkmcnt(0)
 * s_setpc_b64     s[0:1]

 * A normal fetch shader looks like the above, the instructions are generated
 * using input semantics on cpu side. Load instructions can either be separate or interleaved
 * We take the reverse way, extract the original input semantics from these instructions.
 **/

static bool IsTypedBufferLoad(const Gcn::GcnInst& inst) {
    return inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_X ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XY ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZ ||
           inst.opcode == Opcode::TBUFFER_LOAD_FORMAT_XYZW;
}

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base) {
    const u32* code;
    std::memcpy(&code, &info.user_data[sgpr_base], sizeof(code));
    return code;
}

std::optional<FetchShaderData> ParseFetchShader(const Shader::Info& info) {
    if (!info.has_fetch_shader) {
        return std::nullopt;
    }

    const auto* code = GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
    // The GPU command processor and asynchronous compiler own independent caches.
    // Only decoded instructions are reused; vertex descriptors and runtime state
    // continue to be read by StageSpecialization for each draw.
    thread_local FetchShaderCache cache;
    if (const auto* data = cache.Find(code)) {
        return *data;
    }

    std::array<u32, MaxCachedCodeWords> snapshot;
    FetchShaderData data{};
    GcnCodeSlice code_slice(code, code + std::numeric_limits<u32>::max(), snapshot);
    GcnDecodeContext decoder;

    struct VsharpLoad {
        u32 dword_offset{};
        u32 base_sgpr{};
    };
    std::array<VsharpLoad, 104> loads{};

    u32 semantic_index = 0;
    while (!code_slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(code_slice);
        data.size += inst.length;

        if (inst.opcode == Opcode::S_SETPC_B64) {
            if (data.size <= sizeof(snapshot) && data.attributes.size() <= MaxCachedAttributes) {
                cache.Insert(code, std::span{snapshot}.first(data.size / sizeof(u32)), data);
            }
            break;
        }

        if (inst.inst_class == InstClass::ScalarMemRd) {
            loads[inst.dst[0].code] = VsharpLoad{inst.control.smrd.offset, inst.src[0].code * 2};
            continue;
        }

        if (inst.opcode == Opcode::V_ADD_I32) {
            const auto vgpr = inst.dst[0].code;
            const auto sgpr = s8(inst.src[0].code);
            switch (vgpr) {
            case 0: // V0 is always the vertex offset
                data.vertex_offset_sgpr = sgpr;
                break;
            case 3: // V3 is always the instance offset
                data.instance_offset_sgpr = sgpr;
                break;
            default:
                UNREACHABLE();
            }
        }

        if (inst.inst_class == InstClass::VectorMemBufFmt) {
            // SRSRC is in units of 4 SPGRs while SBASE is in pairs of SGPRs
            const u32 base_sgpr = inst.src[2].code * 4;
            auto& attrib = data.attributes.emplace_back();
            attrib.semantic = semantic_index++;
            attrib.dest_vgpr = inst.src[1].code;
            attrib.num_elements = inst.control.mubuf.count;
            attrib.sgpr_base = loads[base_sgpr].base_sgpr;
            attrib.dword_offset = loads[base_sgpr].dword_offset;
            attrib.inst_offset = inst.control.mtbuf.offset;
            attrib.instance_data = inst.src[0].code;
            if (IsTypedBufferLoad(inst)) {
                attrib.data_format = inst.control.mtbuf.dfmt;
                attrib.num_format = inst.control.mtbuf.nfmt;
            }
        }
    }

    return data;
}

} // namespace Shader::Gcn
