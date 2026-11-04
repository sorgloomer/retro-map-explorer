#include "minigame01/skidmarks.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

constexpr float kSkidSlipMinSpeed = 0.4f;     // m/s (for a car of 4.3 m): the tyre marks the road when it slides faster than this...
constexpr float kSkidForceFraction = 0.95f;   // ...and uses this part of the grip it has or more (a bar of the debug panel that is nearly full): a tyre that is cornering within its grip does not mark
constexpr float kLift = 0.020f;            // m (the same): the strip lies this far above the road, so that it does not flicker with it
constexpr float kMinSegment = 0.08f;       // m (the same): a strip grows by a piece this long at least
constexpr float kMaxSegment = 3.0f;        // m (the same): a longer step is a jump (a respawn): the strip starts again
constexpr uint8_t kDarkColor = 14;
constexpr float kWidthUnloadedFraction = 0.5f;   // of the width of the mark of a tyre that carries its share of the weight: the mark of a tyre that is hardly loaded
constexpr float kWidthSidewaysFraction = 0.65f;  // of the width of the mark of a rolling tyre: how wide the mark of a tyre that slides sideways is
constexpr float kMinDarknessCutoff = 0.01f;      // a mark lighter than this is not drawn
constexpr float kMaxAlpha = 0.85f * 255.0f;      // of a mark of full darkness
constexpr float kDarknessScale = 0.6f;

}  // namespace

void SkidMarks::build(Scene& scene) {
    BatchSet marks;
    Batch& b = marks.batchFor(-1, BLEND_ALPHA, true);
    SceneVertex dead;   // (every quad starts as nothing: all four corners in one place)
    dead.x = dead.y = dead.z = 0;
    dead.r = dead.g = dead.b = kDarkColor; dead.a = 0;
    dead.u = dead.v = 0;
    b.verts.assign((size_t)kMaxQuads * 4, dead);
    b.idx.resize((size_t)kMaxQuads * 6);
    for (int q = 0; q < kMaxQuads; q++) {
        const uint32_t base = (uint32_t)q * 4;
        const uint32_t quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        for (int k = 0; k < 6; k++) b.idx[(size_t)q * 6 + k] = quad[k];
    }
    scene.moverGeoms.push_back(std::move(marks));
    geom_ = (int)scene.moverGeoms.size() - 1;
    Mover m;
    m.geom = geom_;
    m.cat = -1;
    m.staticModel = true;   // (the vertices are in the level's space already)
    m.hidden = true;
    m.name = "skid marks";
    scene.movers.push_back(m);
    mover_ = (int)scene.movers.size() - 1;
    head_ = 0;
}

void SkidMarks::clear(Scene& scene) {
    if (geom_ < 0) return;
    for (SceneVertex& v : batch(scene).verts) { v.x = v.y = v.z = 0; v.a = 0; }
    scene.moverGeoms[geom_].revision++;
    head_ = 0;
    for (Strip& s : strip_) s.active = false;
}

void SkidMarks::show(Scene& scene, bool on) {
    if (mover_ >= 0 && mover_ < (int)scene.movers.size()) scene.movers[mover_].hidden = !on;
}

