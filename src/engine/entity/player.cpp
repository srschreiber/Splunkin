#include "engine/entity/player.h"
#include "engine/world/collision.h"
#include <cmath>

namespace dc::entity {

namespace { constexpr float MOUSE_SENS = 0.0025f; }

void Player::front(vec3 out) const {
    out[0] = std::cos(pitch) * std::cos(yaw);
    out[1] = std::sin(pitch);
    out[2] = std::cos(pitch) * std::sin(yaw);
    glm_vec3_normalize(out);
}

void Player::add_look(float dx, float dy) {
    yaw   += dx * MOUSE_SENS;
    pitch -= dy * MOUSE_SENS;
    const float limit = glm_rad(89.0f);
    if (pitch >  limit) pitch =  limit;
    if (pitch < -limit) pitch = -limit;
}

void Player::update(float forward, float strafe, bool jump, float dt,
                    const dc::world::Map& map, const dc::world::Terrain& terrain) {
    // --- Horizontal intent ---
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 walk = { std::cos(yaw), 0.0f, std::sin(yaw) };
    glm_vec3_normalize(walk);
    vec3 right;
    glm_vec3_cross(walk, up, right);
    glm_vec3_normalize(right);

    vec3 delta = {0.0f, 0.0f, 0.0f}, tmp;
    glm_vec3_scale(walk,  forward, tmp); glm_vec3_add(delta, tmp, delta);
    glm_vec3_scale(right, strafe,  tmp); glm_vec3_add(delta, tmp, delta);
    if (glm_vec3_norm(delta) > 1e-6f) {
        glm_vec3_normalize(delta);
        glm_vec3_scale(delta, speed * dt, delta);
    } else {
        delta[0] = delta[1] = delta[2] = 0.0f;
    }

    // --- Knockback + dodge-dash impulses (each decays at its own rate), added to the
    // move; per-axis wall slide kills whichever axis hits a wall ---
    delta[0] += (knock_vel[0] + dash_vel[0]) * dt;
    delta[2] += (knock_vel[2] + dash_vel[2]) * dt;

    // --- Per-axis slide against the map + a terrain step-up gate. You can't walk UP
    // terrain that rises more than MAX_STEP_UP within STEP_LOOKAHEAD ahead of you — steep
    // cliffs act as walls. The gate compares against our FEET, so when a jump lifts us high
    // enough to clear a low ledge, the rise shrinks and we're allowed to step onto it. ---
    const float feet = position[1] - dc::world::EYE_HEIGHT;
    auto step_ok = [&](float nx, float nz, float dirx, float dirz) {
        const float g = terrain.height(nx + dirx * STEP_LOOKAHEAD, nz + dirz * STEP_LOOKAHEAD);
        return g - feet <= MAX_STEP_UP;
    };
    const float sx = delta[0] > 0.0f ? 1.0f : (delta[0] < 0.0f ? -1.0f : 0.0f);
    const float sz = delta[2] > 0.0f ? 1.0f : (delta[2] < 0.0f ? -1.0f : 0.0f);
    if (!dc::world::circle_hits_solid(map, position[0] + delta[0], position[2], PLAYER_RADIUS)
        && step_ok(position[0] + delta[0], position[2], sx, 0.0f))
        position[0] += delta[0];
    else { knock_vel[0] = 0.0f; dash_vel[0] = 0.0f; }   // hit a wall / too-steep rise: kill that axis
    if (!dc::world::circle_hits_solid(map, position[0], position[2] + delta[2], PLAYER_RADIUS)
        && step_ok(position[0], position[2] + delta[2], 0.0f, sz))
        position[2] += delta[2];
    else { knock_vel[2] = 0.0f; dash_vel[2] = 0.0f; }

    const float kdamp = std::exp(-KNOCK_DAMP * dt);
    knock_vel[0] *= kdamp;
    knock_vel[2] *= kdamp;
    const float ddamp = std::exp(-dash_decay * dt);
    dash_vel[0] *= ddamp;
    dash_vel[2] *= ddamp;
    if (iframes > 0.0f) iframes -= dt;
    if (dash_cd > 0.0f) dash_cd -= dt;

    // --- Vertical: gravity + jump, landing on the terrain under us (open-top) ---
    if (jump && on_ground) {
        vel_y = JUMP_SPEED;
        on_ground = false;
    }
    vel_y -= GRAVITY * dt;
    position[1] += vel_y * dt;

    // Eye floor = ground height at our xz + eye offset. Land/walk on it.
    const float eye_floor = terrain.height(position[0], position[2]) + dc::world::EYE_HEIGHT;
    if (position[1] <= eye_floor) {
        position[1] = eye_floor;
        vel_y = 0.0f;
        on_ground = true;
    }
}

} // namespace dc::entity
