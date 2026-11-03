// The car on the Box3D physics engine (https://github.com/erincatto/box3d): the chassis is one rigid body with the car's hull, every wheel
// is a body with a sphere, held to the chassis by a wheel joint (suspension spring and limit, steering, spin motor). The engine does the
// rest: the collision of hull and tyres with the level (Box3dLevel), the contact friction, the rolling. This class makes the settings of the
// car (minigame01/params.h) its joints and motors, drives them from the keys, and reports what the debug display shows. It has the
// interface of Vehicle (vehicle.h, the engine's own raycast vehicle, which is used when the build says --legacy-physics), so the game
// does not care which one it has.
#pragma once
#include <vector>
#include "box3d/box3d.h"
#include "minigame01/box3d_level.h"
#include "minigame01/car_model.h"
#include "minigame01/params.h"
#include "minigame01/vehicle.h"

namespace minigame01 {

class Box3dVehicle {
public:
    static constexpr int kWheelCount = Vehicle::kWheelCount;
    using Wheel = Vehicle::Wheel;

    ~Box3dVehicle() { destroyBodies(); }

    // What the level does to one body over a physics step, from its contacts: the force (N) along the normal, the friction (N, what the
    // engine keeps of it as the accumulated friction impulse of the contacts), where they act, the normal force and the normal.
    struct ContactSum {
        Vec3f force, frictionForce, point, normal;
        float normalForce = 0;
        bool touching = false;
    };

    // The level that the car drives in (Box3dLevel, its Box3D world): call before init.
    void attach(const Box3dLevel& level) { destroyBodies(); level_ = &level; world = level.world(); }
    void detach() { chassis = b3_nullBodyId; for (b3BodyId& b : wheelBody) b = b3_nullBodyId; world = b3_nullWorldId; level_ = nullptr; }

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
    // The centre of mass of the whole car, the wheels included: the drag, the downforce and the rockets act through it (position() is the one
    // of the chassis alone, which is where the model is placed from).
    Vec3f forceCentre() const { return carCentre_; }
    Vec3f centreOfMassInCarSpace() const { return centreOfMass; }
    Vec3f velocity() const { return vel; }
    float speed() const { return length(vel); }
    float gravity() const { return gravity_; }
    Vec3f gravityForce() const { return gravityForce_; }
    Vec3f dragForce() const { return dragForce_; }
    Vec3f downforce() const { return downforce_; }
    Vec3f thrustForce() const { return thrustForce_; }
    // The collision spheres of the debug display: here the four tyres (centre in car space, radius, touching the level).
    Vec3f probeForce(int i) const { return wheels[i].tyreForce; }
    Vec3f probeContactPoint(int i) const { return wheels[i].contactPoint; }
    int probeCount() const { return kWheelCount; }
    // The forces of the level on the hull, one per contact point, for the grey arrows (the most there can be, and which are in use).
    static constexpr int kMaxHullArrows = 12;
    int hullArrowCount() const { return kMaxHullArrows; }
    Vec3f hullArrowPoint(int i) const { return hullArrowPoint_[i]; }
    Vec3f hullArrowForce(int i) const { return hullArrowForce_[i]; }
    bool hullArrowOn(int i) const { return i < hullArrowCount_; }
    // The convex hull of the chassis, the other collision shape of the car, as the engine made it: its triangles (car space), and whether it
    // touched the level.
    bool colliderMesh(std::vector<Vec3f>& vertices, std::vector<uint32_t>& indices) const { vertices = hullVertices_; indices = hullIndices_; return !hullIndices_.empty(); }
    bool hullTouched() const { return hullTouched_; }
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
    // what the spin motor of a wheel does now (N m, rad/s): for the tests
    float spinTorque(int i) const { return b3WheelJoint_GetSpinTorque(wheelJoint[i]); }
    float spinSpeed(int i) const { return wheelSpeed_[i]; }   // rad/s, how the wheel turns
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
    void readContacts(float h);
    void readHullArrows(float h);

    b3WorldId world = b3_nullWorldId;
    const Box3dLevel* level_ = nullptr;
    ContactSum wheelContact_[kWheelCount];   // the contacts of the tyres over the last physics step
    float roadFriction_[kWheelCount] = {1, 1, 1, 1};   // the multiplier of the friction of the surface under each tyre (from the last step)
    b3BodyId chassis = b3_nullBodyId;
    b3BodyId wheelBody[kWheelCount] = {b3_nullBodyId, b3_nullBodyId, b3_nullBodyId, b3_nullBodyId};
    b3JointId wheelJoint[kWheelCount] = {};
    b3ShapeId wheelShape[kWheelCount] = {};
    static constexpr int kSolverSteps = 4;   // the world is stepped this many times in a physics step, one solver step each (see substep)

    // --- the car's data (from the model and the settings)
    Vec3f wheelRest[kWheelCount];
    Vec3f bmin, bmax, hullBoxMin, hullBoxMax;
    bool hasHullBox = false;
    bool toy_ = false;
    float sizeScale_ = 1;
    float gravity_ = 9.81f, engineForce_ = 40000, enginePower_ = 420000, grip_ = 1.1f, dragCoefficient_ = 0.25f, afterburnerForce_ = 36000, kangarooSpeed_ = 8;
    float suspensionLengthMax_ = 0.3f, suspensionLengthAtRest_ = 0.15f, suspensionLengthInLoadedModel_ = 0.15f, suspensionSpringNaturalLength_ = 0.33f;
    float wheelRadii[kWheelCount] = {0.34f, 0.34f, 0.34f, 0.34f};
    float mass = 1250;              // of the whole car, wheels included
    float wheelMass = 25;           // of one wheel
    Vec3f centreOfMass;             // in car space
    Vec3f inertia;                  // of the chassis about its centre of mass: right, up, forward
    std::vector<Vec3f> hullPoints_;                  // what the hull is made of (car space)
    std::vector<uint32_t> modelHullIndices_;         // the faces of the hull of the car's files, as triangles over hullPoints_ (none: the engine's hull is drawn)
    std::vector<Vec3f> hullVertices_;                // the hull as the engine made it: its points...
    std::vector<uint32_t> hullIndices_;              // ...and its faces as triangles
    Vec3f hullArrowPoint_[kMaxHullArrows], hullArrowForce_[kMaxHullArrows];
    int hullArrowCount_ = 0;
    float wheelSpeed_[kWheelCount] = {0, 0, 0, 0};   // rad/s, how the wheels turn (see substep)
    bool hullTouched_ = false;

    // --- state, as the engine has it (readState)
    Vec3f pos, vel, angVel;          // of the centre of mass (the chassis')
    Vec3f carCentre_;                // centre of mass of the chassis and the four wheels together
    Vec3f axes[3];                   // right, up, forward in world space
    Wheel wheels[kWheelCount];
    float steerAngle = 0;
    int groundedCount = 0;
    float drift_ = 0, impact_ = 0;
    Vec3f gravityForce_, dragForce_, downforce_, thrustForce_;
    Vec3f torqueWheels_, torqueHull_;
    Vec3f rollDirection_[kWheelCount];   // where each wheel rolls, along the road (from the last step)
    Vec3f wheelPush_[kWheelCount];       // N, what the engine and the brakes push each wheel with (this step)
};

}  // namespace minigame01
