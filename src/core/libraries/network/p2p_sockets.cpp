// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include <common/assert.h>
#include <common/logging/log.h>
#include <common/singleton.h>
#include "core/libraries/kernel/kernel.h"
#include "net.h"
#include "net_error.h"
#include "net_util.h"
#include "sockets.h"

namespace Libraries::Net {

namespace {

std::mutex p2p_registry_mutex;
std::map<u16, std::weak_ptr<P2PSocket>> p2p_registry;

int SetP2PError(int error) {
    *Libraries::Kernel::__Error() = error;
    return -1;
}

void RemoveSocketFromRegistry(P2PSocket* socket) {
    for (auto it = p2p_registry.begin(); it != p2p_registry.end();) {
        const auto registered = it->second.lock();
        if (!registered || registered.get() == socket) {
            it = p2p_registry.erase(it);
        } else {
            ++it;
        }
    }
}

bool IsLoopbackAddress(u32 address) {
    return (ntohl(address) & 0xff000000) == 0x7f000000;
}

} // namespace

u16 GetP2PConfiguredPort() {
    return 0;
}

u32 GetP2PAdvertisedAddr() {
    auto* netinfo = Common::Singleton<NetUtil::NetUtilInternal>::Instance();
    if (netinfo->RetrieveIp()) {
        const u32 address = inet_addr(netinfo->GetIp().c_str());
        if (address != INADDR_NONE) {
            return address;
        }
    }
    return htonl(INADDR_LOOPBACK);
}

bool EnsureP2PTransport() {
    return true;
}

bool P2PTransportIsReady() {
    return true;
}

int P2PSignalingSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return -1;
}

int P2PSignalingRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return -1;
}

int P2PControlSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return -1;
}

int P2PControlRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return -1;
}

int P2PMatching2SendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return -1;
}

int P2PMatching2RecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return -1;
}

P2PSocket::~P2PSocket() {
    Unregister();
}

void P2PSocket::Unregister() {
    std::scoped_lock lock{p2p_registry_mutex};
    RemoveSocketFromRegistry(this);
    is_bound = false;
}

int P2PSocket::Close() {
    Unregister();
    {
        std::scoped_lock lock{local_receive_mutex};
        is_closed = true;
        local_receive_queue.clear();
    }
    local_receive_cv.notify_all();
    return inner.Close();
}

u32 P2PSocket::GetPendingEvents(u32 requested_events) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return 0;
    }

    u32 pending_events = requested_events & ORBIS_NET_EPOLLOUT;
    std::scoped_lock lock{local_receive_mutex};
    if (!local_receive_queue.empty()) {
        pending_events |= requested_events & ORBIS_NET_EPOLLIN;
    }
    if (is_closed) {
        pending_events |= (requested_events & ORBIS_NET_EPOLLIN) | ORBIS_NET_EPOLLHUP;
    }
    return pending_events;
}

int P2PSocket::Bind(const OrbisNetSockaddr* addr, u32 addrlen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return inner.Bind(addr, addrlen);
    }
    if (addr == nullptr || addrlen < sizeof(OrbisNetSockaddrIn)) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    OrbisNetSockaddrIn new_addr{};
    std::memcpy(&new_addr, addr, sizeof(new_addr));
    if (new_addr.sin_family != ORBIS_NET_AF_INET) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    const auto self = weak_from_this();
    if (self.expired()) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    std::scoped_lock lock{p2p_registry_mutex};
    RemoveSocketFromRegistry(this);
    bound_addr = new_addr;
    is_bound = true;
    if (bound_addr.sin_port != 0) {
        p2p_registry[bound_addr.sin_port] = self;
    }
    if (bound_addr.sin_vport != 0 && bound_addr.sin_vport != bound_addr.sin_port) {
        p2p_registry[bound_addr.sin_vport] = self;
    }
    LOG_DEBUG(Lib_Net, "Bound local P2P datagram socket to port {:#x}, vport {:#x}",
              ntohs(bound_addr.sin_port), ntohs(bound_addr.sin_vport));
    return 0;
}

