#include "engine/renderer/model.h"
#include "cgltf.h"
#include <cstring>

namespace dc::renderer {

namespace {

void read_node_trs(const cgltf_node* node, Node& out) {
    // Defaults (identity) are already set in Node; apply explicit TRS, or
    // decompose a baked matrix as a fallback.
    if (node->has_matrix) {
        mat4 m;
        std::memcpy(m, node->matrix, sizeof(float) * 16);
        vec4 t; mat4 rot; vec3 s;
        glm_decompose(m, t, rot, s);
        out.t[0] = t[0]; out.t[1] = t[1]; out.t[2] = t[2];
        out.s[0] = s[0]; out.s[1] = s[1]; out.s[2] = s[2];
        glm_mat4_quat(rot, out.r);
        return;
    }
    if (node->has_translation) {
        out.t[0] = node->translation[0]; out.t[1] = node->translation[1]; out.t[2] = node->translation[2];
    }
    if (node->has_rotation) {
        out.r[0] = node->rotation[0]; out.r[1] = node->rotation[1];
        out.r[2] = node->rotation[2]; out.r[3] = node->rotation[3];
    }
    if (node->has_scale) {
        out.s[0] = node->scale[0]; out.s[1] = node->scale[1]; out.s[2] = node->scale[2];
    }
}

AnimPath to_path(cgltf_animation_path_type p) {
    switch (p) {
        case cgltf_animation_path_type_rotation: return AnimPath::Rotation;
        case cgltf_animation_path_type_scale:    return AnimPath::Scale;
        default:                                 return AnimPath::Translation;
    }
}

// Reads one glTF animation into our Animation (node TRS keyframes).
Animation read_animation(const cgltf_data* data, const cgltf_animation* anim) {
    Animation out;
    out.name = anim->name ? anim->name : "";
    float duration = 0.0f;
    for (cgltf_size c = 0; c < anim->channels_count; ++c) {
        const cgltf_animation_channel* ch = &anim->channels[c];
        if (!ch->target_node || !ch->sampler) continue;

        AnimChannel oc;
        oc.node = static_cast<int>(ch->target_node - data->nodes);
        oc.path = to_path(ch->target_path);
        // Cubic-spline output stores [inTangent, value, outTangent] per key; we
        // keep only the value and sample it linearly (tangents dropped).
        const bool cubic = (ch->sampler->interpolation == cgltf_interpolation_type_cubic_spline);
        oc.interp = (ch->sampler->interpolation == cgltf_interpolation_type_step)
                  ? AnimInterp::Step : AnimInterp::Linear;

        const cgltf_accessor* in = ch->sampler->input;
        const cgltf_accessor* ov = ch->sampler->output;
        const int comps = (oc.path == AnimPath::Rotation) ? 4 : 3;

        const cgltf_size nkeys = in->count;   // one input time per keyframe
        oc.times.resize(nkeys);
        for (cgltf_size k = 0; k < nkeys; ++k) {
            cgltf_accessor_read_float(in, k, &oc.times[k], 1);
            if (oc.times[k] > duration) duration = oc.times[k];
        }
        oc.values.resize(nkeys * comps);
        for (cgltf_size k = 0; k < nkeys; ++k) {
            const cgltf_size elem = cubic ? (3 * k + 1) : k;   // middle = value
            cgltf_accessor_read_float(ov, elem, &oc.values[k * comps], comps);
        }
        out.channels.push_back(std::move(oc));
    }
    out.duration = duration;
    return out;
}

// The bone node named `name` (a bone has no mesh of its own), or -1.
int find_bone(const cgltf_data* data, const char* name) {
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node* node = &data->nodes[i];
        if (node->name && std::strcmp(node->name, name) == 0 && !node->mesh)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

bool read_model(const char* path, ModelData& out) {
    out.nodes.clear();
    out.parts.clear();
    out.walk = Animation{};
    out.punch = Animation{};
    out.block = Animation{};
    out.open = Animation{};
    out.close = Animation{};

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    const cgltf_size node_count = data->nodes_count;
    out.nodes.resize(node_count);

    for (cgltf_size i = 0; i < node_count; ++i) {
        const cgltf_node* node = &data->nodes[i];
        Node& dst = out.nodes[i];
        read_node_trs(node, dst);
        dst.parent = node->parent ? static_cast<int>(node->parent - data->nodes) : -1;

        // A mesh may have several primitives — one per material (e.g. a flame
        // built from "fire red", "fire orange", ...). Each becomes its own part,
        // all sharing this node's transform.
        if (node->mesh) {
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
                PartCPU part;
                part.node = static_cast<int>(i);
                if (prim->material && prim->material->has_pbr_metallic_roughness) {
                    const cgltf_float* bcf = prim->material->pbr_metallic_roughness.base_color_factor;
                    part.color[0] = bcf[0]; part.color[1] = bcf[1]; part.color[2] = bcf[2];
                }
                if (prim->material) {
                    // Emissive: emissiveFactor scaled by the KHR_materials_emissive_strength
                    // extension if present. Makes self-lit parts (a torch flame) glow.
                    const cgltf_float* ef = prim->material->emissive_factor;
                    float strength = prim->material->has_emissive_strength
                                   ? prim->material->emissive_strength.emissive_strength : 1.0f;
                    part.emissive[0] = ef[0] * strength;
                    part.emissive[1] = ef[1] * strength;
                    part.emissive[2] = ef[2] * strength;
                }
                // Interleave the glTF's separate POSITION/NORMAL/TEXCOORD_0 accessors
                // into one flat array: 8 floats per vertex = pos(3), normal(3), uv(2).
                // THIS ORDER defines the vertex byte layout — it must match the VAO
                // attribute offsets in model_gl.cpp and the model.vert input locations.
                const cgltf_size vcount = pos->count;
                part.vertices.reserve(vcount * 8);
                for (cgltf_size v = 0; v < vcount; ++v) {
                    float pf[3] = {0, 0, 0}, nf[3] = {0, 1, 0}, tf[2] = {0, 0};  // defaults if missing
                    cgltf_accessor_read_float(pos, v, pf, 3);                     // position
                    if (nrm) cgltf_accessor_read_float(nrm, v, nf, 3);           // normal
                    if (uv)  cgltf_accessor_read_float(uv, v, tf, 2);            // uv
                    part.vertices.insert(part.vertices.end(),
                        { pf[0],pf[1],pf[2],  nf[0],nf[1],nf[2],  tf[0],tf[1] }); // pos | normal | uv
                }
                if (prim->indices) {
                    const cgltf_size icount = prim->indices->count;
                    part.indices.reserve(icount);
                    for (cgltf_size k = 0; k < icount; ++k)
                        part.indices.push_back(
                            static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, k)));
                } else {
                    for (cgltf_size k = 0; k < vcount; ++k)
                        part.indices.push_back(static_cast<uint32_t>(k));
                }
                out.parts.push_back(std::move(part));
            }
        }
    }

    // Named bones we drive specially.
    out.head_node  = find_bone(data, "head");   // head-look
    out.body_node  = find_bone(data, "body");   // torso: pitch it so the arms/weapon aim up/down
    out.arm_l_node = find_bone(data, "armL");    // punch layer is masked to this bone
    out.arm_r_node = find_bone(data, "armR");    // block layer is masked to this bone
    out.hand_l_node = find_bone(data, "handL");  // a socket for held items (not used in this example)
    out.hand_r_node = find_bone(data, "handR");  // a socket for held items (not used in this example)

    // Read every animation; route the ones we know by name.
    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        const cgltf_animation* anim = &data->animations[i];
        Animation clip = read_animation(data, anim);
        if (clip.name == "walk")       out.walk = std::move(clip);
        else if (clip.name == "punch") out.punch = std::move(clip);
        else if (clip.name == "block") out.block = std::move(clip);  // example of a block layer that masks no bones
        else if (clip.name == "open")  out.open = std::move(clip);
        else if (clip.name == "close") out.close = std::move(clip);
    }

    cgltf_free(data);
    return !out.parts.empty();
}

} // namespace dc::renderer
