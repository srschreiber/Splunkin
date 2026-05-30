#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);
    std::printf("dungeoncrawl: boot (smoke=%s)\n", smoke ? "true" : "false");
    return 0;
}
