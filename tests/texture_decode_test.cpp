#include "stb_image.h"
#include <cassert>
#include <cstdio>

int main() {
    const char* paths[3] = {
        "assets/textures/stonefloor0.png",
        "assets/textures/stonewall0.png",
        "assets/textures/stoneceiling0.png",
    };
    for (int i = 0; i < 3; ++i) {
        int w = 0, h = 0, n = 0;
        unsigned char* d = stbi_load(paths[i], &w, &h, &n, 4);
        assert(d != nullptr);
        assert(w == 32 && h == 32);
        stbi_image_free(d);
    }
    std::printf("PASS texture_decode\n");
    return 0;
}
