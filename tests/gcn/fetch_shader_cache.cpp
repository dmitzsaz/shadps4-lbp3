// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// Standalone parser tests do not start the emulator or its logging subsystem.
namespace Common {
std::string GetCurrentThreadName() {
    return "FetchShaderTest";
}
namespace Log {
std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;
}
} // namespace Common

void assert_fail_impl() {
    throw std::runtime_error("decoder assertion");
}
[[noreturn]] void unreachable_impl() {
    throw std::runtime_error("decoder unreachable");
}

#ifdef FETCH_SHADER_CACHE_REFERENCE
namespace Shader::Gcn {
std::optional<FetchShaderData> ParseFetchShaderReference(const Shader::Info& info);
}
#endif

namespace {
using Shader::Gcn::FetchShaderData;
using Shader::Gcn::ParseFetchShader;
constexpr u32 Nop = 0xbf800000;
constexpr u32 Return = 0xbe802000; // s_setpc_b64 s[0:1]

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// GCN encodings for interleaved scalar descriptor loads and formatted vertex loads.
std::vector<u32> MakeCode(u32 attributes, bool typed = false, u32 variant = 0) {
    std::vector<u32> code{(37u << 25) | (variant % 8 + 4),
                          (37u << 25) | (3u << 17) | (3u << 9) | 5u};
    for (u32 i = 0; i < attributes; ++i) {
        code.push_back(0xc0000000u | (2u << 22) | (8u << 15) | (1u << 9) | (1u << 8) |
                       ((i * 4 + variant * 4) % 128));
        const u32 count = (i + variant) % 4;
        const u32 offset = (i + variant) % 32;
        code.push_back(typed ? 0xe8000000u | (count << 16) | (7u << 19) | ((variant % 7) << 23) |
                                   (1u << 13) | offset
                             : 0xe0000000u | (count << 18) | (1u << 13) | offset);
        code.push_back((128u << 24) | (2u << 16) | ((4 + i * 4) << 8) | (i % 4));
    }
    code.push_back(Return);
    return code;
}

struct Fixture {
    std::vector<u32> code;
    std::array<u32, 16> user_data{};
    std::array<AmdGpu::Buffer, 32> buffers{};
    Shader::Info info;

    explicit Fixture(u32 attributes = 3, bool typed = false, u32 variant = 0)
        : code{MakeCode(attributes, typed, variant)} {
        info.stage = Shader::Stage::Vertex;
        info.l_stage = Shader::LogicalStage::Vertex;
        info.has_fetch_shader = true;
        info.fetch_shader_sgpr_base = 8;
        info.user_data = user_data;
        SetCode(code.data());
        const auto* ptr = buffers.data();
        std::memcpy(&user_data[2], &ptr, sizeof(ptr));
    }

    void SetCode(const u32* ptr) {
        std::memcpy(&user_data[8], &ptr, sizeof(ptr));
    }
};

void CheckData(const FetchShaderData& data, u32 count, bool typed, u32 variant) {
    Check(data.size == (3 + 3 * count) * sizeof(u32), "consumed shader size");
    Check(data.attributes.size() == count, "attribute count");
    Check(data.vertex_offset_sgpr == variant % 8 + 4, "vertex offset register");
    Check(data.instance_offset_sgpr == 5, "instance offset register");
    for (u32 i = 0; i < count; ++i) {
        const auto& a = data.attributes[i];
        Check(a.semantic == i && a.dest_vgpr == 4 + i * 4, "semantic and destination");
        Check(a.sgpr_base == 2 && a.dword_offset == (i * 4 + variant * 4) % 128,
              "descriptor location");
        Check(a.num_elements == (i + variant) % 4 + 1, "vertex component count");
        Check(a.instance_data == i % 4, "instance rate");
        Check(a.inst_offset == (i + variant) % 32, "instruction offset");
        Check(a.data_format == (typed ? 7 : 0), "typed data format");
        Check(a.num_format == (typed ? variant % 7 : 0), "typed number format");
    }
}

void CheckSame(const FetchShaderData& a, const FetchShaderData& b) {
    // The production equality operator intentionally omits some fields; compare all of them here.
    Check(a.size == b.size && a.vertex_offset_sgpr == b.vertex_offset_sgpr &&
              a.instance_offset_sgpr == b.instance_offset_sgpr &&
              a.attributes.size() == b.attributes.size(),
          "reference header mismatch");
    for (size_t i = 0; i < a.attributes.size(); ++i) {
        const auto& x = a.attributes[i];
        const auto& y = b.attributes[i];
        Check(x == y && x.inst_offset == y.inst_offset && x.data_format == y.data_format &&
                  x.num_format == y.num_format,
              "reference attribute mismatch");
    }
}

void TestColdAndWarm() {
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        CheckData(*ParseFetchShader(f.info), 3, false, 0);
    }
    auto owned = *ParseFetchShader(f.info);
    owned.attributes[0].dest_vgpr = 99;
    CheckData(*ParseFetchShader(f.info), 3, false, 0);
    f.info.has_fetch_shader = false;
    f.info.user_data = {};
    Check(!ParseFetchShader(f.info), "no-fetch stage must not read user data");
}

