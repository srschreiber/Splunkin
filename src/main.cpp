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

// A placed chest. One-way: once opened, the lid stays open.
struct Chest {
    int   col = 0, row = 0;
    float open_t = 0.0f;    // time into the open clip (0 = closed)
    bool  opened = false;
};

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
    bool  punch_struck = false;                        // strike lands once per punch
    float attack_cd = 0.0f;                            // weapon cooldown between swings
    std::vector<dc::renderer::Mat4> part_world;        // posed per-part transforms (player)
    std::vector<dc::renderer::Mat4> chest_part_world;  // posed per-part transforms (a chest)
    std::vector<dc::renderer::AnimLayer> layers;       // reused each frame
    bool e_prev = false;                               // for edge-triggered interact
    bool g_prev = false;                               // edge-triggered debug enemy spawn
    bool v_prev = false;                               // edge-triggered debug-cone toggle
    bool debug_cone = false;                           // draw the shield block cone
    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;

        player.add_look(input.mouse_dx, input.mouse_dy);
        float forward = (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool jump = input.key_down(SDL_SCANCODE_SPACE);
        player.update(forward, strafe, jump, dt, *map);

        // Walk clock: advance while moving, reset when idle.
        bool moving = (forward != 0.0f || strafe != 0.0f);
        if (moving) anim_time += dt; else anim_time = 0.0f;

        // Block held (right mouse): only with a shield. You can't block and swing at
        // once, so holding block also forbids starting a swing (checked below).
        bool block_held = player.shield.has_value() && input.mouse_down(SDL_BUTTON_RIGHT);

        // Attack (left mouse): one-shot swing, gated by the weapon's cooldown and by
        // not blocking. The weapon sets playback speed and cooldown; fists fall back.
        const float PUNCH_STRIKE = 0.18f;  // when in the clip the hit lands (clip seconds)
        const float atk_speed  = player.weapon ? player.weapon->attack_speed : dc::entity::UNARMED_ATTACK_SPEED;
        const float atk_cd_dur = player.weapon ? player.weapon->cooldown     : dc::entity::UNARMED_COOLDOWN;
        if (attack_cd > 0.0f) attack_cd -= dt;
        if (input.mouse_down(SDL_BUTTON_LEFT) && !punching && !block_held && attack_cd <= 0.0f) {
            punching = true; punch_time = 0.0f; punch_struck = false;
        }
        bool player_strike = false;        // true only on the frame the swing connects
        if (punching) {
            punch_time += dt * atk_speed;
            if (!punch_struck && punch_time >= PUNCH_STRIKE) { player_strike = true; punch_struck = true; }
            if (!model_data.punch.valid() || punch_time >= model_data.punch.duration) {
                punching = false;
                attack_cd = atk_cd_dur;    // begin the cooldown once the swing ends
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

        // Interact (E, edge-triggered): open the nearest closed chest within reach.
        bool e_now = input.key_down(SDL_SCANCODE_E);
        if (e_now && !e_prev) {
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
            if (best >= 0) chests[best].opened = true;   // one-way open
        }
        e_prev = e_now;

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
        // Shield drives blocking; only mitigates once fully raised (block_ready).
        if (player.shield) {
            pc.blocking    = block_ready;
            pc.block_cos   = player.shield->block_cos;
            pc.block_power = player.shield->block_power;
        } else {
            pc.blocking = false;   // no shield -> can't block
        }
        dc::entity::EnemyHitPlayer hit = dc::entity::update_enemies(entities, *map, flow, pc, dt);
        player.health -= hit.damage;
        if (player.health < 0.0f) player.health = 0.0f;
        player.knock_vel[0] += hit.knock[0];            // integrated (with collision) in player.update next frame
        player.knock_vel[2] += hit.knock[2];
        if (hit.hit) player.hit_flash = dc::entity::FLASH_TIME;   // only UNBLOCKED hits flash red
        if (player.hit_flash > 0.0f) player.hit_flash -= dt;

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

        // Sword: drawn only when a weapon is equipped, attached to the left hand bone.
        if (player.weapon) {
            mat4 sword_place;
            glm_mat4_mul(placement, l_hand_world.m, sword_place);
            vec3 sword_color = { 0.8f, 0.8f, 0.9f };
            renderer.draw_model(sword_model, sword_offset, sword_place, sword_color);
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
        }
        renderer.draw_particles(particle_verts);

        // Debug readout in the title bar (cheap stand-in for on-screen text).
        if (debug_cone) {
            char buf[160];
            const char* atk = punching ? "SWING" : "-";
            const char* blk = block_ready ? "BLOCK" : (blocking ? "raising" : "-");
            std::snprintf(buf, sizeof buf,
                          "dungeoncrawl  [DBG]  hp:%.0f  enemies:%zu  atk:%s  shield:%s  cd:%.2f",
                          player.health, entities.items.size(), atk, blk, attack_cd);
            window.set_title(buf);
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
