#include "minigame01/minigame.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace minigame01 {

namespace {

constexpr float kRoundSeconds = 90.0f;
constexpr float kPickupRadius = 3.4f;
constexpr float kTwoPi = 6.28318531f;

float wrapAngle(float a) {
    while (a > 3.14159265f) a -= kTwoPi;
    while (a < -3.14159265f) a += kTwoPi;
    return a;
}

void setMessage(Hud& h, const char* text) {
    snprintf(h.message, sizeof h.message, "%s", text);
    h.messageAge = 0;
}

}  // namespace

bool MiniGame::prepare(Scene& scene, const CarModel& model) {
    car = model;
    scale = std::max(1e-3f, car.unitsPerMetre);
    world.build(scene, 1.0f / scale, !scene.cullBackfaces);
    if (world.empty()) return false;
    vehicle.init(car);
    int g0 = (int)scene.moverGeoms.size();
    scene.moverGeoms.push_back(car.body);
    for (int i = 0; i < 4; i++) scene.moverGeoms.push_back(car.wheel[i]);
    auto addMover = [&](int geom) {
        Mover m;
        m.geom = geom;
        m.cat = -1;
        m.external = true;
        m.hidden = true;
        m.name = "minigame car";
        scene.movers.push_back(m);
        return (int)scene.movers.size() - 1;
    };
    carMover[0] = addMover(g0);
    for (int i = 0; i < 4; i++) carMover[1 + i] = addMover(g0 + 1 + i);
    worldFloor = scene.bmin.y / scale - 60.0f;
    makePickupIcons(scene);
    isPrepared = true;
    return true;
}

void MiniGame::forget() {
    isPrepared = false;
    running = false;
    pickups.clear();
    iconCats.clear();
    for (int& m : carMover) m = -1;
    input.releaseAll();
}

bool MiniGame::groundBelow(const Vec3f& from, Vec3f& hit) const {
    float t;
    Vec3f n;
    if (!world.raycast(from, Vec3f(0, -1, 0), 120.0f, false, t, n)) return false;
    hit = from + Vec3f(0, -t, 0);
    return true;
}

void MiniGame::start(Scene& scene, const Vec3f& sceneCenter, bool free) {
    freeDrive = free;
    Vec3f p = sceneCenter * (1.0f / scale);
    float yaw = 0;
    for (const Marker& m : scene.markers)
        if (scene.cats[m.cat].id == "start") { p = m.pos * (1.0f / scale); yaw = m.yaw; break; }
    Vec3f g;
    if (groundBelow(p + Vec3f(0, 3, 0), g) || groundBelow(p + Vec3f(0, 60, 0), g)) startPos = g;
    else startPos = p;
    startYaw = yaw;
    running = true;
    input.releaseAll();
    iconCatShown.clear();
    for (int c : iconCats) {  // the gems stand in for these markers
        iconCatShown.push_back(scene.cats[c].visible);
        scene.cats[c].visible = false;
    }
    camMode = 0;
    newRound(scene);
    syncMovers(scene);
}

