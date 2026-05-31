#include "engine/renderer/model.h"
#include <glad/gl.h>
#include <cstring>

namespace dc::renderer {

void Model::upload(const ModelData& data) {
    destroy();
    parts.reserve(data.parts.size());
    for (const auto& pd : data.parts) {
        Part part;
        part.index_count = static_cast<int>(pd.indices.size());
        std::memcpy(part.node_world, pd.node_world, sizeof(float) * 16);

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

        const GLsizei stride = 8 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
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
