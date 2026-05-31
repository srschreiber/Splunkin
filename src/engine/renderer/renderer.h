#pragma once
#include <cstdint>
#include <cglm/cglm.h>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"

namespace dc::entity { struct Player; }

namespace dc::renderer {

struct Renderer {
    uint32_t world_program = 0;   // textured map
    uint32_t model_program = 0;   // flat-lit model
    uint32_t texture = 0;         // GL_TEXTURE_2D_ARRAY for the map
    int world_viewproj_loc = -1;
    int model_viewproj_loc = -1, model_model_loc = -1, model_color_loc = -1;
    mat4 viewproj;                // computed each frame in begin_frame

    bool init();
    // Set viewport, clear color+depth, compute the frame's view-projection.
    void begin_frame(const Camera& camera, const dc::entity::Player& player, int fb_w, int fb_h);
    // Draw the textured map mesh.
    void draw_map(const Mesh& mesh);
    // Draw a model at `placement` (each part uses placement * node_world), flat color.
    void draw_model(const Model& model, mat4 placement, vec3 color);
    void shutdown();
};

} // namespace dc::renderer
