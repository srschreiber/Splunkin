#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"
#include "engine/renderer/texture.h"
#include "engine/entity/player.h"

#include <glad/gl.h>
#include <cstring>
#include <cstdio>
#include <fstream>

namespace dc::renderer {

namespace {
// Baked glyph range + atlas size. 32..126 covers printable ASCII. Baked once at a
// generous pixel height; draw_text scales down (or up) from there.
constexpr int   FONT_FIRST    = 32;
constexpr int   FONT_COUNT    = 95;     // 32..126
constexpr int   FONT_ATLAS_W  = 512;
constexpr int   FONT_ATLAS_H  = 512;
constexpr float FONT_BAKE_PX  = 64.0f;
constexpr char  FONT_PATH[]   = "assets/fonts/sansrounded.ttf";

bool read_file(const char* path, std::vector<unsigned char>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    out.resize(static_cast<size_t>(n));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}
} // namespace

bool Renderer::init() {
    world_program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!world_program) return false;
    world_viewproj_loc     = glGetUniformLocation(world_program, "u_viewproj");
    world_use_solid_loc    = glGetUniformLocation(world_program, "u_use_solid");
    world_solid_loc        = glGetUniformLocation(world_program, "u_solid");
    world_ambient_loc      = glGetUniformLocation(world_program, "u_ambient");
    world_campos_loc       = glGetUniformLocation(world_program, "u_cam_pos");
    world_fog_loc          = glGetUniformLocation(world_program, "u_fog_color");
    world_time_loc         = glGetUniformLocation(world_program, "u_time");
    world_light_count_loc  = glGetUniformLocation(world_program, "u_light_count");
    world_light_pos_loc    = glGetUniformLocation(world_program, "u_light_pos");
    world_light_color_loc  = glGetUniformLocation(world_program, "u_light_color");
    world_light_radius_loc = glGetUniformLocation(world_program, "u_light_radius");

    model_program = load_program("assets/shaders/model.vert", "assets/shaders/model.frag");
    if (!model_program) { shutdown(); return false; }
    model_viewproj_loc     = glGetUniformLocation(model_program, "u_viewproj");
    model_model_loc        = glGetUniformLocation(model_program, "u_model");
    model_color_loc        = glGetUniformLocation(model_program, "u_color");
    model_alpha_loc        = glGetUniformLocation(model_program, "u_alpha");
    model_emissive_loc     = glGetUniformLocation(model_program, "u_emissive");
    model_ambient_loc      = glGetUniformLocation(model_program, "u_ambient");
    model_campos_loc       = glGetUniformLocation(model_program, "u_cam_pos");
    model_fog_loc          = glGetUniformLocation(model_program, "u_fog_color");
    model_light_count_loc  = glGetUniformLocation(model_program, "u_light_count");
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

