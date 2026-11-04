// The movable objects of a level (Scene::props) as bodies of the Box3D world of the level (Box3dLevel): each one is a rigid body with the
// convex hull of its kind as its collider (from the game's collision file, else from its visible mesh) and the mass that file says. They
// start asleep where the level puts them, and the car (its hull, and its wheels, which push on what they stand on) wakes them up.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "../scene/scene.h"

namespace minigame01 {

class Box3dProps {
public:
    // `unitsPerMetre`: level units per metre (the bodies are in metres). Needs the world of the level.
    // (It also adds the collider shapes of the kinds of objects to the scene, for showColliders.)
    void build(Scene& scene, b3WorldId world, float unitsPerMetre);
    // Forgets the bodies (the world that has them is destroyed, or about to be).
    void destroy() { bodies_.clear(); }
    // Back where the level puts them, asleep; the movers follow.
    void reset(Scene& scene);
    // The movers of the bodies that moved in the last physics step go where their bodies are.
    void sync(Scene& scene);
    // Debug: draws the colliders (solid, blue at rest, orange moving) instead of the meshes of the objects.
    void showColliders(Scene& scene, bool on);
    int count() const { return (int)bodies_.size(); }

private:
    struct Body {
        b3BodyId id = b3_nullBodyId;
        bool wasAwake = false;
    };
    std::vector<Body> bodies_;   // one for every Scene::props
    float unitsPerMetre_ = 1;
    bool showColliders_ = false;
    void show(Scene& scene, size_t index, bool awake) const;   // what the mover of an object draws
};

}  // namespace minigame01
