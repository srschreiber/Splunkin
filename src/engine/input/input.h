#pragma once

union SDL_Event;

namespace dc::input {

// Per-frame input snapshot. Fed by the platform layer each frame.
struct Input {
    float mouse_dx = 0.0f;   // accumulated relative motion this frame
    float mouse_dy = 0.0f;
    bool  quit = false;

    void begin_frame();                  // zero mouse deltas (call before polling)
    void on_event(const SDL_Event& e);   // accumulate motion; set quit on QUIT/Esc
    bool key_down(int scancode) const;   // SDL_Scancode; wraps SDL_GetKeyboardState
    bool mouse_down(int button) const;   // SDL_BUTTON_LEFT/RIGHT/...; wraps SDL_GetMouseState
    void mouse_pos(float& x, float& y) const;  // absolute window position (for menus / free cursor)
};

} // namespace dc::input
