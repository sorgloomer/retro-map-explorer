// The moving objects of a level (Scene::movers that move by themselves: the animated props such as behemoths and lifts, and the path followers) as
// kinematic bodies of the Box3D world of the level. They follow the animation clock of the page exactly (the renderer draws them where the same
// clock puts them), pushing the car and the movable objects, which can also ride on them: the wheels' casts see them as ground, with their speed.
// The collider of an object is what its hierarchy's collision files (.dcol) say (the polyhedra, each in convex pieces, and the spheres); an object
// that has none gets one from its mesh.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "../scene/scene.h"

namespace minigame01 {

constexpr uint64_t kMoverCategory = 8;   // (the level's rays see it as the level, as they see everything but the car and the objects)

class Box3dMovers {
public:
    // `unitsPerMetre`: level units per metre. `time`: the animation time now. Needs the world of the level. (It also adds the collider shapes of the
    // objects to the scene, for showColliders.)
    void build(Scene& scene, b3WorldId world, float unitsPerMetre, double time);
    void destroy() { bodies_.clear(); }
    // Before a physics step of `seconds` of the animation clock: every body is told where it will be at the end of it (so it moves there at the right
    // speed, and what stands on it moves with it).
    void update(const Scene& scene, double time, float seconds);
    // Debug: draws the colliders (solid) instead of the meshes: mode 1 the convex pieces that the physics has, mode 2 the shapes as the game's files
    // have them, 0 the meshes.
    void showColliders(Scene& scene, int mode);
    int count() const { return (int)bodies_.size(); }

private:
    struct Body {
        b3BodyId id = b3_nullBodyId;
        int mover = -1;
        int meshGeom = -1;       // what the mover draws normally
        int colliderGeom = -1;   // the convex pieces that the physics has, as a solid shape
        int rawGeom = -1;        // the shapes as the files have them (none: the pieces are drawn)
    };
    std::vector<Body> bodies_;
    float unitsPerMetre_ = 1;
    double lastTime_ = -1;   // of the last update: a jump of the clock puts the bodies straight where it says
};

}  // namespace minigame01
