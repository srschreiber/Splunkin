#include "engine/net/net.h"
#include <enet/enet.h>

namespace dc::net {

namespace {
bool g_inited = false;
bool ensure_enet() {
    if (!g_inited) {
        if (enet_initialize() != 0) return false;
        g_inited = true;
    }
    return true;
}
// Channel 0 = reliable, channel 1 = unreliable.
ENetPacket* make_packet(const void* data, std::size_t len, bool reliable) {
    return enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
}
} // namespace

bool Net::start_host(uint16_t port, int max_clients) {
    if (!ensure_enet()) return false;
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = port;
    ENetHost* h = enet_host_create(&addr, max_clients, 2, 0, 0);
    if (!h) return false;
    host = h; role = Role::Host;
    return true;
}

bool Net::start_client(const char* host_ip, uint16_t port) {
    if (!ensure_enet()) return false;
    ENetHost* h = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!h) return false;
    ENetAddress addr; enet_address_set_host(&addr, host_ip); addr.port = port;
    ENetPeer* p = enet_host_connect(h, &addr, 2, 0);
    if (!p) { enet_host_destroy(h); return false; }
    host = h; server = p; role = Role::Client;
    return true;
}

void Net::poll(std::vector<Event>& out) {
    if (!host) return;
    ENetEvent ev;
    while (enet_host_service(static_cast<ENetHost*>(host), &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                out.push_back({ Event::Connect, ev.peer->incomingPeerID, {} });
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                out.push_back({ Event::Disconnect, ev.peer->incomingPeerID, {} });
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                Event e; e.type = Event::Receive; e.peer = ev.peer->incomingPeerID;
                e.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                out.push_back(std::move(e));
                enet_packet_destroy(ev.packet);
                break;
            }
            default: break;
        }
    }
}

void Net::send_to_host(const void* data, std::size_t len, bool reliable) {
    if (!server) return;
    enet_peer_send(static_cast<ENetPeer*>(server), reliable ? 0 : 1, make_packet(data, len, reliable));
}

void Net::broadcast(const void* data, std::size_t len, bool reliable) {
    if (!host) return;
    enet_host_broadcast(static_cast<ENetHost*>(host), reliable ? 0 : 1, make_packet(data, len, reliable));
}

void Net::send_to_peer(uint32_t peer, const void* data, std::size_t len, bool reliable) {
    if (!host) return;
    ENetHost* h = static_cast<ENetHost*>(host);
    if (peer >= h->peerCount) return;                       // incomingPeerID == index into peers[]
    ENetPeer* p = &h->peers[peer];
    if (p->state != ENET_PEER_STATE_CONNECTED) return;
    enet_peer_send(p, reliable ? 0 : 1, make_packet(data, len, reliable));
}

void Net::shutdown() {
    if (server) { enet_peer_disconnect_now(static_cast<ENetPeer*>(server), 0); server = nullptr; }
    if (host)   { enet_host_destroy(static_cast<ENetHost*>(host)); host = nullptr; }
    role = Role::Standalone;
}

} // namespace dc::net
