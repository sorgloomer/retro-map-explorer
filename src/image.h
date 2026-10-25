// BMP and TGA decoders producing RGBA8.
#pragma once
#include <cstdint>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4, top row first
    bool hasAlpha = false;      // any pixel with alpha < 255
    uint8_t blendMode = 0;      // from the game data: 0 = normal, 1 = additive, 2 = alpha blended
    bool partialAlpha = false;  // many pixels with in-between alpha: needs blending, not alpha testing
    bool valid() const { return w > 0 && h > 0; }
};

bool decodeBMP(const uint8_t* d, size_t n, Image& out);
bool decodeTGA(const uint8_t* d, size_t n, Image& out);