void SkidMarks::update(Scene& scene, const Box3dVehicle& car, float unitsPerMetre) {
    if (geom_ < 0) return;
    const float sizeScale = car.sizeScale();
    const Mat4f model = car.modelMatrix();
    Batch& b = batch(scene);
    bool changed = false;
    for (int i = 0; i < Box3dVehicle::kWheelCount; i++) {
        const Box3dVehicle::Wheel& wheel = car.wheel(i);
        Strip& strip = strip_[i];
        // how much of its grip the tyre uses: sideways, along, and the two together (the friction circle)
        const float sidewaysUse = wheel.lateralLimit > 0 ? std::fabs(wheel.lateralForce) / wheel.lateralLimit : 0.0f;
        const float alongUse = wheel.frictionLimit > 0 ? std::fabs(wheel.longitudinalForce) / wheel.frictionLimit : 0.0f;
        const float totalUse = wheel.frictionLimit > 0 ? std::sqrt(wheel.lateralForce * wheel.lateralForce + wheel.longitudinalForce * wheel.longitudinalForce) / wheel.frictionLimit : 0.0f;
        // a wheel that a brake holds marks the road whenever it moves; any other tyre when it slides and uses the grip it has
        const bool tooSlow = wheel.roadSpeed < kSkidSlipMinSpeed * sizeScale;
        const bool blocking = wheel.blocked > 0.25f;
        const bool sliding = std::max(sidewaysUse, std::max(alongUse, totalUse)) >= kSkidForceFraction;
        const bool marking = wheel.grounded && wheel.load > 0 && !tooSlow && (blocking || sliding);
        if (!marking) { strip.active = false; continue; }
        // where the tyre touches the road: under the middle of the wheel, along the normal of the road (the direction of the suspension's force)
        const Vec3f centre = model.transformPoint(wheel.centre);
        const float forceLength = length(wheel.force);
        const Vec3f normal = forceLength > 1e-3f ? wheel.force * (1.0f / forceLength) : car.up();
        const Vec3f point = centre - normal * car.probeRadius(i);
        // the axle of the wheel (the steering turns it), along the road: it says how sideways the tyre slides
        const float steer = wheel.steer;
        Vec3f axle = car.right() * std::cos(steer) - car.forward() * std::sin(steer);
        axle = axle - normal * dot(axle, normal);
        const float axleLength = length(axle);
        if (axleLength < 1e-3f) { strip.active = false; continue; }
        axle = axle * (1.0f / axleLength);
        const Box3dVehicle::WheelShape shape = car.wheelShape(i);
        // What the tyre presses on the road decides the mark: how dark it is (the grip factor of the tyre relative to the car's grip, times the load,
        // against the weight that a tyre carries standing still) and how wide (the width of the tyre, down to part of it for a tyre that is hardly loaded)
        const float staticLoad = car.weight() / (float)Box3dVehicle::kWheelCount;
        const float loadFraction = clamp(wheel.load / std::max(staticLoad, 1e-3f), 0.0f, 1.0f);
        // (the grip factor is divided by the car's own grip, so that what is left is the effect of the surface: a car that grips more does not mark more)
        const float darkness = clamp(kDarknessScale * wheel.gripFactor / std::max(carSettings().grip, 1e-3f) * wheel.load / std::max(staticLoad, 1e-3f), 0.0f, 1.0f);
        if (darkness < kMinDarknessCutoff) { strip.active = false; continue; }
        const float halfWidth = 0.9f * (shape.sideOffset + shape.rounding) * (loadFraction * (1.0f - kWidthUnloadedFraction) + kWidthUnloadedFraction);
        const Vec3f lift = normal * (kLift * sizeScale);
        if (!strip.active) {   // the first point of a strip: it has no direction yet
            strip.active = true;
            strip.hasEdges = false;
            strip.point = point;
            continue;
        }
        const Vec3f travel = point - strip.point;
        const float step = length(travel);
        if (step > kMaxSegment * sizeScale) {   // a jump: the strip starts again here
            strip.hasEdges = false;
            strip.point = point;
            continue;
        }
        if (step < kMinSegment * sizeScale) continue;   // (not far enough yet: the strip goes on from where it stopped)
        // The strip is as wide as the road is touched across the way the tyre goes. A tyre that rolls touches the road across all its width; one
        // that slides sideways touches it along its thickness only, which would draw a line: that is made a part of the width instead.
        Vec3f direction = travel - normal * dot(travel, normal);
        const float directionLength = length(direction);
        if (directionLength < 1e-4f) continue;
        direction = direction * (1.0f / directionLength);
        const float sideways = std::fabs(dot(direction, axle));   // 0: rolling along, 1: sliding along the axle
        const float half = halfWidth * (1.0f - (1.0f - kWidthSidewaysFraction) * sideways);
        const Vec3f across = normalize(cross(normal, direction));
        const Vec3f left = point - across * half + lift, right = point + across * half + lift;
        if (!strip.hasEdges) {
            strip.left = strip.point - across * half + lift;
            strip.right = strip.point + across * half + lift;
        }
        const Vec3f corners[4] = {strip.left, strip.right, right, left};
        const size_t first = (size_t)head_ * 4;
        for (int k = 0; k < 4; k++) {
            SceneVertex& v = b.verts[first + k];
            v.x = corners[k].x * unitsPerMetre; v.y = corners[k].y * unitsPerMetre; v.z = corners[k].z * unitsPerMetre;
            v.r = v.g = v.b = kDarkColor;
            v.a = (uint8_t)(darkness * kMaxAlpha);
        }
        head_ = (head_ + 1) % kMaxQuads;
        changed = true;
        strip.hasEdges = true;
        strip.point = point;
        strip.left = left;
        strip.right = right;
    }
    if (changed) scene.moverGeoms[geom_].revision++;
}

}  // namespace minigame01
