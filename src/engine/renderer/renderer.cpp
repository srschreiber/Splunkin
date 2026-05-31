#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"
#include "engine/renderer/texture.h"
#include "engine/entity/player.h"

#include <glad/gl.h>
#include <cstring>

namespace dc::renderer {

bool Renderer::init() {
    world_program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!world_program) return false;
    world_viewproj_loc = glGetUniformLocation(world_program, "u_viewproj");

    model_program = load_program("assets/shaders/model.vert", "assets/shaders/model.frag");
    if (!model_program) { shutdown(); return false; }
    model_viewproj_loc = glGetUniformLocation(model_program, "u_viewproj");
    model_model_loc    = glGetUniformLocation(model_program, "u_model");
    model_color_loc    = glGetUniformLocation(model_program, "u_color");

    const char* paths[3] = {
        "assets/textures/stonefloor0.png",
        "assets/textures/stonewall0.png",
        "assets/textures/stoneceiling0.png",
    };
    texture = load_texture_array(paths, 3);
    if (!texture) { shutdown(); return false; }

    glUseProgram(world_program);
    glUniform1i(glGetUniformLocation(world_program, "u_tex"), 0);

    glEnable(GL_DEPTH_TEST);
    return true;
}

void Renderer::begin_frame(const Camera& camera, const dc::entity::Player& player,
                           int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj;
    camera.view_matrix(view, player);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);
}

void Renderer::draw_map(const Mesh& mesh) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    mesh.draw();
}

void Renderer::draw_model(const Model& model, mat4 placement, vec3 color) {
    glUseProgram(model_program);
    glUniformMatrix4fv(model_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform3fv(model_color_loc, 1, color);
    for (const auto& part : model.parts) {
        mat4 nw, m;
        memcpy(nw, part.node_world, sizeof(float) * 16);
        glm_mat4_mul(placement, nw, m);
        glUniformMatrix4fv(model_model_loc, 1, GL_FALSE, reinterpret_cast<const float*>(m));
        glBindVertexArray(part.vao);
        glDrawElements(GL_TRIANGLES, part.index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    if (texture) glDeleteTextures(1, &texture);
    if (world_program) glDeleteProgram(world_program);
    if (model_program) glDeleteProgram(model_program);
    world_program = model_program = texture = 0;
    world_viewproj_loc = model_viewproj_loc = model_model_loc = model_color_loc = -1;
}

} // namespace dc::renderer
