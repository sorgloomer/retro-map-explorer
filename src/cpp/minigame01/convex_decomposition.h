// Splits the surface of an object into convex pieces, for the collider of an object that is not convex (an arch, a table, a bucket): the
// convex hull of such an object would fill what the car should drive through. The object is voxelised (its triangles, and what they enclose),
// and the voxels are split by planes of the grid, the worst piece first (the one whose convex hull is the most larger than the piece), where the
// planes make the pieces the most like their hulls; until every piece is nearly its own hull or there are enough pieces. It is an
// approximation (like V-HACD, which it is made after): the pieces follow the surface to a precision of the voxels.
#pragma once
#include <cstdint>
#include <vector>
#include "../math/vec.h"

namespace minigame01 {

struct ConvexDecompositionSettings {
    int resolution = 24;          // voxels along the longest side of the object
    float concavity = 0.08f;      // a piece is convex enough when its hull is at most this part of its own volume larger
    int maxPieces = 12;
    bool solid = false;           // the surface is closed (the polyhedron of a collision file): the volume of a piece is what is inside it, counted exactly
};

// The settings for a surface that is exact (the polyhedron of a collision file): a concavity of a few per cent of its volume is meant (the pocket
// under the Eagle is 3% of its hull), which the coarse voxels of a visible mesh would not see. It is only for the polyhedra that are not
// convex: the test for that is exact.
inline ConvexDecompositionSettings exactSurfaceSettings(int resolution = 48) {
    ConvexDecompositionSettings settings;
    settings.resolution = resolution;
    settings.concavity = 0.01f;
    settings.solid = true;
    return settings;
}

// The pieces as points whose convex hulls are the pieces: for the triangles `triangles` (indices into `vertices`, three each). An object that is
// convex (which is tested first, and costs nothing) or too thin to have a volume is one piece.
std::vector<std::vector<Vec3f>> decomposeIntoConvexPieces(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles,
                                                          const ConvexDecompositionSettings& settings = ConvexDecompositionSettings());

// A part of a surface that holds together: its points (those that its triangles use) and its triangles over them.
struct SurfaceComponent {
    std::vector<Vec3f> points;
    std::vector<uint32_t> triangles;
};

// The parts that a surface is made of: the triangles that share an edge are one part (polyhedra that only touch each other at a vertex are
// two). The collision files of the game give an object that is not one solid as several polyhedra in one file (an anti-tank hedgehog is
// three tetrahedra, a lamp post a box and a wedge that share a point), which this tells apart.
std::vector<SurfaceComponent> splitSurfaceIntoComponents(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles);

}  // namespace minigame01
