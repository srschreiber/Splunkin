#pragma once
#include <cstdint>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;
    int u_viewproj_loc = -1;

    // Loads world.{vert,frag} and enables depth testing. Returns false on failure.
    bool init();
    // Clears, sets the view-projection uniform from the camera, and draws the mesh.
    void render(const Mesh& mesh, const Camera& camera, int fb_w, int fb_h);
    void shutdown();
};

} // namespace dc::renderer
