// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "input/profile_menu.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <SDL3/SDL.h>
#include <imgui.h>
#include "common/singleton.h"
#include "core/user_settings.h"
#include "imgui/imgui_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"

namespace Input::Profiles {
namespace {
struct Profile {
    std::string name;
    std::string controller;
    SDL_JoystickID device{};
    bool available{};
};
std::mutex mutex;
std::array<Profile, 4> profiles;
SDL_JoystickID selected_device{};
u8 selected_profile{};
bool open{};
int stick_direction{};

enum Action { SelectDevice = 1, SelectProfile, Assign, Close };
u32 CommandEvent() {
    static const u32 type = SDL_RegisterEvents(1);
    return type;
}
void Send(Action action, SDL_JoystickID device = 0, u8 slot = 0) {
    SDL_Event event{};
    event.type = CommandEvent();
    event.user.code = action;
    event.user.data1 = reinterpret_cast<void*>(uintptr_t(device));
    event.user.data2 = reinterpret_cast<void*>(uintptr_t(slot));
    SDL_PushEvent(&event);
}
void CloseMenu() {
    ReleaseAllInputs();
    std::lock_guard lock(mutex);
    if (!open) return;
    open = false;
    stick_direction = 0;
    ImGui::Core::ReleaseGamepadInputCapture();
}
void MoveProfile(int direction) {
    for (int n = 0; n < 4; ++n) {
        selected_profile = (selected_profile + direction + 4) % 4;
        if (profiles[selected_profile].available) break;
    }
}
void MoveDevice(int direction) {
    auto it = std::ranges::find(profiles, selected_device, &Profile::device);
    int index = it == profiles.end() ? 0 : int(it - profiles.begin());
    for (int n = 0; n < 4; ++n) {
        index = (index + direction + 4) % 4;
        if (profiles[index].device) {
            selected_device = profiles[index].device;
            selected_profile = index;
            break;
        }
    }
}
class Menu final : public ImGui::Layer {
public:
    void Draw() override {
        std::lock_guard lock(mutex);
        if (!open) return;
        const auto& io = ImGui::GetIO();
        const float width = std::min(660.0f, std::max(300.0f, io.DisplaySize.x - 32.0f));
        ImGui::SetNextWindowPos({io.DisplaySize.x / 2, io.DisplaySize.y / 2},
                               ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints({width, 0}, {width, io.DisplaySize.y - 32});
        ImGui::SetNextWindowBgAlpha(1);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
        // Pad/keyboard navigation is handled by device-aware SDL events, so a
        // different controller cannot accidentally activate ImGui's global focus.
        if (ImGui::Begin("Профили и контроллеры", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoMove)) {
            ImGui::TextWrapped("Выберите контроллер и профиль для него.");
            for (u8 i = 0; i < profiles.size(); ++i) {
                const auto& profile = profiles[i];
                if (!profile.device) continue;
                ImGui::PushID(i);
                const std::string label = "Контроллер " + std::to_string(i + 1);
                if (ImGui::Selectable(label.c_str(), selected_device == profile.device))
                    Send(SelectDevice, profile.device);
                if (selected_device == profile.device)
                    ImGui::TextWrapped("%s", profile.controller.c_str());
                ImGui::PopID();
            }
            if (!selected_device) ImGui::TextWrapped("Подключите контроллер, чтобы назначить профиль.");
            ImGui::Separator();
            for (u8 i = 0; i < profiles.size(); ++i) {
                const auto& profile = profiles[i];
                ImGui::PushID(100 + i);
                ImGui::BeginDisabled(!profile.available || !selected_device);
                const std::string label = std::to_string(i + 1) + ". " +
                    (profile.available ? profile.name : "Профиль не настроен");
                if (ImGui::Selectable(label.c_str(), selected_profile == i)) Send(SelectProfile, 0, i);
                if (profile.device) ImGui::TextDisabled("%s", profile.device == selected_device
                    ? "Назначен этому контроллеру" : "Занят — контроллеры поменяются местами");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!selected_device || !profiles[selected_profile].available);
            if (ImGui::Button("Назначить")) Send(Assign, selected_device, selected_profile);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Готово")) Send(Close);
            ImGui::TextWrapped("←/→: контроллер  ·  ↑/↓: профиль\n"
                               "✕ / A: назначить  ·  ○ / B: закрыть\n"
                               "PS / Xbox или F2: открыть меню");
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
} menu;
} // namespace

bool IsOpen() {
    std::lock_guard lock(mutex);
    return open;
}
void Refresh() {
    auto& controllers = *Common::Singleton<GameControllers>::Instance();
    std::lock_guard lock(mutex);
    bool found = false;
    for (u8 i = 0; i < profiles.size(); ++i) {
        auto* user = UserManagement.GetUserByPlayerIndex(i + 1);
        auto* pad = controllers[i]->m_sdl_gamepad;
        const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
        profiles[i] = {user ? user->user_name : "", name ? name : "Контроллер",
                       pad ? SDL_GetGamepadID(pad) : 0, user != nullptr};
        found |= selected_device != 0 && profiles[i].device == selected_device;
    }
    if (!found) {
        selected_device = 0;
        for (u8 i = 0; i < profiles.size(); ++i) {
            if (profiles[i].device) {
                selected_device = profiles[i].device;
                selected_profile = i;
                break;
            }
        }
    }
}
void Open(SDL_JoystickID device) {
    Refresh();
    ReleaseAllInputs();
    std::lock_guard lock(mutex);
    for (u8 i = 0; i < profiles.size(); ++i) {
        if (device && profiles[i].device == device) {
            selected_device = device;
            selected_profile = i;
            break;
        }
    }
    if (!open) {
        ImGui::Core::AcquireGamepadInputCapture();
        open = true;
    }
}
bool ProcessEvent(const SDL_Event& event) {
    if (event.type == CommandEvent()) {
        if (!IsOpen()) return true;
        const auto device = SDL_JoystickID(reinterpret_cast<uintptr_t>(event.user.data1));
        const auto slot = uintptr_t(event.user.data2);
        if (event.user.code == Close) CloseMenu();
        else if (event.user.code == SelectDevice) Open(device);
        else if (event.user.code == SelectProfile && slot < 4) {
            std::lock_guard lock(mutex);
            if (profiles[slot].available) selected_profile = slot;
        } else if (event.user.code == Assign && slot < 4) {
            ReleaseAllInputs();
            Common::Singleton<GameControllers>::Instance()->AssignDeviceToProfile(device, slot);
            Refresh();
        }
        return true;
    }
    const bool guide = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                        event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
                       event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
    const bool shortcut = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                          event.key.key == SDLK_F2;
    if (guide || shortcut) {
        if ((guide && event.gbutton.down) || (shortcut && event.key.down && !event.key.repeat)) {
            if (IsOpen()) CloseMenu();
            else Open(guide ? event.gbutton.which : 0);
        }
        return true;
    }
    if (!IsOpen()) return false;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const bool keyboard = event.type == SDL_EVENT_KEY_DOWN;
        const auto key = keyboard ? event.key.key : SDLK_UNKNOWN;
        const auto button = keyboard ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GamepadButton(event.gbutton.button);
        std::lock_guard lock(mutex);
        if (key == SDLK_ESCAPE || button == SDL_GAMEPAD_BUTTON_EAST) Send(Close);
        else if (key == SDLK_RETURN || button == SDL_GAMEPAD_BUTTON_SOUTH)
            Send(Assign, selected_device, selected_profile);
        else if (key == SDLK_UP || button == SDL_GAMEPAD_BUTTON_DPAD_UP) MoveProfile(-1);
        else if (key == SDLK_DOWN || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) MoveProfile(1);
        else if (key == SDLK_LEFT || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) MoveDevice(-1);
        else if (key == SDLK_RIGHT || key == SDLK_TAB || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) MoveDevice(1);
        return true;
    }
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            const int direction = event.gaxis.value < -18000 ? -1 : event.gaxis.value > 18000 ? 1 : 0;
            std::lock_guard lock(mutex);
            if (direction && direction != stick_direction) MoveProfile(direction);
            stick_direction = direction;
        }
        return true;
    }
    return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT ||
           event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
           event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP ||
           event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION || event.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
}
void Register() { ImGui::Layer::AddLayer(&menu); }
void Unregister() {
    std::lock_guard lock(mutex);
    if (open) ImGui::Core::ReleaseGamepadInputCapture();
    open = false;
    ImGui::Layer::RemoveLayer(&menu);
}
} // namespace Input::Profiles
