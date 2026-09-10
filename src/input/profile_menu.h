// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL_events.h>

namespace Input::Profiles {
// SDL/main-thread entry points. Both hotplug and Guide/F2 use this same menu.
void Open(SDL_JoystickID device = 0);
void Refresh();
bool ProcessEvent(const SDL_Event& event);
bool IsOpen();
// Renderer lifetime; UI sends commands to the SDL thread, never edits controllers.
void Register();
void Unregister();
} // namespace Input::Profiles
