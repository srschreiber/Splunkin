#pragma once
#include <cstdint>

struct SDL_Window;

namespace dc::platform {

struct Window {
    SDL_Window* sdl_window = nullptr;
    void*       gl_context = nullptr; // SDL_GLContext
    int         width = 1280;
    int         height = 720;

    // Initializes SDL video, creates the window and a GL 3.3 core context,
    // and loads GL function pointers via GLAD. Returns false on failure.
    bool init(const char* title);

    // Returns false when the user requested quit (e.g. window close).
    bool pump_events();

    void swap();
    void shutdown();
};

} // namespace dc::platform
