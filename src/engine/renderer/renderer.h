#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"
#include "engine/renderer/animator.h"

namespace dc::entity { struct Player; }

namespace dc::renderer {

struct Renderer {
    uint32_t world_program = 0;   // textured map
    uint32_t model_program = 0;   // flat-lit model
    uint32_t particle_program = 0;// additive billboards
    uint32_t texture = 0;         // GL_TEXTURE_2D_ARRAY for the map
    int world_viewproj_loc = -1, world_use_solid_loc = -1, world_solid_loc = -1;
    int model_viewproj_loc = -1, model_model_loc = -1, model_color_loc = -1, model_emissive_loc = -1, model_alpha_loc = -1;
    int particle_viewproj_loc = -1;
    // Point-light uniforms (one nearest torch), looked up per program.
    int world_light_pos_loc = -1, world_light_color_loc = -1, world_light_radius_loc = -1;
    int model_light_pos_loc = -1, model_light_color_loc = -1, model_light_radius_loc = -1;
    uint32_t particle_vao = 0, particle_vbo = 0;
    mat4 viewproj;                // computed each frame in begin_frame
    vec3 cam_right = {1.0f, 0.0f, 0.0f};  // camera basis (world space), for billboards
    vec3 cam_up    = {0.0f, 1.0f, 0.0f};

    bool init();
    // Set viewport, clear color+depth, compute the frame's view-projection + camera basis.
    void begin_frame(dc::world::Map& map, Camera& camera, dc::entity::Player& player, float dt, int fb_w, int fb_h);
    // Set the single point light for this frame on both lit programs. color=0 -> no torch.
    void set_light(const vec3 pos, const vec3 color, float radius);
    // Draw the textured wall mesh.
    void draw_map(const Mesh& mesh);
    // Draw the solid-color terrain floor mesh (same world program, no texture).
    void draw_terrain(const Mesh& mesh, const vec3 color);
    // Draw a model: each part i uses placement * part_world[i] (from pose_model), flat color.
    // alpha < 1 alpha-blends the model (used to render dead players as faint ghosts).
    void draw_model(const Model& model, const std::vector<Mat4>& part_world, mat4 placement, vec3 color, float alpha = 1.0f);
    // Draw pre-billboarded particle vertices (7 floats each: pos3 + rgba). Additive, depth-write off.
    void draw_particles(const std::vector<float>& verts);
    // Draw 2D HUD triangles in NDC (7 floats each: pos3 + rgba). Alpha-blended, no depth.
    // Reuses the particle program with an identity view-projection.
    void draw_hud(const std::vector<float>& verts);
    void shutdown();
};

} // namespace dc::renderer
