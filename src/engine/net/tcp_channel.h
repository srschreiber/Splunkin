#pragma once
#include <cstdint>
#include <string>
// TCP login/lobby channel — TODO: connect, auth handshake. NOT wired to anything.
namespace dc::net {
struct TcpChannel {
    bool connect(const std::string& host, uint16_t port);
    void send(const void* data, uint32_t len);
    void shutdown();
};
} // namespace dc::net
