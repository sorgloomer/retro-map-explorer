// Bounds-checked little-endian reader.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "vmath.h"

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;

    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}

    bool has(size_t bytes) const { return ok && pos + bytes <= n; }
    size_t left() const { return pos <= n ? n - pos : 0; }
    template <class T>
    T get() {
        T v{};
        if (!has(sizeof(T))) { ok = false; return v; }
        memcpy(&v, p + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    uint8_t u8() { return get<uint8_t>(); }
    uint16_t u16() { return get<uint16_t>(); }
    int16_t i16() { return get<int16_t>(); }
    uint32_t u32() { return get<uint32_t>(); }
    int32_t i32() { return get<int32_t>(); }
    float f32() { return get<float>(); }
    Vec3 vec3() {
        Vec3 v;
        v.x = f32(); v.y = f32(); v.z = f32();
        return v;
    }
    void skip(size_t bytes) {
        if (!has(bytes)) { ok = false; pos = n; return; }
        pos += bytes;
    }
    std::string fixedString(size_t len) {
        if (!has(len)) { ok = false; return {}; }
        std::string s((const char*)p + pos, len);
        pos += len;
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        return s;
    }
};
