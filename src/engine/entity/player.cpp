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
                    const dc::world::Map& map) {
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
        glm_vec3_scale(delta, MOVE_SPEED * dt, delta);
    } else {
        delta[0] = delta[1] = delta[2] = 0.0f;
    }

    // --- Per-axis slide against the map ---
    if (!dc::world::circle_hits_solid(map, position[0] + delta[0], position[2], PLAYER_RADIUS))
        position[0] += delta[0];
    if (!dc::world::circle_hits_solid(map, position[0], position[2] + delta[2], PLAYER_RADIUS))
        position[2] += delta[2];

    // --- Vertical: gravity + jump ---
    if (jump && on_ground) {
        vel_y = JUMP_SPEED;
        on_ground = false;
    }
    vel_y -= GRAVITY * dt;
    position[1] += vel_y * dt;

    const float ceil_limit = dc::world::WALL_HEIGHT - 0.2f;
    if (position[1] > ceil_limit) {
        position[1] = ceil_limit;
        vel_y = 0.0f;   // head bonk: stop vertical motion
    }
    if (position[1] <= dc::world::EYE_HEIGHT) {
        position[1] = dc::world::EYE_HEIGHT;
        vel_y = 0.0f;
        on_ground = true;
    }
}

} // namespace dc::entity
