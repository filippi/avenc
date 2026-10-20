#pragma once
#include <cstdio>
#include <cstring>
#ifndef AVENC_VERSION
#define AVENC_VERSION "unknown"
#endif
inline bool avenc_print_version(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("AVENC %s\n", AVENC_VERSION);
        return true;
    }
    return false;
}
