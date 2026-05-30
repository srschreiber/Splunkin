#include "engine/platform/window.h"
#include "engine/renderer/renderer.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        // process_input
        running = window.pump_events();

        // update(dt)
        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        (void)dt; // subsystems are stubs for now

        // render
        renderer.render();
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    renderer.shutdown();
    window.shutdown();
    return 0;
}