int P2PSocket::SendPacket(const void* msg, u32 len, int flags, const OrbisNetSockaddr* to,
                          u32 tolen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return inner.SendPacket(msg, len, flags, to, tolen);
    }
    if (to == nullptr || tolen < sizeof(OrbisNetSockaddrIn) || (msg == nullptr && len != 0)) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    OrbisNetSockaddrIn destination_addr{};
    std::memcpy(&destination_addr, to, sizeof(destination_addr));
    if (destination_addr.sin_family != ORBIS_NET_AF_INET) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    const u32 advertised_addr = GetP2PAdvertisedAddr();
    OrbisNetSockaddrIn source_addr{};
    bool source_is_bound = false;
    std::shared_ptr<P2PSocket> destination;
    {
        std::scoped_lock lock{p2p_registry_mutex};
        source_addr = bound_addr;
        source_is_bound = is_bound;

        const auto find_destination = [&](u16 port) -> std::shared_ptr<P2PSocket> {
            if (port == 0) {
                return {};
            }
            const auto it = p2p_registry.find(port);
            if (it == p2p_registry.end()) {
                return {};
            }
            auto candidate = it->second.lock();
            if (!candidate) {
                p2p_registry.erase(it);
                return {};
            }

            const u32 address = destination_addr.sin_addr;
            const bool is_local =
                address == htonl(INADDR_ANY) || IsLoopbackAddress(address) ||
                address == advertised_addr ||
                (is_bound && bound_addr.sin_addr != htonl(INADDR_ANY) &&
                 address == bound_addr.sin_addr) ||
                (candidate->is_bound && candidate->bound_addr.sin_addr != htonl(INADDR_ANY) &&
                 address == candidate->bound_addr.sin_addr);
            return is_local ? candidate : std::shared_ptr<P2PSocket>{};
        };

        destination = find_destination(destination_addr.sin_vport);
        if (!destination && destination_addr.sin_port != destination_addr.sin_vport) {
            destination = find_destination(destination_addr.sin_port);
        }
    }

    if (!destination) {
        LOG_ERROR(Lib_Net,
                  "No local P2P datagram destination for address {:#x}, port {:#x}, vport {:#x}",
                  ntohl(destination_addr.sin_addr), ntohs(destination_addr.sin_port),
                  ntohs(destination_addr.sin_vport));
        return SetP2PError(ORBIS_NET_EAGAIN);
    }

    if (!source_is_bound) {
        source_addr = {
            .sin_len = sizeof(OrbisNetSockaddrIn),
            .sin_family = ORBIS_NET_AF_INET,
            .sin_addr = htonl(INADDR_LOOPBACK),
        };
    } else if (source_addr.sin_addr == htonl(INADDR_ANY)) {
        source_addr.sin_addr = destination_addr.sin_addr == htonl(INADDR_ANY)
                                   ? advertised_addr
                                   : destination_addr.sin_addr;
    }
    source_addr.sin_len = sizeof(OrbisNetSockaddrIn);
    source_addr.sin_family = ORBIS_NET_AF_INET;

    LocalDatagram packet{};
    packet.data.resize(len);
    if (len != 0) {
        std::memcpy(packet.data.data(), msg, len);
    }
    packet.source = source_addr;

    {
        std::scoped_lock lock{destination->local_receive_mutex};
        if (destination->is_closed) {
            return SetP2PError(ORBIS_NET_EAGAIN);
        }
        if (destination->local_receive_queue.size() == MaxQueuedDatagrams) {
            destination->local_receive_queue.pop_front();
        }
        destination->local_receive_queue.emplace_back(std::move(packet));
    }
    destination->local_receive_cv.notify_one();
    LOG_DEBUG(Lib_Net, "Delivered {} byte local P2P datagram to port {:#x}, vport {:#x}", len,
              ntohs(destination_addr.sin_port), ntohs(destination_addr.sin_vport));
    return static_cast<int>(len);
}

int P2PSocket::ReceivePacket(void* buf, u32 len, int flags, OrbisNetSockaddr* from,
                             u32* fromlen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return inner.ReceivePacket(buf, len, flags, from, fromlen);
    }
    if (buf == nullptr && len != 0) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    LocalDatagram packet{};
    {
        std::scoped_lock lock{local_receive_mutex};
        if (local_receive_queue.empty()) {
            return SetP2PError(ORBIS_NET_EAGAIN);
        }
        packet = std::move(local_receive_queue.front());
        local_receive_queue.pop_front();
    }

    const size_t received = std::min<size_t>(len, packet.data.size());
    if (received != 0) {
        std::memcpy(buf, packet.data.data(), received);
    }
    if (fromlen != nullptr) {
        const u32 available = *fromlen;
        *fromlen = sizeof(packet.source);
        if (from != nullptr && available != 0) {
            std::memcpy(from, &packet.source, std::min<u32>(available, sizeof(packet.source)));
        }
    }
    return static_cast<int>(received);
}

int P2PSocket::GetSocketAddress(OrbisNetSockaddr* name, u32* namelen) {
    if (socket_type != ORBIS_NET_SOCK_DGRAM_P2P) {
        return inner.GetSocketAddress(name, namelen);
    }
    if (name == nullptr || namelen == nullptr) {
        return SetP2PError(ORBIS_NET_EINVAL);
    }

    OrbisNetSockaddrIn address{};
    bool socket_is_bound = false;
    {
        std::scoped_lock lock{p2p_registry_mutex};
        socket_is_bound = is_bound;
        if (socket_is_bound) {
            address = bound_addr;
        }
    }
    if (!socket_is_bound) {
        return inner.GetSocketAddress(name, namelen);
    }

    const u32 available = *namelen;
    *namelen = sizeof(address);
    if (available != 0) {
        std::memcpy(name, &address, std::min<u32>(available, sizeof(address)));
    }
    return 0;
}

} // namespace Libraries::Net
