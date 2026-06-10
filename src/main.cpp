#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"
#include "engine/renderer/animator.h"
#include "engine/entity/player.h"
#include "engine/entity/entity.h"
#include "engine/entity/enemy.h"
#include "engine/entity/spawner.h"
#include "engine/world/map.h"
#include "engine/world/map_mesh.h"
#include "engine/world/torch.h"
#include "engine/world/pathfind.h"
#include "engine/fx/particles.h"
#include "engine/net/net.h"
#include "engine/net/protocol.h"

#include <SDL3/SDL.h>
#include <cglm/cglm.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// A placed chest. One-way: once opened, the lid stays open. Costs `cost` coins.
struct Chest {
    int   col = 0, row = 0;
    float open_t = 0.0f;    // time into the open clip (0 = closed)
    bool  opened = false;
    int   cost   = 10;      // coins to open
};

// A dropped coin: sits on the floor (settling), then magnets to the player and
// is collected. `age` gates the settle delay so it's always visible briefly.
struct Coin {
    vec3  pos;
    float value = 1.0f;
    float age   = 0.0f;
};

// 7-segment digit: calls box(u0,v0,u1,v1) for each lit segment of digit `d`, in a
// local cell of width w / height h / segment thickness t (origin at bottom-left).
// Callers map the boxes to whatever they draw (HUD rects, billboarded quads, ...).
//
// Segment layout (the shape of an 8) and their bit positions in `seg`:
//        aaaa            a = bit 0
//       f    b           b = bit 1
//       f    b           c = bit 2
//        gggg            d = bit 3
//       e    c           e = bit 4
//       e    c           f = bit 5
//        dddd            g = bit 6
// e.g. seg['8'] lights all 7; seg['1'] lights only b,c.
template <class Box>
static void seven_seg(int d, float w, float h, float t, Box box) {
    static const unsigned char seg[10] =
        { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (d < 0 || d > 9) return;
    const unsigned char m = seg[d];
    const float top = h, mid = h * 0.5f, bot = 0.0f;
    auto on = [&](int bit) { return (m >> bit) & 1; };
    if (on(0)) box(t,        top - t,        w - t, top);              // a (top)
    if (on(1)) box(w - t,    mid,            w,     top);              // b (top-right)
    if (on(2)) box(w - t,    bot,            w,     mid);              // c (bottom-right)
    if (on(3)) box(t,        bot,            w - t, bot + t);          // d (bottom)
    if (on(4)) box(0.0f,     bot,            t,     mid);              // e (bottom-left)
    if (on(5)) box(0.0f,     mid,            t,     top);              // f (top-left)
    if (on(6)) box(t,        mid - t * 0.5f, w - t, mid + t * 0.5f);   // g (middle)
}

// Chest upgrade cards (color-coded, no text for now).
enum class Upgrade { StaminaCost, Knockback, Damage, SwingArc };

// Apply one upgrade to the player (stacks additively/multiplicatively each pick).
static void apply_upgrade(dc::entity::Player& p, Upgrade u) {
    switch (u) {
        case Upgrade::StaminaCost: p.stamina_mult *= 0.85f;       break;  // green: -15% costs
        case Upgrade::Knockback:   p.stats.knockback += 4.0f;     break;  // yellow
        case Upgrade::Damage:      p.damage_mult += 0.25f;        break;  // red: +25% dmg
        case Upgrade::SwingArc:    p.swing_reach_bonus += 0.5f;           // blue: longer + wider swing
                                   p.swing_cone_bonus  += 0.12f;
                                   p.sword_scale       += 0.2f;   break;  // + a bigger blade
    }
}

// Card color per upgrade (green / yellow / red / blue).
static void upgrade_color(Upgrade u, float& r, float& g, float& b) {
    switch (u) {
        case Upgrade::StaminaCost: r = 0.20f; g = 0.85f; b = 0.30f; break;  // green
        case Upgrade::Knockback:   r = 0.90f; g = 0.80f; b = 0.15f; break;  // yellow
        case Upgrade::Damage:      r = 0.85f; g = 0.20f; b = 0.20f; break;  // red
        case Upgrade::SwingArc:    r = 0.25f; g = 0.50f; b = 1.00f; break;  // blue
    }
}

int main(int argc, char** argv) {
    bool smoke = false;
    const char* map_path = "assets/maps/test.txt";
    // Networking: --host [port] listens; --connect <ip> [port] joins. Default = solo.
    dc::net::Role net_role = dc::net::Role::Standalone;
    const char* connect_ip = "127.0.0.1";
    uint16_t net_port = 1234;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (std::strcmp(argv[i], "--host") == 0) {
            net_role = dc::net::Role::Host;
            if (i + 1 < argc && argv[i + 1][0] != '-') net_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--connect") == 0) {
            net_role = dc::net::Role::Client;
            if (i + 1 < argc && argv[i + 1][0] != '-') connect_ip = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') net_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (argv[i][0] != '-') map_path = argv[i];
    }

    std::string text = read_file(map_path);
    if (text.empty()) { std::fprintf(stderr, "could not read map: %s\n", map_path); return 1; }
    auto map = dc::world::parse_map(text);
    if (!map) { std::fprintf(stderr, "could not parse map: %s\n", map_path); return 1; }

    dc::renderer::ModelData model_data;
    if (!dc::renderer::read_model("assets/models/player.glb", model_data)) {
        std::fprintf(stderr, "could not load model: assets/models/player.glb\n");
        return 1;
    }

    dc::renderer::ModelData chest_data;
    if (!dc::renderer::read_model("assets/models/chest.glb", chest_data)) {
        std::fprintf(stderr, "could not load model: assets/models/chest.glb\n");
        return 1;
    }

    dc::renderer::ModelData helmet_data;
    if (!dc::renderer::read_model("assets/models/helmet.glb", helmet_data)) {
        std::fprintf(stderr, "could not load model: assets/models/helmet.glb\n");
        return 1;
    }

    dc::renderer::ModelData shield_data;
    if (!dc::renderer::read_model("assets/models/shield.glb", shield_data)) {
        std::fprintf(stderr, "could not load model: assets/models/shield.glb\n");
        return 1;
    }

    dc::renderer::ModelData torch_data;
    if (!dc::renderer::read_model("assets/models/torch.glb", torch_data)) {
        std::fprintf(stderr, "could not load model: assets/models/torch.glb\n");
        return 1;
    }

    dc::renderer::ModelData sword_data;
    if (!dc::renderer::read_model("assets/models/sword.glb", sword_data)) {
        std::fprintf(stderr, "could not load model: assets/models/sword.glb\n");
        return 1;
    }

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    // Procedural terrain: gentle hills under the open floor (seed -> identical on
    // host + clients). Walls/gameplay come from the tile map; this only shapes height.
    dc::world::Terrain terrain;
    const vec3 terrain_color = { 0.32f, 0.40f, 0.26f };   // mossy green-brown

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map, terrain));   // walls (textured)
    dc::renderer::Mesh terrain_mesh;
    terrain_mesh.upload(dc::world::build_terrain_mesh(*map, terrain));   // floor (solid color)
    dc::renderer::Mesh flyer_mesh;   // rebuilt each frame from hovering flying-enemy cubes

    dc::renderer::Model player_model;
    player_model.upload(model_data);

    dc::renderer::Model chest_model;
    chest_model.upload(chest_data);

    dc::renderer::Model helmet_model;
    helmet_model.upload(helmet_data);

    dc::renderer::Model shield_model;
    shield_model.upload(shield_data);

    dc::renderer::Model sword_model;
    sword_model.upload(sword_data); 

    dc::renderer::Model torch_model;
    torch_model.upload(torch_data);

    // Equipment is attached to a player bone (helmet -> head, shield -> hand).
    // Each item's mesh-node-local TRS is its constant offset relative to that
    // bone; each frame we draw it at: player_placement * boneWorld * offset.
    auto helmet_offset = dc::renderer::mesh_offsets(helmet_data);
    auto shield_offset = dc::renderer::mesh_offsets(shield_data);
    auto sword_offset  = dc::renderer::mesh_offsets(sword_data);

    // Spawn a chest entity per 'C' tile in the map.
    std::vector<Chest> chests;
    for (const auto& cs : map->chests) chests.push_back({ cs.col, cs.row });

    // Spawn a torch per wall-torch tile: precompute its placement + flame point,
    // and give each its own particle emitter.
    struct Torch {
        mat4 placement;
        vec3 flame_pos;
        dc::fx::ParticleSystem ps;
    };
    // The torch is a static model — pose it once at rest for drawing.
    std::vector<dc::renderer::Mat4> torch_part_world;
    dc::renderer::pose_model(torch_data, {}, 0.0f, torch_part_world);

    // Flame anchor: the centroid of the torch's emissive parts, in model-local
    // space (so light + particles sit at the actual flame, whatever its scale).
    vec3 flame_anchor = { 0.0f, 0.0f, 0.0f };
    int  flame_parts = 0;
    for (std::size_t i = 0; i < torch_data.parts.size(); ++i) {
        const auto& p = torch_data.parts[i];
        if (p.emissive[0] + p.emissive[1] + p.emissive[2] <= 0.0f) continue;  // flame parts only
        vec3 c = { 0.0f, 0.0f, 0.0f };
        const std::size_t nv = p.vertices.size() / 8;
        for (std::size_t v = 0; v < nv; ++v) {
            c[0] += p.vertices[v * 8 + 0]; c[1] += p.vertices[v * 8 + 1]; c[2] += p.vertices[v * 8 + 2];
        }
        if (nv) { c[0] /= nv; c[1] /= nv; c[2] /= nv; }
        vec3 cw; glm_mat4_mulv3(torch_part_world[i].m, c, 1.0f, cw);   // part-local -> model-local
        flame_anchor[0] += cw[0]; flame_anchor[1] += cw[1]; flame_anchor[2] += cw[2];
        ++flame_parts;
    }
    if (flame_parts) { flame_anchor[0] /= flame_parts; flame_anchor[1] /= flame_parts; flame_anchor[2] /= flame_parts; }

    std::vector<Torch> torches;
    for (const auto& ts : map->torches) {
        Torch t;
        dc::world::torch_placement(ts.col, ts.row, ts.dir, t.placement);
        glm_mat4_mulv3(t.placement, flame_anchor, 1.0f, t.flame_pos);   // model-local -> world
        torches.push_back(std::move(t));
    }
    std::vector<float> particle_verts;   // rebuilt each frame for draw_particles

    // Dynamic entities (enemies for now). Spawn one per 'X' tile in the map.
    // Clients don't own enemies (host-authoritative; they're not replicated yet).
    dc::entity::EntityList entities;
    if (net_role != dc::net::Role::Client)
        for (const auto& es : map->enemies)
            entities.spawn_enemy((es.col + 0.5f) * dc::world::TILE, (es.row + 0.5f) * dc::world::TILE);
    std::vector<dc::renderer::Mat4> enemy_part_world;   // scratch, reused per enemy

    // One demo spawner at the map center, trickling enemies onto open floor.
    std::vector<dc::entity::Spawner> spawners;
    {
        dc::entity::Spawner sp;
        sp.pos[0] = map->width  * 0.5f * dc::world::TILE;
        sp.pos[2] = map->height * 0.5f * dc::world::TILE;
        sp.radius = 4.0f; sp.rate = 0.5f; sp.max_alive = 8;
        sp.ranged_fraction = 0.30f;   // ~30% ground shooters
        sp.flying_fraction = 0.15f;   // ~15% hovering shooters
        spawners.push_back(sp);
    }

    std::vector<Coin> coins;                 // dropped on kills, magnet to the player
    std::vector<float> frame_deaths;         // enemy death positions this frame (xyz triples)
    int   currency = 0;
    float run_time = 0.0f;                    // seconds survived this run (top-of-screen timer)
    float death_flash = 0.0f;                // red "you died" overlay timer

    dc::renderer::Camera camera;

    dc::entity::Player player;
    player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
    player.position[1] = dc::world::EYE_HEIGHT;
    player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;

    dc::input::Input input;

    // Networking transport (enet). Standalone = no socket; host listens; client joins.
    dc::net::Net net;
    if (net_role == dc::net::Role::Host) {
        if (net.start_host(net_port)) std::printf("[net] hosting on port %u\n", net_port);
        else std::fprintf(stderr, "[net] failed to host on port %u\n", net_port);
    } else if (net_role == dc::net::Role::Client) {
        if (net.start_client(connect_ip, net_port)) std::printf("[net] connecting to %s:%u\n", connect_ip, net_port);
        else std::fprintf(stderr, "[net] failed to start client\n");
    }
    std::vector<dc::net::Event> net_events;

    // Replication state. A "remote" is another player we render (not simulate locally).
    struct Remote {
        uint32_t id; vec3 pos; float yaw, pitch, anim_time; bool moving;
        bool ghost = false;   // dead player: render faint + translucent, no gear
        bool punching = false, blocking = false;
        float punch_time = 0.0f, block_time = 0.0f, hit_flash = 0.0f, sword_scale = 1.0f;
        // Specials (render-only mirror of the owner's thrown/orbit state).
        bool thrown_active = false; float thrown_x = 0.0f, thrown_y = 0.0f, thrown_z = 0.0f, thrown_spin = 0.0f, thrown_size = 1.0f;
        bool orbit_active = false; int orbit_count = 0; float orbit_angle = 0.0f, orbit_spin = 0.0f, orbit_radius = 0.0f;
        bool bash_active = false; float bash_radius = 0.0f;
    };
    std::vector<Remote> remotes;
    std::vector<dc::renderer::Mat4> remote_part_world;   // scratch for posing remotes
    // Host side: one simulated body per connected client (host runs their movement).
    struct HostClient {
        uint32_t id, peer; dc::entity::Player body; dc::net::InputCmd input; float anim_time = 0.0f;
        std::vector<uint32_t> thrown_hits, orbit_hits;   // per-client special hit sets (host-side damage)
        float orbit_tick_cd = 0.0f;                      // host-run orbit damage cadence (client's tick pulse is lossy)
        int currency = 0;                                // this client's own wallet (host-authoritative)
        // Host-run bash (started by a reliable BashCast event, advanced on the host's clock).
        bool  bash_active = false; float bash_time = 0.0f, bash_radius = 0.0f;
        float bash_max_radius = 0.0f, bash_damage = 0.0f, bash_knockback = 0.0f, bash_duration = 0.0f;
        std::vector<uint32_t> bash_hits;
    };
    std::vector<HostClient> host_clients;
    uint32_t next_player_id = 1;   // host = 0; clients get 1,2,...
    uint32_t my_id = 0;            // client: our id (assigned by host)

    float anim_time = 0.0f;                       // walk-clip clock (advances while moving)
    float punch_time = 0.0f;                      // punch-clip clock (advances while punching)
    float block_time = 0.0f;                      // block-clip clock (advances while blocking)
    bool  punching = false;
    bool  blocking = false;
    bool  exhausted = false;                           // winded: must recover before sprint/block
    bool  choosing = false;                            // upgrade-card menu open (freezes movement)
    bool  menu_click_prev = false;                     // edge-detect the card click
    bool  paused = false;                              // ESC pause menu (frees the cursor)
    bool  esc_prev = false;                            // edge-detect ESC
    bool  pause_click_prev = false;                    // edge-detect the quit-button click
    const Upgrade cards[4] = { Upgrade::StaminaCost, Upgrade::Knockback, Upgrade::Damage, Upgrade::SwingArc };
    bool  punch_struck = false;                        // strike lands once per punch
    float attack_cd = 0.0f;                            // weapon cooldown between swings
    bool  punch_is_throw = false;                      // this punch clip is a sword throw
    float throw_cd = 0.0f;                             // cooldown between throws
    // The in-flight thrown sword (one at a time). While active, the hand is empty.
    struct ThrownSword {
        bool  active = false, returning = false;
        vec3  pos = {0.0f, 0.0f, 0.0f};
        vec3  dir = {0.0f, 0.0f, 0.0f};
        float traveled = 0.0f;
        float spin = 0.0f;
        std::vector<uint32_t> hit_ids;   // enemies hit this pass (cleared on the return leg)
    } thrown;
    // Orbit special (2): spinning swords circling the player for a short time.
    struct Orbit {
        bool  active = false;
        float time = 0.0f;       // seconds remaining
        float angle = 0.0f;      // revolve angle around the player
        float spin = 0.0f;       // each sword's own spin
        float tick = 0.0f;       // time until the next damage tick
        std::vector<uint32_t> hit_ids;
    } orbit;
    float orbit_cd = 0.0f;       // cooldown between casts
    // Shield-bash nova (3): an expanding sphere that shoves everything back.
    struct Bash {
        bool  active = false;
        float time = 0.0f;       // seconds since cast
        float radius = 0.0f;     // current shockwave radius (grows over bash_duration)
        std::vector<uint32_t> hit_ids;   // enemies already shoved this cast (hit once as the front passes)
    } bash;
    float bash_cd = 0.0f;        // cooldown between bashes
    std::vector<dc::renderer::Mat4> part_world;        // posed per-part transforms (player)
    std::vector<dc::renderer::Mat4> chest_part_world;  // posed per-part transforms (a chest)
    std::vector<dc::renderer::AnimLayer> layers;       // reused each frame
    bool e_prev = false;                               // for edge-triggered interact
    bool g_prev = false;                               // edge-triggered debug enemy spawn (melee)
    bool h_prev = false;                               // edge-triggered debug enemy spawn (ranged)
    bool j_prev = false;                               // edge-triggered debug enemy spawn (flying)
    bool v_prev = false;                               // edge-triggered debug-cone toggle
    bool debug_cone = false;                           // draw the shield block cone

    const float base_knockback = player.stats.knockback;   // for clearing the yellow upgrade on death

    // Reset the run (solo death = game over -> start over). Player back to spawn at
    // full health/stamina, currency + upgrades cleared, enemies/coins reset.
    auto reset_run = [&]() {
        player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
        player.position[1] = dc::world::EYE_HEIGHT;
        player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
        player.vel_y = 0.0f;
        player.health = player.stats.max_health;
        player.stamina = player.stamina_max;
        player.knock_vel[0] = player.knock_vel[2] = 0.0f;
        player.hit_flash = 0.0f;
        thrown.active = false;
        orbit.active = false;
        bash.active = false;
        if (choosing) { choosing = false; window.set_relative_mouse(true); }
        // Clear all chest upgrades.
        player.stamina_mult = 1.0f;
        player.damage_mult = 1.0f;
        player.swing_reach_bonus = 0.0f;
        player.swing_cone_bonus = 0.0f;
        player.sword_scale = 1.0f;
        player.stats.knockback = base_knockback;
        currency = 0;
        // Revive + reset every connected client (clears wallet, refills health, sends
        // them back to spawn). The full-health bodies go out in the next snapshot, so
        // ghosts come back to life on their own screens.
        for (auto& hc : host_clients) {
            hc.currency = 0;
            hc.body.health = hc.body.stats.max_health;
            hc.body.knock_vel[0] = hc.body.knock_vel[2] = 0.0f;
            hc.body.hit_flash = 0.0f;
            hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
            hc.body.position[1] = dc::world::EYE_HEIGHT;
            hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
        }
        run_time = 0.0f;
        coins.clear();
        entities.items.clear();
        entities.projectiles.clear();
        if (net.role != dc::net::Role::Client)
            for (const auto& es : map->enemies)
                entities.spawn_enemy((es.col + 0.5f) * dc::world::TILE, (es.row + 0.5f) * dc::world::TILE);
        for (auto& sp : spawners) sp.accum = 0.0f;
    };

    // Upgrade-card layout in NDC (shared by hit-testing and drawing).
    const float CARD_W = 0.18f, CARD_GAP = 0.05f, CARD_TOP = 0.45f, CARD_BOT = -0.45f;
    auto card_x0 = [&](int i) {
        const float total = 4 * CARD_W + 3 * CARD_GAP;
        return -total * 0.5f + i * (CARD_W + CARD_GAP);
    };
    // Pause-menu quit button (NDC), shared by hit-testing and drawing.
    const float QX0 = -0.15f, QX1 = 0.15f, QY0 = -0.09f, QY1 = 0.09f;

    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        // Service the network: handle connects, client inputs, and snapshots.
        net_events.clear();
        net.poll(net_events);
        for (auto& ev : net_events) {
            if (ev.type == dc::net::Event::Connect) {
                if (net.role == dc::net::Role::Host) {
                    HostClient hc; hc.id = next_player_id++; hc.peer = ev.peer;
                    hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
                    hc.body.position[1] = dc::world::EYE_HEIGHT;
                    hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
                    host_clients.push_back(hc);
                    unsigned char buf[5];
                    buf[0] = static_cast<unsigned char>(dc::net::MsgType::AssignId);
                    std::memcpy(buf + 1, &hc.id, 4);
                    net.broadcast(buf, sizeof buf, true);   // (2-player: only one client to hear it)
                    std::printf("[net] client connected -> id %u\n", hc.id);
                }
            } else if (ev.type == dc::net::Event::Disconnect) {
                for (std::size_t i = 0; i < host_clients.size(); ++i)
                    if (host_clients[i].peer == ev.peer) { host_clients[i] = host_clients.back(); host_clients.pop_back(); break; }
            } else if (ev.type == dc::net::Event::Receive && !ev.data.empty()) {
                const auto mt = static_cast<dc::net::MsgType>(ev.data[0]);
                if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::Input
                    && ev.data.size() >= 1 + sizeof(dc::net::InputCmd)) {
                    dc::net::InputCmd in; std::memcpy(&in, ev.data.data() + 1, sizeof in);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) { hc.input = in; break; }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BashCast
                           && ev.data.size() >= 1 + sizeof(dc::net::BashCast)) {
                    // A client cast the bash. Start a host-run nova for it (advanced +
                    // damaged on the host's clock in the per-client specials block).
                    dc::net::BashCast bc; std::memcpy(&bc, ev.data.data() + 1, sizeof bc);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) {
                        hc.bash_active = true; hc.bash_time = 0.0f; hc.bash_radius = 0.0f; hc.bash_hits.clear();
                        hc.bash_max_radius = bc.radius; hc.bash_damage = bc.damage;
                        hc.bash_knockback = bc.knockback; hc.bash_duration = bc.duration;
                        break;
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::OpenChest
                           && ev.data.size() >= 5) {
                    // A client wants to open a chest. Events are processed one at a
                    // time, so two simultaneous requests can't both win: the first
                    // marks it opened and the second fails the !opened check (no mutex
                    // needed — the host is the single authority).
                    uint32_t idx; std::memcpy(&idx, ev.data.data() + 1, 4);
                    HostClient* hc = nullptr;
                    for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc && idx < chests.size() && !chests[idx].opened
                        && hc->currency >= chests[idx].cost) {
                        hc->currency -= chests[idx].cost;
                        chests[idx].opened = true;            // one-way; replicated in the snapshot
                        unsigned char buf[1 + sizeof idx];
                        buf[0] = static_cast<unsigned char>(dc::net::MsgType::ChestGranted);
                        std::memcpy(buf + 1, &idx, sizeof idx);
                        net.send_to_peer(ev.peer, buf, sizeof buf, true);   // only the requester
                    }
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::AssignId && ev.data.size() >= 5) {
                    std::memcpy(&my_id, ev.data.data() + 1, 4);
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::ChestGranted
                           && ev.data.size() >= 5 && !choosing) {
                    // Host approved our chest: it already deducted our wallet and
                    // marked the chest open. Open the upgrade menu (card pick is local;
                    // its stat mods flow back to the host via our replicated loadout).
                    choosing = true;
                    menu_click_prev = true;
                    window.set_relative_mouse(false);
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::Snapshot && ev.data.size() >= 5) {
                    const unsigned char* p = ev.data.data() + 1;
                    auto read_u32 = [&]() { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };
                    // Players: our own pos/health from the host; everyone else -> remotes.
                    uint32_t np = read_u32();
                    remotes.clear();
                    for (uint32_t k = 0; k < np; ++k) {
                        dc::net::PlayerState s; std::memcpy(&s, p, sizeof s); p += sizeof s;
                        if (s.id == my_id) {
                            player.position[0] = s.x; player.position[1] = s.y; player.position[2] = s.z;
                            player.health = s.health;
                            currency = s.currency;   // our own wallet (host-authoritative)
                            player.stamina -= s.block_spent;   // stamina the host spent resolving our blocks
                            if (player.stamina < 0.0f) player.stamina = 0.0f;
                            if (s.hit_flash > player.hit_flash) player.hit_flash = s.hit_flash;  // host says we got hit
                        } else {
                            Remote r{}; r.id = s.id;
                            r.pos[0] = s.x; r.pos[1] = s.y; r.pos[2] = s.z;
                            r.yaw = s.yaw; r.pitch = s.pitch; r.anim_time = s.anim_time; r.moving = s.moving != 0;
                            r.ghost = s.health <= 0.0f;
                            r.punching = s.punching != 0; r.blocking = s.blocking != 0;
                            r.punch_time = s.punch_time; r.block_time = s.block_time;
                            r.hit_flash = s.hit_flash; r.sword_scale = s.sword_scale;
                            r.thrown_active = s.thrown_active != 0;
                            r.thrown_x = s.thrown_x; r.thrown_y = s.thrown_y; r.thrown_z = s.thrown_z;
                            r.thrown_spin = s.thrown_spin; r.thrown_size = s.thrown_size;
                            r.bash_active = s.bash_active != 0; r.bash_radius = s.bash_radius;
                            r.orbit_active = s.orbit_active != 0; r.orbit_count = s.orbit_count;
                            r.orbit_angle = s.orbit_angle; r.orbit_spin = s.orbit_spin; r.orbit_radius = s.orbit_radius;
                            remotes.push_back(r);
                        }
                    }
                    // Enemies + coins: render-only mirrors of the host's state.
                    uint32_t ne = read_u32();
                    entities.items.clear();
                    for (uint32_t k = 0; k < ne; ++k) {
                        dc::net::EnemyState e; std::memcpy(&e, p, sizeof e); p += sizeof e;
                        dc::entity::Entity en;
                        en.type = dc::entity::EntityType::Enemy; en.alive = true;
                        en.position[0] = e.x; en.position[1] = 0.0f; en.position[2] = e.z; en.yaw = e.yaw;
                        en.anim_time = e.anim_time; en.attacking = e.attacking != 0;
                        en.attack_time = e.attack_time; en.hit_flash = e.hit_flash;
                        en.kind = static_cast<dc::entity::EnemyKind>(e.kind);
                        entities.items.push_back(en);
                    }
                    uint32_t nc = read_u32();
                    coins.clear();
                    for (uint32_t k = 0; k < nc; ++k) {
                        dc::net::CoinState c; std::memcpy(&c, p, sizeof c); p += sizeof c;
                        Coin co; co.pos[0] = c.x; co.pos[1] = 0.0f; co.pos[2] = c.z; coins.push_back(co);
                    }
                    // Chest open-state (same map order as ours): mirror the host's opens.
                    uint32_t nh = read_u32();
                    for (uint32_t k = 0; k < nh; ++k) {
                        unsigned char o = *p++;
                        if (k < chests.size() && o) chests[k].opened = true;
                    }
                    // Projectiles: render-only mirror of the host's flying shots.
                    uint32_t npr = read_u32();
                    entities.projectiles.clear();
                    for (uint32_t k = 0; k < npr; ++k) {
                        dc::net::ProjectileState ps; std::memcpy(&ps, p, sizeof ps); p += sizeof ps;
                        dc::entity::Projectile pr;
                        pr.pos[0] = ps.x; pr.pos[1] = ps.y; pr.pos[2] = ps.z;
                        pr.color[0] = ps.r; pr.color[1] = ps.g; pr.color[2] = ps.b;
                        entities.projectiles.push_back(pr);
                    }
                }
            }
        }

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;
        run_time += dt;   // survival timer

        // ESC toggles the pause menu, which frees the cursor (so you can alt-tab to
        // the other window during net testing). Re-captures on resume.
        bool esc_now = input.key_down(SDL_SCANCODE_ESCAPE);
        if (esc_now && !esc_prev) {
            paused = !paused;
            if (paused) { window.set_relative_mouse(false); pause_click_prev = true; }
            else if (!choosing) window.set_relative_mouse(true);
        }
        esc_prev = esc_now;

        // Any open UI (upgrade cards or pause) freezes player control.
        const bool ui_open = choosing || paused;
        // Dead = ghost: you can still walk around to spectate, but can't fight, block,
        // use specials, or buy chests. Movement is intentionally NOT gated on this.
        const bool dead = player.health <= 0.0f;

        if (!ui_open) player.add_look(input.mouse_dx, input.mouse_dy);   // freeze look in any menu
        float forward = ui_open ? 0.0f : (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                                       - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = ui_open ? 0.0f : (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                                       - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool jump = !ui_open && input.key_down(SDL_SCANCODE_SPACE);
        bool moving = (forward != 0.0f || strafe != 0.0f);
        // Exhaustion: hitting 0 stamina winds you until it recovers past a threshold.
        // (Otherwise regen re-enables sprint/block one frame at a time = stutter-sprint.)
        const float EXHAUST_RECOVER = player.stamina_max * 0.25f;
        if (player.stamina <= 0.0f) exhausted = true;
        else if (player.stamina >= EXHAUST_RECOVER) exhausted = false;
        // Run while holding Shift (needs stamina). Drains stamina (applied below).
        bool sprinting = moving && !exhausted && input.key_down(SDL_SCANCODE_LSHIFT);
        player.speed = sprinting ? dc::entity::RUN_SPEED : dc::entity::MOVE_SPEED;
        player.update(forward, strafe, jump, dt, *map, terrain);

        // Walk clock: advance while moving (faster while sprinting), reset when idle.
        if (moving) anim_time += dt * (sprinting ? 1.7f : 1.0f); else anim_time = 0.0f;

        // --- Networked player sync (host side) ---
        // The client sends its InputCmd later, once combat flags (strike/blocking)
        // for this frame are known. Here the host advances each client's body.
        if (net.role == dc::net::Role::Host) {
            // Host simulates each connected client's body from their latest input.
            // (The combined snapshot is broadcast later, after enemies/coins update.)
            for (auto& hc : host_clients) {
                hc.body.yaw = hc.input.yaw; hc.body.pitch = hc.input.pitch;
                hc.body.update(hc.input.forward, hc.input.strafe, hc.input.jump != 0, dt, *map, terrain);
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                hc.anim_time = m ? hc.anim_time + dt : 0.0f;
                if (hc.body.hit_flash > 0.0f) hc.body.hit_flash -= dt;   // decay the flash
                if (hc.body.health > 0.0f) {                              // passive regen (not ghosts)
                    hc.body.health += hc.body.health_regen * dt;
                    if (hc.body.health > hc.body.stats.max_health) hc.body.health = hc.body.stats.max_health;
                }
            }
            // (Remotes are rebuilt after combat, so hit_flash this frame is included.)
        }

        // Block held (right mouse): needs a shield and some stamina. You can't block
        // and swing at once, so holding block also forbids starting a swing (below).
        bool block_held = player.shield.has_value() && !exhausted && !ui_open && !dead
                        && input.mouse_down(SDL_BUTTON_RIGHT);

        // Attack (left mouse): one-shot swing, gated by cooldown, not-blocking, and
        // enough stamina. Weapon sets playback/cooldown/cost; fists fall back.
        const float PUNCH_STRIKE = 0.18f;  // when in the clip the hit lands (clip seconds)
        const float atk_speed  = player.weapon ? player.weapon->attack_speed     : dc::entity::UNARMED_ATTACK_SPEED;
        const float atk_cd_dur = player.weapon ? player.weapon->cooldown         : dc::entity::UNARMED_COOLDOWN;
        const float swing_cost = player.weapon ? player.weapon->stamina_per_swing : dc::entity::UNARMED_STAMINA;
        const float throw_cost = player.weapon ? player.weapon->stamina_per_throw : 1e9f;  // no weapon -> can't throw
        if (attack_cd > 0.0f) attack_cd -= dt;
        if (throw_cd  > 0.0f) throw_cd  -= dt;
        if (orbit_cd  > 0.0f) orbit_cd  -= dt;
        if (bash_cd   > 0.0f) bash_cd   -= dt;

        // Shield bash nova (3): needs a shield + stamina, slow cooldown. Fires a sphere
        // that expands and shoves enemies back (resolved in the update block below).
        if (player.shield && !ui_open && !dead && !punching && !thrown.active && !bash.active
            && bash_cd <= 0.0f && input.key_down(SDL_SCANCODE_3)
            && player.stamina >= player.shield->stamina_per_bash) {
            bash.active = true; bash.time = 0.0f; bash.radius = 0.0f; bash.hit_ids.clear();
            player.stamina -= player.shield->stamina_per_bash * player.stamina_mult;
            bash_cd = player.shield->bash_cooldown;
            // Event model: tell the host once (reliable). It runs the damage + tells
            // everyone else; we just predicted the visual above.
            if (net.role == dc::net::Role::Client) {
                dc::net::BashCast bc;
                bc.radius = player.shield->bash_radius;
                bc.damage = player.shield->bash_damage * player.damage_mult;
                bc.knockback = player.shield->bash_knockback;
                bc.duration = player.shield->bash_duration;
                unsigned char buf[1 + sizeof bc];
                buf[0] = static_cast<unsigned char>(dc::net::MsgType::BashCast);
                std::memcpy(buf + 1, &bc, sizeof bc);
                net.send_to_host(buf, sizeof buf, true);   // reliable: a cast can't be dropped
            }
        }

        // Orbit special (2): summon spinning swords for a while (big stamina cost).
        if (player.weapon && !ui_open && !dead && !orbit.active && orbit_cd <= 0.0f
            && input.key_down(SDL_SCANCODE_2)
            && player.stamina >= player.weapon->stamina_per_orbit) {
            orbit.active = true;
            orbit.time = player.weapon->orbit_duration;
            orbit.angle = 0.0f; orbit.spin = 0.0f; orbit.tick = 0.0f;
            orbit.hit_ids.clear();
            player.stamina -= player.weapon->stamina_per_orbit * player.stamina_mult;
            orbit_cd = player.weapon->orbit_cooldown;
        }
        // Start a melee swing (LMB) or a sword throw (MMB) — both play the punch clip;
        // the difference is resolved at the strike frame below.
        if (!punching && !block_held && !thrown.active && !ui_open && !dead) {
            if (player.weapon && input.key_down(SDL_SCANCODE_1)
                && throw_cd <= 0.0f && player.stamina >= throw_cost) {
                punching = true; punch_time = 0.0f; punch_struck = false; punch_is_throw = true;
                player.stamina -= throw_cost * player.stamina_mult;
            } else if (input.mouse_down(SDL_BUTTON_LEFT)
                       && attack_cd <= 0.0f && player.stamina >= swing_cost) {
                punching = true; punch_time = 0.0f; punch_struck = false; punch_is_throw = false;
                player.stamina -= swing_cost * player.stamina_mult;
            }
        }
        bool player_strike = false;        // true only on the frame a MELEE swing connects
        bool thrown_reset  = false;        // frame the thrown sword clears its hit-ids (launch / turn)
        bool orbit_tick_now = false;       // frame an orbit damage-tick fires
        if (punching) {
            punch_time += dt * atk_speed;
            if (!punch_struck && punch_time >= PUNCH_STRIKE) {
                punch_struck = true;
                if (punch_is_throw && player.weapon) {
                    // Release: detach the sword as a spinning projectile flying forward.
                    thrown.active = true; thrown.returning = false;
                    thrown.traveled = 0.0f; thrown.spin = 0.0f; thrown.hit_ids.clear();
                    thrown_reset = true;
                    // Launch from the eye along the full 3D look direction (yaw + pitch),
                    // so the sword flies exactly where the crosshair points.
                    glm_vec3_copy(player.position, thrown.pos);
                    vec3 f; player.front(f);   // already normalized
                    glm_vec3_copy(f, thrown.dir);
                    throw_cd = player.weapon->throw_cooldown;
                } else {
                    player_strike = true;
                }
            }
            if (!model_data.punch.valid() || punch_time >= model_data.punch.duration) {
                punching = false;
                if (!punch_is_throw) attack_cd = atk_cd_dur;
            }
        }

        // Block animation: raise the shield while held (and not mid-swing). It only
        // actually mitigates once the raise finishes (block_time reaches the clip end);
        // block_speed scales how fast it gets there, so timing matters.
        const float block_speed = player.shield ? player.shield->block_speed : 1.0f;
        blocking = block_held && !punching;                 // animating the raise/hold
        if (blocking) block_time += dt * block_speed; else block_time = 0.0f;
        bool block_ready = blocking && model_data.block.valid()
                         && block_time >= model_data.block.duration;   // shield fully up


        // Stamina: blocking and running drain it (and pause regen); otherwise it
        // regenerates. Running dry drops the shield / blocks swings / ends the run.
        if (blocking)      player.stamina -= player.shield->stamina_per_sec * dt * player.stamina_mult;
        else if (sprinting) player.stamina -= dc::entity::RUN_STAMINA_PER_SEC * dt * player.stamina_mult;
        else               player.stamina += player.stamina_regen * dt;
        if (player.stamina > player.stamina_max) player.stamina = player.stamina_max;
        if (player.stamina < 0.0f) player.stamina = 0.0f;

        // Passive health regen while alive (host-authoritative; clients get it via the
        // snapshot). Never regen a ghost — that would revive the dead.
        if (net.role != dc::net::Role::Client && player.health > 0.0f) {
            player.health += player.health_regen * dt;
            if (player.health > player.stats.max_health) player.health = player.stats.max_health;
        }

        // Interact (E, edge-triggered): buy the nearest closed chest within reach.
        bool e_now = input.key_down(SDL_SCANCODE_E);
        if (e_now && !e_prev && !ui_open && !dead) {
            const float reach = 3.0f;            // world units
            int best = -1;
            float best_d2 = reach * reach;
            for (std::size_t i = 0; i < chests.size(); ++i) {
                if (chests[i].opened) continue;
                float cx = (chests[i].col + 0.5f) * dc::world::TILE;
                float cz = (chests[i].row + 0.5f) * dc::world::TILE;
                float dx = cx - player.position[0], dz = cz - player.position[2];
                float d2 = dx * dx + dz * dz;
                if (d2 < best_d2) { best_d2 = d2; best = static_cast<int>(i); }
            }
            if (best >= 0 && currency >= chests[best].cost) {
                if (net.role == dc::net::Role::Client) {
                    // Ask the host to open it. The host re-validates (funds + still
                    // closed), deducts our wallet, and replies ChestGranted -> we open
                    // the upgrade menu then. Reliable so the request isn't dropped.
                    uint32_t idx = static_cast<uint32_t>(best);
                    unsigned char buf[1 + sizeof idx];
                    buf[0] = static_cast<unsigned char>(dc::net::MsgType::OpenChest);
                    std::memcpy(buf + 1, &idx, sizeof idx);
                    net.send_to_host(buf, sizeof buf, true);
                } else {
                    // Host/standalone: pay, open, and present the upgrade menu now.
                    currency -= chests[best].cost;
                    chests[best].opened = true;     // one-way open
                    choosing = true;                // modal upgrade pick (freezes movement)
                    menu_click_prev = true;         // ignore the in-flight E/click frame
                    window.set_relative_mouse(false);  // free the cursor for clicking cards
                }
            }
        }
        e_prev = e_now;

        // Upgrade menu: click a card to pick its upgrade, which closes the menu.
        if (choosing) {
            float mx, my; input.mouse_pos(mx, my);
            int ww, wh; window.window_size(ww, wh);
            const float nx = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;   // pixel -> NDC
            const float ny = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool click = input.mouse_down(SDL_BUTTON_LEFT);
            if (click && !menu_click_prev) {
                for (int i = 0; i < 4; ++i) {
                    const float x0 = card_x0(i), x1 = x0 + CARD_W;
                    if (nx >= x0 && nx <= x1 && ny >= CARD_BOT && ny <= CARD_TOP) {
                        apply_upgrade(player, cards[i]);
                        choosing = false;
                        window.set_relative_mouse(true);
                        break;
                    }
                }
            }
            menu_click_prev = click;
        }

        // Pause menu: click the red Quit button to exit.
        if (paused) {
            float mx, my; input.mouse_pos(mx, my);
            int ww, wh; window.window_size(ww, wh);
            const float nx = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float ny = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool click = input.mouse_down(SDL_BUTTON_LEFT);
            if (click && !pause_click_prev && nx >= QX0 && nx <= QX1 && ny >= QY0 && ny <= QY1)
                running = false;
            pause_click_prev = click;
        }

        // Advance each opened chest's lid toward fully open (then hold).
        const float chest_dur = chest_data.open.valid() ? chest_data.open.duration : 0.0f;
        for (auto& ch : chests) {
            if (ch.opened && ch.open_t < chest_dur) {
                ch.open_t += dt;
                if (ch.open_t > chest_dur) ch.open_t = chest_dur;
            }
        }

        // Update torch particles (each flame flickers on its own phase) and pick
        // the torch nearest the player as this frame's single point light.
        const float t_now = static_cast<float>(now) / 1.0e9f;
        const float LIGHT_RADIUS = 7.0f;
        const vec3  LIGHT_BASE = { 1.4f, 0.8f, 0.4f };   // warm; scaled by flicker
        vec3  light_pos = { 0.0f, -1000.0f, 0.0f };
        vec3  light_color = { 0.0f, 0.0f, 0.0f };        // no torch -> dark
        float best_d2 = 1e30f;
        for (std::size_t i = 0; i < torches.size(); ++i) {
            float fl = dc::fx::flicker(t_now + static_cast<float>(i) * 1.7f);
            torches[i].ps.update(dt, torches[i].flame_pos, fl);
            float dx = torches[i].flame_pos[0] - player.position[0];
            float dz = torches[i].flame_pos[2] - player.position[2];
            float d2 = dx * dx + dz * dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                glm_vec3_copy(torches[i].flame_pos, light_pos);
                light_color[0] = LIGHT_BASE[0] * fl;
                light_color[1] = LIGHT_BASE[1] * fl;
                light_color[2] = LIGHT_BASE[2] * fl;
            }
        }

        // Debug: G spawns a melee enemy, H a ranged one, a couple tiles ahead.
        bool g_now = input.key_down(SDL_SCANCODE_G);
        if (g_now && !g_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 2.0f, player.position[2] + f[2] * 2.0f);
        }
        g_prev = g_now;
        bool h_now = input.key_down(SDL_SCANCODE_H);
        if (h_now && !h_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 2.0f, player.position[2] + f[2] * 2.0f,
                                 dc::entity::EnemyKind::Ranged);
        }
        h_prev = h_now;
        bool j_now = input.key_down(SDL_SCANCODE_J);
        if (j_now && !j_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 4.0f, player.position[2] + f[2] * 4.0f,
                                 dc::entity::EnemyKind::Flying);
        }
        j_prev = j_now;

        // Debug: V toggles the combat cones + the title-bar readout.
        bool v_now = input.key_down(SDL_SCANCODE_V);
        if (v_now && !v_prev) { debug_cone = !debug_cone; if (!debug_cone) window.set_title("dungeoncrawl"); }
        v_prev = v_now;

        // Thrown sword: spin, fly out `throw_distance`, then boomerang back to the
        // player; damage enemies in its path (once per leg). Runs before the enemy
        // sim so kills/knockback are folded into this frame's update.
        if (thrown.active && player.weapon) {
            const auto& w = *player.weapon;
            thrown.spin += 22.0f * dt;                 // procedural horizontal spin
            float step = w.throw_speed * dt;
            if (!thrown.returning) {
                thrown.pos[0] += thrown.dir[0] * step;   // fly straight along the 3D aim
                thrown.pos[1] += thrown.dir[1] * step;
                thrown.pos[2] += thrown.dir[2] * step;
                thrown.traveled += step;
                if (thrown.traveled >= w.throw_distance) { thrown.returning = true; thrown.hit_ids.clear(); thrown_reset = true; }
            } else {
                // Boomerang back to the player's current eye, in 3D.
                float hx = player.position[0] - thrown.pos[0];
                float hy = player.position[1] - thrown.pos[1];
                float hz = player.position[2] - thrown.pos[2];
                float hd = std::sqrt(hx * hx + hy * hy + hz * hz);
                if (hd < 1.0f) thrown.active = false;  // caught -> sword back in hand
                else { thrown.pos[0] += hx / hd * step; thrown.pos[1] += hy / hd * step; thrown.pos[2] += hz / hd * step; }
            }
            if (net.role != dc::net::Role::Client)   // damage is host-authoritative; client flight is cosmetic
                dc::entity::radius_attack(entities, thrown.pos, w.throw_radius * w.throw_size * player.sword_scale,
                                          w.throw_damage * player.damage_mult, player.stats.knockback, thrown.hit_ids);
        }

        // Orbit special: revolve + spin the swords; damage on periodic ticks (each
        // tick all swords share one hit set, so an enemy takes one hit per tick).
        if (orbit.active && player.weapon) {
            const auto& w = *player.weapon;
            orbit.time -= dt;
            if (orbit.time <= 0.0f) orbit.active = false;
            orbit.angle += 3.0f * dt;     // revolve speed around the player
            orbit.spin  += 22.0f * dt;    // each sword's own spin
            orbit.tick  -= dt;
            if (orbit.active && orbit.tick <= 0.0f) {
                orbit.tick = 0.25f;       // damage-tick interval
                orbit.hit_ids.clear();
                orbit_tick_now = true;
                for (int i = 0; i < w.orbit_count; ++i) {
                    float a = orbit.angle + (6.2831853f * i) / w.orbit_count;
                    vec3 p = { player.position[0] + std::cos(a) * w.orbit_radius, 0.0f,
                               player.position[2] + std::sin(a) * w.orbit_radius };
                    if (net.role != dc::net::Role::Client)   // host-authoritative damage
                        dc::entity::radius_attack(entities, p, w.orbit_hit_radius * player.sword_scale,
                                                  w.orbit_damage * player.damage_mult, player.stats.knockback, orbit.hit_ids);
                }
            }
        }

        // Shield bash nova: an expanding sphere; radius_attack with a persistent hit set
        // shoves each enemy once as the growing front reaches it (knockback points
        // outward from the player, so everyone flies back).
        if (bash.active && player.shield) {
            const auto& s = *player.shield;
            bash.time += dt;
            bash.radius = (bash.time / s.bash_duration) * s.bash_radius;
            if (net.role != dc::net::Role::Client) {
                vec3 c = { player.position[0], 0.0f, player.position[2] };
                dc::entity::radius_attack(entities, c, bash.radius,
                                          s.bash_damage * player.damage_mult, s.bash_knockback, bash.hit_ids);
            }
            if (bash.time >= s.bash_duration) bash.active = false;
        }

        // Each connected client's specials: the client simulates the flight/orbit;
        // the host applies the damage here (authoritative) at the reported positions,
        // with a per-client hit set so each enemy is hit once per leg / per tick.
        if (net.role == dc::net::Role::Host) {
            for (auto& hc : host_clients) {
                if (hc.input.thrown_active) {
                    if (hc.input.thrown_reset) hc.thrown_hits.clear();
                    vec3 tp = { hc.input.thrown_x, 0.0f, hc.input.thrown_z };
                    dc::entity::radius_attack(entities, tp, hc.input.thrown_hit_radius,
                                              hc.input.thrown_damage, hc.input.thrown_knockback, hc.thrown_hits);
                } else if (!hc.thrown_hits.empty()) {
                    hc.thrown_hits.clear();   // throw ended -> reset for the next one
                }
                // Orbit damage: the host runs the 0.25s tick cadence itself rather than
                // trusting the client's one-frame `orbit_tick` pulse (sent unreliably,
                // so it's almost always lost before the host's combat step).
                if (hc.input.orbit_active) {
                    hc.orbit_tick_cd -= dt;
                    if (hc.orbit_tick_cd <= 0.0f) {
                        hc.orbit_tick_cd = 0.25f;
                        hc.orbit_hits.clear();    // each tick shares one hit set (one hit per tick)
                        for (int i = 0; i < hc.input.orbit_count; ++i) {
                            float a = hc.input.orbit_angle + (6.2831853f * i) / hc.input.orbit_count;
                            vec3 op = { hc.body.position[0] + std::cos(a) * hc.input.orbit_radius, 0.0f,
                                        hc.body.position[2] + std::sin(a) * hc.input.orbit_radius };
                            dc::entity::radius_attack(entities, op, hc.input.orbit_hit_radius,
                                                      hc.input.orbit_damage, hc.input.orbit_knockback, hc.orbit_hits);
                        }
                    }
                } else {
                    hc.orbit_tick_cd = 0.0f;   // ready to bite immediately next activation
                    if (!hc.orbit_hits.empty()) hc.orbit_hits.clear();
                }
                // Bash nova (host-run from the BashCast event): expand + shove outward.
                if (hc.bash_active) {
                    hc.bash_time += dt;
                    hc.bash_radius = (hc.bash_duration > 1e-4f)
                                   ? (hc.bash_time / hc.bash_duration) * hc.bash_max_radius : 0.0f;
                    vec3 c = { hc.body.position[0], 0.0f, hc.body.position[2] };
                    dc::entity::radius_attack(entities, c, hc.bash_radius,
                                              hc.bash_damage, hc.bash_knockback, hc.bash_hits);
                    if (hc.bash_time >= hc.bash_duration) hc.bash_active = false;
                }
            }
        }

        // Combat targets: the local player (index 0) plus every connected client
        // (host-authoritative co-op). Enemies chase the nearest of them and each
        // player's strike is resolved against the enemies in one tick.
        std::vector<dc::entity::PlayerCombat> players;
        dc::entity::PlayerCombat pc{};
        pc.id = my_id;                 // host/standalone = 0; client = its assigned id
        pc.alive = !dead;              // ghosts aren't targeted and deal no damage
        glm_vec3_copy(player.position, pc.pos);
        { vec3 aim; player.front(aim); glm_vec3_copy(aim, pc.aim); }   // 3D look dir for the swing cone
        pc.yaw = player.yaw;
        pc.strike = player_strike;
        pc.strike_knockback = player.stats.knockback;
        pc.weight           = player.stats.weight;
        // Feet height above our own ground (0 standing, >0 mid-jump): drives the swing's
        // vertical reach so a jump can clip the hovering flyer.
        pc.strike_height = player.position[1]
                         - (terrain.height(player.position[0], player.position[2]) + dc::world::EYE_HEIGHT);
        // Weapon drives damage/reach/cone; fall back to fists when unarmed.
        if (player.weapon) {
            pc.strike_damage = player.stats.attack_damage + player.weapon->attack_bonus;
            pc.strike_reach  = player.weapon->reach;
            pc.strike_cos    = player.weapon->cone_cos;
        } else {
            pc.strike_damage = player.stats.attack_damage;
            pc.strike_reach  = dc::entity::UNARMED_REACH;
            pc.strike_cos    = dc::entity::UNARMED_CONE;
        }
        // Apply upgrade modifiers (red damage, blue longer+wider swing).
        pc.strike_damage *= player.damage_mult;
        pc.strike_reach  += player.swing_reach_bonus;
        pc.strike_cos    -= player.swing_cone_bonus;
        if (pc.strike_cos < -0.5f) pc.strike_cos = -0.5f;   // cap arc width
        // Shield drives blocking; only mitigates once fully raised (block_ready). The
        // sim spends stamina to negate damage, so it needs our current stamina + rate.
        pc.stamina = player.stamina;
        if (player.shield) {
            pc.blocking   = block_ready;
            pc.block_cos  = player.shield->block_cos;
            pc.block_rate = player.shield->block_rate * player.stamina_mult;   // green upgrade cheapens blocking too
        } else {
            pc.blocking = false;   // no shield -> can't block
        }
        players.push_back(pc);

        // Client: send our input + resolved combat loadout now that pc is built (its
        // weapon/upgrade-derived stats). The host resolves our strike/block against
        // the enemies with our real stats; our pos/health come back in the snapshot.
        if (net.role == dc::net::Role::Client) {
            dc::net::InputCmd cmd;
            cmd.forward = forward; cmd.strafe = strafe; cmd.jump = jump ? 1 : 0;
            cmd.yaw = player.yaw; cmd.pitch = player.pitch;
            cmd.strike = player_strike ? 1 : 0; cmd.blocking = block_ready ? 1 : 0;
            cmd.anim_punch = punching ? 1 : 0; cmd.anim_block = blocking ? 1 : 0;
            cmd.punch_time = punch_time; cmd.block_time = block_time;
            cmd.strike_damage = pc.strike_damage; cmd.strike_reach = pc.strike_reach;
            cmd.strike_cos = pc.strike_cos; cmd.strike_knockback = pc.strike_knockback;
            cmd.weight = pc.weight; cmd.block_cos = pc.block_cos; cmd.block_rate = pc.block_rate;
            cmd.stamina = player.stamina;
            cmd.sword_scale = player.sword_scale;
            // Specials: report flight/orbit state + effective damage stats so the host
            // can apply the damage authoritatively and everyone can render them.
            if (thrown.active && player.weapon) {
                const auto& w = *player.weapon;
                cmd.thrown_active = 1; cmd.thrown_reset = thrown_reset ? 1 : 0;
                cmd.thrown_x = thrown.pos[0]; cmd.thrown_y = thrown.pos[1]; cmd.thrown_z = thrown.pos[2]; cmd.thrown_spin = thrown.spin;
                cmd.thrown_size = w.throw_size;
                cmd.thrown_hit_radius = w.throw_radius * w.throw_size * player.sword_scale;
                cmd.thrown_damage = w.throw_damage * player.damage_mult;
                cmd.thrown_knockback = player.stats.knockback;
            }
            if (orbit.active && player.weapon) {
                const auto& w = *player.weapon;
                cmd.orbit_active = 1; cmd.orbit_tick = orbit_tick_now ? 1 : 0;
                cmd.orbit_count = w.orbit_count; cmd.orbit_angle = orbit.angle; cmd.orbit_spin = orbit.spin;
                cmd.orbit_radius = w.orbit_radius;
                cmd.orbit_hit_radius = w.orbit_hit_radius * player.sword_scale;
                cmd.orbit_damage = w.orbit_damage * player.damage_mult;
                cmd.orbit_knockback = player.stats.knockback;
            }
            unsigned char buf[1 + sizeof cmd];
            buf[0] = static_cast<unsigned char>(dc::net::MsgType::Input);
            std::memcpy(buf + 1, &cmd, sizeof cmd);
            net.send_to_host(buf, sizeof buf, false);
        }

        // Each client fights with its own resolved loadout (weapon + upgrades),
        // sent in its InputCmd. Strike/block are gameplay flags from the same input.
        for (auto& hc : host_clients) {
            dc::entity::PlayerCombat cc{};
            cc.id = hc.id;
            cc.alive = hc.body.health > 0.0f;
            glm_vec3_copy(hc.body.position, cc.pos);
            cc.yaw = hc.body.yaw;
            cc.strike           = hc.input.strike != 0;
            // 3D aim from the client's reported yaw+pitch.
            cc.aim[0] = std::cos(hc.input.pitch) * std::cos(hc.input.yaw);
            cc.aim[1] = std::sin(hc.input.pitch);
            cc.aim[2] = std::cos(hc.input.pitch) * std::sin(hc.input.yaw);
            cc.strike_height    = hc.body.position[1]
                                - (terrain.height(hc.body.position[0], hc.body.position[2]) + dc::world::EYE_HEIGHT);
            cc.strike_damage    = hc.input.strike_damage;
            cc.strike_reach     = hc.input.strike_reach;
            cc.strike_cos       = hc.input.strike_cos;
            cc.strike_knockback = hc.input.strike_knockback;
            cc.weight           = hc.input.weight;
            cc.blocking         = hc.input.blocking != 0;
            cc.block_cos        = hc.input.block_cos;
            cc.block_rate       = hc.input.block_rate;
            cc.stamina          = hc.input.stamina;   // its reported stamina drives block negation
            players.push_back(cc);
        }

        // One flow field per player (parallel to `players`), so an enemy can path to
        // its committed target — not just whoever's nearest. A handful of small BFS;
        // cheap at these player counts.
        std::vector<dc::world::FlowField> flows;
        flows.reserve(players.size());
        for (auto& p : players) {
            int gc = static_cast<int>(p.pos[0] / dc::world::TILE);
            int gr = static_cast<int>(p.pos[2] / dc::world::TILE);
            flows.push_back(dc::world::compute_flow(*map, gc, gr));
        }

        // Enemy sim is host-authoritative; clients render replicated enemies instead.
        frame_deaths.clear();
        std::vector<dc::entity::EnemyHitPlayer> hits;
        if (net.role != dc::net::Role::Client) {
            dc::entity::update_enemies(entities, *map, flows, players, hits, dt, &frame_deaths);
            // Advance ranged enemies' shots; their hits add into the same `hits`.
            dc::entity::update_projectiles(entities, *map, players, hits, dt);
            // out[0] -> local player.
            const dc::entity::EnemyHitPlayer& hit = hits[0];
            player.health -= hit.damage;
            if (player.health < 0.0f) player.health = 0.0f;
            player.knock_vel[0] += hit.knock[0];        // integrated (with collision) in player.update next frame
            player.knock_vel[2] += hit.knock[2];
            if (hit.hit) player.hit_flash = dc::entity::FLASH_TIME;   // damage got through -> flash red
            player.stamina -= hit.stamina_cost;                      // blocking spent this much stamina
            if (player.stamina < 0.0f) player.stamina = 0.0f;
            // out[i+1] -> connected clients' bodies (health + knockback only; their
            // own stamina/flash are cosmetic and handled client-side for now).
            for (std::size_t i = 0; i < host_clients.size(); ++i) {
                const dc::entity::EnemyHitPlayer& h = hits[i + 1];
                auto& b = host_clients[i].body;
                b.health -= h.damage;
                if (b.health < 0.0f) b.health = 0.0f;
                b.knock_vel[0] += h.knock[0];
                b.knock_vel[2] += h.knock[2];
                if (h.hit) b.hit_flash = dc::entity::FLASH_TIME;   // unblocked -> flash red
            }
            // Rebuild remotes from the (now combat-resolved) client bodies so their
            // hit-flash/swing/block show this frame.
            remotes.clear();
            for (auto& hc : host_clients) {
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                Remote r{}; r.id = hc.id;
                r.pos[0] = hc.body.position[0]; r.pos[1] = hc.body.position[1]; r.pos[2] = hc.body.position[2];
                r.yaw = hc.body.yaw; r.pitch = hc.body.pitch; r.anim_time = hc.anim_time; r.moving = m;
                r.ghost = hc.body.health <= 0.0f;
                r.punching = hc.input.anim_punch != 0; r.blocking = hc.input.anim_block != 0;
                r.punch_time = hc.input.punch_time; r.block_time = hc.input.block_time;
                r.hit_flash = hc.body.hit_flash; r.sword_scale = hc.input.sword_scale;
                r.thrown_active = hc.input.thrown_active != 0;
                r.thrown_x = hc.input.thrown_x; r.thrown_y = hc.input.thrown_y; r.thrown_z = hc.input.thrown_z;
                r.thrown_spin = hc.input.thrown_spin; r.thrown_size = hc.input.thrown_size;
                r.orbit_active = hc.input.orbit_active != 0; r.orbit_count = hc.input.orbit_count;
                r.orbit_angle = hc.input.orbit_angle; r.orbit_spin = hc.input.orbit_spin;
                r.orbit_radius = hc.input.orbit_radius;
                r.bash_active = hc.bash_active; r.bash_radius = hc.bash_radius;
                remotes.push_back(r);
            }
        }
        if (player.hit_flash > 0.0f) player.hit_flash -= dt;   // decay the flash (cosmetic, both sides)

        // Drop a coin where each enemy died (frame_deaths holds xyz triples).
        for (std::size_t i = 0; i + 2 < frame_deaths.size(); i += 3) {
            Coin c; c.pos[0] = frame_deaths[i]; c.pos[1] = frame_deaths[i + 1]; c.pos[2] = frame_deaths[i + 2];
            coins.push_back(c);
        }

        // Coins: settle briefly (so they're always visible), then magnet toward the
        // NEAREST player and collect on contact into that player's own wallet
        // (per-player economy). Host-authoritative; clients render replicated coins
        // and read their balance back from the snapshot.
        const float MAGNET_RADIUS = 1.8f, COLLECT_RADIUS = 0.6f, COIN_SPEED = 7.0f, COIN_SETTLE = 0.35f;
        if (net.role != dc::net::Role::Client) {
            // All LIVING collectors: the local player + every connected client, each
            // with its own wallet to credit. Ghosts (dead players) don't collect.
            struct Collector { float x, z; int* wallet; };
            std::vector<Collector> collectors;
            if (!dead) collectors.push_back({ player.position[0], player.position[2], &currency });
            for (auto& hc : host_clients)
                if (hc.body.health > 0.0f)
                    collectors.push_back({ hc.body.position[0], hc.body.position[2], &hc.currency });

            for (std::size_t i = 0; i < coins.size() && !collectors.empty();) {
                coins[i].age += dt;
                if (coins[i].age < COIN_SETTLE) { ++i; continue; }   // sit until visible
                // Find the nearest collector to this coin.
                int best = 0; float best_d = 1e30f, bdx = 0.0f, bdz = 0.0f;
                for (std::size_t c = 0; c < collectors.size(); ++c) {
                    float dx = collectors[c].x - coins[i].pos[0];
                    float dz = collectors[c].z - coins[i].pos[2];
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < best_d) { best_d = d; best = static_cast<int>(c); bdx = dx; bdz = dz; }
                }
                if (best_d < COLLECT_RADIUS) {
                    *collectors[best].wallet += static_cast<int>(coins[i].value);
                    coins[i] = coins.back(); coins.pop_back(); continue;
                }
                if (best_d < MAGNET_RADIUS && best_d > 1e-4f) {
                    float step = COIN_SPEED * dt;
                    coins[i].pos[0] += bdx / best_d * step;
                    coins[i].pos[2] += bdz / best_d * step;
                }
                ++i;
            }
        }

        // Host: broadcast the combined world snapshot (players + enemies + coins),
        // built now that everything has advanced this frame.
        if (net.role == dc::net::Role::Host && !host_clients.empty()) {
            std::vector<unsigned char> buf;
            buf.push_back(static_cast<unsigned char>(dc::net::MsgType::Snapshot));
            auto put = [&](const void* d, std::size_t n) {
                const unsigned char* b = static_cast<const unsigned char*>(d);
                buf.insert(buf.end(), b, b + n);
            };
            uint32_t np = 1 + static_cast<uint32_t>(host_clients.size()); put(&np, 4);
            { dc::net::PlayerState s{}; s.id = 0;
              s.x = player.position[0]; s.y = player.position[1]; s.z = player.position[2];
              s.yaw = player.yaw; s.pitch = player.pitch; s.anim_time = anim_time;
              s.health = player.health; s.moving = moving ? 1 : 0; s.currency = currency;
              s.punching = punching ? 1 : 0; s.blocking = blocking ? 1 : 0;
              s.punch_time = punch_time; s.block_time = block_time;
              s.hit_flash = player.hit_flash; s.sword_scale = player.sword_scale;
              if (thrown.active && player.weapon) {
                  s.thrown_active = 1; s.thrown_x = thrown.pos[0]; s.thrown_y = thrown.pos[1]; s.thrown_z = thrown.pos[2];
                  s.thrown_spin = thrown.spin; s.thrown_size = player.weapon->throw_size;
              }
              if (orbit.active && player.weapon) {
                  s.orbit_active = 1; s.orbit_count = player.weapon->orbit_count;
                  s.orbit_angle = orbit.angle; s.orbit_spin = orbit.spin; s.orbit_radius = player.weapon->orbit_radius;
              }
              s.bash_active = bash.active ? 1 : 0; s.bash_radius = bash.radius;
              put(&s, sizeof s); }
            for (std::size_t ci = 0; ci < host_clients.size(); ++ci) {
                auto& hc = host_clients[ci];
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                dc::net::PlayerState s{}; s.id = hc.id;
                // Stamina the host's block resolution spent for this client this tick;
                // the client subtracts it from its own (client-authoritative) stamina.
                if (ci + 1 < hits.size()) s.block_spent = hits[ci + 1].stamina_cost;
                s.x = hc.body.position[0]; s.y = hc.body.position[1]; s.z = hc.body.position[2];
                s.yaw = hc.body.yaw; s.pitch = hc.body.pitch; s.anim_time = hc.anim_time;
                s.health = hc.body.health; s.moving = m ? 1 : 0; s.currency = hc.currency;
                s.punching = hc.input.anim_punch; s.blocking = hc.input.anim_block;
                s.punch_time = hc.input.punch_time; s.block_time = hc.input.block_time;
                s.hit_flash = hc.body.hit_flash; s.sword_scale = hc.input.sword_scale;
                s.thrown_active = hc.input.thrown_active; s.thrown_x = hc.input.thrown_x; s.thrown_y = hc.input.thrown_y;
                s.thrown_z = hc.input.thrown_z; s.thrown_spin = hc.input.thrown_spin; s.thrown_size = hc.input.thrown_size;
                s.orbit_active = hc.input.orbit_active; s.orbit_count = hc.input.orbit_count;
                s.orbit_angle = hc.input.orbit_angle; s.orbit_spin = hc.input.orbit_spin; s.orbit_radius = hc.input.orbit_radius;
                s.bash_active = hc.bash_active ? 1 : 0; s.bash_radius = hc.bash_radius;
                put(&s, sizeof s);
            }
            uint32_t ne = 0; for (auto& en : entities.items) if (en.type == dc::entity::EntityType::Enemy) ++ne;
            put(&ne, 4);
            for (auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy) continue;
                dc::net::EnemyState e{}; e.x = en.position[0]; e.z = en.position[2]; e.yaw = en.yaw;
                e.anim_time = en.anim_time; e.attack_time = en.attack_time; e.hit_flash = en.hit_flash;
                e.attacking = en.attacking ? 1 : 0; e.kind = static_cast<uint8_t>(en.kind);
                put(&e, sizeof e);
            }
            uint32_t nc = static_cast<uint32_t>(coins.size()); put(&nc, 4);
            for (auto& c : coins) { dc::net::CoinState cs{}; cs.x = c.pos[0]; cs.z = c.pos[2]; put(&cs, sizeof cs); }
            // Chest open-state (stable map order, so an index identifies the same chest
            // on every peer). One byte each — cheap, and lets clients render opens.
            uint32_t nh = static_cast<uint32_t>(chests.size()); put(&nh, 4);
            for (auto& ch : chests) { unsigned char o = ch.opened ? 1 : 0; put(&o, 1); }
            // In-flight projectiles (ranged enemy shots) for clients to render.
            uint32_t npr = static_cast<uint32_t>(entities.projectiles.size()); put(&npr, 4);
            for (auto& pr : entities.projectiles) {
                dc::net::ProjectileState ps{}; ps.x = pr.pos[0]; ps.y = pr.pos[1]; ps.z = pr.pos[2];
                ps.r = pr.color[0]; ps.g = pr.color[1]; ps.b = pr.color[2];
                put(&ps, sizeof ps);
            }
            net.broadcast(buf.data(), buf.size(), false);
        }

        // Game over only when EVERYONE is down (co-op). The host decides and resets;
        // the revive goes out in the snapshot, so clients come back to life on their
        // own. Clients never reset themselves — they ghost and wait for the host.
        // (Standalone: host_clients is empty, so this is just "you died".)
        if (net.role != dc::net::Role::Client && death_flash <= 0.0f) {
            bool all_dead = player.health <= 0.0f;
            for (auto& hc : host_clients) all_dead = all_dead && hc.body.health <= 0.0f;
            if (all_dead) { reset_run(); death_flash = 1.2f; }
        }
        if (death_flash > 0.0f) death_flash -= dt;

        // Difficulty ramps with survival time: faster spawns + a higher cap.
        // (run_time resets on death, so this scales back down too.)
        const float difficulty = 1.0f + run_time / 25.0f;   // +1x base every 25s
        if (net.role != dc::net::Role::Client)               // host owns enemy spawning
            for (auto& sp : spawners) {
                sp.rate = 0.5f * difficulty;                     // 0.5 = configured base rate
                sp.max_alive = static_cast<int>(8 * difficulty); // 8 = configured base cap
                sp.update(dt, entities, *map);
            }

        // Build the animation layers for this frame: walk drives the body, punch
        // is masked to the armL bone so you can punch while walking. (No layers
        // when idle -> rest pose.)
        layers.clear();
        if (moving)   layers.push_back({ &model_data.walk,  anim_time,  -1 });
        if (punching) layers.push_back({ &model_data.punch, punch_time, model_data.arm_l_node });
        if (blocking) layers.push_back({ &model_data.block, block_time, model_data.arm_r_node, false });  // right arm, one-shot: play to end and hold (no loop)
        // Pose the player, and also grab the head bone's world matrix as a socket
        // for the helmet.
        dc::renderer::Mat4 head_world;
        dc::renderer::Mat4 l_hand_world;
        dc::renderer::Mat4 r_hand_world;
        dc::renderer::pose_model(model_data, layers, player.pitch, part_world,
                                 { model_data.head_node, model_data.hand_l_node, model_data.hand_r_node }, { &head_world, &l_hand_world, &r_hand_world });

        // Avatar placement: stand at the player's XZ, facing the look direction.
        // The model's origin sits at its waist (local feet at y~=-1.0), so lift it so
        // the feet rest on the floor; follow the player's vertical position so the
        // avatar rises with the camera on a jump.
        const float MODEL_FOOT_LIFT = 1.0f;
        const float MODEL_YAW_OFFSET = glm_rad(-90.0f);  // tune to face forward (try -90 / 0 / 180)
        float feet_y = (player.position[1] - dc::world::EYE_HEIGHT) + MODEL_FOOT_LIFT;
        mat4 placement;
        glm_mat4_identity(placement);
        vec3 foot = { player.position[0], feet_y, player.position[2] };
        glm_translate(placement, foot);
        glm_rotate_y(placement, -player.yaw + MODEL_YAW_OFFSET, placement);

        int w, h; window.framebuffer_size(w, h);
        camera.third_person = debug_cone;   // debug view (V): pull back so you see yourself + the cones
        renderer.begin_frame(*map, camera, player, dt, w, h);
        renderer.set_light(light_pos, light_color, LIGHT_RADIUS);
        renderer.draw_map(mesh);
        renderer.draw_terrain(terrain_mesh, terrain_color);
        const float GHOST_ALPHA = 0.18f;               // dead remote players render faint + translucent
        // First person (default): don't draw our own body/helmet — only the held sword
        // and shield, as a "viewmodel" tilted by pitch about the eye so the gear aims
        // where we point. Debug third-person (V): draw the full body + helmet instead,
        // and attach the gear normally (no view tilt) so you can see the swing + cones.
        const bool tp = camera.third_person;
        mat4 vm_place;
        {
            mat4 tilt; glm_mat4_identity(tilt);
            vec3 eye = { player.position[0], player.position[1], player.position[2] };
            vec3 neg = { -eye[0], -eye[1], -eye[2] };
            glm_translate(tilt, eye);
            glm_rotate(tilt, player.pitch, renderer.cam_right);   // tilt up/down about camera-right
            glm_translate(tilt, neg);
            glm_mat4_mul(tilt, placement, vm_place);
        }
        // First-person viewmodel: the rig's hands sit at waist height, below the view —
        // lift the gear up + push it forward along the look so both sword and shield
        // read on screen. (Tune these two if it sits too high/low or clips.)
        if (!camera.third_person) {
            const float VM_RAISE = 0.2f, VM_FWD = 0.5f;   // low: gear just peeks into the lower view
            vec3 fr; player.front(fr);
            vec3 d = { fr[0] * VM_FWD, VM_RAISE + fr[1] * VM_FWD, fr[2] * VM_FWD };
            mat4 off; glm_mat4_identity(off); glm_translate(off, d);
            glm_mat4_mul(off, vm_place, vm_place);   // world-space lift, applied to the whole viewmodel
        }
        // The matrix the hand-attached gear hangs off of: real placement in 3rd person,
        // the pitch-tilted viewmodel placement in 1st person.
        mat4 gear_place;
        glm_mat4_copy(tp ? placement : vm_place, gear_place);

        if (tp) {   // third-person: draw the body + helmet
            vec3 body_color = { 0.80f, 0.45f, 0.35f };
            if (dead) { vec3 pale = { 0.55f, 0.65f, 0.95f }; glm_vec3_copy(pale, body_color); }
            else if (player.hit_flash > 0.0f) {
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(body_color, red, player.hit_flash / dc::entity::FLASH_TIME, body_color);
            }
            renderer.draw_model(player_model, part_world, placement, body_color, dead ? GHOST_ALPHA : 1.0f);
            if (!dead) {
                mat4 helmet_place; glm_mat4_mul(placement, head_world.m, helmet_place);
                vec3 helmet_color = { 1.0f, 1.0f, 1.0f };
                renderer.draw_model(helmet_model, helmet_offset, helmet_place, helmet_color);
            }
        }

        // Gear hidden while a ghost (dead = no weapon).
        if (!dead) {
        // Sword in hand: only when equipped AND not currently thrown.
        if (player.weapon && !thrown.active) {
            mat4 sword_place;
            glm_mat4_mul(gear_place, l_hand_world.m, sword_place);
            vec3 sws = { player.sword_scale, player.sword_scale, player.sword_scale };
            glm_scale(sword_place, sws);   // blue upgrade grows the blade
            vec3 sword_color = { 0.8f, 0.8f, 0.9f };
            renderer.draw_model(sword_model, sword_offset, sword_place, sword_color);
        }
        // Thrown sword: spinning in flight. Match the in-hand size by reusing the
        // hand bone's world scale (the player rig is ~0.22x), times the throw_size
        // upgrade. Without this it'd draw at full model scale (way too big).
        if (thrown.active) {
            float rig_scale = std::sqrt(l_hand_world.m[0][0] * l_hand_world.m[0][0]
                                      + l_hand_world.m[0][1] * l_hand_world.m[0][1]
                                      + l_hand_world.m[0][2] * l_hand_world.m[0][2]);
            float s = rig_scale * player.sword_scale * (player.weapon ? player.weapon->throw_size : 1.0f);
            mat4 tplace;
            glm_mat4_identity(tplace);
            vec3 tpos = { thrown.pos[0], thrown.pos[1], thrown.pos[2] };   // 3D flight (follows aim)
            glm_translate(tplace, tpos);
            glm_rotate_y(tplace, thrown.spin, tplace);
            vec3 sc = { s, s, s };
            glm_scale(tplace, sc);
            vec3 sword_color = { 0.85f, 0.85f, 0.95f };
            renderer.draw_model(sword_model, sword_offset, tplace, sword_color);
        }

        // Orbit special: spinning swords circling the player at waist height.
        if (orbit.active && player.weapon) {
            float rig_scale = std::sqrt(l_hand_world.m[0][0] * l_hand_world.m[0][0]
                                      + l_hand_world.m[0][1] * l_hand_world.m[0][1]
                                      + l_hand_world.m[0][2] * l_hand_world.m[0][2]);
            float s = rig_scale * player.sword_scale;
            const auto& w = *player.weapon;
            for (int i = 0; i < w.orbit_count; ++i) {
                float a = orbit.angle + (6.2831853f * i) / w.orbit_count;
                mat4 op; glm_mat4_identity(op);
                vec3 opos = { player.position[0] + std::cos(a) * w.orbit_radius,
                              (player.position[1] - dc::world::EYE_HEIGHT) + 1.2f,   // ride the player's ground
                              player.position[2] + std::sin(a) * w.orbit_radius };
                glm_translate(op, opos);
                glm_rotate_y(op, orbit.spin, op);
                vec3 osc = { s, s, s };
                glm_scale(op, osc);
                vec3 oc = { 0.85f, 0.85f, 0.95f };
                renderer.draw_model(sword_model, sword_offset, op, oc);
            }
        }

        // Shield: drawn only when a shield is equipped, attached to the right hand bone.
        if (player.shield) {
            mat4 shield_place;
            glm_mat4_mul(gear_place, r_hand_world.m, shield_place);
            vec3 shield_color = { 0.5f, 0.5f, 0.8f };
            renderer.draw_model(shield_model, shield_offset, shield_place, shield_color);
        }
        }   // end if (!dead)

        // Draw each chest, lid posed by its open_t (open clip only animates the lid).
        // White tint -> the per-part material colors from the .glb show through unchanged.
        vec3 chest_color = { 1.0f, 1.0f, 1.0f };
        for (const auto& ch : chests) {
            std::vector<dc::renderer::AnimLayer> cl = {{ &chest_data.open, ch.open_t, -1, false }};  // one-shot: hold open
            dc::renderer::pose_model(chest_data, cl, 0.0f, chest_part_world);
            mat4 cplace;
            glm_mat4_identity(cplace);
            float ccx = (ch.col + 0.5f) * dc::world::TILE, ccz = (ch.row + 0.5f) * dc::world::TILE;
            vec3 cpos = { ccx, terrain.height(ccx, ccz), ccz };   // sit on the terrain surface
            glm_translate(cplace, cpos);            // move to the tile (origin at the chest's base)
            vec3 cscale = { 0.75f, 0.75f, 0.75f };
            glm_scale(cplace, cscale);              // half size, scaled around its base -> stays on floor
            renderer.draw_model(chest_model, chest_part_world, cplace, chest_color);
        }

        // Draw enemies — reuse the player model; melee tinted green, ranged purple,
        // posed by their own walk/attack clocks and facing the player. Flyers are
        // hovering cubes (batched + drawn solid below) to prove out flying enemies.
        vec3 enemy_color  = { 0.25f, 0.80f, 0.30f };
        vec3 ranged_color = { 0.70f, 0.30f, 0.85f };
        std::vector<float> flyer_verts;   // batched cube faces (9-float world-space verts)
        auto box_face = [&](float ax,float ay,float az, float bx,float by,float bz,
                            float cx,float cy,float cz, float dx,float dy,float dz, float nx,float ny,float nz) {
            auto V = [&](float x,float y,float z){ flyer_verts.insert(flyer_verts.end(), {x,y,z,nx,ny,nz,0.f,0.f,0.f}); };
            V(ax,ay,az); V(bx,by,bz); V(cx,cy,cz);  V(ax,ay,az); V(cx,cy,cz); V(dx,dy,dz);
        };
        auto append_cube = [&](float cx,float cy,float cz, float hx,float hy,float hz) {
            const float x0=cx-hx,x1=cx+hx,y0=cy-hy,y1=cy+hy,z0=cz-hz,z1=cz+hz;
            box_face(x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1, 0,0,1);   box_face(x1,y0,z0,x0,y0,z0,x0,y1,z0,x1,y1,z0, 0,0,-1);
            box_face(x1,y0,z1,x1,y0,z0,x1,y1,z0,x1,y1,z1, 1,0,0);   box_face(x0,y0,z0,x0,y0,z1,x0,y1,z1,x0,y1,z0, -1,0,0);
            box_face(x0,y1,z1,x1,y1,z1,x1,y1,z0,x0,y1,z0, 0,1,0);   box_face(x0,y0,z0,x1,y0,z0,x1,y0,z1,x0,y0,z1, 0,-1,0);
        };
        for (const auto& en : entities.items) {
            if (en.type != dc::entity::EntityType::Enemy) continue;
            if (en.kind == dc::entity::EnemyKind::Flying) {   // a hovering 2-tall cube
                const float gx = en.position[0], gz = en.position[2];
                const float cy = terrain.height(gx, gz) + dc::entity::FLY_HOVER;
                append_cube(gx, cy, gz, 0.7f, 1.0f, 0.7f);
                continue;
            }
            std::vector<dc::renderer::AnimLayer> el;
            if (en.attacking)        el.push_back({ &model_data.punch, en.attack_time, model_data.arm_l_node });
            else if (en.anim_time > 0.0f) el.push_back({ &model_data.walk, en.anim_time, -1 });
            dc::renderer::pose_model(model_data, el, 0.0f, enemy_part_world);
            mat4 eplace;
            glm_mat4_identity(eplace);
            vec3 epos = { en.position[0], terrain.height(en.position[0], en.position[2]) + MODEL_FOOT_LIFT, en.position[2] };
            glm_translate(eplace, epos);
            glm_rotate_y(eplace, -en.yaw + MODEL_YAW_OFFSET, eplace);
            vec3 col; glm_vec3_copy(en.kind == dc::entity::EnemyKind::Ranged ? ranged_color : enemy_color, col);
            if (en.hit_flash > 0.0f) {                 // flash red when struck
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(col, red, en.hit_flash / dc::entity::FLASH_TIME, col);
            }
            renderer.draw_model(player_model, enemy_part_world, eplace, col);
        }
        if (!flyer_verts.empty()) {                       // upload + draw this frame's flyer cubes
            flyer_mesh.upload(flyer_verts);
            vec3 flyer_color = { 0.85f, 0.25f, 0.30f };   // menacing red
            renderer.draw_terrain(flyer_mesh, flyer_color);
        }

        // Draw remote players (other connected clients), blue-tinted, posed by their
        // replicated walk clock + head pitch.
        for (const auto& rp : remotes) {
            // Same layered pose as the local avatar: walk + masked punch + masked block.
            std::vector<dc::renderer::AnimLayer> rl;
            if (rp.moving)   rl.push_back({ &model_data.walk,  rp.anim_time,  -1 });
            if (rp.punching) rl.push_back({ &model_data.punch, rp.punch_time, model_data.arm_l_node });
            if (rp.blocking) rl.push_back({ &model_data.block, rp.block_time, model_data.arm_r_node, false });
            dc::renderer::Mat4 r_head, r_lhand, r_rhand;
            dc::renderer::pose_model(model_data, rl, rp.pitch, remote_part_world,
                                     { model_data.head_node, model_data.hand_l_node, model_data.hand_r_node },
                                     { &r_head, &r_lhand, &r_rhand });
            float rfeet = (rp.pos[1] - dc::world::EYE_HEIGHT) + MODEL_FOOT_LIFT;
            mat4 rplace;
            glm_mat4_identity(rplace);
            vec3 rpos = { rp.pos[0], rfeet, rp.pos[2] };
            glm_translate(rplace, rpos);
            glm_rotate_y(rplace, -rp.yaw + MODEL_YAW_OFFSET, rplace);

            vec3 remote_color = { 0.4f, 0.5f, 0.95f };
            if (rp.ghost) {
                vec3 pale = { 0.55f, 0.65f, 0.95f };   // dead teammate: faint wisp
                glm_vec3_copy(pale, remote_color);
            } else if (rp.hit_flash > 0.0f) {          // flash red when hit (like the local player)
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(remote_color, red, rp.hit_flash / dc::entity::FLASH_TIME, remote_color);
            }
            renderer.draw_model(player_model, remote_part_world, rplace, remote_color, rp.ghost ? GHOST_ALPHA : 1.0f);

            if (!rp.ghost) {   // a ghost teammate shows only its faint body, no gear/effects
            // Helmet on the head socket.
            mat4 r_helmet; glm_mat4_mul(rplace, r_head.m, r_helmet);
            vec3 helmet_white = { 1.0f, 1.0f, 1.0f };
            renderer.draw_model(helmet_model, helmet_offset, r_helmet, helmet_white);

            // Sword in the left hand (scaled by the remote's blade-size upgrade).
            mat4 r_sword; glm_mat4_mul(rplace, r_lhand.m, r_sword);
            vec3 rsws = { rp.sword_scale, rp.sword_scale, rp.sword_scale };
            glm_scale(r_sword, rsws);
            vec3 sword_color = { 0.8f, 0.8f, 0.9f };
            renderer.draw_model(sword_model, sword_offset, r_sword, sword_color);

            // Shield on the right hand.
            mat4 r_shield; glm_mat4_mul(rplace, r_rhand.m, r_shield);
            vec3 shield_color = { 0.5f, 0.5f, 0.8f };
            renderer.draw_model(shield_model, shield_offset, r_shield, shield_color);

            // Remote specials: match the local render. rig_scale (the rig's hand-bone
            // scale, ~0.22) comes from the posed hand matrix, same as the local avatar.
            if (rp.thrown_active || rp.orbit_active) {
                float rig_scale = std::sqrt(r_lhand.m[0][0] * r_lhand.m[0][0]
                                          + r_lhand.m[0][1] * r_lhand.m[0][1]
                                          + r_lhand.m[0][2] * r_lhand.m[0][2]);
                vec3 spc = { 0.85f, 0.85f, 0.95f };
                if (rp.thrown_active) {
                    float s = rig_scale * rp.sword_scale * rp.thrown_size;
                    mat4 tp; glm_mat4_identity(tp);
                    vec3 tpos = { rp.thrown_x, rp.thrown_y, rp.thrown_z };
                    glm_translate(tp, tpos);
                    glm_rotate_y(tp, rp.thrown_spin, tp);
                    vec3 sc = { s, s, s }; glm_scale(tp, sc);
                    renderer.draw_model(sword_model, sword_offset, tp, spc);
                }
                if (rp.orbit_active && rp.orbit_count > 0) {
                    float s = rig_scale * rp.sword_scale;
                    for (int i = 0; i < rp.orbit_count; ++i) {
                        float a = rp.orbit_angle + (6.2831853f * i) / rp.orbit_count;
                        mat4 op; glm_mat4_identity(op);
                        vec3 opos = { rp.pos[0] + std::cos(a) * rp.orbit_radius,
                                      (rp.pos[1] - dc::world::EYE_HEIGHT) + 1.2f,   // ride the remote's ground
                                      rp.pos[2] + std::sin(a) * rp.orbit_radius };
                        glm_translate(op, opos);
                        glm_rotate_y(op, rp.orbit_spin, op);
                        vec3 osc = { s, s, s }; glm_scale(op, osc);
                        renderer.draw_model(sword_model, sword_offset, op, spc);
                    }
                }
            }
            }   // end if (!rp.ghost)
        }

        // Draw torch models (opaque, posed once at rest above). The flame part
        // glows via its emissive material.
        vec3 torch_tint = { 1.0f, 1.0f, 1.0f };
        for (const auto& tr : torches)
            renderer.draw_model(torch_model, torch_part_world, const_cast<vec4*>(tr.placement), torch_tint);

        // Particle pass last: build camera-facing billboards for every flame and
        // draw them additively over the opaque scene.
        const float PARTICLE_SIZE = 0.12f;
        particle_verts.clear();
        for (const auto& tr : torches)
            dc::fx::append_billboards(tr.ps, renderer.cam_right, renderer.cam_up, PARTICLE_SIZE, particle_verts);

        // Coins: glowing gold billboards that bob just off the floor.
        {
            const float cs = 0.25f;
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (const auto& c : coins) {
                float bob = terrain.height(c.pos[0], c.pos[2]) + 0.45f + 0.08f * std::sin(t_now * 5.0f + c.pos[0]);
                vec3 ctr = { c.pos[0], bob, c.pos[2] };
                auto P = [&](float ax, float ay) {
                    particle_verts.insert(particle_verts.end(), {
                        ctr[0] + (R[0] * ax + U[0] * ay) * cs,
                        ctr[1] + (R[1] * ax + U[1] * ay) * cs,
                        ctr[2] + (R[2] * ax + U[2] * ay) * cs,
                        1.0f, 0.85f, 0.2f, 1.0f });
                };
                P(-1,-1); P(1,-1); P(1,1);
                P(-1,-1); P(1,1); P(-1,1);
            }
        }

        // Enemy projectiles: glowing spheres (additive billboards), per-shot color.
        {
            const float ps = 0.3f;
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (const auto& pr : entities.projectiles) {
                vec3 ctr = { pr.pos[0], pr.pos[1], pr.pos[2] };
                const float cr = pr.color[0], cg = pr.color[1], cb = pr.color[2];
                auto P = [&](float ax, float ay) {
                    particle_verts.insert(particle_verts.end(), {
                        ctr[0] + (R[0] * ax + U[0] * ay) * ps,
                        ctr[1] + (R[1] * ax + U[1] * ay) * ps,
                        ctr[2] + (R[2] * ax + U[2] * ay) * ps,
                        cr, cg, cb, 1.0f });
                };
                P(-1,-1); P(1,-1); P(1,1);
                P(-1,-1); P(1,1); P(-1,1);
            }
        }

        // Shield-bash nova: an expanding translucent shell of white points (a sphere
        // growing from the caster), additive + fading as it spreads. Drawn for the local
        // caster (predicted) and any remote that's bashing.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            auto bash_sphere = [&](float cx, float cy, float cz, float radius, float alpha) {
                if (radius <= 0.05f || alpha <= 0.0f) return;
                const float ps = 0.16f;
                const int N = 64;
                for (int i = 0; i < N; ++i) {
                    const float gy = 1.0f - 2.0f * (i + 0.5f) / N;           // -1..1
                    const float rr = std::sqrt(std::max(0.0f, 1.0f - gy * gy));
                    const float phi = i * 2.399963f;                         // golden angle
                    const float px = cx + std::cos(phi) * rr * radius;
                    const float py = cy + gy * radius;
                    const float pz = cz + std::sin(phi) * rr * radius;
                    auto P = [&](float ax, float ay) {
                        particle_verts.insert(particle_verts.end(), {
                            px + (R[0]*ax + U[0]*ay) * ps, py + (R[1]*ax + U[1]*ay) * ps,
                            pz + (R[2]*ax + U[2]*ay) * ps, 1.0f, 1.0f, 1.0f, alpha });
                    };
                    P(-1,-1); P(1,-1); P(1,1);  P(-1,-1); P(1,1); P(-1,1);
                }
            };
            if (bash.active && player.shield) {
                const float prog = bash.time / player.shield->bash_duration;
                bash_sphere(player.position[0], (player.position[1] - dc::world::EYE_HEIGHT) + 1.0f,
                            player.position[2], bash.radius, (1.0f - prog) * 0.6f);
            }
            for (const auto& rp : remotes)
                if (rp.bash_active)
                    bash_sphere(rp.pos[0], (rp.pos[1] - dc::world::EYE_HEIGHT) + 1.0f, rp.pos[2],
                                rp.bash_radius, 0.4f);
        }

        // Chest price tags: the cost in 7-segment digits, billboarded above each
        // unopened chest, shrinking with distance and culled when far (declutter).
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float COST_MAX = 0.30f;                 // world height cap (near)
            const float COST_NEAR = 6.0f, COST_CULL = 22.0f;
            // one billboarded quad in the (R,U) plane, offset (u,v) from `base`, scaled by sc
            auto quad = [&](const vec3 base, float u0, float v0, float u1, float v1, float sc) {
                auto P = [&](float u, float v) {
                    particle_verts.insert(particle_verts.end(), {
                        base[0] + (R[0]*u + U[0]*v) * sc, base[1] + (R[1]*u + U[1]*v) * sc,
                        base[2] + (R[2]*u + U[2]*v) * sc, 1.0f, 0.85f, 0.2f, 1.0f });
                };
                P(u0,v0); P(u1,v0); P(u1,v1);
                P(u0,v0); P(u1,v1); P(u0,v1);
            };
            for (const auto& ch : chests) {
                if (ch.opened) continue;
                float cx = (ch.col + 0.5f) * dc::world::TILE, cz = (ch.row + 0.5f) * dc::world::TILE;
                float dx = cx - player.position[0], dz = cz - player.position[2];
                float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > COST_CULL) continue;                       // too far: don't render
                float sc = COST_MAX;
                if (dist > COST_NEAR) sc *= COST_NEAR / dist;          // shrink with distance (capped near)

                char num[16]; std::snprintf(num, sizeof num, "%d", ch.cost);
                const int n = static_cast<int>(std::strlen(num));
                const float dw = 0.6f, dh = 1.0f, dt = 0.16f, gap = dw + 0.3f;
                const float total = n * gap - 0.3f;
                vec3 base = { cx, terrain.height(cx, cz) + 2.3f, cz };   // float above the chest
                for (int i = 0; i < n; ++i) {
                    float ox = -total * 0.5f + i * gap;                // center the number, per-digit u offset
                    seven_seg(num[i] - '0', dw, dh, dt, [&](float u0, float v0, float u1, float v1) {
                        quad(base, ox + u0, v0, ox + u1, v1, sc);
                    });
                }
            }
        }

        // Debug: draw the combat cones as flat fans on the floor in front of the
        // player (reuses the additive particle pass: 7 floats/vertex pos+rgba).
        // Red = sword/attack arc; blue = shield block arc.
        if (debug_cone) {
            auto draw_cone = [&](float cx, float cz, float center_yaw, float half, float radius,
                                 float r, float g, float b) {
                const int segs = 18;
                auto push = [&](float x, float z) {   // drape just above the terrain so hills don't bury it
                    particle_verts.insert(particle_verts.end(), { x, terrain.height(x, z) + 0.12f, z, r, g, b, 0.22f });
                };
                for (int s = 0; s < segs; ++s) {
                    float a0 = (center_yaw - half) + (2.0f * half) * (s)     / segs;
                    float a1 = (center_yaw - half) + (2.0f * half) * (s + 1) / segs;
                    push(cx, cz);                                                       // apex
                    push(cx + std::cos(a0) * radius, cz + std::sin(a0) * radius);
                    push(cx + std::cos(a1) * radius, cz + std::sin(a1) * radius);
                }
            };
            // The player's swing is a real 3D cone around the aim direction (so you can
            // see it tilt up at the flyer). Apex at the swing origin; ring at `radius`.
            auto draw_cone3d = [&](const vec3 dir, float half, float radius, float r, float g, float b) {
                vec3 apex = { player.position[0],
                              player.position[1] - dc::world::EYE_HEIGHT + dc::entity::STRIKE_ORIGIN_Y,
                              player.position[2] };
                vec3 up = { 0.0f, 1.0f, 0.0f }, u, v;
                glm_vec3_cross(const_cast<float*>(dir), up, u);
                if (glm_vec3_norm(u) < 1e-3f) { vec3 alt = {1,0,0}; glm_vec3_cross(const_cast<float*>(dir), alt, u); }
                glm_vec3_normalize(u);
                glm_vec3_cross(const_cast<float*>(dir), u, v); glm_vec3_normalize(v);
                const int segs = 20; const float cs = std::cos(half), sn = std::sin(half);
                auto ring = [&](float phi, vec3 out) {
                    for (int k = 0; k < 3; ++k)
                        out[k] = apex[k] + radius * (cs * dir[k] + sn * (std::cos(phi) * u[k] + std::sin(phi) * v[k]));
                };
                auto push = [&](const vec3 p) {
                    particle_verts.insert(particle_verts.end(), { p[0], p[1], p[2], r, g, b, 0.22f });
                };
                for (int s = 0; s < segs; ++s) {
                    vec3 p0, p1;
                    ring(6.2831853f * s / segs, p0);
                    ring(6.2831853f * (s + 1) / segs, p1);
                    push(apex); push(p0); push(p1);
                }
            };
            if (punching)                                                               // attack: red 3D cone, mid-swing
                draw_cone3d(pc.aim, std::acos(pc.strike_cos), pc.strike_reach, 0.9f, 0.15f, 0.15f);
            if (blocking)                                                               // block: blue arc on the floor
                draw_cone(player.position[0], player.position[2], player.yaw,
                          std::acos(pc.block_cos), pc.strike_reach * 0.9f, 0.2f, 0.5f, 1.0f);
            for (const auto& en : entities.items)                                       // enemy swings: orange
                if (en.type == dc::entity::EntityType::Enemy && en.attacking)
                    draw_cone(en.position[0], en.position[2], en.attack_yaw,
                              std::acos(dc::entity::ENEMY_ATTACK_CONE), dc::entity::ENEMY_ATTACK_REACH,
                              1.0f, 0.55f, 0.1f);
            for (const auto& sp : spawners)                                              // spawn zones: green disc
                draw_cone(sp.pos[0], sp.pos[2], 0.0f, 3.14159265f, sp.radius, 0.1f, 0.9f, 0.2f);
            if (thrown.active && player.weapon)                                          // thrown hit area: red disc
                draw_cone(thrown.pos[0], thrown.pos[2], 0.0f, 3.14159265f,
                          player.weapon->throw_radius * player.weapon->throw_size * player.sword_scale, 0.9f, 0.2f, 0.2f);
        }
        renderer.draw_particles(particle_verts);

        // Debug readout in the title bar (cheap stand-in for on-screen text).
        if (debug_cone) {
            char buf[160];
            const char* atk = punching ? "SWING" : "-";
            const char* blk = block_ready ? "BLOCK" : (blocking ? "raising" : "-");
            std::snprintf(buf, sizeof buf,
                          "dungeoncrawl  [DBG]  hp:%.0f  stam:%.0f  coins:%d  enemies:%zu  atk:%s  shield:%s",
                          player.health, player.stamina, currency, entities.items.size(), atk, blk);
            window.set_title(buf);
        }

        // HUD: green stamina bar, bottom-left (NDC rects via the reused particle shader).
        {
            std::vector<float> hud;
            auto hud_rect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
                hud.insert(hud.end(), {
                    x0,y0,0.0f, r,g,b,a,  x1,y0,0.0f, r,g,b,a,  x1,y1,0.0f, r,g,b,a,
                    x0,y0,0.0f, r,g,b,a,  x1,y1,0.0f, r,g,b,a,  x0,y1,0.0f, r,g,b,a });
            };
            auto clamp01 = [](float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); };
            const float x0 = -0.96f, x1 = -0.56f;
            // Health bar (red), just above the stamina bar.
            const float hy0 = -0.88f, hy1 = -0.84f;
            hud_rect(x0, hy0, x1, hy1, 0.05f, 0.05f, 0.05f, 0.6f);
            float hf = clamp01(player.stats.max_health > 0.0f ? player.health / player.stats.max_health : 0.0f);
            // linear interp from dark gray to bright red as health goes from 0% to 100%
            hud_rect(x0, hy0, x0 + (x1 - x0) * hf, hy1, 0.9f, 0.2f, 0.2f, 0.95f);
            // Stamina bar (green).
            const float y0 = -0.93f, y1 = -0.89f;
            hud_rect(x0, y0, x1, y1, 0.05f, 0.05f, 0.05f, 0.6f);
            float sf = clamp01(player.stamina_max > 0.0f ? player.stamina / player.stamina_max : 0.0f);
            hud_rect(x0, y0, x0 + (x1 - x0) * sf, y1, 0.2f, 0.9f, 0.3f, 0.95f);
            // Coin counter (top-left): a gold coin icon + the currency as 7-segment
            // digits built from rects — no font/text renderer needed.
            auto draw_digit = [&](float bx, float by, float w, float h, float t, int d) {
                seven_seg(d, w, h, t, [&](float u0, float v0, float u1, float v1) {
                    hud_rect(bx + u0, by + v0, bx + u1, by + v1, 1.0f, 0.85f, 0.2f, 1.0f);
                });
            };
            {
                const float dw = 0.035f, dh = 0.07f, dt = 0.011f, gap = dw + dt * 2.0f;
                const float by = 0.86f;
                hud_rect(-0.95f, by, -0.91f, by + dh, 1.0f, 0.85f, 0.2f, 1.0f);   // coin icon
                char num[16]; std::snprintf(num, sizeof num, "%d", currency);
                float dx = -0.88f;
                for (char* p = num; *p; ++p) { draw_digit(dx, by, dw, dh, dt, *p - '0'); dx += gap; }
            }
            // Survival timer (top-center): seconds survived, in 7-segment digits.
            {
                const float dw = 0.04f, dh = 0.085f, dt = 0.013f, gap = dw + dt * 2.0f;
                const float by = 0.88f;
                char num[16]; std::snprintf(num, sizeof num, "%d", static_cast<int>(run_time));
                const int n = static_cast<int>(std::strlen(num));
                float dx = -(n * gap) * 0.5f;        // center horizontally
                for (char* p = num; *p; ++p) { draw_digit(dx, by, dw, dh, dt, *p - '0'); dx += gap; }
            }

            // Special-ability icons (bottom-right): "1" throw, "2" orbit (weapon),
            // "3" shield-bash (shield), each boxed. On use a white overlay fills the box
            // and drains down as the cooldown elapses (height = remaining / total).
            {
                auto draw_special = [&](float x0, float y0, float w, float h, int d, float frac) {
                    const float x1 = x0 + w, y1 = y0 + h;
                    hud_rect(x0 - 0.007f, y0 - 0.007f, x1 + 0.007f, y1 + 0.007f, 0.9f, 0.9f, 0.9f, 0.9f);  // border
                    hud_rect(x0, y0, x1, y1, 0.10f, 0.10f, 0.12f, 0.85f);                                  // backing
                    const float t = (w < h ? w : h) * 0.12f;
                    draw_digit(x0 + w * 0.28f, y0 + h * 0.18f, w * 0.45f, h * 0.64f, t, d);                // label
                    if (frac > 0.0f) {                                                                     // cooldown drain
                        const float fh = h * (frac > 1.0f ? 1.0f : frac);
                        hud_rect(x0, y0, x1, y0 + fh, 1.0f, 1.0f, 1.0f, 0.55f);
                    }
                };
                const float bw = 0.06f, bh = 0.105f, by = -0.93f, gap = 0.09f, x3 = 0.74f;
                if (player.weapon) {
                    const float tf = player.weapon->throw_cooldown > 0.0f ? throw_cd / player.weapon->throw_cooldown : 0.0f;
                    const float of = player.weapon->orbit_cooldown > 0.0f ? orbit_cd / player.weapon->orbit_cooldown : 0.0f;
                    draw_special(x3,            by, bw, bh, 1, tf);
                    draw_special(x3 + gap,      by, bw, bh, 2, of);
                }
                if (player.shield) {
                    const float bf = player.shield->bash_cooldown > 0.0f ? bash_cd / player.shield->bash_cooldown : 0.0f;
                    draw_special(x3 + gap * 2.0f, by, bw, bh, 3, bf);
                }
            }

            // Death flash: full-screen red overlay that fades out.
            if (death_flash > 0.0f)
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.7f, 0.0f, 0.0f, clamp01(death_flash / 1.2f) * 0.6f);

            // Upgrade menu: dim the scene + 4 color-coded cards to click.
            if (choosing) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f);   // dim backdrop
                for (int i = 0; i < 4; ++i) {
                    const float x0 = card_x0(i), x1 = x0 + CARD_W;
                    float r, g, b; upgrade_color(cards[i], r, g, b);
                    hud_rect(x0 - 0.008f, CARD_BOT - 0.008f, x1 + 0.008f, CARD_TOP + 0.008f, 0.95f, 0.95f, 0.95f, 0.95f);  // border
                    hud_rect(x0, CARD_BOT, x1, CARD_TOP, r, g, b, 0.95f);   // card
                }
            }

            // First-person crosshair: a small + at screen center (skip while a menu's up).
            // Correct the horizontal extent by aspect so it isn't stretched wide.
            if (!ui_open && !dead) {
                int cw, chh; window.window_size(cw, chh);
                const float aspect = (cw > 0 && chh > 0) ? static_cast<float>(cw) / chh : 1.0f;
                const float t = 0.004f, len = 0.022f;       // thickness, arm length (NDC-y units)
                const float tx = t / aspect, lx = len / aspect;
                hud_rect(-lx, -t, lx, t, 0.95f, 0.95f, 0.95f, 0.85f);   // horizontal arm
                hud_rect(-tx, -len, tx, len, 0.95f, 0.95f, 0.95f, 0.85f);   // vertical arm
            }

            // Pause menu (ESC): dim + a red Quit button (click to exit; ESC to resume).
            if (paused) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.55f);  // dim backdrop
                hud_rect(QX0 - 0.01f, QY0 - 0.01f, QX1 + 0.01f, QY1 + 0.01f, 0.95f, 0.95f, 0.95f, 0.95f);  // border
                hud_rect(QX0, QY0, QX1, QY1, 0.8f, 0.15f, 0.15f, 0.95f);     // red = quit
            }
            renderer.draw_hud(hud);
        }

        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    chest_model.destroy();
    helmet_model.destroy();
    shield_model.destroy();
    torch_model.destroy();
    player_model.destroy();
    sword_model.destroy();
    mesh.destroy();
    terrain_mesh.destroy();
    flyer_mesh.destroy();
    renderer.shutdown();
    net.shutdown();
    window.shutdown();
    return 0;
}
