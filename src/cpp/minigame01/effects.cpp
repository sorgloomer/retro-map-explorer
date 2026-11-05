#include "minigame01/effects.h"
#include <algorithm>
#include <cmath>

namespace minigame01 {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr bool kBlobShadow = false;
constexpr float kDustSlipSpeed = 2.0f;        // m/s (for a car of 4.3 m): a tyre that slides faster than this over the road kicks up dust...
constexpr float kDustFullSlipSpeed = 10.0f;   // m/s (the same): ...and a tyre that slides this fast, as much as it can
constexpr float kDustMarkingMinimum = 0.35f;  // the least dust of a tyre that marks the road (the same as the skid marks)
constexpr float kDustRate = 55.0f;            // particles per second of one tyre, at the most
constexpr float kFlameRate = 150.0f;          // particles per second of the afterburner
constexpr float kShadowAlpha = 0.85f;         // of the shadow of a car on the road
constexpr float kShadowFadeHeight = 3.5f;     // m (for a car of 4.3 m): the car is this far over the road or more: no shadow
constexpr float kShadowLift = 0.012f;         // m (the same): the shadow lies this far above the road, so that it does not flicker with it
constexpr float kShadowMaxDistance = 8.0f;    // m (the same): the road under the car is looked for this far down

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// A blob: the vertices first .. first + 8 of the batch: the middle of it and a ring round it (axisX and axisY are the radius of the blob along two
// directions that the blob lies between), the middle of colour (r, g, b) and the opacity `alpha`, the rim clear.
void setBlob(Batch& b, size_t first, const Vec3f& centre, const Vec3f& axisX, const Vec3f& axisY, const float colour[3], float alpha, float toUnits, int rimPoints) {
    auto put = [&](size_t at, const Vec3f& p, float a) {
        SceneVertex& v = b.verts[at];
        v.x = p.x * toUnits; v.y = p.y * toUnits; v.z = p.z * toUnits;
        v.r = (uint8_t)colour[0]; v.g = (uint8_t)colour[1]; v.b = (uint8_t)colour[2];
        v.a = (uint8_t)(std::max(0.0f, std::min(1.0f, a)) * 255.0f);
    };
    put(first, centre, alpha);
    for (int k = 0; k < rimPoints; k++) {
        const float angle = kTwoPi * (float)k / (float)rimPoints;
        put(first + 1 + (size_t)k, centre + axisX * std::cos(angle) + axisY * std::sin(angle), 0.0f);
    }
}

void clearVertices(Batch& b, size_t first, size_t count) {
    for (size_t i = 0; i < count; i++) {
        SceneVertex& v = b.verts[first + i];
        v.x = v.y = v.z = 0;
        v.a = 0;
    }
}

}  // namespace

