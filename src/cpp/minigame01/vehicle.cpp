#include "minigame01/vehicle.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

constexpr float kGravity = 9.81f;
constexpr float kStep = 1.0f / 150.0f;  // physics step

// --- suspension (distances along the car's up axis, metres)
constexpr float kSuspRest = 0.20f;      // wheel centre below the upper mount at rest
constexpr float kSuspMin = 0.04f;       // fully compressed
constexpr float kSuspMax = 0.42f;       // fully extended
constexpr float kSpringFree = 0.33f;    // free spring length (about 13 cm of preload at rest)
constexpr float kSpringK = 30000.0f;    // N/m
constexpr float kDamper = 3300.0f;      // N s/m

// --- tyres and engine
constexpr float kGrip = 1.12f;          // friction coefficient on the road
constexpr float kSlideGrip = 0.86f;     // part of the grip left while sliding
constexpr float kEngineForce = 8800.0f; // N at the wheels at low speed
constexpr float kEnginePower = 210000.0f;  // W
constexpr float kBrakeForce = 1.0f;     // of the friction limit
constexpr float kRollResist = 0.016f;
constexpr float kDrag = 0.46f;          // 0.5 * rho * Cd * A
constexpr float kDownforce = 0.9f;      // N per (m/s)^2
constexpr float kMaxSpeed = 95.0f;      // m/s (hard limit)
constexpr float kRollCentre = 0.30f;    // tyre side forces act this far above the road: less body roll

Vec3 quatRotate(const Quat& q, const Vec3& v) {
    Vec3 u(q.x, q.y, q.z);
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Quat quatMul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quat quatNormalize(Quat q) {
    float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-9f) return {};
    return {q.w / l, q.x / l, q.y / l, q.z / l};
}

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

void Vehicle::init(const CarModel& m) {
    for (int i = 0; i < 4; i++) wheelRest[i] = m.wheelPos[i];
    wheelR = m.wheelRadius;
    bmin = m.boundsMin;
    bmax = m.boundsMax;
    float frontZ = 0.5f * (m.wheelPos[0].z + m.wheelPos[1].z), rearZ = 0.5f * (m.wheelPos[2].z + m.wheelPos[3].z);
    float axleY = 0.25f * (m.wheelPos[0].y + m.wheelPos[1].y + m.wheelPos[2].y + m.wheelPos[3].y);
    // centre of mass: slightly ahead of the middle, about 0.55 m above the road
    com = Vec3(0, axleY - wheelR + 0.55f, rearZ + 0.54f * (frontZ - rearZ));
    float w = bmax.x - bmin.x, h = bmax.y - bmin.y, l = bmax.z - bmin.z;
    mass = 1250.0f;
    inertia = Vec3(mass / 12.0f * (h * h + l * l), mass / 12.0f * (w * w + l * l), mass / 12.0f * (w * w + h * h) * 1.5f);
    // hull probes: a lattice of spheres inside the chassis bounds; the lowest row hovers above the road
    probes.clear();
    float groundY = axleY - wheelR;
    float yLow = groundY + 0.16f + probeR, yHigh = std::max(yLow + 0.1f, bmax.y - probeR);
    float xs = std::max(0.1f, 0.5f * w - probeR);
    for (float sx : {-xs, xs})
        for (int iz = 0; iz < 3; iz++)
            for (int iy = 0; iy < 3; iy++) {
                float z = bmin.z + probeR + (bmax.z - bmin.z - 2 * probeR) * 0.5f * iz;
                float y = yLow + (yHigh - yLow) * 0.5f * iy;
                probes.push_back(Vec3(sx, y, z));
            }
    probes.push_back(Vec3(0, yHigh, bmin.z + probeR));
    probes.push_back(Vec3(0, yHigh, bmax.z - probeR));
    reset(Vec3(), 0);
}

void Vehicle::reset(const Vec3& groundPos, float yaw) {
    float axleY = 0.25f * (wheelRest[0].y + wheelRest[1].y + wheelRest[2].y + wheelRest[3].y);
    // car origin height so that the wheels touch the road with the suspension at its rest length
    Vec3 origin = groundPos + Vec3(0, wheelR - axleY + 0.02f, 0);
    q = quatNormalize({std::cos(yaw * 0.5f), 0, std::sin(yaw * 0.5f), 0});
    updateAxes();
    // pos is the centre of mass; the car origin is pos - R * com
    pos = origin + axes[0] * com.x + axes[1] * com.y + axes[2] * com.z;
    vel = angVel = Vec3();
    steerAngle = 0;
    groundedCount = 0;
    drift_ = impact_ = 0;
    for (int i = 0; i < 4; i++) {
        wheels[i] = Wheel();
        wheels[i].centre = wheelRest[i];
    }
}