void MiniGame::newRound(Scene& scene) {
    hud_ = Hud();
    hud_.timeLeft = kRoundSeconds;
    if (freeDrive) hud_.state = 3;
    pickups.clear();
    for (size_t i = 0; i < scene.movers.size(); i++) {
        Mover& m = scene.movers[i];
        if (!m.powerupMesh || m.path.empty() || m.cat < 0) continue;
        if (m.billboard) continue;
        Pickup p;
        p.pos = m.path[0] * (1.0f / scale);
        const std::string& id = scene.cats[m.cat].id;
        p.points = 50;
        if (id == "notable" || id == "missionitem") { p.points = 150; p.label = id == "notable" ? "Notable powerup" : "Mission item"; }
        else if (id == "timepowerup") { p.points = 50; p.seconds = 15; p.label = "Time bonus"; }
        else if (id == "upgrade") { p.points = 500; p.label = "Upgrade"; }
        else if (id == "credit") { p.points = 300; p.label = "Credits"; }
        else if (id == "pedpowerup") { p.points = 50; p.label = "Pedestrian powerup"; }
        else if (id == "pickup") { p.points = 100; p.label = "Pickup"; }
        else if (id == "star") { p.points = 250; p.label = "Star"; }
        p.movers.push_back((int)i);
        pickups.push_back(p);
    }
    for (size_t i = 0; i < scene.movers.size(); i++) {  // the halos belong to the icon at the same spot
        Mover& m = scene.movers[i];
        if (!m.powerupMesh || !m.billboard || m.path.empty()) continue;
        for (Pickup& p : pickups)
            if (length(p.pos - m.path[0] * (1.0f / scale)) < 0.01f) { p.movers.push_back((int)i); break; }
    }
    for (Pickup& p : pickups)
        for (int mi : p.movers) scene.movers[mi].hidden = false;
    hud_.total = (int)pickups.size();
    driftAccum = 0;
    respawn(scene, Spawn::Start);
    setMessage(hud_, pickups.empty() ? "No powerups here - just drive!" : freeDrive ? "Free drive - no clock, no rules" : "Collect the powerups!");
}

void MiniGame::makePickupIcons(Scene& scene) {
    for (const Mover& m : scene.movers)
        if (m.powerupMesh && !m.billboard) return;  // the level has its own pickup meshes
    iconCats.clear();
    const float r = 0.9f * scale;
    for (size_t c = 0; c < scene.cats.size(); c++) {
        const std::string& id = scene.cats[c].id;
        if (id != "pickup" && id != "star") continue;
        std::vector<Vec3f> at;
        for (const Marker& m : scene.markers)
            if (m.cat == (int)c) at.push_back(m.pos);
        if (at.empty()) continue;
        // a gem: an octahedron in the colour of the category
        auto col = Color3f::from3b(scene.cats[c].color);
        auto vtx = [&](float x, float y, float z, float shade) {
            return SceneVertex(Vec3f(x, y, z), Color4b::from1b3f(0xff, col * shade), Vec2f::zero());
        };
        SceneVertex gv[6] = {vtx(r, 0, 0, 0.8f), vtx(-r, 0, 0, 0.8f), vtx(0, r * 1.5f, 0, 1.0f), vtx(0, -r * 1.5f, 0, 0.55f), vtx(0, 0, r, 0.9f), vtx(0, 0, -r, 0.9f)};
        const uint32_t gi[24] = {2, 0, 4, 2, 4, 1, 2, 1, 5, 2, 5, 0, 3, 4, 0, 3, 1, 4, 3, 5, 1, 3, 0, 5};
        BatchSet gem;
        gem.appendIndexed(-1, BLEND_OPAQUE, gv, 6, gi, 24, true);
        scene.moverGeoms.push_back(gem);
        int geom = (int)scene.moverGeoms.size() - 1;
        for (size_t i = 0; i < at.size(); i++) {
            Mover mv;
            mv.geom = geom;
            mv.cat = (int)c;
            mv.fixed = true;
            mv.powerupMesh = true;
            mv.hidden = true;
            mv.phase = (float)(i % 7);
            mv.path = {at[i] + Vec3f(0, r * 1.4f, 0)};
            scene.movers.push_back(mv);
        }
        iconCats.push_back((int)c);
    }
}

void MiniGame::stop(Scene& scene) {
    running = false;
    input.releaseAll();
    for (size_t i = 0; i < iconCats.size() && i < iconCatShown.size(); i++) scene.cats[iconCats[i]].visible = iconCatShown[i] != 0;
    for (int m : carMover)
        if (m >= 0 && m < (int)scene.movers.size()) scene.movers[m].hidden = true;
    for (Pickup& p : pickups)
        for (int mi : p.movers) scene.movers[mi].hidden = false;
    pickups.clear();
}

void MiniGame::placeCar(const Vec3f& groundPos, float yaw) {
    vehicle.reset(groundPos, yaw);
    camYaw = yaw;
    safePos = groundPos;
    safeYaw = yaw;
    safeTimer = stuckTimer = 0;
    updateCamera(0, true);
}

