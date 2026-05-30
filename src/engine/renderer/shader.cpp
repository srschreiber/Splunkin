#include "engine/renderer/shader.h"

#include <glad/gl.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

namespace dc::renderer {

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "shader: cannot open %s\n", path); return {}; }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static uint32_t compile(GLenum type, const std::string& src) {
    uint32_t s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

uint32_t load_program(const char* vert_path, const char* frag_path) {
    std::string vsrc = read_file(vert_path);
    std::string fsrc = read_file(frag_path);
    if (vsrc.empty() || fsrc.empty()) return 0;
    uint32_t vs = compile(GL_VERTEX_SHADER, vsrc);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) return 0;
    uint32_t prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    int ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "program link error: %s\n", log);
        glDeleteProgram(prog); prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

} // namespace dc::renderer
