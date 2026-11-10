// C library calls that MSVC names differently: on Windows the same calls as before.
#pragma once

#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <strings.h>
#endif

namespace plat {

// fopen; null if the file cannot be opened.
inline std::FILE* open_file(const char* path, const char* mode) {
#ifdef _WIN32
    std::FILE* f = nullptr;
    return fopen_s(&f, path, mode) == 0 ? f : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

// 0 if both are the same text apart from upper and lower case (ASCII).
inline int compare_ignoring_case(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

}  // namespace plat