    // Text: textured-quad program + a baked glyph atlas. Optional — if either the
    // shader or the font is missing we warn and run without text (text_program stays 0).
    text_program = load_program("assets/shaders/text.vert", "assets/shaders/text.frag");
    if (text_program) {
        text_atlas_loc = glGetUniformLocation(text_program, "u_atlas");
        glGenVertexArrays(1, &text_vao);
        glGenBuffers(1, &text_vbo);
        glBindVertexArray(text_vao);
        glBindBuffer(GL_ARRAY_BUFFER, text_vbo);
        const GLsizei tstride = 8 * sizeof(float);   // pos(2) + uv(2) + rgba(4)
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, tstride, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, tstride, (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, tstride, (void*)(4 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);

        std::vector<unsigned char> ttf;
        if (read_file(FONT_PATH, ttf)) {
            std::vector<unsigned char> bitmap(static_cast<size_t>(FONT_ATLAS_W) * FONT_ATLAS_H);
            text_chars.resize(FONT_COUNT);
            stbtt_BakeFontBitmap(ttf.data(), 0, FONT_BAKE_PX, bitmap.data(),
                                 FONT_ATLAS_W, FONT_ATLAS_H, FONT_FIRST, FONT_COUNT, text_chars.data());
            text_bake_px = FONT_BAKE_PX;
            // Baseline drop from the top of the EM box, so callers can treat (x,y) as the
            // text's top-left corner.
            stbtt_fontinfo info;
            if (stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
                const float s = stbtt_ScaleForPixelHeight(&info, FONT_BAKE_PX);
                int asc = 0, desc = 0, gap = 0;
                stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
                text_ascent = asc * s;
            } else {
                text_ascent = FONT_BAKE_PX * 0.8f;
            }
            // Upload the coverage atlas as a single-channel (R8) texture. Rows are byte-
            // packed, so drop the unpack alignment to 1 for the upload.
            glGenTextures(1, &text_atlas);
            glBindTexture(GL_TEXTURE_2D, text_atlas);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_W, FONT_ATLAS_H, 0,
                         GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        } else {
            std::fprintf(stderr, "renderer: could not load font %s — text disabled\n", FONT_PATH);
        }
    } else {
        std::fprintf(stderr, "renderer: text shader failed to load — text disabled\n");
    }

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

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj;
    camera.view_matrix(view, player, map, dt);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    // Camera right/up in world space = first two rows of the view rotation
    // (column-major: view[col][row]). Used to billboard particles toward the camera.
    cam_right[0] = view[0][0]; cam_right[1] = view[1][0]; cam_right[2] = view[2][0];
    cam_up[0]    = view[0][1]; cam_up[1]    = view[1][1]; cam_up[2]    = view[2][1];
    // Camera WORLD position = translation of the inverse view matrix (for fog + rim light).
    mat4 inv_view; glm_mat4_inv(view, inv_view);
    cam_pos[0] = inv_view[3][0]; cam_pos[1] = inv_view[3][1]; cam_pos[2] = inv_view[3][2];

    // Atmospheric horizon/fog color from day/night ambient: soft blue-grey by day, deep
    // blue at night. The sky is cleared to this so distant geometry melts into the horizon.
    const float a = ambient_state < 0.0f ? 0.0f : (ambient_state > 1.0f ? 1.0f : ambient_state);
    const float t = a * a * (3.0f - 2.0f * a);   // smoothstep day<->night
    // Night = a deep dark BLUE; day = soft blue-grey haze.
    vec3 fog = { 0.03f + t * 0.53f, 0.05f + t * 0.58f, 0.17f + t * 0.54f };
    glClearColor(fog[0], fog[1], fog[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    time_state += dt;
    glUseProgram(world_program);
    glUniform3fv(world_campos_loc, 1, cam_pos);
    glUniform3fv(world_fog_loc, 1, fog);
    glUniform1f(world_time_loc, time_state);
    glUseProgram(model_program);
    glUniform3fv(model_campos_loc, 1, cam_pos);
    glUniform3fv(model_fog_loc, 1, fog);
}

void Renderer::set_ambient(float ambient) {
    ambient_state = ambient;   // begin_frame derives the fog/sky color from this
    glUseProgram(world_program); glUniform1f(world_ambient_loc, ambient);
    glUseProgram(model_program); glUniform1f(model_ambient_loc, ambient);
}

void Renderer::set_lights(int count, const float* pos, const float* color, const float* radius) {
    if (count > MAX_LIGHTS) count = MAX_LIGHTS;
    if (count < 0) count = 0;
    glUseProgram(world_program);
    glUniform1i(world_light_count_loc, count);
    if (count > 0) {
        glUniform3fv(world_light_pos_loc, count, pos);
        glUniform3fv(world_light_color_loc, count, color);
        glUniform1fv(world_light_radius_loc, count, radius);
    }
    glUseProgram(model_program);
    glUniform1i(model_light_count_loc, count);
    if (count > 0) {
        glUniform3fv(model_light_pos_loc, count, pos);
        glUniform3fv(model_light_color_loc, count, color);
        glUniform1fv(model_light_radius_loc, count, radius);
    }
}

void Renderer::draw_map(const Mesh& mesh) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform1i(world_use_solid_loc, 0);   // textured
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    mesh.draw();
}

void Renderer::draw_terrain(const Mesh& mesh, const vec3 color, bool plain) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform1i(world_use_solid_loc, plain ? 2 : 1);   // 1 = terrain albedo, 2 = flat solid color
    glUniform3fv(world_solid_loc, 1, color);
    mesh.draw();
}

void Renderer::draw_water(const Mesh& mesh, const vec3 color) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform1i(world_use_solid_loc, 3);   // 3 = animated reflective water
    glUniform3fv(world_solid_loc, 1, color);
    mesh.draw();
}

void Renderer::draw_glow(const Mesh& mesh, const vec3 color) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform1i(world_use_solid_loc, 4);   // 4 = unlit emissive glow
    glUniform3fv(world_solid_loc, 1, color);
    mesh.draw();
}

