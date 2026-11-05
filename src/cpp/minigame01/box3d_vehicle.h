// The car on the Box3D physics engine (https://github.com/erincatto/box3d): the car is one rigid body with the convex hull of its files, and
// Box3D does the collision of that hull with the level (Box3dLevel) and the hull's contact friction. The wheels are not bodies: each one is
// a shape cast down from the upper end of its suspension (the shape of the tyre: a rounded cylinder; also by Box3D, against the level only), and the suspension and the tyres are the
// forces of the raycast vehicle of vehicle.h, applied to the body at the points where the tyres touch the road. The wheels have no mass and no
// inertia; the speed and the angle that they turn with are worked out here (the engine and the brakes ask for them). This class makes the
// settings of the car (minigame01/params.h) its body and its forces, drives them from the keys, and reports what the debug display shows.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "minigame01/box3d_level.h"
#include "minigame01/car_model.h"
#include "minigame01/params.h"

namespace minigame01 {

struct VehicleInput {
    float accel = 0;      // 0..1
    float brake = 0;      // 0..1 (also reverses once the car stands still)
    float steer = 0;      // -1 left .. 1 right
    float handbrake = 0;  // 0..1
    bool afterburner = false;  // thrust forward while true
    bool sideThrust = false;   // thrust to the car's right while true (same force as the afterburner)
    bool jump = false;         // one upward impulse (kangaroo); the caller sets it for a single step per key press
};

class Box3dVehicle {
public:
    static constexpr int kWheelCount = 4;
    struct Wheel {
        Vec3f centre;        // car space, including the suspension travel
        float spin = 0;     // radians
        float steer = 0;    // radians (front wheels)
        float load = 0;     // N
        float slip = 0;     // slip angle, radians
        float blocked = 0;       // 0..1: how much a brake (or the handbrake) holds the wheel from turning
        float roadSpeed = 0;     // m/s: how fast the road moves under the tyre
        float slipSpeed = 0;     // m/s: how fast the tyre slides over the road, sideways and along (the wheel's own turning against the road's speed)
        float lateralForce = 0;  // N, across the wheel on the road (positive: towards the car's right); for the debug display
        float lateralLimit = 0;  // N, the most the tyre could push sideways (grip limit, handbrake included)
        float longitudinalForce = 0;  // N, along the wheel on the road (positive: forward); for the debug display
        float frictionLimit = 0;      // N, the most the tyre could push in any direction (the friction circle)
        float gripFactor = 0;         // the tyre's grip on this road: the car's friction coefficient with the effect of the surface on it (what the load is multiplied by)
        Vec3f contactPoint;      // world position where the tyre touches the road (model metres), when grounded
        Vec3f force;             // N, world: what the suspension pushes the car with, at tyreForceOrigin; for the debug display
        Vec3f tyreForceOrigin;   // world position where the forces of the wheel act, the suspension's and the tyre's: at contactPoint
        Vec3f suspensionForceOrigin;  // where the suspension's force acts (the same point for this vehicle, the centre of the wheel for the Box3D one)
        Vec3f tyreForce;         // N, world: what the tyre (engine, brakes, grip) pushes the car with, at tyreForceOrigin; for the debug display
        bool sliding = false;    // the sideways force wanted was over the limit: the tyre slides
        bool grounded = false;
    };

    ~Box3dVehicle() { destroyBodies(); }

    // What the level does to the body over a physics step, from its contacts: the force (N) along the normal, where it acts and the normal.
    struct ContactSum {
        Vec3f force, frictionForce, point, normal;
        float normalForce = 0;
        bool touching = false;
    };

    // The level that the car drives in (Box3dLevel, its Box3D world): call before init.
    void attach(const Box3dLevel& level) { destroyBodies(); level_ = &level; world = level.world(); }
    void detach() { chassis = b3_nullBodyId; world = b3_nullWorldId; level_ = nullptr; }

    void init(const CarModel& model);
    // Makes the car again from the settings (mass, wheels, suspension, ...): it stays where it is, with its speed.
    void applyTuning();
    void reset(const Vec3f& groundPos, float yaw);
    void step(float dt, const VehicleInput& input);
    // Moves the car `metres` up and rolls it by 90 degrees about its own nose axis, around the centre of mass; speed and spin stay as they are.
    void liftAndRoll(float metres);

