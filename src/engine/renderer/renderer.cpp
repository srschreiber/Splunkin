#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/tri.vert", "assets/shaders/tri.frag");
    if (!program) return false;

    // pos.xy, color.rgb
    const float verts[] = {
        -0.6f, -0.5f,  1.0f, 0.0f, 0.0f,
         0.6f, -0.5f,  0.0f, 1.0f, 0.0f,
         0.0f,  0.6f,  0.0f, 0.0f, 1.0f,
    };
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    return true;
}

void Renderer::render() {
    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    program = vao = vbo = 0;
}

} // namespace dc::renderer