void Renderer::draw_model(const Model& model, const std::vector<Mat4>& part_world,
                          mat4 placement, vec3 color, float alpha) {
    glUseProgram(model_program);
    glUniformMatrix4fv(model_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform1f(model_alpha_loc, alpha);
    const bool ghost = alpha < 0.999f;   // dead players: alpha-blend over the scene
    if (ghost) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
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
    if (ghost) glDisable(GL_BLEND);   // restore the opaque default
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

void Renderer::draw_hud(const std::vector<float>& verts) {
    if (verts.empty()) return;
    glUseProgram(particle_program);
    mat4 id; glm_mat4_identity(id);   // NDC straight through (a_pos already in clip space)
    glUniformMatrix4fv(particle_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(id));

    glBindVertexArray(particle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, particle_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);

    // HUD draws on top with normal alpha blending and no depth.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 7));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

void Renderer::draw_text(const char* text, float x, float y, float px_height,
                         const vec3 color, float alpha, int fb_w, int fb_h) {
    if (!text_program || !text_atlas || !text || fb_w <= 0 || fb_h <= 0) return;
    const float scale = (text_bake_px > 0.0f) ? px_height / text_bake_px : 1.0f;
    // Anchor (top-left) in device pixels (NDC y is up; pixel y is down from the top).
    const float ax = (x * 0.5f + 0.5f) * fb_w;
    const float top = (1.0f - (y * 0.5f + 0.5f)) * fb_h;
    const float baseline = top + text_ascent * scale;

    std::vector<float> verts;
    verts.reserve(std::strlen(text) * 48);
    float pen_x = 0.0f, pen_y = 0.0f;   // baked-pixel pen; baseline kept at y=0
    const float r = color[0], g = color[1], b = color[2];
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < FONT_FIRST || c >= FONT_FIRST + text_chars.size()) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(text_chars.data(), FONT_ATLAS_W, FONT_ATLAS_H,
                           c - FONT_FIRST, &pen_x, &pen_y, &q, 1);
        // Quad corners: baked-pixel offsets from the pen, scaled and placed at the anchor,
        // then converted to NDC (flip y).
        const float px0 = ax + q.x0 * scale, px1 = ax + q.x1 * scale;
        const float py0 = baseline + q.y0 * scale, py1 = baseline + q.y1 * scale;
        const float nx0 = px0 / fb_w * 2.0f - 1.0f, nx1 = px1 / fb_w * 2.0f - 1.0f;
        const float ny0 = 1.0f - py0 / fb_h * 2.0f, ny1 = 1.0f - py1 / fb_h * 2.0f;
        auto V = [&](float vx, float vy, float s, float t) {
            verts.insert(verts.end(), { vx, vy, s, t, r, g, b, alpha });
        };
        V(nx0, ny0, q.s0, q.t0); V(nx1, ny0, q.s1, q.t0); V(nx1, ny1, q.s1, q.t1);
        V(nx0, ny0, q.s0, q.t0); V(nx1, ny1, q.s1, q.t1); V(nx0, ny1, q.s0, q.t1);
    }
    if (verts.empty()) return;

    glUseProgram(text_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, text_atlas);
    glUniform1i(text_atlas_loc, 0);

    glBindVertexArray(text_vao);
    glBindBuffer(GL_ARRAY_BUFFER, text_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STREAM_DRAW);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

float Renderer::text_width(const char* text, float px_height, int fb_w) const {
    if (!text_program || !text || fb_w <= 0) return 0.0f;
    const float scale = (text_bake_px > 0.0f) ? px_height / text_bake_px : 1.0f;
    float w = 0.0f;
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < FONT_FIRST || c >= FONT_FIRST + text_chars.size()) continue;
        w += text_chars[c - FONT_FIRST].xadvance * scale;
    }
    return w / fb_w * 2.0f;   // device px -> NDC-x
}

void Renderer::shutdown() {
    if (texture) glDeleteTextures(1, &texture);
    if (text_atlas) glDeleteTextures(1, &text_atlas);
    if (particle_vbo) glDeleteBuffers(1, &particle_vbo);
    if (text_vbo) glDeleteBuffers(1, &text_vbo);
    if (particle_vao) glDeleteVertexArrays(1, &particle_vao);
    if (text_vao) glDeleteVertexArrays(1, &text_vao);
    if (world_program) glDeleteProgram(world_program);
    if (model_program) glDeleteProgram(model_program);
    if (particle_program) glDeleteProgram(particle_program);
    if (text_program) glDeleteProgram(text_program);
    world_program = model_program = particle_program = texture = 0;
    text_program = text_atlas = text_vao = text_vbo = 0;
    particle_vao = particle_vbo = 0;
    world_viewproj_loc = model_viewproj_loc = model_model_loc = model_color_loc = -1;
}

} // namespace dc::renderer