void MiniGame::respawn(Scene& scene, Spawn where) {
    (void)scene;
    if (where == Spawn::Start) {
        placeCar(startPos, startYaw);
        return;
    }
    Vec3f g = safePos;
    float yaw = safeYaw;
    if (where == Spawn::Here) {  // upright on the ground below, heading as before
        Vec3f below;
        if (groundBelow(vehicle.position() + Vec3f(0, 1.5f, 0), below) && vehicle.position().y > worldFloor) {
            g = below;
            yaw = std::atan2(vehicle.forward().x, vehicle.forward().z);
        }
    }
    placeCar(g, yaw);
}

void MiniGame::collect(Scene& scene) {
    Vec3f c = vehicle.position();
    for (Pickup& p : pickups) {
        if (p.taken) continue;
        float dx = p.pos.x - c.x, dz = p.pos.z - c.z, dy = p.pos.y - c.y;
        if (dx * dx + dz * dz > kPickupRadius * kPickupRadius || std::fabs(dy) > 3.4f) continue;
        p.taken = true;
        for (int mi : p.movers) scene.movers[mi].hidden = true;
        hud_.score += p.points;
        hud_.timeLeft += p.seconds;
        hud_.collected++;
        char buf[64];
        if (p.seconds > 0) snprintf(buf, sizeof buf, "%s  +%d s  +%d", p.label, (int)p.seconds, p.points);
        else snprintf(buf, sizeof buf, "%s  +%d", p.label, p.points);
        setMessage(hud_, buf);
    }
    if (hud_.state == 0 && hud_.total > 0 && hud_.collected == hud_.total) {
        hud_.state = 2;
        int bonus = (int)(hud_.timeLeft * 10.0f);
        hud_.score += bonus;
        char buf[64];
        snprintf(buf, sizeof buf, "All collected!  time bonus +%d", bonus);
        setMessage(hud_, buf);
    }
}

void MiniGame::updateCamera(float dt, bool snap) {
    Vec3f pos = vehicle.position();
    Vec3f fwd = vehicle.forward();
    float carYaw = std::atan2(fwd.x, fwd.z);
    Vec3f vel = vehicle.velocity();
    float flat = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    float targetYaw = carYaw;
    if (flat > 5.0f && vehicle.forwardSpeed() > 0) {  // look where the car is going when it slides
        float velYaw = std::atan2(vel.x, vel.z);
        targetYaw = carYaw + wrapAngle(velYaw - carYaw) * 0.6f;
    }
    float dist = (camMode == 0 ? 6.4f : 11.0f) + std::min(flat, 40.0f) * 0.035f;
    float height = camMode == 0 ? 2.5f : 5.0f;
    if (snap) camYaw = targetYaw;
    else camYaw += wrapAngle(targetYaw - camYaw) * std::min(1.0f, dt * 3.2f);
    Vec3f lookAt = pos + Vec3f(0, 0.9f, 0) + Vec3f(std::sin(camYaw), 0, std::cos(camYaw)) * 2.2f;
    Vec3f desired = pos + Vec3f(0, height, 0) - Vec3f(std::sin(camYaw), 0, std::cos(camYaw)) * dist;
    if (snap) camPos = desired;
    else camPos += (desired - camPos) * std::min(1.0f, dt * 7.0f);
    // keep the camera out of walls: pull it in along the line to the car
    Vec3f toCam = camPos - lookAt;
    float len = length(toCam);
    if (len > 0.5f) {
        Vec3f dir = toCam * (1.0f / len);
        float t;
        Vec3f n;
        if (world.raycast(lookAt, dir, len + 0.4f, true, t, n)) camPos = lookAt + dir * std::max(0.6f, t - 0.4f);
    }
    Vec3f look = lookAt - camPos;
    cam.pos = camPos * scale;
    cam.yaw = std::atan2(look.x, look.z);
    cam.pitch = std::asin(std::max(-1.0f, std::min(1.0f, look.y / std::max(1e-3f, length(look)))));
}