    Vec3f right() const { return axes[0]; }
    Vec3f up() const { return axes[1]; }
    Vec3f forward() const { return axes[2]; }
    Vec3f position() const { return pos; }   // centre of mass
    Vec3f forceCentre() const { return pos; }   // (the wheels have no mass)
    Vec3f centreOfMassInCarSpace() const { return centreOfMass; }
    Vec3f velocity() const { return vel; }
    float speed() const { return length(vel); }
    float gravity() const { return gravity_; }
    Vec3f gravityForce() const { return gravityForce_; }
    Vec3f dragForce() const { return dragForce_; }
    Vec3f downforce() const { return downforce_; }
    Vec3f thrustForce() const { return thrustForce_; }
    // The collision spheres of the debug display: here the four tyres (centre in car space, radius, touching the road).
    Vec3f probeForce(int i) const { return wheels[i].tyreForce; }
    Vec3f probeContactPoint(int i) const { return wheels[i].contactPoint; }
    int probeCount() const { return kWheelCount; }
    // The forces of the level on the hull, one per contact point, for the grey arrows (the most there can be, and which are in use).
    static constexpr int kMaxHullArrows = 12;
    int hullArrowCount() const { return kMaxHullArrows; }
    Vec3f hullArrowPoint(int i) const { return hullArrowPoint_[i]; }
    Vec3f hullArrowForce(int i) const { return hullArrowForce_[i]; }
    bool hullArrowOn(int i) const { return i < hullArrowCount_; }
    // The collision shape of the car: convex pieces (the polyhedra of its collision file, each split into convex ones; a car whose hull is not
    // convex has several), their triangles in car space, and whether it touched the level.
    struct HullPiece {
        std::vector<Vec3f> vertices;   // car space
        std::vector<uint32_t> indices; // the faces as triangles
    };
    const std::vector<HullPiece>& colliderPieces() const { return hullPieces_; }
    // The hull as the car's collision file has it (the polyhedra, as they are): points in car space and the faces as triangles; none for a car that has no faces.
    const std::vector<Vec3f>& rawHullPoints() const { return hullPoints_; }
    const std::vector<uint32_t>& rawHullTriangles() const { return modelHullIndices_; }
    bool hullTouched() const { return hullTouched_; }
    // The shape of a tyre that is cast down to find the road under it: polygons of kWheelCastSides corners in the plane that the wheel rolls in
    // (a corner straight up), kWheelCastRings of them evenly spaced across the tyre (the outer two at +- sideOffset), each a little smaller than
    // the one in the middle (the tread bulges out a little in the middle, so that the tyre touches the road at a point), swept by a sphere of
    // radius `rounding` (so the edges are rounded; the tyre is `radius[middle] + rounding` tall and 2 * (sideOffset + rounding) wide). Wheel space:
    // x is the axle. The rings go from the +x side to the -x side.
    static constexpr int kWheelCastSides = 16;
    static constexpr int kWheelCastRings = 5;
    struct WheelShape {
        float offset[kWheelCastRings];   // where each ring is across the tyre
        float radius[kWheelCastRings];   // and how big it is
        float sideOffset, rounding;
    };
    WheelShape wheelShape(int i) const;
    Vec3f probeCentre(int i) const { return wheels[i].centre; }
    float probeRadius(int i) const { return wheelRadii[i]; }
    bool probeTouched(int i) const { return wheels[i].grounded; }
    Vec3f torqueFromWheels() const { return torqueWheels_; }
    Vec3f torqueFromHull() const { return torqueHull_; }
    Vec3f spin() const { return angVel; }
    Vec3f inCarAxes(const Vec3f& v) const { return Vec3f(dot(v, axes[0]), dot(v, axes[1]), dot(v, axes[2])); }
    float weight() const { return mass * gravity(); }
    float worldSpeed() const { return length(vel); }
    bool isToy() const { return toy_; }
    float sizeScale() const { return sizeScale_; }
    float forwardSpeed() const { return dot(vel, axes[2]); }
    // what the engine and the brakes push a wheel with, as a torque (N m), and how it turns (rad/s): for the tests
    float spinTorque(int i) const { return wheelDriveForce_[i] * wheelRadii[i]; }
    float spinSpeed(int i) const { return wheelSpeed_[i]; }
    Mat4f modelMatrix() const;
    const Wheel& wheel(int i) const { return wheels[i]; }
    bool anyGrounded() const { return groundedCount > 0; }
    float drift() const { return drift_; }
    float impact() const { return impact_; }
    bool upsideDown() const { return axes[1].y < -0.3f; }
    float halfHeight() const { return (bmax.y - bmin.y) * 0.5f; }
    float boundingRadius() const { return 0.5f * length(bmax - bmin); }

private:
    void destroyBodies();
    void readState();
    void substep(float h, const VehicleInput& in);
    void readHullArrows(float h);
    float bumpStiffnessAt(float suspensionLength) const;  // 1 above the bump stop zone, up to bumpStopStiffness at the stop
    Vec3f invInertia(const Vec3f& torque) const;           // I^-1 * torque in world space

