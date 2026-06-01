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

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map));

    dc::renderer::Model player_model;
    player_model.upload(model_data);

    dc::renderer::Camera camera;

    dc::entity::Player player;
    player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
    player.position[1] = dc::world::EYE_HEIGHT;
    player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;

    dc::input::Input input;
    float anim_time = 0.0f;                       // walk-clip clock (advances while moving)
    std::vector<dc::renderer::Mat4> part_world;   // posed per-part transforms
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

        // Walk animation: advance the clip while moving; rest pose when idle.
        bool moving = (forward != 0.0f || strafe != 0.0f);
        if (moving) anim_time += dt; else anim_time = 0.0f;
        dc::renderer::pose_model(model_data, anim_time, moving, part_world);

        // Avatar placement: stand at the player's XZ, facing the look direction.
        // The model's origin sits at its waist (local feet at y~=-1.0), so lift it so
        // the feet rest on the floor; follow the player's vertical position so the
        // avatar rises with the camera on a jump.
        const float MODEL_FOOT_LIFT = 1.0f;
        const float MODEL_YAW_OFFSET = glm_rad(90.0f);  // tune to face forward (try -90 / 0 / 180)
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
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    player_model.destroy();
    mesh.destroy();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
