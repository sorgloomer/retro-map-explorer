// The collision shapes as meshes for the debug display ("draw colliders"): solid, flat shaded (every triangle has its own vertices and the normal of
// its face), for the lit shader.
#pragma once
#include <cstdint>
#include <vector>
#include "../scene/scene.h"

namespace minigame01 {

struct ColliderPiece {
    std::vector<Vec3f> points;
    std::vector<uint32_t> indices;   // triangles over the points
    // The surface is closed and wound the same way all over (the polyhedron of a collision file, which can be concave): which side is out is the
    // winding then (the volume it has is negative or positive). Otherwise (a convex hull) a face points away from the middle of the piece.
    bool closed = false;
};

// `toUnits` multiplies the points (metres to the units of the level, or 1). Every piece has the colour r, g, b made a little lighter or darker and a
// little off in each channel, by chance that `seed` and the number of the piece decide: pieces of one shape can be told apart, and the same shape
// drawn again (at rest and moving, touching and free) has the same pieces in the same colours.
BatchSet solidColliderShape(const std::vector<ColliderPiece>& pieces, float toUnits, uint8_t r, uint8_t g, uint8_t b, uint32_t seed = 0);

}  // namespace minigame01
