#include "engine/renderer/model.h"
#include <cassert>
#include <cstdio>
#include <cstddef>
#include <cstring>

using namespace dc::renderer;

int main() {
    ModelData md;
    bool ok = read_model("assets/models/player.glb", md);
    assert(ok);
    assert(md.parts.size() == 6);

    float miny = 1e9f, maxy = -1e9f, minx = 1e9f, maxx = -1e9f;
    for (const auto& part : md.parts) {
        assert(!part.vertices.empty());
        assert(!part.indices.empty());
        assert(part.vertices.size() % 8 == 0);
        const std::size_t vcount = part.vertices.size() / 8;
        for (std::size_t v = 0; v < vcount; ++v) {
            vec4 p = { part.vertices[v*8+0], part.vertices[v*8+1], part.vertices[v*8+2], 1.0f };
            vec4 w;
            mat4 nw;
            std::memcpy(nw, part.node_world, sizeof(nw));   // copy: cglm's mulv wants non-const
            glm_mat4_mulv(nw, p, w);
            if (w[1] < miny) miny = w[1];
            if (w[1] > maxy) maxy = w[1];
            if (w[0] < minx) minx = w[0];
            if (w[0] > maxx) maxx = w[0];
        }
    }
    const float height = maxy - miny;
    std::printf("model bbox: y[%.3f, %.3f] height=%.3f  x[%.3f, %.3f]\n",
                miny, maxy, height, minx, maxx);
    assert(height > 1.0f && height < 2.5f);   // ~1.8 tall
    assert(maxx > minx);                       // non-degenerate

    std::printf("PASS model_load\n");
    return 0;
}
