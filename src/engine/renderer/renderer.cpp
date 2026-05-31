#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"
#include "engine/renderer/texture.h"
#include "engine/entity/player.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!program) return false;
    u_viewproj_loc = glGetUniformLocation(program, "u_viewproj");

    const char* paths[3] = {
        "assets/textures/stonefloor0.png",
        "assets/textures/stonewall0.png",
        "assets/textures/stoneceiling0.png",
    };
    texture = load_texture_array(paths, 3);
    if (!texture) { shutdown(); return false; }  // don't leak the linked program

    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_tex"), 0);  // sampler uses texture unit 0

    glEnable(GL_DEPTH_TEST);
    return true;
}

void Renderer::render(const Mesh& mesh, const Camera& camera,
                      const dc::entity::Player& player, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj, viewproj;
    camera.view_matrix(view, player);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    glUseProgram(program);
    glUniformMatrix4fv(u_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);

    mesh.draw();
}

void Renderer::shutdown() {
    if (texture) glDeleteTextures(1, &texture);
    if (program) glDeleteProgram(program);
    program = 0;
    texture = 0;
    u_viewproj_loc = -1;
}

} // namespace dc::renderer
