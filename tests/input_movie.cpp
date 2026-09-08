// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "input/input_movie.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace Input::Movie;
using namespace Libraries::Pad;
using namespace std::chrono_literals;
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(3); }
}
int main(int argc, char** argv) {
    Require(argc == 2, "test output path required");
    const std::filesystem::path out{argv[1]};
    std::string error;
    const auto recording = out / "record";
    OrbisPadData pad{};
    pad.connected = true;
    pad.connectedCount = 3;
    pad.leftStick = pad.rightStick = {128, 128};
    pad.orientation = {0, 0, 0, 1};
    {
        Session record;
        Require(record.OpenRecord(recording, error), error.c_str());
        record.Record(0, {&pad, 1});
        for (unsigned i = 0; i < 100; ++i) { ++pad.timestamp; record.Record(0, {&pad, 1}); }
        record.Flip(); // frame 1: press
        pad.buttons = OrbisPadButtonDataOffset::Cross;
        record.Record(0, {&pad, 1});
        record.Flip(); // frame 2: preserve both edges inside one frame
        pad.buttons = OrbisPadButtonDataOffset::None;
        record.Record(0, {&pad, 1});
        pad.buttons = OrbisPadButtonDataOffset::Square;
        record.Record(0, {&pad, 1});
        pad.buttons = OrbisPadButtonDataOffset::None;
        record.Record(0, {&pad, 1});
        OrbisPadData other = pad;
        other.connectedCount = 1;
        other.leftStick.x = 250;
        other.analogButtons.l2 = 92;
        other.touchData.touchNum = 2;
        other.touchData.touch[0] = {.x = 341, .y = 72, .id = 9};
        other.touchData.touch[1] = {.x = 542, .y = 372, .id = 10};
        other.acceleration = {1, 2, 3};
        other.angularVelocity = {4, 5, 6};
        other.orientation = {0, 1, 0, 0};
        record.Record(2, {&other, 1});
        record.Flip(); // frame 3: a held axis, even if the recording ends while held
        pad.leftStick.x = 255;
        pad.buttons = OrbisPadButtonDataOffset::R1;
        record.Record(0, {&pad, 1});
        record.Flip(); // end frame 4
        record.Stop();
    }
    const auto movie = recording / "controller.bin";
    Require(std::filesystem::file_size(movie) == sizeof(Header) + 12 * sizeof(Event), "wire/dedup count");
    for (bool slow : {false, true}) {
        Session replay;
        Require(replay.OpenReplay(movie, out / (slow ? "slow" : "fast"), {}, error), error.c_str());
        bool connected{}; u8 count{};
        Require(replay.Connection(0, connected, count) && connected && count == 3, "virtual connection");
        std::array<OrbisPadData, 64> values{};
        Require(replay.Read(0, values.data(), 64, false, 1000) == 1, "frame zero");
        Require(replay.Read(0, values.data(), 64, false, 1001) == 0, "duplicate read consumed future input");
        replay.Flip();
        if (slow) std::this_thread::sleep_for(50ms);
        Require(replay.Read(0, values.data(), 1, true, 1100) == 1 && values[0].buttons == OrbisPadButtonDataOffset::Cross, "press frame");
        Require(replay.Read(0, values.data(), 1, false, 1101) == 1, "ReadState consumed buffered Read history");
        replay.Flip();
        for (auto expected : {OrbisPadButtonDataOffset::None, OrbisPadButtonDataOffset::Square,
                              OrbisPadButtonDataOffset::None}) {
            Require(replay.Read(0, values.data(), 1, true, 1200) == 1 && values[0].buttons == expected,
                    "ReadState lost an in-frame tap");
        }
        Require(replay.Read(0, values.data(), 1, false, 1200) == 1 && values[0].buttons == OrbisPadButtonDataOffset::None, "release");
        Require(replay.Read(0, values.data(), 64, false, 1200) == 2 && values[0].buttons == OrbisPadButtonDataOffset::Square && values[1].buttons == OrbisPadButtonDataOffset::None, "in-frame tap/capacity truncation");
        Require(values[0].timestamp < values[1].timestamp, "timestamps did not rebase monotonically");
        Require(replay.Read(2, values.data(), 1, true, 1200) == 1, "second virtual controller");
        Require(values[0].leftStick.x == 250 && values[0].analogButtons.l2 == 92 &&
            values[0].touchData.touchNum == 2 && values[0].touchData.touch[1].x == 542 &&
            values[0].touchData.touch[0].id == 9 && values[0].acceleration.z == 3 &&
            values[0].angularVelocity.y == 5 && values[0].orientation.y == 1, "touch/motion/axis roundtrip");
        replay.Flip();
        Require(replay.Read(0, values.data(), 1, true, 1300) == 1 && values[0].leftStick.x == 255, "axis frame");
        replay.Flip();
        Require(replay.Read(0, values.data(), 1, true, 1400) == 1 && values[0].leftStick.x == 128 &&
            values[0].buttons == OrbisPadButtonDataOffset::None, "EOF did not release held controls");
        Require(replay.Read(0, values.data(), 0, false, 1400) == 0, "invalid capacity");
    }
    for (const auto name : {"truncated", "corrupt"}) {
        const auto bad = out / name;
        std::filesystem::copy_file(movie, bad);
        if (std::string_view{name} == "truncated") std::filesystem::resize_file(bad, std::filesystem::file_size(bad) - 1);
        else {
            std::fstream file{bad, std::ios::binary | std::ios::in | std::ios::out};
            file.seekp(sizeof(Header) + 24); file.put(42);
        }
        Session replay;
        Require(!replay.OpenReplay(bad, out / "bad-replay", {}, error), "damaged movie accepted");
    }
    {
        Session capped;
        Require(capped.OpenRecord(out / "capped", error, 512), error.c_str());
        for (unsigned i = 0; i < 10; ++i) { capped.Flip(); capped.Record(0, {&pad, 1}); }
        capped.Stop();
        Require(std::filesystem::file_size(out / "capped/controller.bin") <= 512, "file cap exceeded");
        Session replay;
        Require(!replay.OpenReplay(out / "capped/controller.bin", out / "bad-cap", {}, error), "partial capped movie accepted");
    }
    {
        std::atomic<unsigned> quit{}, screenshots{};
        Session replay;
        replay.SetScreenshotCallback([&] { ++screenshots; });
        Require(replay.OpenReplay(movie, out / "stop", [&] { ++quit; }, error), error.c_str());
        std::ofstream{out / "stop/screenshot"}.put('1');
        std::ofstream{out / "stop/stop"}.put('1');
        for (unsigned i = 0; i < 100 && !quit; ++i) std::this_thread::sleep_for(10ms);
        Require(quit == 1 && screenshots == 1, "automatic screenshot/quit command");
        OrbisPadData neutral;
        replay.Read(0, &neutral, 1, true, 1);
        Require(neutral.buttons == OrbisPadButtonDataOffset::None, "stop did not neutralize input");
    }
    {
        Session overflowing;
        Require(overflowing.OpenRecord(out / "overflow", error), error.c_str());
        for (unsigned i = 0; i < 20000; ++i) {
            pad.leftStick.x = i % 256;
            overflowing.Record(0, {&pad, 1});
        }
        overflowing.Stop();
        std::ifstream status{out / "overflow/status.json"};
        std::string text{std::istreambuf_iterator<char>{status}, {}};
        Require(text.find("\"error_code\":3") != std::string::npos, "queue overflow was not reported");
        Session replay;
        Require(!replay.OpenReplay(out / "overflow/controller.bin", out / "bad-overflow", {}, error),
                "lossy movie accepted for replay");
    }
    std::puts("PASS: frame replay at different wall speeds, taps, buffered/latest reads, virtual pads, touch/gyro, timestamps, EOF release, checksum/truncation, byte cap and automatic screenshot/quit");
}
