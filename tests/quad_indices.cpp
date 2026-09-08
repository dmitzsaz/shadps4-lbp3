// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <numeric>
#include <vector>
#include "video_core/buffer_cache/quad_indices.h"

template <typename Index>
void TestIndices() {
    // Non-sequential indices, shared vertices and a degenerate second quad.
    // Include the largest index: with restart disabled it is an ordinary index.
    const Index max = std::numeric_limits<Index>::max();
    const std::array<Index, 8> input{7, max, 42, 3, 42, 3, 3, 42};
    std::array<Index, 14> storage{};
    std::copy(input.begin(), input.end(), storage.begin());
    storage[12] = 91;
    storage[13] = 92;
    const auto count = VideoCore::ExpandQuadIndicesInPlace(std::span(storage).first(12), 8);
    assert(count == 12);
    const std::array<Index, 14> expected{max, 42, 7, 42, 3, 7, 3, 3, 42, 3, 42, 42, 91, 92};
    assert(storage == expected);

    // An enabled restart flag without a marker is equivalent to restart disabled.
    std::array<Index, 6> no_marker{20, 21, 22, 23, 0, 0};
    assert(VideoCore::ExpandQuadIndicesInPlace(std::span<Index>(no_marker), 4,
                                               std::optional<Index>(max)) == 6);
    assert((no_marker == std::array<Index, 6>{21, 22, 20, 22, 23, 20}));
    for (u32 marker = 0; marker < 8; ++marker) {
        std::array<Index, 12> with_marker{10, 11, 12, 13, 14, 15, 16, 17, 0, 0, 0, 0};
        with_marker[marker] = max;
        const auto original = with_marker;
        assert(VideoCore::ExpandQuadIndicesInPlace(std::span<Index>(with_marker), 8,
                                                   std::optional<Index>(max)) == 0);
        assert(with_marker == original);
    }

    // Empty and incomplete patches must not emit a triangle or write the destination.
    for (u32 n = 0; n < 4; ++n) {
        const auto before = storage;
        assert(VideoCore::ExpandQuadIndicesInPlace(std::span<Index>(storage), n) == 0);
        assert(storage == before);
    }
    for (u32 n = 4; n < 8; ++n) {
        std::array<Index, 8> values{10, 11, 12, 13, 14, 15, 16, 17};
        assert(VideoCore::ExpandQuadIndicesInPlace(std::span(values).first(6), n) == 6);
        assert((values == std::array<Index, 8>{11, 12, 10, 12, 13, 10, 16, 17}));
    }
    const auto before = storage;
    assert(VideoCore::ExpandQuadIndicesInPlace(std::span(storage).first(11), 8) == 0);
    assert(VideoCore::ExpandQuadIndicesInPlace(std::span<Index>(storage), UINT32_MAX) == 0);
    assert(storage == before);

    // Exercise enough overlapping groups to detect a forward expansion corrupting
    // the next source quad. No vertex/base-instance offset belongs in this buffer.
    for (u32 quads : {1U, 2U, 3U, 127U, 4096U}) {
        std::vector<Index> values(quads * 6);
        for (u32 i = 0; i < quads * 4; ++i) values[i] = static_cast<Index>(i * 13 + 5);
        const auto source = values;
        assert(VideoCore::ExpandQuadIndicesInPlace(std::span(values), quads * 4) == quads * 6);
        for (u32 q = 0; q < quads; ++q) {
            const auto a = source[q * 4], b = source[q * 4 + 1];
            const auto c = source[q * 4 + 2], d = source[q * 4 + 3];
            assert(values[q * 6 + 2] == a && values[q * 6 + 5] == a);
            assert(values[q * 6] == b && values[q * 6 + 1] == c);
            assert(values[q * 6 + 3] == c && values[q * 6 + 4] == d);
        }
    }
}

int main() {
    TestIndices<u16>();
    TestIndices<u32>();
    assert(VideoCore::QuadListIndexCount(UINT32_MAX) == 6442450938ULL);
    std::cout << "PASS: 16/32-bit indexed quads, shared/degenerate vertices, provoking vertex, "
                 "overlapping expansion, trailing vertices, capacity and overflow\n";
}
