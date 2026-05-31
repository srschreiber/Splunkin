#pragma once
#include <cstdint>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"

namespace dc::entity { struct Player; }

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;
    uint32_t texture = 0;          // GL_TEXTURE_2D_ARRAY
    int u_viewproj_loc = -1;

    // Loads world.{vert,frag}, the texture array, enables depth testing. False on failure.
    bool init();
    void render(const Mesh& mesh, const Camera& camera,
                const dc::entity::Player& player, int fb_w, int fb_h);
    void shutdown();
};

} // namespace dc::renderer
