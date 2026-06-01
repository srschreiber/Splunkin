#include "engine/renderer/model.h"
#include <glad/gl.h>

namespace dc::renderer {

// Uploads the parsed model to the GPU: for each CPU part (PartCPU), creates a
// VAO/VBO/EBO and copies its vertices + indices into GPU memory via glBufferData,
// producing a drawable PartMesh. Call once after the GL context exists (the data
// lives on the GPU afterward; draw_model just binds these buffers each frame).
void Model::upload(const ModelData& data) {
    destroy();
    parts.reserve(data.parts.size());
    for (const auto& pd : data.parts) {
        PartMesh part;
        part.index_count = static_cast<int>(pd.indices.size());
        part.color[0] = pd.color[0]; part.color[1] = pd.color[1]; part.color[2] = pd.color[2];

        glGenVertexArrays(1, &part.vao);
        glGenBuffers(1, &part.vbo);
        glGenBuffers(1, &part.ebo);

        glBindVertexArray(part.vao);

        glBindBuffer(GL_ARRAY_BUFFER, part.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pd.vertices.size() * sizeof(float)),
                     pd.vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pd.indices.size() * sizeof(uint32_t)),
                     pd.indices.data(), GL_STATIC_DRAW);

        // Describe the vertex layout for this VAO. Must match how read_model packs
        // PartCPU::vertices AND the model.vert `layout(location=...)` inputs:
        //   8 floats per vertex = pos(3) + normal(3) + uv(2).
        const GLsizei stride = 8 * sizeof(float);                         // bytes per vertex
        // location 0 = position: 3 floats at offset 0
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        // location 1 = normal: 3 floats at offset 3 floats
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // location 2 = uv: 2 floats at offset 6 floats
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
        parts.push_back(part);
    }
}

void Model::destroy() {
    for (auto& part : parts) {
        if (part.ebo) glDeleteBuffers(1, &part.ebo);
        if (part.vbo) glDeleteBuffers(1, &part.vbo);
        if (part.vao) glDeleteVertexArrays(1, &part.vao);
    }
    parts.clear();
}

} // namespace dc::renderer
