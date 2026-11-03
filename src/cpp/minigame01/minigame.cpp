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
#ifndef RME_PHYSICS_LEGACY
    if (!physicsLevel.build(scene, scale, !scene.cullBackfaces)) {
        error = "the physics engine could not use this level";
        return false;
    }
    vehicle.attach(physicsLevel);
#endif
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
                                        {255, 70, 70}, {60, 230, 230}, {255, 150, 30}, {190, 190, 190}, {90, 235, 110}};
        int geomOf[10];
        for (int i = 0; i < 10; i++) geomOf[i] = i == 0 || i >= 4 ? makeArrow(colours[i][0], colours[i][1], colours[i][2]) : -1;
        for (int i = 0; i < 8; i++) {
            carMover[5 + i] = addMover(geomOf[i < 4 ? 0 : i]);
            scene.movers[carMover[5 + i]].lit = false;
            scene.movers[carMover[5 + i]].alwaysOnTop = true;
        }
        for (int i = 0; i < 4; i++) {  // green: the tyres' forces, where the physics applies them
            carMover[13 + i] = addMover(geomOf[9]);
            scene.movers[carMover[13 + i]].lit = false;
            scene.movers[carMover[13 + i]].alwaysOnTop = true;
        }
        hullArrowGeom = geomOf[8];  // grey: one arrow per force the level puts on the hull, as many as there are hull spheres
        hullArrowMovers.clear();
        for (int i = 0; i < vehicle.hullArrowCount(); i++) {
            hullArrowMovers.push_back(addMover(hullArrowGeom));
            scene.movers[hullArrowMovers.back()].lit = false;
            scene.movers[hullArrowMovers.back()].alwaysOnTop = true;
        }
    }
    // the debug spheres of the hull: a unit sphere, green or red (translucent), one mover per collision sphere
    {
        auto makeSphere = [&](uint8_t r, uint8_t g, uint8_t b) {
            // a sphere of 8 segments around and 5 bands from pole to pole (64 triangles), poles on the y axis
            constexpr int kSegments = 8, kBands = 5;
            std::vector<Vec3f> pts;
            pts.push_back(Vec3f(0, 1, 0));
            for (int band = 1; band < kBands; band++) {
                const float polar = 3.14159265f * (float)band / (float)kBands;
                for (int segment = 0; segment < kSegments; segment++) {
                    const float around = 6.28318531f * (float)segment / (float)kSegments;
                    pts.push_back(Vec3f(std::sin(polar) * std::cos(around), std::cos(polar), std::sin(polar) * std::sin(around)));
                }
            }
            pts.push_back(Vec3f(0, -1, 0));
            const uint32_t south = (uint32_t)pts.size() - 1;
            auto ring = [&](int band, int segment) { return (uint32_t)(1 + (band - 1) * kSegments + (segment % kSegments)); };
            std::vector<uint32_t> fine;
            for (int segment = 0; segment < kSegments; segment++) {
                fine.insert(fine.end(), {0u, ring(1, segment), ring(1, segment + 1)});                                 // the cap at the top
                for (int band = 1; band < kBands - 1; band++) {                                                        // the bands between
                    fine.insert(fine.end(), {ring(band, segment), ring(band + 1, segment), ring(band + 1, segment + 1)});
                    fine.insert(fine.end(), {ring(band, segment), ring(band + 1, segment + 1), ring(band, segment + 1)});
                }
                fine.insert(fine.end(), {ring(kBands - 1, segment), south, ring(kBands - 1, segment + 1)});            // the cap at the bottom
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
            sphere.lit = true;  // shaded by the level's light; the renderer derives the smooth normals
            sphere.appendIndexed(-1, BLEND_ALPHA, verts.data(), verts.size(), fine.data(), fine.size(), true);
            scene.moverGeoms.push_back(sphere);
            return (int)scene.moverGeoms.size() - 1;
        };
        sphereGeoms[0] = makeSphere(60, 220, 90);
        sphereGeoms[1] = makeSphere(255, 60, 60);
        sphereMovers.clear();
        for (int i = 0; i < vehicle.probeCount(); i++) {
            sphereMovers.push_back(addMover(sphereGeoms[0]));
            scene.movers[sphereMovers.back()].lit = true;
            scene.movers[sphereMovers.back()].alwaysOnTop = true;
        }
    }
    // the convex hull of the chassis as the physics engine made it: its triangles, green or red (translucent), one mover, in car space
    {
        std::vector<Vec3f> hullVertices;
        std::vector<uint32_t> hullIndices;
        if (vehicle.colliderMesh(hullVertices, hullIndices)) {
            auto makeHull = [&](uint8_t r, uint8_t g, uint8_t b) {
                // The renderer shades it with the level's light. Flat, so that the facets can be told apart: every triangle has its
                // own vertices and the normal of its face, pointing away from the hull's middle whichever way the triangle is wound.
                Vec3f middle(0, 0, 0);
                for (const Vec3f& p : hullVertices) middle = middle + p;
                if (!hullVertices.empty()) middle = middle * (1.0f / (float)hullVertices.size());
                std::vector<SceneVertex> verts;
                std::vector<Vec3f> normals;
                std::vector<uint32_t> indices;
                for (size_t k = 0; k + 2 < hullIndices.size(); k += 3) {
                    const Vec3f& p0 = hullVertices[hullIndices[k]];
                    const Vec3f& p1 = hullVertices[hullIndices[k + 1]];
                    const Vec3f& p2 = hullVertices[hullIndices[k + 2]];
                    Vec3f normal = cross(p1 - p0, p2 - p0);
                    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                    if (length > 1e-12f) normal = normal * (1.0f / length);
                    const Vec3f centre = (p0 + p1 + p2) * (1.0f / 3.0f);
                    const Vec3f outward = centre - middle;
                    if (normal.x * outward.x + normal.y * outward.y + normal.z * outward.z < 0) normal = normal * -1.0f;
                    for (const Vec3f* p : {&p0, &p1, &p2}) {
                        SceneVertex v;
                        v.x = p->x; v.y = p->y; v.z = p->z;
                        v.r = r; v.g = g; v.b = b; v.a = 150;
                        v.u = v.v = 0;
                        indices.push_back((uint32_t)verts.size());
                        verts.push_back(v);
                        normals.push_back(normal);
                    }
                }
                BatchSet hull;
                hull.lit = true;
                hull.appendIndexed(-1, BLEND_ALPHA, verts.data(), verts.size(), indices.data(), indices.size(), true, normals.data());
                scene.moverGeoms.push_back(hull);
                return (int)scene.moverGeoms.size() - 1;
            };
            hullGeoms[0] = makeHull(60, 220, 90);
            hullGeoms[1] = makeHull(255, 60, 60);
            hullMover = addMover(hullGeoms[0]);
            scene.movers[hullMover].lit = true;
            scene.movers[hullMover].alwaysOnTop = true;
            // the vertices of the hull: a little white octahedron on each, sized by the hull (one mover, unlit, opaque)
            {
                Vec3f low = hullVertices[0], high = hullVertices[0];
                for (const Vec3f& p : hullVertices) {
                    low = Vec3f(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
                    high = Vec3f(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
                }
                const float dot = 0.008f * std::max(high.x - low.x, std::max(high.y - low.y, high.z - low.z));
                std::vector<bool> used(hullVertices.size(), false);
                for (uint32_t index : hullIndices) used[index] = true;   // (the points that no face uses are not part of the shape)
                std::vector<SceneVertex> dotVerts;
                std::vector<uint32_t> dotIndices;
                const uint32_t octahedron[24] = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
                for (size_t k = 0; k < hullVertices.size(); k++) {
                    if (!used[k]) continue;
                    const Vec3f& p = hullVertices[k];
                    const Vec3f corners[6] = {Vec3f(dot, 0, 0), Vec3f(-dot, 0, 0), Vec3f(0, dot, 0), Vec3f(0, -dot, 0), Vec3f(0, 0, dot), Vec3f(0, 0, -dot)};
                    const uint32_t base = (uint32_t)dotVerts.size();
                    for (const Vec3f& c : corners) {
                        SceneVertex v;
                        v.x = p.x + c.x; v.y = p.y + c.y; v.z = p.z + c.z;
                        v.r = v.g = v.b = v.a = 255;
                        v.u = v.v = 0;
                        dotVerts.push_back(v);
                    }
                    for (uint32_t corner : octahedron) dotIndices.push_back(base + corner);
                }
                BatchSet dots;
                dots.appendIndexed(-1, BLEND_OPAQUE, dotVerts.data(), dotVerts.size(), dotIndices.data(), dotIndices.size(), true);
                scene.moverGeoms.push_back(dots);
                hullDotMover = addMover((int)scene.moverGeoms.size() - 1);
                scene.movers[hullDotMover].lit = false;
                scene.movers[hullDotMover].alwaysOnTop = true;
            }
        }
    }
    worldFloor = scene.bmin.y / scale - tuning().worldFloorBelow;
    makePickupIcons(scene);
    isPrepared = true;
    return true;
}

void MiniGame::forget() {
#ifndef RME_PHYSICS_LEGACY
    vehicle.detach();
    physicsLevel.destroy();
#endif
    isPrepared = false;
    running = false;
    pickups.clear();
    iconCats.clear();
    for (int& m : carMover) m = -1;
    sphereMovers.clear();
    hullMover = -1;
    hullDotMover = -1;
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
    if (hullMover >= 0 && hullMover < (int)scene.movers.size()) scene.movers[hullMover].hidden = true;
    if (hullDotMover >= 0 && hullDotMover < (int)scene.movers.size()) scene.movers[hullDotMover].hidden = true;
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
    // The camera is a mass on a spring and a damper that pull it to the wanted position, and the spring moves with the car: it is
    // measured against the car's own velocity. So the camera has inertia (it swings in the turns, it trails behind when the car
    // accelerates), but when the car moves in a straight line at a steady speed it rests exactly at the wanted position, whatever
    // the speed (a plain smoothing would trail by speed / follow rate: 10 m at 70 m/s, and the camera could not get to the front).
    if (snap) {
        camPos = desired;
        camVel = vel;
    } else {
        const float omega = P.cameraPositionFollow, zeta = P.cameraPositionDampingRatio;
        const int steps = std::max(1, (int)std::ceil(dt * omega * 3.0f));  // (small steps: the integration stays stable at any frame time)
        const float h = dt / (float)steps;
        Vec3f target = desired - vel * dt;  // the wanted position moves on with the car during the frame: it starts where it was
        for (int i = 0; i < steps; i++) {
            target += vel * h;
            camVel += ((target - camPos) * (omega * omega) - (camVel - vel) * (2.0f * zeta * omega)) * h;  // (damped against the car's velocity)
            camPos += camVel * h;
        }
    }
    // keep the camera out of walls: pull it in along the line to the car. The line starts above the car, one radius of its bounding sphere over
    // the centre of mass: not at the point the camera looks at (ahead of the car, it can lie inside the surface the car climbs, which
    // pulls the camera in on the car), and not so low that the line meets the road behind the car.
    const Vec3f rayStart = pos + Vec3f(0, vehicle.boundingRadius(), 0);
    Vec3f toCam = camPos - rayStart;
    float len = length(toCam);
    if (len > C.cameraWallCheckLength) {
        Vec3f dir = toCam * (1.0f / len);
        float t;
        Vec3f n;
        if (world.raycast(rayStart, dir, len + C.cameraWallMargin, true, t, n)) camPos = rayStart + dir * std::max(C.cameraMinDistance, t - C.cameraWallMargin);
    }
    Vec3f look = lookAt - camPos;
    cam.pos = camPos * scale;
    cam.yaw = std::atan2(look.x, look.z);
    cam.pitch = std::asin(std::max(-1.0f, std::min(1.0f, look.y / std::max(1e-3f, length(look)))));
}

void MiniGame::syncMovers(Scene& scene) {
    scene.carBrightness = carSettings().rendererCarBrightness;
    Mat4f M = vehicle.modelMatrix();
    // the car is in metres, the level in its own units: scale the matrices (axes and translation) up
    auto toLevel = [&](Mat4f m) {
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) m.m[i] *= scale;
        return m;
    };
    scene.movers[carMover[0]].model = toLevel(M);
    scene.movers[carMover[0]].hidden = false;
    for (int i = 0; i < 4; i++) {
        const CarPhysics::Wheel& w = vehicle.wheel(i);
        float cs = std::cos(w.steer), sn = std::sin(w.steer);
        float spin = std::fmod(w.spin, kTwoPi);
        float cp = std::cos(spin), sp = std::sin(spin);
        Vec3f ex(cs, 0, -sn), ez0(sn, 0, cs), ey(0, 1, 0);
        Vec3f eY = ey * cp + ez0 * sp, eZ = ez0 * cp - ey * sp;
        Mat4f L = Mat4f::identity();
        L.m[0] = ex.x; L.m[1] = ex.y; L.m[2] = ex.z;
        L.m[4] = eY.x; L.m[5] = eY.y; L.m[6] = eY.z;
        L.m[8] = eZ.x; L.m[9] = eZ.y; L.m[10] = eZ.z;
        L.m[12] = w.centre.x; L.m[13] = w.centre.y; L.m[14] = w.centre.z;
        Mover& m = scene.movers[carMover[1 + i]];
        m.model = toLevel(Mat4f::mul(M, L));
        m.hidden = false;
    }
    // the collision shapes of the car: the spheres at their centres in the car, the hull; red when they touched the level during the last frame
    for (size_t i = 0; i < sphereMovers.size(); i++) {
        Mover& m = scene.movers[sphereMovers[i]];
        m.hidden = !showColliders_ || (int)i >= vehicle.probeCount();
        if (m.hidden) continue;
        const float r = vehicle.probeRadius((int)i);
        const Vec3f c = vehicle.probeCentre((int)i);
        Mat4f S = Mat4f::identity();
        S.m[0] = r; S.m[5] = r; S.m[10] = r;
        S.m[12] = c.x; S.m[13] = c.y; S.m[14] = c.z;
        m.geom = sphereGeoms[vehicle.probeTouched((int)i) ? 1 : 0];
        m.model = toLevel(Mat4f::mul(M, S));
    }
    if (hullMover >= 0) {
        Mover& m = scene.movers[hullMover];
        m.hidden = !showColliders_;
        if (!m.hidden) {
            m.geom = hullGeoms[vehicle.hullTouched() ? 1 : 0];
            m.model = toLevel(M);
        }
    }
    if (hullDotMover >= 0) {
        Mover& m = scene.movers[hullDotMover];
        m.hidden = !showColliders_;
        if (!m.hidden) m.model = toLevel(M);
    }
    // the force arrows: from the contact point along the force, one car weight per tuning length (params), thin
    const CarSettings& C = carSettings();
    // the four suspensions at the points where the physics applies their forces, then the forces on the whole car at its centre of mass,
    // then the four tyres at the same points as their suspensions
    struct Arrow { Vec3f from, force; bool on; };
    const Vec3f centre = vehicle.forceCentre();
    std::vector<Arrow> arrows(12);
    std::vector<int> arrowMover(12);
    for (int i = 0; i < 8; i++) arrowMover[i] = carMover[5 + i];
    for (int i = 0; i < 4; i++) arrowMover[8 + i] = carMover[13 + i];
    for (int i = 0; i < 4; i++) arrows[i] = {vehicle.wheel(i).suspensionForceOrigin, vehicle.wheel(i).force, vehicle.wheel(i).grounded};
    arrows[4] = {centre, vehicle.gravityForce(), true};
    arrows[5] = {centre, vehicle.dragForce(), true};
    arrows[6] = {centre, vehicle.downforce(), true};
    arrows[7] = {centre, vehicle.thrustForce(), true};
    for (int i = 0; i < 4; i++) arrows[8 + i] = {vehicle.wheel(i).tyreForceOrigin, vehicle.wheel(i).tyreForce, vehicle.wheel(i).grounded};
    // one grey arrow per force of the level on the hull, from the point of the sphere that touched it
    for (int i = 0; i < vehicle.hullArrowCount() && i < (int)hullArrowMovers.size(); i++) {
        arrows.push_back({vehicle.hullArrowPoint(i), vehicle.hullArrowForce(i), vehicle.hullArrowOn(i)});
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
        Mat4f A = Mat4f::identity();
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
#ifdef RME_PHYSICS_LEGACY
    vehicle.step(dt, vi, world);
#else
    vehicle.step(dt, vi);
#endif

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
        driftAccum += vehicle.drift() * vehicle.speed() * dt * C.driftPointsFactor;
        int whole = (int)driftAccum;
        if (whole > 0) { driftAccum -= (float)whole; hud_.driftScore += whole; hud_.score += whole; }
    }
    hud_.speedKmh = vehicle.worldSpeed() * 3.6f;  // in the world's metres, not the car's
    hud_.messageAge += dt;
    syncMovers(scene);
    updateCamera(dt, false);
}

}  // namespace minigame01