void Effects::build(Scene& scene) {
    BatchSet set;
    set.batchFor(-1, BLEND_ALPHA, true);   // the shadow and the dust
    set.batchFor(-1, BLEND_ADD, true);     // the flame
    SceneVertex dead;
    dead.x = dead.y = dead.z = 0;
    dead.r = dead.g = dead.b = 0; dead.a = 0;
    dead.u = dead.v = 0;
    {
        Batch& b = set.batches[0];
        b.verts.assign((size_t)kShadowVertices + (size_t)kMaxDust * kBlobVertices, dead);
        for (int k = 0; k < kRimPoints; k++) {
            const uint32_t a = 1 + (uint32_t)k, c = 1 + (uint32_t)((k + 1) % kRimPoints);
            const uint32_t outerA = 1 + kRimPoints + (uint32_t)k, outerC = 1 + kRimPoints + (uint32_t)((k + 1) % kRimPoints);
            const uint32_t fan[3] = {0, a, c};
            b.idx.insert(b.idx.end(), fan, fan + 3);
            const uint32_t ring[6] = {a, outerA, outerC, a, outerC, c};
            b.idx.insert(b.idx.end(), ring, ring + 6);
        }
        for (int p = 0; p < kMaxDust; p++) {
            const uint32_t first = (uint32_t)(kShadowVertices + p * kBlobVertices);
            for (int k = 0; k < kRimPoints; k++) {
                const uint32_t tri[3] = {first, first + 1 + (uint32_t)k, first + 1 + (uint32_t)((k + 1) % kRimPoints)};
                b.idx.insert(b.idx.end(), tri, tri + 3);
            }
        }
    }
    {
        Batch& b = set.batches[1];
        b.verts.assign((size_t)kMaxFlame * kBlobVertices, dead);
        for (int p = 0; p < kMaxFlame; p++) {
            const uint32_t first = (uint32_t)(p * kBlobVertices);
            for (int k = 0; k < kRimPoints; k++) {
                const uint32_t tri[3] = {first, first + 1 + (uint32_t)k, first + 1 + (uint32_t)((k + 1) % kRimPoints)};
                b.idx.insert(b.idx.end(), tri, tri + 3);
            }
        }
    }
    scene.moverGeoms.push_back(std::move(set));
    geom_ = (int)scene.moverGeoms.size() - 1;
    Mover m;
    m.geom = geom_;
    m.cat = -1;
    m.staticModel = true;   // (the vertices are in the level's space already)
    m.hidden = true;
    m.name = "effects";
    scene.movers.push_back(m);
    mover_ = (int)scene.movers.size() - 1;
}

void Effects::clear(Scene& scene) {
    for (Particle& p : dust_) p.alive = false;
    for (Particle& p : flame_) p.alive = false;
    for (float& d : dustDebt_) d = 0;
    flameDebt_ = 0;
    if (geom_ < 0) return;
    for (Batch& b : scene.moverGeoms[geom_].batches) clearVertices(b, 0, b.verts.size());
    scene.moverGeoms[geom_].revision++;
}

void Effects::show(Scene& scene, bool on) {
    if (mover_ >= 0 && mover_ < (int)scene.movers.size()) scene.movers[mover_].hidden = !on;
}

template <size_t N>
void Effects::advance(Particle (&particles)[N], float dt, float drag, float rise) {
    for (Particle& p : particles) {
        if (!p.alive) continue;
        p.age += dt;
        if (p.age >= p.life) { p.alive = false; continue; }
        p.position += p.velocity * dt;
        p.velocity = p.velocity * std::max(0.0f, 1.0f - drag * dt);
        p.velocity.y += rise * dt;
    }
}

void Effects::emitDust(const Box3dVehicle& car, int wheelIndex, bool marking, float dt) {
    const Box3dVehicle::Wheel& wheel = car.wheel(wheelIndex);
    const float s = car.sizeScale();
    if (!wheel.grounded || wheel.load <= 0) { dustDebt_[wheelIndex] = 0; return; }
    float intensity = std::max(0.0f, std::min(1.0f, (wheel.slipSpeed - kDustSlipSpeed * s) / ((kDustFullSlipSpeed - kDustSlipSpeed) * s)));
    if (marking) intensity = std::min(1.0f, kDustMarkingMinimum + intensity * 0.8f);   // (where the tyre marks the road, the dust is there too)
    if (intensity <= 0) { dustDebt_[wheelIndex] = 0; return; }
    dustDebt_[wheelIndex] += intensity * kDustRate * dt;
    int slot = 0;
    while (dustDebt_[wheelIndex] >= 1.0f) {
        dustDebt_[wheelIndex] -= 1.0f;
        while (slot < kMaxDust && dust_[slot].alive) slot++;
        if (slot >= kMaxDust) return;
        Particle& p = dust_[slot];
        p.alive = true;
        p.age = 0;
        p.life = between(0.7f, 1.3f);
        p.position = wheel.contactPoint + Vec3f(between(-1, 1), between(0, 1), between(-1, 1)) * (0.12f * s);
        p.velocity = car.velocity() * 0.2f + Vec3f(between(-1, 1), between(0.4f, 1.4f), between(-1, 1)) * (0.8f * s);
        p.sizeStart = between(0.25f, 0.4f) * s * (0.6f + 0.4f * intensity);
        p.sizeEnd = between(1.0f, 1.7f) * s;
        p.alpha = 0.1f + 0.14f * intensity;
        const float tint = between(-12.0f, 12.0f);
        p.colourStart[0] = 205 + tint; p.colourStart[1] = 190 + tint; p.colourStart[2] = 165 + tint;
        p.colourEnd[0] = 175; p.colourEnd[1] = 165; p.colourEnd[2] = 150;
    }
}

