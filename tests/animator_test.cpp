#include "engine/renderer/model.h"
#include "engine/renderer/animator.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>

using namespace dc::renderer;

static float elem(const Mat4& M, int e) { return M.m[e / 4][e % 4]; }

int main() {
    ModelData md;
    assert(read_model("assets/models/player.glb", md));
    assert(md.walk.valid());
    assert(md.walk.duration > 0.0f);

    std::vector<Mat4> rest, mid, loop0, loopd;

    // Rest pose: one matrix per part, all finite.
    pose_model(md, 0.0f, false, rest);
    assert(rest.size() == md.parts.size());
    for (const auto& M : rest)
        for (int e = 0; e < 16; ++e) assert(std::isfinite(elem(M, e)));

    // Animation has a visible effect: mid-clip differs from rest for some part.
    pose_model(md, md.walk.duration * 0.5f, true, mid);
    bool differs = false;
    for (std::size_t i = 0; i < rest.size() && !differs; ++i)
        for (int e = 0; e < 16; ++e)
            if (std::fabs(elem(rest[i], e) - elem(mid[i], e)) > 1e-4f) { differs = true; break; }
    assert(differs);

    // The clip loops: pose(t) == pose(t + duration).
    pose_model(md, 0.1f, true, loop0);
    pose_model(md, 0.1f + md.walk.duration, true, loopd);
    assert(loop0.size() == loopd.size());
    for (std::size_t i = 0; i < loop0.size(); ++i)
        for (int e = 0; e < 16; ++e)
            assert(std::fabs(elem(loop0[i], e) - elem(loopd[i], e)) < 1e-3f);

    std::printf("PASS animator\n");
    return 0;
}