void Vehicle::updateAxes() {
    axes[0] = quatRotate(q, Vec3(1, 0, 0));
    axes[1] = quatRotate(q, Vec3(0, 1, 0));
    axes[2] = quatRotate(q, Vec3(0, 0, 1));
}

Mat4 Vehicle::modelMatrix() const {
    Mat4 m = Mat4::identity();
    Vec3 origin = pos - (axes[0] * com.x + axes[1] * com.y + axes[2] * com.z);
    m.m[0] = axes[0].x; m.m[1] = axes[0].y; m.m[2] = axes[0].z;
    m.m[4] = axes[1].x; m.m[5] = axes[1].y; m.m[6] = axes[1].z;
    m.m[8] = axes[2].x; m.m[9] = axes[2].y; m.m[10] = axes[2].z;
    m.m[12] = origin.x;
    m.m[13] = origin.y;
    m.m[14] = origin.z;
    return m;
}

Vec3 Vehicle::invInertia(const Vec3& t) const {
    Vec3 b(dot(t, axes[0]) / inertia.x, dot(t, axes[1]) / inertia.y, dot(t, axes[2]) / inertia.z);
    return axes[0] * b.x + axes[1] * b.y + axes[2] * b.z;
}

void Vehicle::step(float dt, const VehicleInput& in, const CollisionWorld& world) {
    dt = std::min(dt, 0.1f);
    int n = std::max(1, (int)std::ceil(dt / kStep));
    float h = dt / (float)n;
    impact_ = 0;
    for (int i = 0; i < n; i++) substep(h, in, world);
}

