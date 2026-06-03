#include "engine/renderer/animator.h"
#include <cmath>
#include <vector>

namespace dc::renderer {

namespace {

void sample_vec3(const AnimChannel& ch, float t, vec3 out) {
    const int n = static_cast<int>(ch.times.size());
    const float* v = ch.values.data();
    if (n == 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    if (t <= ch.times[0]) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return; }
    if (t >= ch.times[n - 1]) {
        const float* a = &v[(n - 1) * 3];
        out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; return;
    }
    int k = 0;
    while (k + 1 < n && ch.times[k + 1] < t) ++k;
    const float* a = &v[k * 3];
    const float* b = &v[(k + 1) * 3];
    if (ch.interp == AnimInterp::Step) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; return; }
    const float dt = ch.times[k + 1] - ch.times[k];
    const float f = dt > 0.0f ? (t - ch.times[k]) / dt : 0.0f;
    out[0] = a[0] + (b[0] - a[0]) * f;
    out[1] = a[1] + (b[1] - a[1]) * f;
    out[2] = a[2] + (b[2] - a[2]) * f;
}

void sample_quat(const AnimChannel& ch, float t, versor out) {
    const int n = static_cast<int>(ch.times.size());
    const float* v = ch.values.data();
    if (n == 0) { glm_quat_identity(out); return; }
    if (t <= ch.times[0]) { glm_quat_init(out, v[0], v[1], v[2], v[3]); return; }
    if (t >= ch.times[n - 1]) {
        const float* a = &v[(n - 1) * 4];
        glm_quat_init(out, a[0], a[1], a[2], a[3]); return;
    }
    int k = 0;
    while (k + 1 < n && ch.times[k + 1] < t) ++k;
    const float* a = &v[k * 4];
    const float* b = &v[(k + 1) * 4];
    versor qa, qb;
    glm_quat_init(qa, a[0], a[1], a[2], a[3]);
    glm_quat_init(qb, b[0], b[1], b[2], b[3]);
    if (ch.interp == AnimInterp::Step) { glm_quat_copy(qa, out); return; }
    const float dt = ch.times[k + 1] - ch.times[k];
    const float f = dt > 0.0f ? (t - ch.times[k]) / dt : 0.0f;
    glm_quat_slerp(qa, qb, f, out);
}

// True if `node` is `root` or any descendant of it (walk up the parent chain).
// Lets a layer mask to a whole limb by naming its root bone (armR -> the whole
// right arm: upperArmR, lowerArmR, handR, ...).
bool node_in_subtree(const ModelData& model, int node, int root) {
    while (node != -1) {
        if (node == root) return true;
        node = model.nodes[node].parent;
    }
    return false;
}

void local_matrix(const vec3 t, const versor r, const vec3 s, mat4 out) {
    mat4 T, R, S, tmp;
    glm_translate_make(T, const_cast<float*>(t));
    versor rc; glm_quat_copy(const_cast<float*>(r), rc);
    glm_quat_mat4(rc, R);
    glm_scale_make(S, const_cast<float*>(s));
    glm_mat4_mul(T, R, tmp);
    glm_mat4_mul(tmp, S, out);
}

struct V3 { vec3 v; };
struct Q  { versor q; };

} // namespace

