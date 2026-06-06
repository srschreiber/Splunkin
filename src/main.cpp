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

#include <SDL3/SDL.h>
#include <cglm/cglm.h>
#include <cmath>
#include <cstdio>
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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (argv[i][0] != '-') map_path = argv[i];
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

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map));

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
    dc::entity::EntityList entities;
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
    float anim_time = 0.0f;                       // walk-clip clock (advances while moving)
    float punch_time = 0.0f;                      // punch-clip clock (advances while punching)
    float block_time = 0.0f;                      // block-clip clock (advances while blocking)
    bool  punching = false;
    bool  blocking = false;
    bool  exhausted = false;                           // winded: must recover before sprint/block
    bool  choosing = false;                            // upgrade-card menu open (freezes movement)
    bool  menu_click_prev = false;                     // edge-detect the card click
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
    std::vector<dc::renderer::Mat4> part_world;        // posed per-part transforms (player)
    std::vector<dc::renderer::Mat4> chest_part_world;  // posed per-part transforms (a chest)
    std::vector<dc::renderer::AnimLayer> layers;       // reused each frame
    bool e_prev = false;                               // for edge-triggered interact
    bool g_prev = false;                               // edge-triggered debug enemy spawn
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
        if (choosing) { choosing = false; window.set_relative_mouse(true); }
        // Clear all chest upgrades.
        player.stamina_mult = 1.0f;
        player.damage_mult = 1.0f;
        player.swing_reach_bonus = 0.0f;
        player.swing_cone_bonus = 0.0f;
        player.sword_scale = 1.0f;
        player.stats.knockback = base_knockback;
        currency = 0;
        run_time = 0.0f;
        coins.clear();
        entities.items.clear();
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

    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;
        run_time += dt;   // survival timer

        if (!choosing) player.add_look(input.mouse_dx, input.mouse_dy);   // freeze look in the menu
        float forward = choosing ? 0.0f : (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                                        - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = choosing ? 0.0f : (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                                        - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool jump = !choosing && input.key_down(SDL_SCANCODE_SPACE);
        bool moving = (forward != 0.0f || strafe != 0.0f);
        // Exhaustion: hitting 0 stamina winds you until it recovers past a threshold.
        // (Otherwise regen re-enables sprint/block one frame at a time = stutter-sprint.)
        const float EXHAUST_RECOVER = player.stamina_max * 0.25f;
        if (player.stamina <= 0.0f) exhausted = true;
        else if (player.stamina >= EXHAUST_RECOVER) exhausted = false;
        // Run while holding Shift (needs stamina). Drains stamina (applied below).
        bool running = moving && !exhausted && input.key_down(SDL_SCANCODE_LSHIFT);
        player.speed = running ? dc::entity::RUN_SPEED : dc::entity::MOVE_SPEED;
        player.update(forward, strafe, jump, dt, *map);

        // Walk clock: advance while moving (faster while running), reset when idle.
        if (moving) anim_time += dt * (running ? 1.7f : 1.0f); else anim_time = 0.0f;

        // Block held (right mouse): needs a shield and some stamina. You can't block
        // and swing at once, so holding block also forbids starting a swing (below).
        bool block_held = player.shield.has_value() && !exhausted && !choosing
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

        // Orbit special (2): summon spinning swords for a while (big stamina cost).
        if (player.weapon && !choosing && !orbit.active && orbit_cd <= 0.0f
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
        if (!punching && !block_held && !thrown.active && !choosing) {
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
        if (punching) {
            punch_time += dt * atk_speed;
            if (!punch_struck && punch_time >= PUNCH_STRIKE) {
                punch_struck = true;
                if (punch_is_throw && player.weapon) {
                    // Release: detach the sword as a spinning projectile flying forward.
                    thrown.active = true; thrown.returning = false;
                    thrown.traveled = 0.0f; thrown.spin = 0.0f; thrown.hit_ids.clear();
                    thrown.pos[0] = player.position[0]; thrown.pos[1] = 0.0f; thrown.pos[2] = player.position[2];
                    vec3 f; player.front(f);
                    float fl = std::sqrt(f[0] * f[0] + f[2] * f[2]);
                    thrown.dir[0] = fl > 1e-4f ? f[0] / fl : 0.0f;
                    thrown.dir[2] = fl > 1e-4f ? f[2] / fl : 1.0f;
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
        else if (running)  player.stamina -= dc::entity::RUN_STAMINA_PER_SEC * dt * player.stamina_mult;
        else               player.stamina += player.stamina_regen * dt;
        if (player.stamina > player.stamina_max) player.stamina = player.stamina_max;
        if (player.stamina < 0.0f) player.stamina = 0.0f;

        // Interact (E, edge-triggered): buy the nearest closed chest within reach.
        bool e_now = input.key_down(SDL_SCANCODE_E);
        if (e_now && !e_prev && !choosing) {
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
            // Pay to open: deduct coins, open the chest, and present the upgrade menu.
            if (best >= 0 && currency >= chests[best].cost) {
                currency -= chests[best].cost;
                chests[best].opened = true;     // one-way open
                choosing = true;                // modal upgrade pick (freezes movement)
                menu_click_prev = true;         // ignore the in-flight E/click frame
                window.set_relative_mouse(false);  // free the cursor for clicking cards
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

        // Debug: G spawns an enemy a couple tiles in front of the player.
        bool g_now = input.key_down(SDL_SCANCODE_G);
        if (g_now && !g_prev) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 2.0f, player.position[2] + f[2] * 2.0f);
        }
        g_prev = g_now;

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
            if (!thrown.returning) {
                float step = w.throw_speed * dt;
                thrown.pos[0] += thrown.dir[0] * step;
                thrown.pos[2] += thrown.dir[2] * step;
                thrown.traveled += step;
                if (thrown.traveled >= w.throw_distance) { thrown.returning = true; thrown.hit_ids.clear(); }
            } else {
                float hx = player.position[0] - thrown.pos[0];
                float hz = player.position[2] - thrown.pos[2];
                float hd = std::sqrt(hx * hx + hz * hz);
                if (hd < 1.0f) thrown.active = false;  // caught -> sword back in hand
                else { thrown.pos[0] += hx / hd * w.throw_speed * dt; thrown.pos[2] += hz / hd * w.throw_speed * dt; }
            }
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
                for (int i = 0; i < w.orbit_count; ++i) {
                    float a = orbit.angle + (6.2831853f * i) / w.orbit_count;
                    vec3 p = { player.position[0] + std::cos(a) * w.orbit_radius, 0.0f,
                               player.position[2] + std::sin(a) * w.orbit_radius };
                    dc::entity::radius_attack(entities, p, w.orbit_hit_radius * player.sword_scale,
                                              w.orbit_damage * player.damage_mult, player.stats.knockback, orbit.hit_ids);
                }
            }
        }

        // Enemies: flow-field from the player's tile, then step the enemy sim.
        int pcol = static_cast<int>(player.position[0] / dc::world::TILE);
        int prow = static_cast<int>(player.position[2] / dc::world::TILE);
        dc::world::FlowField flow = dc::world::compute_flow(*map, pcol, prow);
        dc::entity::PlayerCombat pc{};
        glm_vec3_copy(player.position, pc.pos);
        pc.yaw = player.yaw;
        pc.strike = player_strike;
        pc.strike_knockback = player.stats.knockback;
        pc.weight           = player.stats.weight;
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
        // Shield drives blocking; only mitigates once fully raised (block_ready).
        if (player.shield) {
            pc.blocking    = block_ready;
            pc.block_cos   = player.shield->block_cos;
            pc.block_power = player.shield->block_power;
        } else {
            pc.blocking = false;   // no shield -> can't block
        }
        frame_deaths.clear();
        dc::entity::EnemyHitPlayer hit = dc::entity::update_enemies(entities, *map, flow, pc, dt, &frame_deaths);
        player.health -= hit.damage;
        if (player.health < 0.0f) player.health = 0.0f;
        player.knock_vel[0] += hit.knock[0];            // integrated (with collision) in player.update next frame
        player.knock_vel[2] += hit.knock[2];
        if (hit.hit) player.hit_flash = dc::entity::FLASH_TIME;   // only UNBLOCKED hits flash red
        if (player.hit_flash > 0.0f) player.hit_flash -= dt;
        if (hit.blocked && player.shield) {                      // absorbing a hit costs stamina
            player.stamina -= player.shield->stamina_per_hit * player.stamina_mult;
            if (player.stamina < 0.0f) player.stamina = 0.0f;
        }

        // Drop a coin where each enemy died (frame_deaths holds xyz triples).
        for (std::size_t i = 0; i + 2 < frame_deaths.size(); i += 3) {
            Coin c; c.pos[0] = frame_deaths[i]; c.pos[1] = frame_deaths[i + 1]; c.pos[2] = frame_deaths[i + 2];
            coins.push_back(c);
        }

        // Coins: settle briefly (so they're always visible), then magnet to the
        // player when close and collect on contact.
        const float MAGNET_RADIUS = 1.8f, COLLECT_RADIUS = 0.6f, COIN_SPEED = 7.0f, COIN_SETTLE = 0.35f;
        for (std::size_t i = 0; i < coins.size();) {
            coins[i].age += dt;
            float dx = player.position[0] - coins[i].pos[0];
            float dz = player.position[2] - coins[i].pos[2];
            float d = std::sqrt(dx * dx + dz * dz);
            if (coins[i].age >= COIN_SETTLE) {        // only after it has settled
                if (d < COLLECT_RADIUS) { currency += static_cast<int>(coins[i].value); coins[i] = coins.back(); coins.pop_back(); continue; }
                if (d < MAGNET_RADIUS && d > 1e-4f) {
                    float step = COIN_SPEED * dt;
                    coins[i].pos[0] += dx / d * step;
                    coins[i].pos[2] += dz / d * step;
                }
            }
            ++i;
        }

        // Death = game over (solo): reset the run, flash the screen red.
        if (player.health <= 0.0f && death_flash <= 0.0f) {
            reset_run();
            death_flash = 1.2f;
        }
        if (death_flash > 0.0f) death_flash -= dt;

        // Difficulty ramps with survival time: faster spawns + a higher cap.
        // (run_time resets on death, so this scales back down too.)
        const float difficulty = 1.0f + run_time / 25.0f;   // +1x base every 25s
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
        renderer.begin_frame(*map, camera, player, dt, w, h);
        renderer.set_light(light_pos, light_color, LIGHT_RADIUS);
        renderer.draw_map(mesh);
        vec3 player_color = { 0.80f, 0.45f, 0.35f };
        if (player.hit_flash > 0.0f) {                 // flash red when hit
            vec3 red = { 1.0f, 0.1f, 0.1f };
            glm_vec3_lerp(player_color, red, player.hit_flash / dc::entity::FLASH_TIME, player_color);
        }
        renderer.draw_model(player_model, part_world, placement, player_color);

        // Helmet: attached to the head bone socket. Its world = placement * headWorld
        // * offset, so it rides head-look/walk/jump for free. White tint -> material color.
        mat4 helmet_place;
        glm_mat4_mul(placement, head_world.m, helmet_place);
        vec3 helmet_color = { 1.0f, 1.0f, 1.0f };
        renderer.draw_model(helmet_model, helmet_offset, helmet_place, helmet_color);

        // Sword in hand: only when equipped AND not currently thrown.
        if (player.weapon && !thrown.active) {
            mat4 sword_place;
            glm_mat4_mul(placement, l_hand_world.m, sword_place);
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
            vec3 tpos = { thrown.pos[0], 0.7f, thrown.pos[2] };   // waist height
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
                vec3 opos = { player.position[0] + std::cos(a) * w.orbit_radius, 0.8f,
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
            glm_mat4_mul(placement, r_hand_world.m, shield_place);
            vec3 shield_color = { 0.5f, 0.5f, 0.8f };
            renderer.draw_model(shield_model, shield_offset, shield_place, shield_color);
        }
        // Draw each chest, lid posed by its open_t (open clip only animates the lid).
        // White tint -> the per-part material colors from the .glb show through unchanged.
        vec3 chest_color = { 1.0f, 1.0f, 1.0f };
        for (const auto& ch : chests) {
            std::vector<dc::renderer::AnimLayer> cl = {{ &chest_data.open, ch.open_t, -1, false }};  // one-shot: hold open
            dc::renderer::pose_model(chest_data, cl, 0.0f, chest_part_world);
            mat4 cplace;
            glm_mat4_identity(cplace);
            vec3 cpos = { (ch.col + 0.5f) * dc::world::TILE, 0.0f, (ch.row + 0.5f) * dc::world::TILE };
            glm_translate(cplace, cpos);            // move to the tile (origin at the chest's base)
            vec3 cscale = { 0.75f, 0.75f, 0.75f };
            glm_scale(cplace, cscale);              // half size, scaled around its base -> stays on floor
            renderer.draw_model(chest_model, chest_part_world, cplace, chest_color);
        }

        // Draw enemies — reuse the player model, tinted green, posed by their
        // own walk/attack clocks and facing the player.
        vec3 enemy_color = { 0.25f, 0.80f, 0.30f };
        for (const auto& en : entities.items) {
            if (en.type != dc::entity::EntityType::Enemy) continue;
            std::vector<dc::renderer::AnimLayer> el;
            if (en.attacking)        el.push_back({ &model_data.punch, en.attack_time, model_data.arm_l_node });
            else if (en.anim_time > 0.0f) el.push_back({ &model_data.walk, en.anim_time, -1 });
            dc::renderer::pose_model(model_data, el, 0.0f, enemy_part_world);
            mat4 eplace;
            glm_mat4_identity(eplace);
            vec3 epos = { en.position[0], MODEL_FOOT_LIFT, en.position[2] };
            glm_translate(eplace, epos);
            glm_rotate_y(eplace, -en.yaw + MODEL_YAW_OFFSET, eplace);
            vec3 col; glm_vec3_copy(enemy_color, col);
            if (en.hit_flash > 0.0f) {                 // flash red when struck
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(col, red, en.hit_flash / dc::entity::FLASH_TIME, col);
            }
            renderer.draw_model(player_model, enemy_part_world, eplace, col);
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
                float bob = 0.45f + 0.08f * std::sin(t_now * 5.0f + c.pos[0]);
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
                vec3 base = { cx, 2.3f, cz };                          // float above the chest
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
            const float cy = 0.05f;                       // just above the floor
            auto draw_cone = [&](float cx, float cz, float center_yaw, float half, float radius,
                                 float r, float g, float b) {
                const int segs = 18;
                auto push = [&](float x, float z) {
                    particle_verts.insert(particle_verts.end(), { x, cy, z, r, g, b, 0.22f });
                };
                for (int s = 0; s < segs; ++s) {
                    float a0 = (center_yaw - half) + (2.0f * half) * (s)     / segs;
                    float a1 = (center_yaw - half) + (2.0f * half) * (s + 1) / segs;
                    push(cx, cz);                                                       // apex
                    push(cx + std::cos(a0) * radius, cz + std::sin(a0) * radius);
                    push(cx + std::cos(a1) * radius, cz + std::sin(a1) * radius);
                }
            };
            if (punching)                                                               // attack: red, only mid-swing
                draw_cone(player.position[0], player.position[2], player.yaw,
                          std::acos(pc.strike_cos), pc.strike_reach, 0.9f, 0.15f, 0.15f);
            if (blocking)                                                               // block: blue, while raising/raised
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
    renderer.shutdown();
    window.shutdown();
    return 0;
}