void Effects::emitFlame(const Box3dVehicle& car, float dt) {
    const float s = car.sizeScale();
    const Vec3f lo = car.boundsMin(), hi = car.boundsMax();
    const Vec3f exhaust = car.modelMatrix().transformPoint(Vec3f(0.5f * (lo.x + hi.x), lo.y + 0.4f * (hi.y - lo.y), lo.z + 0.05f * s));
    flameDebt_ += kFlameRate * dt;
    int slot = 0;
    while (flameDebt_ >= 1.0f) {
        flameDebt_ -= 1.0f;
        while (slot < kMaxFlame && flame_[slot].alive) slot++;
        if (slot >= kMaxFlame) return;
        Particle& p = flame_[slot];
        p.alive = true;
        p.age = 0;
        p.life = between(0.18f, 0.34f);
        // the flame goes back from the car, as fast as the car and then some, a little spread out
        const float ahead = random() * dt;   // (the particles of a frame are spread over the way that the car went)
        p.position = exhaust - car.forward() * 0.0f + car.velocity() * ahead + Vec3f(between(-1, 1), between(-1, 1), between(-1, 1)) * (0.04f * s);
        p.velocity = car.velocity() - car.forward() * (between(6.0f, 10.0f) * s) + Vec3f(between(-1, 1), between(-1, 1), between(-1, 1)) * (0.7f * s);
        p.sizeStart = between(0.24f, 0.34f) * s;
        p.sizeEnd = between(0.05f, 0.1f) * s;
        p.alpha = 0.85f;
        p.colourStart[0] = 255; p.colourStart[1] = 235; p.colourStart[2] = 170;
        p.colourEnd[0] = 255; p.colourEnd[1] = 90; p.colourEnd[2] = 10;
    }
}