void MiniGame::syncMovers(Scene& scene) {
    Mat4 M = vehicle.modelMatrix();
    // the car is in metres, the level in its own units: scale the matrices (axes and translation) up
    auto toLevel = [&](Mat4 m) {
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) m.m[i] *= scale;
        return m;
    };
    scene.movers[carMover[0]].model = toLevel(M);
    scene.movers[carMover[0]].hidden = false;
    for (int i = 0; i < 4; i++) {
        const Vehicle::Wheel& w = vehicle.wheel(i);
        float cs = std::cos(w.steer), sn = std::sin(w.steer);
        float spin = std::fmod(w.spin, kTwoPi);
        float cp = std::cos(spin), sp = std::sin(spin);
        Vec3f ex(cs, 0, -sn), ez0(sn, 0, cs), ey(0, 1, 0);
        Vec3f eY = ey * cp + ez0 * sp, eZ = ez0 * cp - ey * sp;
        Mat4 L = Mat4::identity();
        L.m[0] = ex.x; L.m[1] = ex.y; L.m[2] = ex.z;
        L.m[4] = eY.x; L.m[5] = eY.y; L.m[6] = eY.z;
        L.m[8] = eZ.x; L.m[9] = eZ.y; L.m[10] = eZ.z;
        L.m[12] = w.centre.x; L.m[13] = w.centre.y; L.m[14] = w.centre.z;
        Mover& m = scene.movers[carMover[1 + i]];
        m.model = toLevel(Mat4::mul(M, L));
        m.hidden = false;
    }
}

void MiniGame::update(float dt, const Controls& c, Scene& scene) {
    if (!running || !isPrepared) return;
    dt = std::min(dt, 0.05f);
    if (c.restart) { newRound(scene); syncMovers(scene); return; }
    if (c.reset) respawn(scene, Spawn::Here);
    if (c.toggleCamera) camMode = 1 - camMode;

    VehicleInput vi;
    vi.accel = c.accel;
    vi.brake = c.brake;
    vi.steer = c.steer;
    vi.handbrake = c.handbrake;
    vehicle.step(dt, vi, world);

    // last safe spot (wheels down, upright), for falling out of the world
    safeTimer += dt;
    if (safeTimer > 0.7f && vehicle.anyGrounded() && !vehicle.upsideDown()) {
        Vec3f g;
        if (groundBelow(vehicle.position() + Vec3f(0, 1.0f, 0), g)) {
            safePos = g;
            safeYaw = std::atan2(vehicle.forward().x, vehicle.forward().z);
            safeTimer = 0;
        }
    }
    if (vehicle.upsideDown() && vehicle.speed() < 2.0f) stuckTimer += dt;
    else stuckTimer = 0;
    if (vehicle.position().y < worldFloor) {  // fell out of the world
        respawn(scene, Spawn::Safe);
        setMessage(hud_, "Back on the road");
    } else if (stuckTimer > 2.5f) {
        respawn(scene, Spawn::Here);
        setMessage(hud_, "Back on the road");
    }

    if (hud_.state == 0) {
        hud_.timeLeft -= dt;
        if (hud_.timeLeft <= 0) {
            hud_.timeLeft = 0;
            hud_.state = 1;
            setMessage(hud_, "Time is up!");
        }
    }
    collect(scene);

    // drift points: sliding sideways at speed
    if (vehicle.anyGrounded() && (hud_.state == 0 || hud_.state == 3)) {
        driftAccum += vehicle.drift() * vehicle.speed() * dt * 0.5f;
        int whole = (int)driftAccum;
        if (whole > 0) { driftAccum -= (float)whole; hud_.driftScore += whole; hud_.score += whole; }
    }
    hud_.speedKmh = vehicle.speed() * 3.6f;
    hud_.messageAge += dt;
    syncMovers(scene);
    updateCamera(dt, false);
}

}  // namespace minigame01
