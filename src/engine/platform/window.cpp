#include "engine/platform/window.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <cstdio>

namespace dc::platform {

bool Window::init(const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    sdl_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL);
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(sdl_window, static_cast<SDL_GLContext>(gl_context));

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        return false;
    }
    std::printf("GL %s\n", glGetString(GL_VERSION));
    glViewport(0, 0, width, height);
    return true;
}

bool Window::pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return false;
    }
    return true;
}

void Window::swap() { SDL_GL_SwapWindow(sdl_window); }

void Window::shutdown() {
    if (gl_context) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context));
    if (sdl_window) SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    gl_context = nullptr;
    sdl_window = nullptr;
}

} // namespace dc::platform
