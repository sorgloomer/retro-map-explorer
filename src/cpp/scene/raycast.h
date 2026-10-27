// Picking: view rays through screen pixels and ray casts against the level geometry.
#pragma once
#include "camera.h"
#include "../math/vec2.h"
#include "scene.h"

constexpr float kFovY = 1.05f;  // vertical field of view of the main camera (radians)

// Distance of the near clipping plane. Nothing closer to the camera than this is drawn, so picking ignores it too.
float nearPlane(float sceneRadius);

// Unit ray through a canvas pixel (CSS coordinates) for the given camera and canvas size.
Vec3f viewRay(const Camera& cam, Vec2i canvas, Vec2i at);

// Closest visible triangle hit along a ray (brute force; only run on clicks). Faces that the renderer culls
// (cullBackfaces) are invisible and let the ray through. Optionally returns the unit normal of the hit triangle.
bool raycastScene(const Scene& scene, bool cullBackfaces, const Vec3f& origin, const Vec3f& dir, float& tOut, Vec3f* normalOut = nullptr);

struct PivotPick {
    Vec3f point;
    float normalY = 1.0f;  // |y| of the unit normal of the picked surface (1 = floor, 0 = wall)
};

// What is under the cursor: the first visible surface behind the near clipping plane, else the ground below the cursor,
// else a point half a scene radius away. The ray starts on the near plane (at near / cos(angle to the view axis) along
// the ray), because anything in front of it cannot be seen.
PivotPick pickPivotPoint(const Scene* scene, bool cullBackfaces, const Camera& cam, Vec2i canvas, Vec2i at,
                         const Vec3f& sceneCenter, float sceneRadius);
