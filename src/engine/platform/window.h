#pragma once
#include <cstdint>

struct SDL_Window;

namespace dc::input { struct Input; }

namespace dc::platform {

struct Window {
    SDL_Window* sdl_window = nullptr;
    void*       gl_context = nullptr; // SDL_GLContext
    int         width = 1280;
    int         height = 720;

    // Initializes SDL video, creates the window and a GL 3.3 core context with
    // a depth buffer, loads GL via GLAD, and enables relative mouse mode.
    bool init(const char* title);

    // Polls events into `input`; returns false when quit was requested.
    bool pump_events(dc::input::Input& input);

    // Current framebuffer size in pixels (Retina-correct).
    void framebuffer_size(int& w, int& h) const;

    void swap();
    void shutdown();
};

} // namespace dc::platform
