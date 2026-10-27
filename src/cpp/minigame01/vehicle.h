// A raycast vehicle: one rigid body (mass, inertia, quaternion orientation) with four spring / damper wheels that
// are ray cast against the level, tyre forces limited by a friction circle, and sphere probes around the hull for
// walls, ceilings and the ground when the chassis bottoms out.
#pragma once
#include <vector>
#include "minigame01/car_model.h"
#include "minigame01/collision_world.h"

namespace minigame01 {

struct VehicleInput {
    float accel = 0;      // 0..1
    float brake = 0;      // 0..1 (also reverses once the car stands still)
    float steer = 0;      // -1 left .. 1 right
    float handbrake = 0;  // 0..1
};

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
};

class Vehicle {
public:
    struct Wheel {
        Vec3f centre;        // car space, including the suspension travel
        float spin = 0;     // radians
        float steer = 0;    // radians (front wheels)
        float load = 0;     // N
        float slip = 0;     // slip angle, radians
        bool grounded = false;
    };

    void init(const CarModel& model);
    // Places the car upright with its wheels' lowest point at `groundY`, heading `yaw` (0 = +z), at rest.
    void reset(const Vec3f& groundPos, float yaw);
    void step(float dt, const VehicleInput& input, const CollisionWorld& world);

    Vec3f right() const { return axes[0]; }
    Vec3f up() const { return axes[1]; }
    Vec3f forward() const { return axes[2]; }
    Vec3f position() const { return pos; }   // centre of mass
    Vec3f velocity() const { return vel; }
    float speed() const { return length(vel); }
    float forwardSpeed() const { return dot(vel, axes[2]); }
    Mat4 modelMatrix() const;               // car space -> world
    const Wheel& wheel(int i) const { return wheels[i]; }
    bool anyGrounded() const { return groundedCount > 0; }
    float drift() const { return drift_; }  // 0..1: how sideways the car is sliding (only above a minimum speed)
    float impact() const { return impact_; }  // strongest hull hit of the last step (m/s), for effects / scoring
    bool upsideDown() const { return axes[1].y < -0.3f; }
    float halfHeight() const { return (bmax.y - bmin.y) * 0.5f; }

private:
    void updateAxes();
    void substep(float h, const VehicleInput& in, const CollisionWorld& world);
    Vec3f invInertia(const Vec3f& t) const;  // I^-1 * t in world space

    // --- shape
    Vec3f wheelRest[4];
    float wheelR = 0.34f;
    Vec3f com;                    // centre of mass in car space
    Vec3f bmin, bmax;
    std::vector<Vec3f> probes;    // hull probe centres in car space (relative to the car origin)
    float probeR = 0.3f;
    // --- mass properties
    float mass = 1250;
    Vec3f inertia;                // principal moments about right, up, forward
    // --- state
    Vec3f pos, vel, angVel;
    Quat q;
    Vec3f axes[3];                // right, up, forward in world space
    Wheel wheels[4];
    float steerAngle = 0;
    int groundedCount = 0;
    float drift_ = 0, impact_ = 0;
};

}  // namespace minigame01