void TestSameAddressChanges() {
    Fixture f(8);
    const auto* address = f.code.data();
    for (u32 variant = 0; variant < 1000; ++variant) {
        const bool typed = variant % 2;
        const auto changed = MakeCode(8, typed, variant);
        std::ranges::copy(changed, f.code.begin());
        Check(f.code.data() == address, "mutation must preserve guest address");
        const auto result = *ParseFetchShader(f.info);
        CheckData(result, 8, typed, variant);
        CheckSame(result, *ParseFetchShader(f.info));
#ifdef FETCH_SHADER_CACHE_REFERENCE
        CheckSame(result, *Shader::Gcn::ParseFetchShaderReference(f.info));
#endif
    }
    f.code[0] = Return;
    auto short_result = *ParseFetchShader(f.info);
    Check(short_result.size == 4 && short_result.attributes.empty(), "shortened shader");
    const auto restored = MakeCode(8);
    std::ranges::copy(restored, f.code.begin());
    CheckData(*ParseFetchShader(f.info), 8, false, 0);
}

void TestLiveDescriptorsAndPointers() {
    Fixture f;
    for (u32 i = 0; i < 100; ++i) {
        f.buffers[0].base_address = 0x10000 + i * 256;
        const auto data = *ParseFetchShader(f.info);
        Check(data.attributes[0].GetSharp(f.info).base_address == 0x10000 + i * 256,
              "cached instructions must read current descriptor contents");
    }
    std::array<AmdGpu::Buffer, 32> other{};
    other[0].base_address = 0x123400;
    const auto* ptr = other.data();
    std::memcpy(&f.user_data[2], &ptr, sizeof(ptr));
    Check(ParseFetchShader(f.info)->attributes[0].GetSharp(f.info).base_address == 0x123400,
          "cached instructions must read current descriptor table pointer");
    auto second_code = MakeCode(6, true, 3);
    f.SetCode(second_code.data());
    CheckData(*ParseFetchShader(f.info), 6, true, 3);
    f.SetCode(f.code.data());
    CheckData(*ParseFetchShader(f.info), 3, false, 0);
}

void TestDecoderSnapshot() {
    // A literal equal to the return instruction must not terminate the parser.
    Fixture f;
    f.code.insert(f.code.begin(), {0xbe9403ff, Return}); // s_mov_b32 s20, literal
    f.SetCode(f.code.data());
    const auto result = *ParseFetchShader(f.info);
    Check(result.size == f.code.size() * 4 && result.attributes.size() == 3, "literal decoding");
    CheckSame(result, *ParseFetchShader(f.info));

    std::array<u32, 3> source{0xbe9403ff, Return, Return};
    std::array<u32, 3> snapshot{};
    Shader::Gcn::GcnCodeSlice slice(source.data(), source.data() + source.size(), snapshot);
    Check(slice.at(0) == source[0], "peek value");
    source[0] = Nop;
    Shader::Gcn::GcnDecodeContext decoder;
    const auto inst = decoder.decodeInstruction(slice);
    Check(inst.length == 8 && snapshot[0] == 0xbe9403ff && snapshot[1] == Return,
          "decoder and cache must use the same captured words");
}

