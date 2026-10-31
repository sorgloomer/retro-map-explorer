// A raycast vehicle: one rigid body (mass, inertia, quaternion orientation) with four spring / damper wheels that
// are ray cast against the level, tyre forces limited by a friction circle, and sphere probes around the hull for
// walls, ceilings and the ground when the chassis bottoms out.
#pragma once
#include <vector>
#include "minigame01/car_model.h"
#include "minigame01/collision_world.h"
#include "minigame01/params.h"
#include "./quaternion.h"

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

class Vehicle {
public:
    static constexpr int kWheelCount = 4;
    struct Wheel {
        Vec3f centre;        // car space, including the suspension travel
        float spin = 0;     // radians
        float steer = 0;    // radians (front wheels)
        float load = 0;     // N
        float slip = 0;     // slip angle, radians
        float lateralForce = 0;  // N, across the wheel on the road (positive: towards the car's right); for the debug display
        float lateralLimit = 0;  // N, the most the tyre could push sideways (grip limit, handbrake included)
        float longitudinalForce = 0;  // N, along the wheel on the road (positive: forward); for the debug display
        float frictionLimit = 0;      // N, the most the tyre could push in any direction (the friction circle)
        Vec3f contactPoint;      // world position where the tyre touches the road (model metres), when grounded
        Vec3f force;             // N, world: everything the wheel pushes the car with (suspension and tyre); for the debug display
        bool sliding = false;    // the sideways force wanted was over the limit: the tyre slides
        bool grounded = false;
    };

    void init(const CarModel& model);
    // Recomputes mass, centre of mass, inertia and hull probes after the tunable parameters changed (the car stays where it is).
    void applyTuning();
    float bumpStiffnessAt(float suspensionLength) const;  // 1 above the bump stop zone, up to bumpStopStiffness at the stop
    // Places the car upright with its wheels' lowest point at `groundY`, heading `yaw` (0 = +z), at rest.
    void reset(const Vec3f& groundPos, float yaw);
    void step(float dt, const VehicleInput& input, const CollisionWorld& world);
    // Moves the car `metres` up and rolls it by 90 degrees about its own nose axis, around the centre of mass; speed and spin stay as they are.
    void liftAndRoll(float metres);

    Vec3f right() const { return axes[0]; }
    Vec3f up() const { return axes[1]; }
    Vec3f forward() const { return axes[2]; }
    Vec3f position() const { return pos; }   // centre of mass
    Vec3f centreOfMassInCarSpace() const { return centreOfMass; }  // relative to the car's origin, for the debug marker
    Vec3f velocity() const { return vel; }
    float speed() const { return length(vel); }  // in the model's metres per second
    // The speed in the world: real metres per second. A small real car is modelled in enlarged metres, so the model's speed
    // says little about how fast it moves through the level.
    float gravity() const { return gravity_; }  // m/s^2 in the model's metres
    // The forces on the whole car besides the wheels, world, N, of the last physics step (debug display)
    Vec3f gravityForce() const { return gravityForce_; }
    Vec3f dragForce() const { return dragForce_; }
    Vec3f downforce() const { return downforce_; }
    Vec3f thrustForce() const { return thrustForce_; }   // the afterburner and the side thrust
    // What the level pushed the chassis with through each hull sphere in the last frame (average force, N) and where: the point of
    // the sphere that touched
    Vec3f probeForce(int i) const { return i < (int)probeForce_.size() ? probeForce_[i] : Vec3f(); }
    Vec3f probeContactPoint(int i) const { return i < (int)probeContact_.size() ? probeContact_[i] : Vec3f(); }
    // The hull's collision spheres (debug display): centres in car space, radius, and which touched the level during the last frame
    int probeCount() const { return (int)probes.size(); }
    Vec3f probeCentre(int i) const { return probes[i]; }
    float probeRadius(int i) const { return i < (int)probeRadii.size() ? probeRadii[i] : probeR; }
    bool probeTouched(int i) const { return i < (int)probeTouched_.size() && probeTouched_[i]; }
    // The torques about the centre of mass in the last physics step (N m, world), by source, and the spin (rad/s, world): debug display.
    // The forces of gravity, drag, downforce and the afterburner act on the centre of mass itself, so they turn nothing.
    Vec3f torqueFromWheels() const { return torqueWheels_; }
    Vec3f torqueFromHull() const { return torqueHull_; }
    Vec3f spin() const { return angVel; }
    Vec3f inCarAxes(const Vec3f& v) const { return Vec3f(dot(v, axes[0]), dot(v, axes[1]), dot(v, axes[2])); }  // about right, up, forward
    float weight() const { return mass * gravity(); }  // N
    float worldSpeed() const { return length(vel); }
    bool isToy() const { return toy_; }
    // Length of the car against a car of 4.3 m: 1 for the cars, about 0.07 for the toy. The distances and speeds that were set for
    // a car follow it.
    float sizeScale() const { return sizeScale_; }
    float forwardSpeed() const { return dot(vel, axes[2]); }
    Mat4f modelMatrix() const;               // car space -> world
    const Wheel& wheel(int i) const { return wheels[i]; }
    bool anyGrounded() const { return groundedCount > 0; }
    float drift() const { return drift_; }  // 0..1: how sideways the car is sliding (only above a minimum speed)
    float impact() const { return impact_; }  // strongest hull hit of the last step (m/s), for effects / scoring
    bool upsideDown() const { return axes[1].y < -0.3f; }
    float halfHeight() const { return (bmax.y - bmin.y) * 0.5f; }
    float boundingRadius() const { return 0.5f * length(bmax - bmin); }  // of the sphere around the car (half the diagonal of its bounds)

private:
    void updateAxes();
    void substep(float h, const VehicleInput& in, const CollisionWorld& world);
    Vec3f invInertia(const Vec3f& t) const;  // I^-1 * t in world space