void Effects::update(Scene& scene, float dt, const Box3dVehicle& car, const Box3dLevel& level, const SkidMarks& skid, const Vec3f& viewForward,
                     bool afterburner, float unitsPerMetre) {
    if (geom_ < 0) return;
    const float s = car.sizeScale();
    advance(dust_, dt, 1.6f, 0.25f);
    advance(flame_, dt, 2.0f, 0.0f);
    for (int i = 0; i < Box3dVehicle::kWheelCount; i++) emitDust(car, i, skid.marking(i), dt);
    if (afterburner) emitFlame(car, dt); else flameDebt_ = 0;

    // the blobs face the camera
    Vec3f forward = viewForward;
    const float forwardLength = length(forward);
    forward = forwardLength > 1e-4f ? forward * (1.0f / forwardLength) : Vec3f(0, 0, 1);
    Vec3f right = cross(forward, Vec3f(0, 1, 0));
    const float rightLength = length(right);
    right = rightLength > 1e-4f ? right * (1.0f / rightLength) : Vec3f(1, 0, 0);
    const Vec3f up = cross(right, forward);

    Batch& alpha = alphaBatch(scene);
    Batch& add = addBatch(scene);
    for (int k = 0; k < kMaxDust; k++) {
        const size_t first = (size_t)kShadowVertices + (size_t)k * kBlobVertices;
        const Particle& p = dust_[k];
        if (!p.alive) { clearVertices(alpha, first, kBlobVertices); continue; }
        const float t = p.age / p.life;
        const float size = lerp(p.sizeStart, p.sizeEnd, std::sqrt(t));
        const float opacity = p.alpha * std::min(1.0f, p.age / 0.1f) * (1.0f - t) * (1.0f - t * 0.5f);
        const float colour[3] = {lerp(p.colourStart[0], p.colourEnd[0], t), lerp(p.colourStart[1], p.colourEnd[1], t), lerp(p.colourStart[2], p.colourEnd[2], t)};
        setBlob(alpha, first, p.position, right * size, up * size, colour, opacity, unitsPerMetre, kRimPoints);
    }
    for (int k = 0; k < kMaxFlame; k++) {
        const size_t first = (size_t)k * kBlobVertices;
        const Particle& p = flame_[k];
        if (!p.alive) { clearVertices(add, first, kBlobVertices); continue; }
        const float t = p.age / p.life;
        const float size = lerp(p.sizeStart, p.sizeEnd, t);
        const float colour[3] = {lerp(p.colourStart[0], p.colourEnd[0], t), lerp(p.colourStart[1], p.colourEnd[1], t), lerp(p.colourStart[2], p.colourEnd[2], t)};
        setBlob(add, first, p.position, right * size, up * size, colour, p.alpha * (1.0f - t * t), unitsPerMetre, kRimPoints);
    }

    // The shadow of the car: a soft oval on the road under it, as dark as the car is close to the road, lying on it (the normal of the road under
    // the centre of mass), along the car.
    clearVertices(alpha, 0, kShadowVertices);
    float distance;
    Vec3f normal;
    // (the renderer's shadow maps do the shadow of the car: this oval is only for trying it without them)
    if (kBlobShadow && !scene.softShadows && level.rayCast(car.position(), Vec3f(0, -1, 0), kShadowMaxDistance * s, distance, normal)) {
        if (normal.y < 0) normal = normal * -1.0f;
        const Vec3f ground = car.position() + Vec3f(0, -distance, 0) + normal * (kShadowLift * s);
        const float height = std::max(0.0f, distance - car.halfHeight());
        const float strength = kShadowAlpha * std::max(0.0f, 1.0f - height / (kShadowFadeHeight * s));
        Vec3f along = car.forward() - normal * dot(car.forward(), normal);
        if (length(along) < 0.2f) along = car.right() - normal * dot(car.right(), normal);
        along = normalize(along);
        const Vec3f across = cross(normal, along);
        const Vec3f lo = car.boundsMin(), hi = car.boundsMax();
        const float halfLength = 0.5f * (hi.z - lo.z) * (1.0f + 0.12f * height / s), halfWidth = 0.5f * (hi.x - lo.x) * (1.0f + 0.12f * height / s);
        if (strength > 0.01f) {
            const float black[3] = {0, 0, 0};
            auto put = [&](size_t at, const Vec3f& p, float a) {
                SceneVertex& v = alpha.verts[at];
                v.x = p.x * unitsPerMetre; v.y = p.y * unitsPerMetre; v.z = p.z * unitsPerMetre;
                v.r = v.g = v.b = (uint8_t)black[0];
                v.a = (uint8_t)(std::max(0.0f, std::min(1.0f, a)) * 255.0f);
            };
            put(0, ground, strength);
            for (int k = 0; k < kRimPoints; k++) {
                const float angle = kTwoPi * (float)k / (float)kRimPoints;
                const Vec3f direction = along * (halfLength * std::cos(angle)) + across * (halfWidth * std::sin(angle));
                put(1 + (size_t)k, ground + direction * 0.8f, strength * 0.9f);
                put(1 + (size_t)kRimPoints + (size_t)k, ground + direction * 1.25f, 0.0f);
            }
        }
    }
    scene.moverGeoms[geom_].revision++;
}

}  // namespace minigame01
