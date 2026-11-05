// Simple graphical effects of the minigame, all drawn as soft blobs (a centre that fades out to the rim) of one geometry of the scene that is
// made again every frame (BatchSet::revision): the flame of the afterburner (added light), the dust that the sliding tyres kick up (more of it where
// they also mark the road), and a soft shadow of the car on the road under it.
#pragma once
#include <vector>
#include "../scene/scene.h"
#include "minigame01/box3d_level.h"
#include "minigame01/box3d_vehicle.h"
#include "minigame01/skidmarks.h"

namespace minigame01 {

class Effects {
public:
    // Adds the geometry and its mover to the scene (once per scene).
    void build(Scene& scene);
    void forget() { geom_ = mover_ = -1; }
    void clear(Scene& scene);
    void show(Scene& scene, bool on);
    // Called once per frame after the car has moved. `viewForward` is where the camera looks (the blobs face it).
    void update(Scene& scene, float dt, const Box3dVehicle& car, const Box3dLevel& level, const SkidMarks& skid, const Vec3f& viewForward,
                bool afterburner, float unitsPerMetre);

private:
    static constexpr int kRimPoints = 8;
    static constexpr int kMaxDust = 320, kMaxFlame = 160;
    static constexpr int kBlobVertices = 1 + kRimPoints;
    static constexpr int kShadowVertices = 1 + 2 * kRimPoints;
    struct Particle {
        bool alive = false;
        Vec3f position, velocity;
        float age = 0, life = 1;
        float sizeStart = 0, sizeEnd = 0, alpha = 0;
        float colourStart[3] = {0, 0, 0}, colourEnd[3] = {0, 0, 0};
    };
    int geom_ = -1, mover_ = -1;
    Particle dust_[kMaxDust], flame_[kMaxFlame];
    float dustDebt_[Box3dVehicle::kWheelCount] = {0, 0, 0, 0};   // the fraction of a particle that a wheel owes
    float flameDebt_ = 0;
    uint32_t random_ = 12345;
    float random() { random_ = random_ * 1664525u + 1013904223u; return (float)(random_ >> 8) / 16777216.0f; }   // 0..1
    float between(float a, float b) { return a + (b - a) * random(); }
    Batch& alphaBatch(Scene& scene) { return scene.moverGeoms[geom_].batches[0]; }
    Batch& addBatch(Scene& scene) { return scene.moverGeoms[geom_].batches[1]; }
    void emitDust(const Box3dVehicle& car, int wheel, bool marking, float dt);
    void emitFlame(const Box3dVehicle& car, float dt);
    template <size_t N> void advance(Particle (&particles)[N], float dt, float drag, float rise);
};

}  // namespace minigame01
