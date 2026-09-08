#pragma once

#include <cstddef>
#include <vector>

namespace engine {

struct EnvMapImage {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;
};

bool envMapLoadExr(const void* data, size_t dataSize, EnvMapImage& out);
bool envMapLoadHdr(const void* data, size_t dataSize, EnvMapImage& out);
bool envMapLoadFromMemory(const void* data, size_t dataSize, const char* path, EnvMapImage& out);
}
