// Skid marks: a dark strip on the road behind every tyre that slides (sideways, locked by the brake or spinning against the road). The strips are
// quads of one geometry of the scene that is used as a ring: when it is full the oldest marks are overwritten. The renderer uploads it again
// when it changes (BatchSet::revision).
#pragma once
#include <vector>
#include "../scene/scene.h"
#include "minigame01/box3d_vehicle.h"

namespace minigame01 {

class SkidMarks {
public:
    // Adds the geometry and its mover to the scene (once per scene).
    void build(Scene& scene);
    void forget() { geom_ = mover_ = -1; }
    // Rubs the marks out.
    void clear(Scene& scene);
    void show(Scene& scene, bool on);
    // Called once per frame after the car has moved: lays the marks of this frame (the car is in metres, the scene in `unitsPerMetre` per metre).
    void update(Scene& scene, const Box3dVehicle& car, float unitsPerMetre);

private:
    static constexpr int kMaxQuads = 8192;
    int geom_ = -1, mover_ = -1;
    int head_ = 0;                   // where the next quad goes
    struct Strip {
        bool active = false;         // the tyre marked in the last frame: the strip goes on from here
        bool hasEdges = false;       // the strip has a quad already: left and right are its last edge (else only the point is known)
        Vec3f point, left, right;    // where it stopped (metres)
    } strip_[Box3dVehicle::kWheelCount];
    Batch& batch(Scene& scene) { return scene.moverGeoms[geom_].batches[0]; }
};

}  // namespace minigame01
