#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") != 0) {
        std::fprintf(stderr, "usage: memwhy [--version]\n");
        return 2;
    }
    std::printf("memwhy %s\n", MEMWHY_VERSION);
    return 0;
}
