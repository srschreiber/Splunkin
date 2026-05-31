#include "engine/renderer/model.h"
#include "cgltf.h"
#include <cstring>

namespace dc::renderer {

bool read_model(const char* path, ModelData& out) {
    out.parts.clear();

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;

        float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv  = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                const cgltf_attribute* at = &prim->attributes[a];
                if (at->type == cgltf_attribute_type_position) pos = at->data;
                else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
                else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv = at->data;
            }
            if (!pos) continue;

            PartData part;
            std::memcpy(part.node_world, world, sizeof(float) * 16);

            const cgltf_size vcount = pos->count;
            part.vertices.reserve(vcount * 8);
            for (cgltf_size v = 0; v < vcount; ++v) {
                float pf[3] = {0, 0, 0}, nf[3] = {0, 1, 0}, tf[2] = {0, 0};
                cgltf_accessor_read_float(pos, v, pf, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, nf, 3);
                if (uv)  cgltf_accessor_read_float(uv, v, tf, 2);
                part.vertices.insert(part.vertices.end(),
                    { pf[0],pf[1],pf[2], nf[0],nf[1],nf[2], tf[0],tf[1] });
            }

            if (prim->indices) {
                const cgltf_size icount = prim->indices->count;
                part.indices.reserve(icount);
                for (cgltf_size i = 0; i < icount; ++i)
                    part.indices.push_back(
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i)));
            } else {
                for (cgltf_size i = 0; i < vcount; ++i)
                    part.indices.push_back(static_cast<uint32_t>(i));
            }

            out.parts.push_back(std::move(part));
        }
    }

    cgltf_free(data);
    return !out.parts.empty();
}

} // namespace dc::renderer
