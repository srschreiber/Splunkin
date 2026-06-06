#include "engine/input/input.h"
#include <SDL3/SDL.h>

namespace dc::input {

void Input::begin_frame() {
    mouse_dx = 0.0f;
    mouse_dy = 0.0f;
}

void Input::on_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) {
        quit = true;
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        quit = true;
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_dx += e.motion.xrel;
        mouse_dy += e.motion.yrel;
    }
}

bool Input::key_down(int scancode) const {
    const bool* state = SDL_GetKeyboardState(nullptr);
    return state[scancode];
}

bool Input::mouse_down(int button) const {
    return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
}

void Input::mouse_pos(float& x, float& y) const {
    SDL_GetMouseState(&x, &y);   // window coords (points); only meaningful when not in relative mode
}

} // namespace dc::input
