#include "engine/platform/window.h"
#include "engine/input/input.h"

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
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    sdl_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL);
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    SDL_GL_MakeCurrent(sdl_window, static_cast<SDL_GLContext>(gl_context));

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        shutdown();
        return false;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    SDL_SetWindowRelativeMouseMode(sdl_window, true);

    int w, h; framebuffer_size(w, h);
    glViewport(0, 0, w, h);
    return true;
}

bool Window::pump_events(dc::input::Input& input) {
    input.begin_frame();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        input.on_event(e);
    }
    return !input.quit;
}

void Window::framebuffer_size(int& w, int& h) const {
    SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
}

void Window::set_title(const char* title) {
    if (sdl_window) SDL_SetWindowTitle(sdl_window, title);
}

void Window::window_size(int& w, int& h) const {
    SDL_GetWindowSize(sdl_window, &w, &h);
}

void Window::set_relative_mouse(bool on) {
    SDL_SetWindowRelativeMouseMode(sdl_window, on);
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
