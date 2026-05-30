#pragma once
#include <cstdint>
// UDP gameplay channel — TODO: enet host, (un)reliable packets. NOT wired to anything.
namespace dc::net {
struct UdpChannel {
    bool bind(uint16_t port);
    void send(const void* data, uint32_t len);
    void poll();
    void shutdown();
};
} // namespace dc::net