void Vehicle::substep(float h, const VehicleInput& in, const CollisionWorld& world) {
    updateAxes();
    const Vec3 r = axes[0], u = axes[1], f = axes[2];
    float speed = length(vel);
    float fwdSpeed = dot(vel, f);

    // steering: full lock at a standstill, a fraction of it at speed
    float maxSteer = 0.58f / (1.0f + std::pow(speed / 20.0f, 1.6f));
    float target = clampf(in.steer, -1.0f, 1.0f) * maxSteer;
    steerAngle += (target - steerAngle) * std::min(1.0f, h * 9.0f);

    // engine / brake intent
    float drive = 0, brake = in.brake, hand = in.handbrake;
    if (in.accel > 0.01f) {
        float fmax = std::min(kEngineForce, kEnginePower / std::max(std::fabs(fwdSpeed), 4.0f));
        drive = in.accel * fmax;
        if (fwdSpeed < -1.0f) { brake = std::max(brake, in.accel); drive = 0; }  // accelerating while rolling backwards: brake first
    } else if (in.brake > 0.01f && fwdSpeed < 1.0f) {  // brake pedal doubles as reverse once the car has (almost) stopped
        drive = -in.brake * std::min(kEngineForce * 0.55f, kEnginePower * 0.4f / std::max(std::fabs(fwdSpeed), 3.0f));
        brake = 0;
    }
    if (std::fabs(fwdSpeed) > 55.0f && drive * fwdSpeed > 0) drive *= std::max(0.0f, 1.0f - (std::fabs(fwdSpeed) - 55.0f) / 15.0f);

    Vec3 force(0, -kGravity * mass, 0);
    Vec3 torque;
    // drag and downforce
    force += vel * (-kDrag * speed);
    force += u * (-kDownforce * speed * speed);

    groundedCount = 0;
    float maxSlipSide = 0;
    for (int i = 0; i < 4; i++) {
        Wheel& w = wheels[i];
        const bool front = i < 2, rear = !front;
        Vec3 local = wheelRest[i] - com;
        Vec3 mount = pos + r * local.x + u * (local.y + kSuspRest) + f * local.z;  // upper end of the suspension
        float rayLen = kSuspMax + wheelR;
        float t;
        Vec3 n;
        w.grounded = false;
        w.load = 0;
        float s = kSuspMax;
        if (world.raycast(mount, u * -1.0f, rayLen, false, t, n)) {
            s = std::max(kSuspMin, t - wheelR);
            if (s < kSuspMax) w.grounded = true;
        }
        if (w.grounded) {
            groundedCount++;
            Vec3 contact = mount - u * t;
            Vec3 rp = contact - pos;
            Vec3 vMount = vel + cross(angVel, mount - pos);
            float comp = kSpringFree - s;
            float fs = kSpringK * comp * (s < kSuspMin + 0.03f ? 4.0f : 1.0f) + kDamper * (-dot(vMount, u));
            fs = std::max(0.0f, fs);
            w.load = fs;
            Vec3 fsv = u * fs;
            force += fsv;
            torque += cross(rp, fsv);

            // wheel axes on the road
            float st = front ? steerAngle : 0.0f;
            Vec3 wf0 = f * std::cos(st) + r * std::sin(st);
            Vec3 wf = normalize(wf0 - n * dot(wf0, n));
            Vec3 lat = normalize(cross(n, wf));
            Vec3 vP = vel + cross(angVel, rp);
            float vlong = dot(vP, wf), vlat = dot(vP, lat);
            float meff = mass * 0.25f;

            float fx = 0;
            if (rear) fx += drive * 0.5f;
            float lim = kGrip * fs;
            float brk = brake * kBrakeForce * lim;
            if (rear) brk = std::max(brk, hand * 0.85f * lim);
            float sgn = vlong > 0 ? 1.0f : -1.0f;
            float brkMax = std::fabs(vlong) * meff / h;                  // never brake past a standstill
            fx -= sgn * std::min(brk, brkMax) * std::min(1.0f, std::fabs(vlong) * 2.0f);
            float rr = std::min(kRollResist * fs, std::fabs(vlong) * meff / h);
            fx -= sgn * rr * (std::fabs(drive) > 1.0f ? 0.3f : 1.0f);

            float grip = (rear && hand > 0.05f) ? 1.0f - 0.62f * hand : 1.0f;
            float limY = lim * grip;
            float fy = -vlat * meff / h * 0.85f;
            bool sliding = std::fabs(fy) > limY;
            if (sliding) fy = (fy > 0 ? 1.0f : -1.0f) * limY * kSlideGrip;
            float limX = std::sqrt(std::max(0.0f, lim * lim - fy * fy));
            fx = clampf(fx, -limX, limX);

            Vec3 ft = wf * fx + lat * fy;
            force += ft;
            Vec3 rpTyre = rp + u * kRollCentre;
            torque += cross(rpTyre, ft);

            w.slip = std::atan2(std::fabs(vlat), std::fabs(vlong) + 1.0f);
            maxSlipSide = std::max(maxSlipSide, w.slip);
            w.spin += (vlong / wheelR) * h;
            if (front) w.steer = st;
        } else {
            w.spin += (fwdSpeed / wheelR) * h * 0.5f;
            w.slip = 0;
            if (front) w.steer = steerAngle;
        }
        w.centre = wheelRest[i] + Vec3(0, kSuspRest - s, 0);  // for drawing: lower when the suspension is extended
    }
    drift_ = speed > 8.0f ? clampf((maxSlipSide - 0.12f) / 0.45f, 0.0f, 1.0f) : 0.0f;

    // in the air: a little damping and a gentle pull towards level flight so jumps land on the wheels
    if (groundedCount == 0) {
        angVel = angVel * (1.0f - 0.6f * h);
        Vec3 corr = cross(u, Vec3(0, 1, 0));
        torque += corr * (inertia.x * 2.0f);
    } else {
        angVel = angVel * (1.0f - 0.35f * h);
    }

    // integrate
    vel += force * (h / mass);
    angVel += invInertia(torque) * h;
    float sp2 = length(vel);
    if (sp2 > kMaxSpeed) vel = vel * (kMaxSpeed / sp2);
    pos += vel * h;
    Quat dq = quatMul({0, angVel.x, angVel.y, angVel.z}, q);
    q = quatNormalize({q.w + 0.5f * h * dq.w, q.x + 0.5f * h * dq.x, q.y + 0.5f * h * dq.y, q.z + 0.5f * h * dq.z});
    updateAxes();

    // hull against the level: walls, ceilings, the road when the chassis bottoms out
    Vec3 origin = pos - (axes[0] * com.x + axes[1] * com.y + axes[2] * com.z);
    for (const Vec3& pr : probes) {
        Vec3 c = origin + axes[0] * pr.x + axes[1] * pr.y + axes[2] * pr.z;
        Vec3 n;
        float depth;
        if (!world.sphereContact(c, probeR, n, depth)) continue;
        Vec3 rr = c - pos;
        Vec3 vP = vel + cross(angVel, rr);
        float vn = dot(vP, n);
        const bool floor = n.y > 0.7f;
        if (vn < 0) {
            Vec3 a = invInertia(cross(rr, n));
            float denom = 1.0f / mass + dot(n, cross(a, rr));
            float e = floor ? 0.0f : 0.25f;
            float j = -(1.0f + e) * vn / denom;
            vel += n * (j / mass);
            angVel += invInertia(cross(rr, n * j));
            impact_ = std::max(impact_, -vn);
            // Coulomb friction along the surface
            vP = vel + cross(angVel, rr);
            Vec3 vt = vP - n * dot(vP, n);
            float tl = length(vt);
            if (tl > 1e-4f) {
                Vec3 tdir = vt * (1.0f / tl);
                Vec3 at = invInertia(cross(rr, tdir));
                float denomT = 1.0f / mass + dot(tdir, cross(at, rr));
                float jt = std::min(tl / denomT, (floor ? 0.9f : 0.5f) * j);
                vel += tdir * (-jt / mass);
                angVel += invInertia(cross(rr, tdir * -jt));
            }
        }
        pos += n * std::min(depth * 0.7f, 0.25f);
    }
}

}  // namespace minigame01