    // --- shape
    Vec3f wheelRest[4];
    float wheelR = 0.34f;         // mean wheel radius (the car's ride height and centre of mass are measured from it)
    float wheelRadii[4] = {0.34f, 0.34f, 0.34f, 0.34f};  // per wheel, the ones the suspension and the spin use
    Vec3f gravityForce_, dragForce_, downforce_, thrustForce_;
    Vec3f torqueWheels_, torqueHull_;
    std::vector<Vec3f> probeImpulse_, probeForce_, probeContact_;  // per hull sphere: N s in the frame, N, world position
    bool toy_ = false;
    float sizeScale_ = 1;
    // the values in use: the small cars (the radio controlled ones) have parameters of their own, the others share the common ones
    float gravity_ = 9.81f, engineForce_ = 40000, enginePower_ = 420000, grip_ = 1.1f, dragCoefficient_ = 0.25f, afterburnerForce_ = 36000, kangarooSpeed_ = 8;
    float suspensionLengthMax_ = 0.3f, suspensionLengthAtRest_ = 0.15f, suspensionLengthInLoadedModel_ = 0.15f, suspensionSpringNaturalLength_ = 0.33f, bumpZone_ = 0.03f;  // the suspension in use (small cars: more travel)
    float springStiffness = 30000, damping = 3300;  // the ones in use (N/m, N s/m)
    Vec3f centreOfMass;                    // centre of mass in car space
    bool hasHullBox = false;      // the chassis hull box of the car's files
    Vec3f hullBoxMin, hullBoxMax;
    Vec3f bmin, bmax;
    std::vector<char> probeTouched_;  // per probe: it overlapped the level in a step of the last frame
    std::vector<Vec3f> probes;    // hull probe centres in car space (relative to the car origin)
    float probeR = 0.3f;
    std::vector<float> probeRadii;  // per probe (the small cars have two sizes)
    // --- mass properties
    float mass = 1250;
    Vec3f inertia;                // principal moments about right, up, forward
    // --- state
    Vec3f pos, vel, angVel;
    Quat q;
    Vec3f axes[3];                // right, up, forward in world space
    Wheel wheels[kWheelCount];
    float steerAngle = 0;
    int groundedCount = 0;
    float drift_ = 0, impact_ = 0;
};

}  // namespace minigame01
