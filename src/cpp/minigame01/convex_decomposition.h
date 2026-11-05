// Splits the surface of an object into convex pieces, for the collider of an object that is not convex (an arch, a table, a bucket): the
// convex hull of such an object would fill what the car should drive through.
//
// Every part of the surface that holds together (a polyhedron) is done on its own: a convex one is one piece; a closed one of at most 100
// vertices is cut where it is concave, until its parts are convex: for a concave edge, by the plane through the edge and a vertex (first one of
// another concave edge that can be seen from it from the inside, else the vertex the closest to the bisector of the angle inside); the pieces that
// make a convex whole again are joined. Where that does not work out, or the polyhedron is more complex, or not a solid, it is one box around it
// (along its principal axes if that is the smaller).
#pragma once
#include <cstdint>
#include <vector>
#include "../math/vec.h"

namespace minigame01 {

// The pieces as points whose convex hulls are the pieces: for the triangles `triangles` (indices into `vertices`, three each).
std::vector<std::vector<Vec3f>> decomposeIntoConvexPieces(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles);

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
