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
    world_viewproj_loc     = glGetUniformLocation(world_program, "u_viewproj");
    world_light_pos_loc    = glGetUniformLocation(world_program, "u_light_pos");
    world_light_color_loc  = glGetUniformLocation(world_program, "u_light_color");
    world_light_radius_loc = glGetUniformLocation(world_program, "u_light_radius");

    model_program = load_program("assets/shaders/model.vert", "assets/shaders/model.frag");
    if (!model_program) { shutdown(); return false; }
    model_viewproj_loc     = glGetUniformLocation(model_program, "u_viewproj");
    model_model_loc        = glGetUniformLocation(model_program, "u_model");
    model_color_loc        = glGetUniformLocation(model_program, "u_color");
    model_emissive_loc     = glGetUniformLocation(model_program, "u_emissive");
    model_light_pos_loc    = glGetUniformLocation(model_program, "u_light_pos");
    model_light_color_loc  = glGetUniformLocation(model_program, "u_light_color");
    model_light_radius_loc = glGetUniformLocation(model_program, "u_light_radius");

    particle_program = load_program("assets/shaders/particle.vert", "assets/shaders/particle.frag");
    if (!particle_program) { shutdown(); return false; }
    particle_viewproj_loc = glGetUniformLocation(particle_program, "u_viewproj");

    // Dynamic VBO rebuilt every frame from the live particles. Layout: 7 floats
    // per vertex = pos(3) at loc 0, rgba(4) at loc 1.
    glGenVertexArrays(1, &particle_vao);
    glGenBuffers(1, &particle_vbo);
    glBindVertexArray(particle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, particle_vbo);
    const GLsizei pstride = 7 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, pstride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, pstride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

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

void Renderer::begin_frame(dc::world::Map& map, Camera& camera, dc::entity::Player& player,
                           float dt, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj;
    camera.view_matrix(view, player, map, dt);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    // Camera right/up in world space = first two rows of the view rotation
    // (column-major: view[col][row]). Used to billboard particles toward the camera.
    cam_right[0] = view[0][0]; cam_right[1] = view[1][0]; cam_right[2] = view[2][0];
    cam_up[0]    = view[0][1]; cam_up[1]    = view[1][1]; cam_up[2]    = view[2][1];
}

void Renderer::set_light(const vec3 pos, const vec3 color, float radius) {
    glUseProgram(world_program);
    glUniform3fv(world_light_pos_loc, 1, pos);
    glUniform3fv(world_light_color_loc, 1, color);
    glUniform1f(world_light_radius_loc, radius);
    glUseProgram(model_program);
    glUniform3fv(model_light_pos_loc, 1, pos);
    glUniform3fv(model_light_color_loc, 1, color);
    glUniform1f(model_light_radius_loc, radius);
}

void Renderer::draw_map(const Mesh& mesh) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    mesh.draw();
}

void Renderer::draw_model(const Model& model, const std::vector<Mat4>& part_world,
                          mat4 placement, vec3 color) {
    glUseProgram(model_program);
    glUniformMatrix4fv(model_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    const size_t count = model.parts.size() < part_world.size()
                       ? model.parts.size() : part_world.size();
    for (size_t i = 0; i < count; ++i) {
        mat4 nw, m;
        memcpy(nw, part_world[i].m, sizeof(float) * 16);
        glm_mat4_mul(placement, nw, m);
        glUniformMatrix4fv(model_model_loc, 1, GL_FALSE, reinterpret_cast<const float*>(m));
        // per-part material color, modulated by the caller's tint
        const vec3& pc = model.parts[i].color;
        float c[3] = { pc[0] * color[0], pc[1] * color[1], pc[2] * color[2] };
        glUniform3fv(model_color_loc, 1, c);
        // per-part emissive (self-lit, not tinted) — e.g. a glowing torch flame
        glUniform3fv(model_emissive_loc, 1, model.parts[i].emissive);
        // use vao which also references the uploaded ebo/vbo and the format
        glBindVertexArray(model.parts[i].vao);
        glDrawElements(GL_TRIANGLES, model.parts[i].index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void Renderer::draw_particles(const std::vector<float>& verts) {
    if (verts.empty()) return;
    glUseProgram(particle_program);
    glUniformMatrix4fv(particle_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));

    glBindVertexArray(particle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, particle_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);

    // Additive glow: blend onto the scene, and don't write depth (so overlapping
    // particles all show) but still test it (so walls occlude them).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 7));
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    if (texture) glDeleteTextures(1, &texture);
    if (particle_vbo) glDeleteBuffers(1, &particle_vbo);
    if (particle_vao) glDeleteVertexArrays(1, &particle_vao);
    if (world_program) glDeleteProgram(world_program);
    if (model_program) glDeleteProgram(model_program);
    if (particle_program) glDeleteProgram(particle_program);
    world_program = model_program = particle_program = texture = 0;
    particle_vao = particle_vbo = 0;
    world_viewproj_loc = model_viewproj_loc = model_model_loc = model_color_loc = -1;
}

} // namespace dc::renderer
