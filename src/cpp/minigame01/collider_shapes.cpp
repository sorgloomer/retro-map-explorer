#include "minigame01/collider_shapes.h"
#include <algorithm>
#include <cmath>

namespace {

// A number from 0 to 1 that follows from the three that are given, and looks like chance.
float chance(uint32_t seed, uint32_t piece, uint32_t channel) {
    uint32_t h = seed * 2654435761u ^ (piece + 1) * 40503u ^ (channel + 1) * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (float)(h & 0xffffff) / (float)0x1000000;
}

}  // namespace

namespace minigame01 {

BatchSet solidColliderShape(const std::vector<ColliderPiece>& pieces, float toUnits, uint8_t r, uint8_t g, uint8_t b, uint32_t seed) {
    std::vector<SceneVertex> vertices;
    std::vector<Vec3f> normals;
    std::vector<uint32_t> triangles;
    for (size_t pieceNumber = 0; pieceNumber < pieces.size(); pieceNumber++) {
        const ColliderPiece& piece = pieces[pieceNumber];
        // this piece's colour: the hue turned by up to 100 degrees either way, the saturation and the lightness changed
        const float light = 0.6f + 0.6f * chance(seed, (uint32_t)pieceNumber, 0);
        const float saturation = 0.55f + 0.45f * chance(seed, (uint32_t)pieceNumber, 1);
        const float turn = (chance(seed, (uint32_t)pieceNumber, 2) * 2.0f - 1.0f) * 100.0f;
        float rgb[3];
        {
            const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
            const float maxc = std::max(rf, std::max(gf, bf)), minc = std::min(rf, std::min(gf, bf));
            float hue = 0;
            if (maxc > minc) {
                const float d = maxc - minc;
                if (maxc == rf) hue = std::fmod((gf - bf) / d + 6.0f, 6.0f);
                else if (maxc == gf) hue = (bf - rf) / d + 2.0f;
                else hue = (rf - gf) / d + 4.0f;
                hue *= 60.0f;
            }
            hue = std::fmod(hue + turn + 360.0f, 360.0f);
            const float s = (maxc > 0 ? (maxc - minc) / maxc : 0.0f) * saturation;
            const float value = std::min(1.0f, maxc * light);
            for (int k = 0; k < 3; k++) {
                const float n = k == 0 ? 5.0f : k == 1 ? 3.0f : 1.0f;
                const float h = std::fmod(n + hue / 60.0f, 6.0f);
                const float f = std::max(0.0f, std::min(std::min(h, 4.0f - h), 1.0f));
                rgb[k] = value - value * s * f;
            }
        }
        const uint8_t pieceR = (uint8_t)(rgb[0] * 255.0f), pieceG = (uint8_t)(rgb[1] * 255.0f), pieceB = (uint8_t)(rgb[2] * 255.0f);
        Vec3f middle;
        for (const Vec3f& p : piece.points) middle += p;
        if (!piece.points.empty()) middle = middle * (1.0f / (float)piece.points.size());
        // the side that is out, from the winding: the cross product of a triangle points out when the volume of the surface is positive
        float volume = 0;
        if (piece.closed)
            for (size_t k = 0; k + 2 < piece.indices.size(); k += 3)
                volume += dot(piece.points[piece.indices[k]], cross(piece.points[piece.indices[k + 1]], piece.points[piece.indices[k + 2]]));
        for (size_t k = 0; k + 2 < piece.indices.size(); k += 3) {
            const Vec3f& p0 = piece.points[piece.indices[k]];
            const Vec3f& p1 = piece.points[piece.indices[k + 1]];
            const Vec3f& p2 = piece.points[piece.indices[k + 2]];
            Vec3f normal = cross(p1 - p0, p2 - p0);
            const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (length > 1e-12f) normal = normal * (1.0f / length);
            if (piece.closed) {
                if (volume < 0) normal = normal * -1.0f;
            } else if (dot(normal, (p0 + p1 + p2) * (1.0f / 3.0f) - middle) < 0) {
                normal = normal * -1.0f;
            }
            for (const Vec3f* p : {&p0, &p1, &p2}) {
                SceneVertex v;
                v.x = p->x * toUnits; v.y = p->y * toUnits; v.z = p->z * toUnits;
                v.r = pieceR; v.g = pieceG; v.b = pieceB; v.a = 255;
                v.u = v.v = 0;
                triangles.push_back((uint32_t)vertices.size());
                vertices.push_back(v);
                normals.push_back(normal);
            }
        }
    }
    BatchSet shape;
    shape.lit = true;
    shape.appendIndexed(-1, BLEND_OPAQUE, vertices.data(), vertices.size(), triangles.data(), triangles.size(), true, normals.data());
    return shape;
}

}  // namespace minigame01
