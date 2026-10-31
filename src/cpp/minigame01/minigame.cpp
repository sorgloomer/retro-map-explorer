#include "minigame01/minigame.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace minigame01 {

namespace {

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

bool MiniGame::prepare(Scene& scene, const CarModel& model, std::string& error) {
    car = model;
    scale = std::max(1e-3f, car.unitsPerMetre);
    // the game's own collision meshes when it has them (read now: the viewer alone never needs them), else the visible triangles
    const bool haveMeshes = scene.ensureCollision();
    if (scene.collisionRequired && (!haveMeshes || !scene.collisionErrors.empty())) {
        // without them the car would fall through the level: do not start
        if (scene.collisionErrors.empty()) {
            error = "this level has no collision mesh";
        } else {
            error = "the collision mesh could not be loaded (";
            for (size_t i = 0; i < scene.collisionErrors.size(); i++) error += (i ? ", " : "") + scene.collisionErrors[i];
            error += ")";
        }
        return false;
    }
    if (haveMeshes) world.build(scene.collisionMeshes, 1.0f / scale);
    else world.build(scene, 1.0f / scale, !scene.cullBackfaces);
    if (world.empty()) {
        error = "this level has no solid ground to drive on";
        return false;
    }
    vehicle.init(car);
    int g0 = (int)scene.moverGeoms.size();
    scene.moverGeoms.push_back(car.body);
    for (int i = 0; i < 4; i++) scene.moverGeoms.push_back(car.wheel[i]);
    for (int i = g0; i < (int)scene.moverGeoms.size(); i++) scene.moverGeoms[i].lit = true;  // the car is shaded by the level's light
    auto addMover = [&](int geom) {
        Mover m;
        m.geom = geom;
        m.cat = -1;
        m.external = true;
        m.lit = true;
        m.hidden = true;
        m.name = "minigame car";
        scene.movers.push_back(m);
        return (int)scene.movers.size() - 1;
    };
    carMover[0] = addMover(g0);
    for (int i = 0; i < 4; i++) carMover[1 + i] = addMover(g0 + 1 + i);
    // the debug arrows of the forces: a long pointed shape along +z (length 1), unshaded and seen through everything. Yellow for
    // the wheels, then blue gravity, red air drag, cyan downforce, orange afterburner, grey what the level pushes the hull with.
    {
        auto makeArrow = [&](uint8_t r, uint8_t g, uint8_t b) {
            auto vertex = [&](float x, float y, float z) {
                SceneVertex v;
                v.x = x; v.y = y; v.z = z;
                v.r = r; v.g = g; v.b = b; v.a = 255;
                v.u = v.v = 0;
                return v;
            };
            // a thin square shaft from z = 0 to 0.95 and a pointed head from there to the tip at z = 1: the head is 5% of the length
            const float shaft = 0.15f, head = 0.5f;
            const SceneVertex corners[13] = {
                vertex(shaft, shaft, 0), vertex(-shaft, shaft, 0), vertex(-shaft, -shaft, 0), vertex(shaft, -shaft, 0),
                vertex(shaft, shaft, 0.95f), vertex(-shaft, shaft, 0.95f), vertex(-shaft, -shaft, 0.95f), vertex(shaft, -shaft, 0.95f),
                vertex(head, 0, 0.95f), vertex(0, head, 0.95f), vertex(-head, 0, 0.95f), vertex(0, -head, 0.95f), vertex(0, 0, 1)};
            std::vector<uint32_t> indices;
            for (uint32_t i = 0; i < 4; i++) {
                const uint32_t j = (i + 1) % 4;
                indices.insert(indices.end(), {i, j, 4 + j, i, 4 + j, 4 + i});   // a side of the shaft
                indices.insert(indices.end(), {8 + i, 8 + j, 12});               // a side of the head
            }
            indices.insert(indices.end(), {0, 1, 2, 0, 2, 3, 8, 9, 10, 8, 10, 11});  // the tail, and the underside of the head
            BatchSet arrow;
            arrow.appendIndexed(-1, BLEND_OPAQUE, corners, 13, indices.data(), indices.size(), true);
            scene.moverGeoms.push_back(arrow);
            return (int)scene.moverGeoms.size() - 1;
        };
        const uint8_t colours[10][3] = {{255, 220, 60}, {255, 220, 60}, {255, 220, 60}, {255, 220, 60}, {70, 120, 255},
                                        {255, 70, 70}, {60, 230, 230}, {255, 150, 30}, {190, 190, 190}, {0, 0, 0}};
        int geomOf[9];
        for (int i = 0; i < 9; i++) geomOf[i] = i == 0 || i >= 4 ? makeArrow(colours[i][0], colours[i][1], colours[i][2]) : -1;
        for (int i = 0; i < 8; i++) {
            carMover[5 + i] = addMover(geomOf[i < 4 ? 0 : i]);
            scene.movers[carMover[5 + i]].lit = false;
            scene.movers[carMover[5 + i]].alwaysOnTop = true;
        }
        hullArrowGeom = geomOf[8];  // grey: one arrow per force the level puts on the hull, as many as there are hull spheres
        hullArrowMovers.clear();
        for (int i = 0; i < vehicle.probeCount(); i++) {
            hullArrowMovers.push_back(addMover(hullArrowGeom));
            scene.movers[hullArrowMovers.back()].lit = false;
            scene.movers[hullArrowMovers.back()].alwaysOnTop = true;
        }
    }
    // the debug spheres of the hull: a unit sphere, green or red (translucent), one mover per collision sphere
    {
        auto makeSphere = [&](uint8_t r, uint8_t g, uint8_t b) {
            std::vector<Vec3f> pts = {Vec3f(1, 0, 0), Vec3f(-1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, -1, 0), Vec3f(0, 0, 1), Vec3f(0, 0, -1)};
            const uint32_t tris[24] = {2, 0, 4, 2, 4, 1, 2, 1, 5, 2, 5, 0, 3, 4, 0, 3, 1, 4, 3, 5, 1, 3, 0, 5};
            std::vector<uint32_t> fine;
            for (size_t t = 0; t < 24; t += 3) {  // one round of subdivision, the new points pushed out to the sphere
                uint32_t v[3] = {tris[t], tris[t + 1], tris[t + 2]}, m[3];
                for (int k = 0; k < 3; k++) {
                    pts.push_back(normalize(pts[v[k]] + pts[v[(k + 1) % 3]]));
                    m[k] = (uint32_t)pts.size() - 1;
                }
                const uint32_t quad[12] = {v[0], m[0], m[2], m[0], v[1], m[1], m[2], m[1], v[2], m[0], m[1], m[2]};
                fine.insert(fine.end(), quad, quad + 12);
            }
            std::vector<SceneVertex> verts;
            for (const Vec3f& p : pts) {
                SceneVertex v;
                v.x = p.x; v.y = p.y; v.z = p.z;
                v.r = r; v.g = g; v.b = b; v.a = 90;
                v.u = v.v = 0;
                verts.push_back(v);
            }
            BatchSet sphere;
            sphere.appendIndexed(-1, BLEND_ALPHA, verts.data(), verts.size(), fine.data(), fine.size(), true);
            scene.moverGeoms.push_back(sphere);
            return (int)scene.moverGeoms.size() - 1;
        };
        sphereGeoms[0] = makeSphere(60, 220, 90);
        sphereGeoms[1] = makeSphere(255, 60, 60);
        sphereMovers.clear();
        for (int i = 0; i < vehicle.probeCount(); i++) {
            sphereMovers.push_back(addMover(sphereGeoms[0]));
            scene.movers[sphereMovers.back()].lit = false;
            scene.movers[sphereMovers.back()].alwaysOnTop = true;
        }
    }
    worldFloor = scene.bmin.y / scale - tuning().worldFloorBelow;
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
    sphereMovers.clear();
    hullArrowMovers.clear();
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
    const float cameraDropScale = carSettings().cameraDropScale;  // the heights the ground is looked for from follow the size of the car
    if (groundBelow(p + Vec3f(0, 3 * cameraDropScale, 0), g) || groundBelow(p + Vec3f(0, 60 * cameraDropScale, 0), g)) startPos = g;
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
    const Params& P = tuning();
    hud_.timeLeft = P.roundSeconds;
    if (freeDrive) hud_.state = 3;
    pickups.clear();
    for (size_t i = 0; i < scene.movers.size(); i++) {
        Mover& m = scene.movers[i];
        if (!m.powerupMesh || m.path.empty() || m.cat < 0) continue;
        if (m.billboard) continue;
        Pickup p;
        p.pos = m.path[0] * (1.0f / scale);
        const std::string& id = scene.cats[m.cat].id;
        p.points = (int)P.pointsPowerup;
        if (id == "notable" || id == "missionitem") { p.points = (int)P.pointsNotable; p.label = id == "notable" ? "Notable powerup" : "Mission item"; }
        else if (id == "timepowerup") { p.points = (int)P.pointsTimeBonus; p.seconds = P.timeBonusSeconds; p.label = "Time bonus"; }
        else if (id == "upgrade") { p.points = (int)P.pointsUpgrade; p.label = "Upgrade"; }
        else if (id == "credit") { p.points = (int)P.pointsCredit; p.label = "Credits"; }
        else if (id == "pedpowerup") { p.points = (int)P.pointsPowerup; p.label = "Pedestrian powerup"; }
        else if (id == "pickup") { p.points = (int)P.pointsPickup; p.label = "Pickup"; }
        else if (id == "star") { p.points = (int)P.pointsStar; p.label = "Star"; }
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
    for (int m : sphereMovers)
        if (m >= 0 && m < (int)scene.movers.size()) scene.movers[m].hidden = true;
    for (int m : hullArrowMovers)
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
    safeTimer = 0;
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
        if (groundBelow(vehicle.position() + Vec3f(0, 1.5f * carSettings().cameraDropScale, 0), below) && vehicle.position().y > worldFloor) {
            g = below;
            yaw = std::atan2(vehicle.forward().x, vehicle.forward().z);
        }
    }
    placeCar(g, yaw);
}

void MiniGame::collect(Scene& scene) {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    Vec3f c = vehicle.position();
    for (Pickup& p : pickups) {
        if (p.taken) continue;
        float dx = p.pos.x - c.x, dz = p.pos.z - c.z, dy = p.pos.y - c.y;
        if (dx * dx + dz * dz > C.pickupRadius * C.pickupRadius || std::fabs(dy) > C.pickupReachY) continue;  // the reach of a car follows its size
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
        int bonus = (int)(hud_.timeLeft * P.finishBonusPerSecond);
        hud_.score += bonus;
        char buf[64];
        snprintf(buf, sizeof buf, "All collected!  time bonus +%d", bonus);
        setMessage(hud_, buf);
    }
}

void MiniGame::orbitCamera(float dYaw, float dPitch) {
    orbitYaw = wrapAngle(orbitYaw + dYaw);
    orbitPitch = std::max(-0.6f, std::min(1.2f, orbitPitch + dPitch));
}

void MiniGame::zoomCamera(float factor) { zoomScale = std::max(0.25f, std::min(6.0f, zoomScale * factor)); }

void MiniGame::updateCamera(float dt, bool snap) {
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    Vec3f pos = vehicle.position();
    Vec3f fwd = vehicle.forward();
    float carYaw = std::atan2(fwd.x, fwd.z);
    Vec3f vel = vehicle.velocity();
    float flat = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    float targetYaw = carYaw;
    if (flat > C.cameraSlideMinSpeed && vehicle.forwardSpeed() > 0) {  // look where the car is going when it slides
        float velYaw = std::atan2(vel.x, vel.z);
        targetYaw = carYaw + wrapAngle(velYaw - carYaw) * P.cameraSlideBlend;
    }
    float dist = (camMode == 0 ? C.cameraDistance : C.cameraFarDistance) + std::min(flat, C.cameraSpeedCap) * P.cameraSpeedDistance;
    float height = camMode == 0 ? C.cameraHeight : C.cameraFarHeight;
    if (snap) camYaw = targetYaw;
    else camYaw += wrapAngle(targetYaw - camYaw) * std::min(1.0f, dt * P.cameraYawFollow);
    Vec3f lookAt = pos + Vec3f(0, C.cameraLookHeight, 0) + Vec3f(std::sin(camYaw), 0, std::cos(camYaw)) * C.cameraLookAhead;
    // the player's turn and zoom: the camera moves on a sphere around the car's chase point
    const float elevation = std::max(-0.1f, std::min(1.45f, std::atan2(height, dist) + orbitPitch));
    const float radius = std::sqrt(dist * dist + height * height) * zoomScale;
    dist = radius * std::cos(elevation);
    height = radius * std::sin(elevation);
    const float viewYaw = camYaw + orbitYaw;
    Vec3f desired = pos + Vec3f(0, height, 0) - Vec3f(std::sin(viewYaw), 0, std::cos(viewYaw)) * dist;
    if (snap) camPos = desired;
    else camPos += (desired - camPos) * std::min(1.0f, dt * P.cameraPositionFollow);
    // keep the camera out of walls: pull it in along the line to the car
    Vec3f toCam = camPos - lookAt;
    float len = length(toCam);
    if (len > C.cameraWallCheckLength) {
        Vec3f dir = toCam * (1.0f / len);
        float t;
        Vec3f n;
        if (world.raycast(lookAt, dir, len + C.cameraWallMargin, true, t, n)) camPos = lookAt + dir * std::max(C.cameraMinDistance, t - C.cameraWallMargin);
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
    // the hull's collision spheres: at their centres in the car, red when they touched the level during the last frame
    for (size_t i = 0; i < sphereMovers.size(); i++) {
        Mover& m = scene.movers[sphereMovers[i]];
        m.hidden = !showSpheres_ || (int)i >= vehicle.probeCount();
        if (m.hidden) continue;
        const float r = vehicle.probeRadius((int)i);
        const Vec3f c = vehicle.probeCentre((int)i);
        Mat4 S = Mat4::identity();
        S.m[0] = r; S.m[5] = r; S.m[10] = r;
        S.m[12] = c.x; S.m[13] = c.y; S.m[14] = c.z;
        m.geom = sphereGeoms[vehicle.probeTouched((int)i) ? 1 : 0];
        m.model = toLevel(Mat4::mul(M, S));
    }
    // the force arrows: from the contact point along the force, one car weight per tuning length (params), thin
    const CarSettings& C = carSettings();
    // the four wheels at their contact points, then the forces on the whole car at its centre of mass
    struct Arrow { Vec3f from, force; bool on; };
    const Vec3f centre = vehicle.position();
    std::vector<Arrow> arrows(8);
    std::vector<int> arrowMover(8);
    for (int i = 0; i < 8; i++) arrowMover[i] = carMover[5 + i];
    for (int i = 0; i < 4; i++) arrows[i] = {vehicle.wheel(i).contactPoint, vehicle.wheel(i).force, vehicle.wheel(i).grounded};
    arrows[4] = {centre, vehicle.gravityForce(), true};
    arrows[5] = {centre, vehicle.dragForce(), true};
    arrows[6] = {centre, vehicle.downforce(), true};
    arrows[7] = {centre, vehicle.thrustForce(), true};
    // one grey arrow per force of the level on the hull, from the point of the sphere that touched it
    for (int i = 0; i < vehicle.probeCount() && i < (int)hullArrowMovers.size(); i++) {
        arrows.push_back({vehicle.probeContactPoint(i), vehicle.probeForce(i), vehicle.probeTouched(i)});
        arrowMover.push_back(hullArrowMovers[i]);
    }
    for (size_t i = 0; i < arrows.size(); i++) {
        Mover& m = scene.movers[arrowMover[i]];
        const float magnitude = length(arrows[i].force);
        m.hidden = !showForces_ || !arrows[i].on || magnitude < 1.0f;
        if (m.hidden) continue;
        const Vec3f dir = arrows[i].force * (1.0f / magnitude);
        const Vec3f helper = std::fabs(dir.y) < 0.9f ? Vec3f(0, 1, 0) : Vec3f(1, 0, 0);
        const Vec3f side = normalize(cross(helper, dir)), up = cross(dir, side);
        const float arrowLength = magnitude / (vehicle.weight()) * C.forceArrowLength;
        const float thickness = std::max(0.1f * arrowLength, 0.04f * vehicle.sizeScale());
        Mat4 A = Mat4::identity();
        A.m[0] = side.x * thickness; A.m[1] = side.y * thickness; A.m[2] = side.z * thickness;
        A.m[4] = up.x * thickness;   A.m[5] = up.y * thickness;   A.m[6] = up.z * thickness;
        A.m[8] = dir.x * arrowLength; A.m[9] = dir.y * arrowLength; A.m[10] = dir.z * arrowLength;
        A.m[12] = arrows[i].from.x; A.m[13] = arrows[i].from.y; A.m[14] = arrows[i].from.z;
        m.model = toLevel(A);
    }
}

void MiniGame::update(float dt, const Controls& c, Scene& scene) {
    if (!running || !isPrepared) return;
    const Params& P = tuning();
    const CarSettings& C = carSettings();
    dt = std::min(dt, P.maxFrameSeconds);
    if (c.restart) { newRound(scene); syncMovers(scene); return; }
    if (c.reset) respawn(scene, Spawn::Here);
    if (c.flip) vehicle.liftAndRoll(1.0f);
    if (c.toggleCamera) { camMode = 1 - camMode; orbitYaw = orbitPitch = 0; zoomScale = 1; }

    VehicleInput vi;
    vi.accel = c.accel;
    vi.brake = c.brake;
    vi.steer = c.steer;
    vi.handbrake = c.handbrake;
    vi.afterburner = c.afterburner;
    vi.sideThrust = c.sideThrust;
    vi.jump = c.kangaroo;
    vehicle.step(dt, vi, world);

    // last safe spot (wheels down, upright), for falling out of the world
    safeTimer += dt;
    if (safeTimer > P.safeSpotSeconds && vehicle.anyGrounded() && !vehicle.upsideDown()) {
        Vec3f g;
        if (groundBelow(vehicle.position() + Vec3f(0, 1.0f * C.cameraDropScale, 0), g)) {
            safePos = g;
            safeYaw = std::atan2(vehicle.forward().x, vehicle.forward().z);
            safeTimer = 0;
        }
    }
    if (vehicle.position().y < worldFloor) {  // fell out of the world
        respawn(scene, Spawn::Safe);
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
        driftAccum += vehicle.drift() * vehicle.speed() * dt * P.driftPointsFactor;
        int whole = (int)driftAccum;
        if (whole > 0) { driftAccum -= (float)whole; hud_.driftScore += whole; hud_.score += whole; }
    }
    hud_.speedKmh = vehicle.worldSpeed() * 3.6f;  // in the world's metres, not the car's
    hud_.messageAge += dt;
    syncMovers(scene);
    updateCamera(dt, false);
}

}  // namespace minigame01
