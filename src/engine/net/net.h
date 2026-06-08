#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace dc::net {

enum class Role { Standalone, Host, Client };

// One networking event surfaced by poll().
struct Event {
    enum Type { Connect, Disconnect, Receive } type;
    uint32_t peer = 0;                  // peer id (host side: which client)
    std::vector<unsigned char> data;    // payload (Receive only)
};

// Thin UDP transport over enet. Encapsulates enet entirely so the rest of the
// codebase never includes its headers — a Steam-sockets backend could implement
// this same shape later. Host listens; clients connect; messages are raw bytes.
struct Net {
    Role  role   = Role::Standalone;
    void* host   = nullptr;   // ENetHost*
    void* server = nullptr;   // ENetPeer* to the host (client side only)

    bool start_host(uint16_t port, int max_clients = 8);
    bool start_client(const char* host_ip, uint16_t port);

    // Service the socket; append connect/disconnect/receive events. Call each frame.
    void poll(std::vector<Event>& out);

    void send_to_host(const void* data, std::size_t len, bool reliable);  // client -> host
    void broadcast(const void* data, std::size_t len, bool reliable);     // host -> all clients
    void send_to_peer(uint32_t peer, const void* data, std::size_t len, bool reliable);  // host -> one client
    void shutdown();
};

} // namespace dc::net
