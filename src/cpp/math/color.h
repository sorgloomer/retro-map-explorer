// RGB colours: ColorRgb32 (packed 0xRRGGBB), Color3b (one byte per channel, as stored in files and vertices) and Color3f
// (floats 0..1, as the shaders take them). They convert only explicitly.
#pragma once
#include <cmath>

struct Color3b;
struct Color4b;
struct Color3f;
struct Color4f;

template <class _Tp>
inline constexpr const _Tp & clamp(
    const _Tp & x,
    const _Tp & min,
    const _Tp & max
    ) {
    return std::min(std::max(x, min), max);
}

Color3b rgb(uint32_t rgb);

uint8_t color_f_to_b(float f);
float color_b_to_f(uint8_t b);

struct Color3b {
    uint8_t r = 0, g = 0, b = 0;
    Color3b() {}
    Color3b(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}

    Color3f to_f() const;

    template <class C>
    static Color3b from3b(const C& c) {
        return {
            static_cast<uint8_t>(c.getRb()),
            static_cast<uint8_t>(c.getGb()),
            static_cast<uint8_t>(c.getBb()),
        };
    }

    static Color3b from_rgb(const uint32_t rgb) {
        return {
            static_cast<uint8_t>((rgb >> 16) & 0xff),
            static_cast<uint8_t>((rgb >> 8) & 0xff),
            static_cast<uint8_t>((rgb >> 0) & 0xff),
        };
    }

    uint32_t to_rgb32() const {
        return (static_cast<uint32_t>(r) << 16)
             | (static_cast<uint32_t>(g) << 8)
             | (static_cast<uint32_t>(b) << 0);
    }

    int getRb() const { return r; }
    int getGb() const { return g; }
    int getBb() const { return b; }
};

struct Color4b {
    uint8_t r = 0, g = 0, b = 0, a = 0;

    Color4f to_f() const;

    template <class C>
    static Color4b from(const C& c) {
        return {
            static_cast<uint8_t>(c.getRb()),
            static_cast<uint8_t>(c.getGb()),
            static_cast<uint8_t>(c.getBb()),
            static_cast<uint8_t>(c.getAb()),
        };
    }

    template <class C>
    static Color4b from1b3f(uint8_t a, const C& c) {
        return {
            .r = color_f_to_b(c.getRf()),
            .g = color_f_to_b(c.getGf()),
            .b = color_f_to_b(c.getBf()),
            .a = a,
        };
    }

    static Color4b fromArgb(uint32_t argb) {
        return {
            .r = static_cast<uint8_t>((argb >> 16) & 0xff),
            .g = static_cast<uint8_t>((argb >> 8) & 0xff),
            .b = static_cast<uint8_t>((argb >> 0) & 0xff),
            .a = static_cast<uint8_t>((argb >> 24) & 0xff),
        };
    }

    int getRb() const { return r; }
    int getGb() const { return g; }
    int getBb() const { return b; }
    int getAb() const { return a; }
};

struct Color3f {
    float r = 0, g = 0, b = 0;

    float getRf() const { return r; }
    float getGf() const { return g; }
    float getBf() const { return b; }

    Color3f clamp() const {
        return {
            .r = ::clamp(r, 0.0f, 1.0f),
            .g = ::clamp(g, 0.0f, 1.0f),
            .b = ::clamp(b, 0.0f, 1.0f),
        };
    }

    template <class C>
    static Color3f from3b(const C& c) {
        return {
            .r = color_b_to_f(c.getRb()),
            .g = color_b_to_f(c.getGb()),
            .b = color_b_to_f(c.getBb()),
        };
    }

    Color3f operator *(const float s) const {
        return {
            .r = r * s,
            .g = g * s,
            .b = b * s,
        };
    }
};

struct Color4f {
    float r = 0, g = 0, b = 0, a = 0;

    float getAf() const { return a; }
    float getRf() const { return r; }
    float getGf() const { return g; }
    float getBf() const { return b; }

    Color4f clamp(float cmax) const {
        return {
            .r = ::clamp(r, 0.0f, cmax),
            .g = ::clamp(g, 0.0f, cmax),
            .b = ::clamp(b, 0.0f, cmax),
            .a = ::clamp(a, 0.0f, cmax),
        };
    }

    Color4f clamp() const {
        return clamp(1.0f);
    }

    template <class C>
    static Color4f from4b(const C& c) {
        return {
            .r = color_b_to_f(c.getRb()),
            .g = color_b_to_f(c.getGb()),
            .b = color_b_to_f(c.getBb()),
            .a = color_b_to_f(c.getAb()),
        };
    }

    Color4f operator *(const float s) const {
        return {
            .r = r * s,
            .g = g * s,
            .b = b * s,
            .a = a * s,
        };
    }
};


inline Color3f Color3b::to_f() const {
    return Color3f {
        color_b_to_f(r),
        color_b_to_f(g),
        color_b_to_f(b),
    };
}

inline Color4f Color4b::to_f() const {
    return Color4f {
        color_b_to_f(r),
        color_b_to_f(g),
        color_b_to_f(b),
        color_b_to_f(a),
    };
}

inline Color3b rgb(const uint32_t rgb) {
    return Color3b::from_rgb(rgb);
}

inline uint8_t color_f_to_b(float f) {
    return static_cast<uint8_t>(lroundf(clamp(f * 255.0f, 0.0f, 255.0f)));
}
inline float color_b_to_f(uint8_t b) {
    return static_cast<float>(b) / 255.0f;
}
