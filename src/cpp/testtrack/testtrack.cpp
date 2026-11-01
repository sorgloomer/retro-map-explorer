// A level made by the program itself, for testing the driving: a flat plain without an end that matters, with a checkerboard of slightly
// different squares so that the speed can be felt. It has no files of its own: the car comes from the games that have one, so the level
// is listed only when a car can be driven.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "game.h"

namespace {

// The plain is a grid of quads that share their corners: huge triangles are hard on the renderer (clipping, precision), and the collision
// grid tests every triangle larger than 80 m on every query. A quad is two repeats of the texture across: 16 x 16 squares.
constexpr float kQuadSize = 64.0f;     // m
constexpr int kQuadsPerSide = 512;     // the plain is 32.8 km across (the minigame's far plane, 12 km, hides its edge)
constexpr float kHalfSize = 0.5f * kQuadSize * kQuadsPerSide;
constexpr float kCellSize = 4.0f;      // m: one square of the checkerboard
constexpr int kCellsPerTile = 8;       // squares along the side of the texture, which repeats over the plain
constexpr int kCellPixels = 16;

// A checkerboard of light and dark squares; every square has a shade of its own (a fixed random sequence), and a darker line runs along
// its edges, so that there is something to see at every speed.
Image checkerboardImage() {
    Image img;
    img.w = img.h = kCellsPerTile * kCellPixels;
    img.rgba.resize((size_t)img.w * img.h * 4);
    uint32_t seed = 12345;
    auto random01 = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (float)((seed >> 8) & 0xffff) / 65535.0f;
    };
    for (int cy = 0; cy < kCellsPerTile; cy++) {
        for (int cx = 0; cx < kCellsPerTile; cx++) {
            const float shade = ((cx + cy) & 1 ? 175.0f : 105.0f) + (random01() - 0.5f) * 36.0f;
            for (int py = 0; py < kCellPixels; py++) {
                for (int px = 0; px < kCellPixels; px++) {
                    const bool edge = px == 0 || py == 0;
                    const uint8_t value = (uint8_t)(edge ? shade * 0.75f : shade);
                    uint8_t* p = &img.rgba[((size_t)(cy * kCellPixels + py) * img.w + (cx * kCellPixels + px)) * 4];
                    p[0] = value;
                    p[1] = value;
                    p[2] = (uint8_t)std::min(255.0f, value * 1.06f);  // a little blue
                    p[3] = 255;
                }
            }
        }
    }
    return img;
}

class TestTrackGame : public Game {
public:
    explicit TestTrackGame(std::function<bool()> carAvailable) : carAvailable(std::move(carAvailable)) {}

    const char* id() const override { return "testtrack"; }
    const char* name() const override { return "Test track"; }
    const char* dropHint() const override { return "(needs the files of a game that has a car)"; }
    bool detect() override { return carAvailable(); }
    std::vector<LevelInfo> listLevels() override { return {{"flat", "Flat checkerboard plain", ""}}; }

    bool load(const std::string& levelId, Scene& out, std::string& error) override {
        if (levelId != "flat") { error = "unknown test level"; return false; }
        out.title = "Flat checkerboard plain";
        out.info = "A flat plain, 33 km across, for testing the driving.\nSquares of 4 m.";
        out.cullBackfaces = false;
        const int texture = out.addTexture(checkerboardImage());
        const float tile = kCellSize * kCellsPerTile;  // m covered by one repeat of the texture
        auto vertex = [&](float x, float z) {
            SceneVertex v;
            v.x = x; v.y = 0; v.z = z;
            v.r = v.g = v.b = v.a = 255;
            v.u = x / tile;
            v.v = z / tile;
            return v;
        };
        constexpr int kCornersPerSide = kQuadsPerSide + 1;
        std::vector<SceneVertex> corners;
        corners.reserve((size_t)kCornersPerSide * kCornersPerSide);
        for (int iz = 0; iz < kCornersPerSide; iz++)
            for (int ix = 0; ix < kCornersPerSide; ix++) corners.push_back(vertex(-kHalfSize + ix * kQuadSize, -kHalfSize + iz * kQuadSize));
        std::vector<uint32_t> indices;
        indices.reserve((size_t)kQuadsPerSide * kQuadsPerSide * 6);
        for (int iz = 0; iz < kQuadsPerSide; iz++) {
            for (int ix = 0; ix < kQuadsPerSide; ix++) {
                const uint32_t a = (uint32_t)(iz * kCornersPerSide + ix), b = a + kCornersPerSide;  // a: (x, z), b: (x, z + 1)
                indices.insert(indices.end(), {a, b, b + 1, a, b + 1, a + 1});  // two triangles facing up
            }
        }
        out.addIndexed(texture, BLEND_OPAQUE, corners.data(), corners.size(), indices.data(), indices.size(), true);
        const int startCategory = out.addCategory("start", "Start position", Color3b::from_rgb(0x39ff7a), SHAPE_ARROW, 2.0f, true);
        out.addMarker(startCategory, Vec3f(0, 0, 0), 0, "Start");
        return true;
    }

private:
    std::function<bool()> carAvailable;
};

}  // namespace

Game* makeTestTrack(std::function<bool()> carAvailable) { return new TestTrackGame(std::move(carAvailable)); }
