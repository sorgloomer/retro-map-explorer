// Small self-contained DEFLATE / zlib decoder (no external dependencies).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Decodes a raw DEFLATE stream. Returns false on malformed input.
bool inflateRaw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t sizeHint = 0);
// Decodes a zlib stream (2 byte header + deflate, adler ignored).
bool inflateZlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t sizeHint = 0);
