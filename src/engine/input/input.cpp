#include "engine/input/input.h"
#include <SDL3/SDL.h>

namespace dc::input {

void Input::begin_frame() {
    mouse_dx = 0.0f;
    mouse_dy = 0.0f;
    text_input.clear();
    backspaces = 0;
}

void Input::on_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) {
        quit = true;   // window close button; ESC is handled by the game as a pause toggle
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_dx += e.motion.xrel;
        mouse_dy += e.motion.yrel;
    } else if (e.type == SDL_EVENT_TEXT_INPUT) {
        if (e.text.text) text_input += e.text.text;   // only arrives while text input is active
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_BACKSPACE) {
        ++backspaces;
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
