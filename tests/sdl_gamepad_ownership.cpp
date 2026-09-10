// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the actual backend to exercise its private handle list without a
// graphics window or guest runtime. No SDL controller APIs are mocked.
#ifndef SHADPS4_SDL_BACKEND_SOURCE
#define SHADPS4_SDL_BACKEND_SOURCE "../src/imgui/renderer/imgui_impl_sdl3.cpp"
#endif
#include SHADPS4_SDL_BACKEND_SOURCE

extern void Check(bool, const char*);
void TestGamepadBackend(SDL_JoystickID first, int count) {
    using namespace ImGui::Sdl;
    SdlData backend{};
    auto& io = ImGui::GetIO();
    io.BackendPlatformUserData = &backend;
    SDL_Gamepad* owned_by_game = SDL_GetGamepadFromID(first);
    Check(owned_by_game != nullptr, "game owns a handle before backend opens");
    backend.gamepad_mode = ImGui_ImplSDL3_GamepadMode_AutoFirst;
    for (int n = 0; n < 12; ++n) {
        backend.want_update_gamepads_list = true;
        UpdateGamepads();
        Check(backend.gamepads.Size == 1, "backend refresh must not accumulate borrowed handles");
    }
    CloseGamepads();
    Check(SDL_GetGamepadFromID(first) == owned_by_game && SDL_GamepadConnected(owned_by_game),
          "backend close must not close gameplay handle");
    backend.gamepad_mode = ImGui_ImplSDL3_GamepadMode_AutoAll;
    backend.want_update_gamepads_list = true;
    UpdateGamepads();
    Check(backend.gamepads.Size == count, "backend opens exactly the connected devices");
    CloseGamepads();
    Check(SDL_GamepadConnected(owned_by_game), "all-mode cleanup preserves gameplay handle");
    io.BackendPlatformUserData = nullptr;
}
