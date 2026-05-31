#include "engine/renderer/texture.h"
#include "stb_image.h"
#include <glad/gl.h>
#include <cstdio>
#include <vector>

namespace dc::renderer {

uint32_t load_texture_array(const char* const* paths, int count) {
    if (count <= 0) return 0;

    std::vector<unsigned char*> data(count, nullptr);
    int w0 = 0, h0 = 0;
    for (int i = 0; i < count; ++i) {
        int w = 0, h = 0, n = 0;
        data[i] = stbi_load(paths[i], &w, &h, &n, 4);
        if (!data[i]) {
            std::fprintf(stderr, "texture: cannot load %s\n", paths[i]);
            for (int j = 0; j < i; ++j) stbi_image_free(data[j]);
            return 0;
        }
        if (i == 0) { w0 = w; h0 = h; }
        else if (w != w0 || h != h0) {
            std::fprintf(stderr, "texture: size mismatch %s (%dx%d vs %dx%d)\n",
                         paths[i], w, h, w0, h0);
            for (int j = 0; j <= i; ++j) stbi_image_free(data[j]);
            return 0;
        }
    }

    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w0, h0, count, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < count; ++i) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w0, h0, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, data[i]);
        stbi_image_free(data[i]);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return tex;
}

} // namespace dc::renderer