void TestGuardPage() {
#if defined(__unix__) || defined(__APPLE__)
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* mapping = static_cast<u8*>(
        mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    Check(mapping != MAP_FAILED, "guard-page mmap");
    auto* code = reinterpret_cast<u32*>(mapping + page - 16);
    std::fill_n(code, 4, Nop);
    const auto body = MakeCode(3);
    std::ranges::copy(body, code + 4);
    Fixture f;
    f.SetCode(code);
    const auto first = *ParseFetchShader(f.info);
    Check(first.size == (body.size() + 4) * 4, "shader across page boundary");
    CheckSame(first, *ParseFetchShader(f.info));
    // The old shader spans both pages. The replacement ends at the first page boundary.
    code[3] = Return;
    Check(mprotect(mapping + page, page, PROT_NONE) == 0, "protect removed shader tail");
    const auto shortened = *ParseFetchShader(f.info);
    Check(shortened.size == 16 && shortened.attributes.empty(), "unmapped old shader tail");
    CheckSame(shortened, *ParseFetchShader(f.info));
    Check(mprotect(mapping + page, page, PROT_READ | PROT_WRITE) == 0, "restore tail page");
    code[3] = Nop;
    CheckSame(first, *ParseFetchShader(f.info));
    Check(munmap(mapping, page * 2) == 0, "guard-page munmap");
#endif
}

void TestBoundedCacheAndLongShader() {
    std::vector<std::unique_ptr<Fixture>> fixtures;
    for (u32 i = 0; i < 600; ++i) {
        auto f = std::make_unique<Fixture>(1 + i % 16, i % 2, i);
        CheckData(*ParseFetchShader(f->info), 1 + i % 16, i % 2, i);
        fixtures.push_back(std::move(f));
    }
    for (u32 i = 0; i < fixtures.size(); ++i) {
        CheckData(*ParseFetchShader(fixtures[i]->info), 1 + i % 16, i % 2, i);
    }
    Fixture long_shader;
    long_shader.code.insert(long_shader.code.begin(), 1200, Nop);
    long_shader.SetCode(long_shader.code.data());
    for (int i = 0; i < 3; ++i) {
        const auto result = *ParseFetchShader(long_shader.info);
        Check(result.size == long_shader.code.size() * 4 && result.attributes.size() == 3,
              "oversized shaders must still decode without caching");
    }
}

void TestCompilerThreads() {
    const auto shared_code = MakeCode(8, true, 2);
    std::atomic<bool> failed{};
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            try {
                Fixture f;
                f.SetCode(shared_code.data());
                for (u32 iteration = 0; iteration < 2000; ++iteration) {
                    CheckData(*ParseFetchShader(f.info), 8, true, 2);
                }
            } catch (...) {
                failed = true;
            }
        });
    }
    workers.clear();
    Check(!failed, "independent GPU/async compiler thread caches");
}

#ifdef FETCH_SHADER_CACHE_REFERENCE
void Benchmark() {
    using Parser = std::optional<FetchShaderData> (*)(const Shader::Info&);
    for (u32 attributes : {3u, 8u, 16u}) {
        Fixture f(attributes);
        constexpr u32 Iterations = 200000;
        const auto time = [&](Parser parser) {
            const auto start = std::chrono::steady_clock::now();
            size_t checksum{};
            for (u32 i = 0; i < Iterations; ++i) {
                const auto result = parser(f.info);
                checksum += result->size + result->attributes.back().dest_vgpr;
            }
            Check(checksum != 0, "benchmark checksum");
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() -
                                                            start)
                       .count() /
                   Iterations;
        };
        std::vector<double> before, after;
        for (int run = 0; run < 5; ++run) {
            if (run % 2 == 0) {
                before.push_back(time(Shader::Gcn::ParseFetchShaderReference));
                after.push_back(time(ParseFetchShader));
            } else {
                after.push_back(time(ParseFetchShader));
                before.push_back(time(Shader::Gcn::ParseFetchShaderReference));
            }
        }
        std::ranges::sort(before);
        std::ranges::sort(after);
        std::cout << "BENCH attributes=" << attributes << " original_ns=" << before[2]
                  << " cached_ns=" << after[2] << " speedup=" << before[2] / after[2] << '\n';
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        const std::pair<const char*, void (*)()> tests[] = {
            {"cold/warm lookup, owned results, no-fetch stage", TestColdAndWarm},
            {"same-address mutation, typed formats, shortening and extension",
             TestSameAddressChanges},
            {"live vertex descriptors and changed shader pointers", TestLiveDescriptorsAndPointers},
            {"literal instructions and consistent decoder snapshot", TestDecoderSnapshot},
            {"shortened shader with unmapped tail page", TestGuardPage},
            {"eviction and oversized uncached shaders", TestBoundedCacheAndLongShader},
            {"parallel compiler threads", TestCompilerThreads},
        };
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS " << name << '\n';
        }
#ifdef FETCH_SHADER_CACHE_REFERENCE
        if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
            Benchmark();
        }
#endif
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    return 0;
}