    b3WorldId world = b3_nullWorldId;
    const Box3dLevel* level_ = nullptr;
    b3BodyId chassis = b3_nullBodyId;
    static constexpr int kWorldSubSteps = 4;   // the solver steps of the world in a physics step

    // --- the car's data (from the model and the settings)
    Vec3f wheelRest[kWheelCount];
    Vec3f bmin, bmax, hullBoxMin, hullBoxMax;
    bool hasHullBox = false;
    bool toy_ = false;
    float sizeScale_ = 1;
    float gravity_ = 9.81f, engineForce_ = 40000, enginePower_ = 420000, grip_ = 1.1f, dragCoefficient_ = 0.25f, afterburnerForce_ = 36000, kangarooSpeed_ = 8;
    float suspensionLengthMax_ = 0.3f, suspensionLengthAtRest_ = 0.15f, suspensionLengthInLoadedModel_ = 0.15f, suspensionSpringNaturalLength_ = 0.33f, bumpZone_ = 0.03f;
    float springStiffness_ = 30000, damping_ = 3300;   // N/m, N s/m
    float wheelRadii[kWheelCount] = {0.34f, 0.34f, 0.34f, 0.34f};
    float wheelWidth_ = 0.25f;      // of the tyres (the shape that is cast to find the road under a wheel)
    float mass = 1250;              // of the whole car
    Vec3f centreOfMass;             // in car space
    Vec3f inertia;                  // about the centre of mass: right, up, forward
    std::vector<Vec3f> hullPoints_;                  // what the hull is made of (car space)
    std::vector<uint32_t> modelHullIndices_;         // the faces of the hull of the car's files, as triangles over hullPoints_ (none: the engine's hull is drawn)
    std::vector<std::vector<Vec3f>> hullParts_;      // the points of the convex pieces of the hull (made once, when the model is set: it takes a while for a concave one)
    std::vector<HullPiece> hullPieces_;              // the convex pieces that the hull is made of, as they are drawn
    Vec3f hullArrowPoint_[kMaxHullArrows], hullArrowForce_[kMaxHullArrows];
    int hullArrowCount_ = 0;
    float wheelSpeed_[kWheelCount] = {0, 0, 0, 0};   // rad/s, how the wheels turn (see substep)
    float wheelDriveForce_[kWheelCount] = {0, 0, 0, 0};   // N, what the engine pushes the wheel with along the road (this step; negative: reverse)
    bool hullTouched_ = false;

    // --- state, as the engine has it (readState)
    Vec3f pos, vel, angVel;          // of the centre of mass
    Vec3f axes[3];                   // right, up, forward in world space
    Wheel wheels[kWheelCount];
    float steerAngle = 0;
    int groundedCount = 0;
    float drift_ = 0, impact_ = 0;
    Vec3f gravityForce_, dragForce_, downforce_, thrustForce_;
    Vec3f torqueWheels_, torqueHull_;
};

}  // namespace minigame01
