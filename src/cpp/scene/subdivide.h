// Smoothing by subdivision: Loop subdivision with creases. Every triangle becomes four and the vertices move so that gently
// curved surfaces round off. Edges sharper than `creaseDegrees` and open borders keep their shape (walls, corners, the rim of
// a ramp), so boxy geometry is not melted; vertices that meet more than two such edges stay where they are.
// Each pass multiplies the triangle count by four, so the passes stop early rather than go past `maxTriangles`; both
// functions return the number of passes that were done.
#pragma once
#include "scene/scene.h"

constexpr size_t kSmoothMaxTriangles = 3000000;

// The level's triangle batches (vertex colours and texture coordinates are interpolated; the batches keep their materials).
int smoothScene(Scene& scene, int passes, float creaseDegrees = 40.0f, size_t maxTriangles = kSmoothMaxTriangles);

// A game's collision mesh: triangulated, subdivided and welded. The game's own bounding volume hierarchy no longer fits it,
// so the result carries none (the collision world indexes it with its grid instead).
int smoothCollisionMesh(CollisionMesh& mesh, int passes, float creaseDegrees = 40.0f, size_t maxTriangles = kSmoothMaxTriangles);
