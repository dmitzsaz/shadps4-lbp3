// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/types.h"

namespace Libraries::Net::Lbp3OnlineBridge {

enum class Channel : u8 {
    Game = 0,
    Signaling = 1,
    Control = 2,
};

// Addresses and ports use the same network byte order as OrbisNetSockaddrIn.
struct Endpoint {
    u32 addr{};
    u16 port{};
    u16 vport{};
};

bool IsSupportedTitle();
bool EnsureConnected();
bool IsConnected();
u32 AdvertisedAddr();
u16 ConfiguredPort();
std::string OnlineId();

u32 VirtualAddrForOnlineId(std::string_view online_id);
bool ResolvePeer(std::string_view online_id, u32* out_addr, u16* out_port);

// Arms the title's own FindBestRoom request after the helper has confirmed that this
// client is a member of a host-first PartyChat roster. MaybeQueueFindBestRoom must run
// from a guest thread; NP callback pumps provide that safe delivery point.
void ObserveMatchingRequest(std::string_view body);
void MaybeQueueFindBestRoom();

int Send(Channel channel, const void* data, u32 len, const Endpoint& source,
         const Endpoint& destination);
int Receive(Channel channel, void* data, u32 len, const Endpoint* local, Endpoint* source);
bool HasPending(Channel channel, const Endpoint* local);

} // namespace Libraries::Net::Lbp3OnlineBridge
