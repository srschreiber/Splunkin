#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!program) return false;
    glEnable(GL_DEPTH_TEST);
    u_viewproj_loc = glGetUniformLocation(program, "u_viewproj");
    return true;
}

void Renderer::render(const Mesh& mesh, const Camera& camera, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj, viewproj;
    camera.view_matrix(view);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    glUseProgram(program);
    glUniformMatrix4fv(u_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));

    mesh.draw();
}

void Renderer::shutdown() {
    if (program) glDeleteProgram(program);
    program = 0;
}

} // namespace dc::renderer
