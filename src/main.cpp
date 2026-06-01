#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"
#include "engine/renderer/animator.h"
#include "engine/entity/player.h"
#include "engine/world/map.h"
#include "engine/world/map_mesh.h"

#include <SDL3/SDL.h>
#include <cglm/cglm.h>
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

    // Spawn a chest entity per 'C' tile in the map.
    std::vector<Chest> chests;
    for (const auto& cs : map->chests) chests.push_back({ cs.col, cs.row });

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
    std::vector<dc::renderer::Mat4> part_world;        // posed per-part transforms (player)
    std::vector<dc::renderer::Mat4> chest_part_world;  // posed per-part transforms (a chest)
    std::vector<dc::renderer::AnimLayer> layers;       // reused each frame
    bool e_prev = false;                               // for edge-triggered interact
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

        // Punch (left mouse): one-shot — start on press, advance until the clip finishes.
        const float PUNCH_SPEED = 1.5f;   // play the punch 1.5x faster than authored
        if (input.mouse_down(SDL_BUTTON_LEFT) && !punching) { punching = true; punch_time = 0.0f; }
        if (punching) {
            punch_time += dt * PUNCH_SPEED;
            if (!model_data.punch.valid() || punch_time >= model_data.punch.duration) punching = false;
        }

        // Block (right mouse): held — advance the clip while down, hold the pose
        // (sampling clamps at the last keyframe), reset when released.
        blocking = input.mouse_down(SDL_BUTTON_RIGHT);
        if (blocking) block_time += dt; else block_time = 0.0f;

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

        // Build the animation layers for this frame: walk drives the body, punch
        // is masked to the armL bone so you can punch while walking. (No layers
        // when idle -> rest pose.)
        layers.clear();
        if (moving)   layers.push_back({ &model_data.walk,  anim_time,  -1 });
        if (punching) layers.push_back({ &model_data.punch, punch_time, model_data.arm_l_node });
        if (blocking) layers.push_back({ &model_data.block, block_time, model_data.arm_r_node });  // block masked to the right arm
        dc::renderer::pose_model(model_data, layers, player.pitch, part_world);

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
        renderer.draw_map(mesh);
        vec3 player_color = { 0.80f, 0.45f, 0.35f };
        renderer.draw_model(player_model, part_world, placement, player_color);

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
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    chest_model.destroy();
    player_model.destroy();
    mesh.destroy();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