void pose_model(const ModelData& model, const std::vector<AnimLayer>& layers,
                float head_pitch, std::vector<Mat4>& out_part_world,
                std::vector<int> attach_nodes, std::vector<Mat4*> out_attach) {
    const int n = static_cast<int>(model.nodes.size());

    // Working TRS per node = base (rest), then each layer overrides.
    std::vector<V3> nt(n), ns(n);
    std::vector<Q>  nr(n);
    for (int i = 0; i < n; ++i) {
        glm_vec3_copy(const_cast<float*>(model.nodes[i].t), nt[i].v);
        glm_vec3_copy(const_cast<float*>(model.nodes[i].s), ns[i].v);
        glm_quat_copy(const_cast<float*>(model.nodes[i].r), nr[i].q);
    }

    // Apply each layer in order; later layers win per bone. A layer with
    // only_node >= 0 writes just that one bone, so a masked punch leaves the
    // rest of the body on the layers below it (e.g. the walk).
    for (const auto& layer : layers) {
        if (!layer.clip || !layer.clip->valid()) continue;
        const float dur = layer.clip->duration;
        float ct;
        if (layer.loop) {
            ct = (dur > 0.0f) ? std::fmod(layer.time, dur) : 0.0f;  // wrap (single-frame -> hold)
            if (ct < 0.0f) ct += dur;
        } else {
            ct = layer.time;   // one-shot: the sampler clamps to the first/last keyframe
        }
        for (const auto& ch : layer.clip->channels) {
            if (ch.node < 0 || ch.node >= n) continue;
            // Masked layer: apply only to the named bone and its descendants
            // (e.g. armR -> the whole right arm, so the elbow/wrist bend too).
            if (layer.only_node >= 0 && !node_in_subtree(model, ch.node, layer.only_node)) continue;
            if (ch.path == AnimPath::Rotation)   sample_quat(ch, ct, nr[ch.node].q);
            else if (ch.path == AnimPath::Scale) sample_vec3(ch, ct, ns[ch.node].v);
            else                                 sample_vec3(ch, ct, nt[ch.node].v);
        }
    }

    // head rotation
    if (model.head_node >= 0 && model.head_node < n) {
        float hp = head_pitch;
        const float limit = glm_rad(65.0f);
        if (hp < -limit) hp = -limit;
        if (hp > limit) hp = limit;

        vec3 axis = {1.0f, 0.0f, 0.0f};
        versor pitch_q, result;
        glm_quatv(pitch_q, hp, axis);
        glm_quat_mul(pitch_q, nr[model.head_node].q, result);
        glm_quat_copy(result, nr[model.head_node].q);
    }

    std::vector<Mat4> local(n), world(n);
    std::vector<char> done(n, 0);
    for (int i = 0; i < n; ++i) local_matrix(nt[i].v, nr[i].q, ns[i].v, local[i].m);

    // Resolve each node's world matrix (parent chain, memoized).
    constexpr int MAX_DEPTH = 64;   // node hierarchy depth bound
    for (int i = 0; i < n; ++i) {
        int chain[MAX_DEPTH];
        int sp = 0;
        int cur = i;
        while (cur != -1 && !done[cur] && sp < MAX_DEPTH) { chain[sp++] = cur; cur = model.nodes[cur].parent; }
        while (sp > 0) {
            const int node = chain[--sp];
            const int p = model.nodes[node].parent;
            if (p == -1) glm_mat4_copy(local[node].m, world[node].m);
            else         glm_mat4_mul(world[p].m, local[node].m, world[node].m);
            done[node] = 1;
        }
    }

    // Optional attachment socket: a chosen bone's world matrix (e.g. the head),
    // so a separate model (helmet) can be hung off it.
    if (out_attach.size() > 0) {
        for (size_t i = 0; i < attach_nodes.size(); ++i) {
            int attach_node = attach_nodes[i];
            if (attach_node >= 0 && attach_node < n)
                glm_mat4_copy(world[attach_node].m, out_attach[i]->m);
        }
    }

    // Each part is drawn at the world transform of the node it belongs to.
    out_part_world.assign(model.parts.size(), Mat4{});
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        const int nd = model.parts[i].node;
        if (nd >= 0 && nd < n) glm_mat4_copy(world[nd].m, out_part_world[i].m);
        else                   glm_mat4_identity(out_part_world[i].m);
    }
}

std::vector<Mat4> mesh_offsets(const ModelData& model) {
    std::vector<Mat4> out(model.parts.size());
    // Each part's node-local TRS is its placement relative to its parent bone —
    // the constant offset we want for hanging this part off that bone.
    const int nn = static_cast<int>(model.nodes.size());
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        const int nd = model.parts[i].node;
        if (nd >= 0 && nd < nn)
            local_matrix(model.nodes[nd].t, model.nodes[nd].r, model.nodes[nd].s, out[i].m);
        else
            glm_mat4_identity(out[i].m);
    }
    return out;
}

} // namespace dc::renderer
