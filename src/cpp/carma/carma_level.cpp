#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include "loader/binio.h"
#include "carma/carma_game.h"
#include "loader/inflate.h"
#include "vfs.h"

namespace carma {

void CarmaGame::beginLoad(const std::string& levelId, Scene& out) {
    sc = &out;
    out.cullBackfaces = true;  // the game draws single-sided faces
    paks.clear();
    carma = nullptr;
    trackPaks.clear();
    texCache.clear();
    texScripts.clear();
    missing.clear();
    missingTex = 0;
    missingTexNames.clear();
    trackDir = levelId;
    std::string name = vfs::baseOf(trackDir);
    // Base track = folder name before the first '_' when such a sibling folder exists.
    baseDir = trackDir;
    size_t us = name.find('_');
    if (us != std::string::npos) {
        std::string parent = vfs::dirOf(trackDir);
        for (const std::string& sd : vfs::subdirs(parent))
            if (vfs::lower(sd) == vfs::lower(name.substr(0, us))) baseDir = parent + "/" + sd;
    }
}

void CarmaGame::openArchives() {
    std::vector<std::unique_ptr<Pak>> tp;
    openDirPaks(trackDir, tp);
    if (baseDir != trackDir) openDirPaks(baseDir, tp);
    for (auto& p : tp) trackPaks.push_back(adopt(std::move(p)));
    std::string root = vfs::dirOf(vfs::dirOf(trackDir));
    // Archives in the sub folders of the track too (Level Props holds walls and gates, Animated Props the lifts and
    // behemoths, ...): the hierarchies are listed by name in the level's descriptors, not by archive.
    std::vector<std::string> levelDirs{trackDir};
    if (baseDir != trackDir) levelDirs.push_back(baseDir);
    for (const std::string& d : levelDirs) {
        for (const std::string& sd : vfs::subdirs(d)) {
            std::vector<std::unique_ptr<Pak>> sp;
            openDirPaks(d + "/" + sd, sp);
            for (auto& p : sp) trackPaks.push_back(adopt(std::move(p)));
        }
    }
    for (const std::string& sd : vfs::subdirs(root))
        if (vfs::lower(sd) == "pathfollowers" || vfs::lower(sd) == "powerups" || vfs::lower(sd) == "movableobjects" || vfs::lower(sd) == "drones" || vfs::lower(sd) == "stuff") {
            std::vector<std::unique_ptr<Pak>> sp;
            openDirPaks(root + "/" + sd, sp);
            for (auto& p : sp) trackPaks.push_back(adopt(std::move(p)));
        }
}

bool CarmaGame::readGeometry(const KeyFile& kf, int& meshCount, std::string& err) {
    std::vector<std::string> hies, collisionDescs;
    {
        std::string md;
        if (!readNamedText(kf.str("STATIC_MESH_DESCRIPTOR"), md)) { err = "cannot read mesh descriptor"; return false; }
        for (const std::string& raw : splitLines(md)) {
            auto q = quotedStrings(stripComment(raw));
            if (q.empty()) continue;
            const std::string& first = q[0];
            // the rows are: "hierarchy or script" renderNode optimize "collision descriptor" (the numbers are not quoted, "" = none)
            if (q.size() > 1 && !q[1].empty()) collisionDescs.push_back(q[1]);
            if (endsWith(vfs::lower(first), ".hie")) {
                hies.push_back(first.substr(0, first.size() - 4));
            } else if (endsWith(vfs::lower(first), ".txt")) {
                std::string cd;
                if (readNamedText(first, cd))
                    for (const std::string& cl : splitLines(cd))
                        for (const std::string& s : quotedStrings(stripComment(cl)))
                            if (endsWith(vfs::lower(s), ".hie")) hies.push_back(s.substr(0, s.size() - 4));
            }
        }
    }
    {
        std::string wm = kf.str("WATER_MESH");
        if (endsWith(vfs::lower(wm), ".hie")) {
            std::string wb = wm.substr(0, wm.size() - 4);
            bool have = false;
            for (const std::string& h : hies) have |= vfs::lower(h) == vfs::lower(wb);
            if (!have) hies.push_back(wb);
        }
    }
    for (const std::string& h : hies) emitHie(h, meshCount, vfs::lower(h).find("water") != std::string::npos);
    findCollision(collisionDescs, *sc);
    if (sc->triCount == 0) {
        err = "no geometry found (" + std::to_string(hies.size()) + " hierarchies";
        for (const std::string& m : missing) err += ", missing " + m;
        err += ")";
        return false;
    }
    return true;
}

Cats CarmaGame::addCategories(Scene& out) {
    Cats c;
    c.start = out.addCategory("start", "Start position", rgb(0x39ff7a), SHAPE_ARROW, 2.0f, true);
    c.pup = out.addCategory("powerup", "Powerups", rgb(0xffb870), SHAPE_DIAMOND, 1.0f, true);
    c.notable = out.addCategory("notable", "Notable powerups", rgb(0xe07b00), SHAPE_DIAMOND, 1.15f, true);
    c.pedPup = out.addCategory("pedpowerup", "Pedestrian powerups", rgb(0xffe600), SHAPE_DIAMOND, 1.0f, true);
    c.time = out.addCategory("timepowerup", "Time bonuses", rgb(0x8fd8ff), SHAPE_DIAMOND, 1.0f, true);
    c.upgrade = out.addCategory("upgrade", "Permanent Upgrades", rgb(0xff2a2a), SHAPE_DIAMOND, 1.2f, true);
    c.credit = out.addCategory("credit", "Credits", rgb(0x35e04a), SHAPE_DIAMOND, 1.0f, true);
    c.missionItem = out.addCategory("missionitem", "Mission items", rgb(0x35c7e8), SHAPE_DIAMOND, 1.0f, true);
    c.mission = out.addCategory("mission", "Mission objectives", rgb(0xff4fd8), SHAPE_CUBE, 1.0f, true);
    c.check = out.addCategory("checkpoint", "Checkpoints", rgb(0xff5a5a), SHAPE_CUBE, 1.0f, false, true, true);
    c.race = out.addCategory("raceline", "Race line", rgb(0x37c8ff), SHAPE_DIAMOND, 1.0f, false, true, true);
    c.startLine = out.addCategory("startline", "AI race-line start (path flag)", rgb(0xffffff), SHAPE_DIAMOND, 1.0f, false, false, true);
    c.ai = out.addCategory("aipath", "Opponent roam paths", rgb(0x4a7dff), SHAPE_DIAMOND, 1.0f, false, false, true);
    c.drone = out.addCategory("drone", "Drone paths (traffic, trains)", rgb(0xc27aff), SHAPE_DIAMOND, 1.0f, false, false, true);
    c.ped = out.addCategory("ped", "Pedestrians", rgb(0x1f7a2e), SHAPE_DIAMOND, 0.6f, false, false);
    c.respawn = out.addCategory("respawn", "Respawn points", rgb(0x7af0ff), SHAPE_ARROW, 1.2f, false, false);
    return c;
}

void CarmaGame::addStart(const KeyFile& kf, Scene& out, const Cats& c) {
    float sp[3], ang;
    if (kf.nums("START_POS", sp, 3)) {
        float a = 0;
        if (kf.nums("START_ANGLE", &ang, 1)) a = ang;
        Vec3f p = fixX({sp[0], sp[1], sp[2]});
        out.addMarker(c.start, p, fixA(a), "Start position");
        out.hasCameraStart = true;
        out.cameraYaw = fixA(a);
        out.cameraPitch = -0.5f;
        out.cameraPos = p + Vec3f(std::sin(fixA(a)), 0, std::cos(fixA(a))) * -60.0f + Vec3f(0, 50, 0);
    }
}

void CarmaGame::readPupFile(const std::string& pt, Scene& out, const Cats& c, std::vector<Placed>& placed) {
    std::string label;
    std::vector<float> nums;
    for (const std::string& l : splitLines(pt)) {
        if (l.empty()) continue;
        if (l.compare(0, 2, "//") == 0) { label = l.substr(2); continue; }
        std::istringstream is(l);
        float f;
        while (is >> f) nums.push_back(f);
        if (nums.size() >= 6) {
            int id = (int)nums[0];
            Vec3f p = fixX({nums[1], nums[2], nums[3]});
            std::string lb = label.empty() ? "Powerup" : label;
            int cat = c.pup;
            switch (pupTable.classify(id, lb)) {
                case 1: cat = c.notable; break;
                case 2: cat = c.pedPup; break;
                case 3: cat = c.upgrade; break;
                case 4: cat = c.credit; break;
                case 5: cat = c.time; break;
                default: break;
            }
            out.addMarker(cat, p, 0, lb + " (id " + std::to_string(id) + ")");
            placed.push_back({id, cat, p});
            nums.clear();
            label.clear();
        }
    }
}

void CarmaGame::readMissionScripts(const std::string& name, const KeyFile& race, bool listed, Scene& out, const Cats& c, std::vector<Placed>& placed) {
    // the script is the MISSION_DESC of the race block; without a block, look for "<level>..._script.txt"
    std::vector<std::string> scripts;
    std::string md = race.str("MISSION_DESC"), mt;
    if (!md.empty() && readNamedText(md, mt)) scripts.push_back(mt);
    else if (!listed) {
        std::string lname = vfs::lower(name);
        for (Pak* p : trackPaks)
            for (size_t ei = 0; ei < p->entries.size(); ei++) {
                std::string en = vfs::lower(p->entries[ei].name);
                if (!endsWith(en, "_script.txt") || en.find(lname) == std::string::npos) continue;
                std::vector<uint8_t> sd;
                if (p->extract((int)ei, sd)) scripts.push_back(std::string((const char*)sd.data(), sd.size()));
            }
    }
    for (const std::string& scriptText : scripts) {
        for (const std::string& raw : splitLines(scriptText)) {
            std::string l = stripComment(raw);
            if (l.compare(0, 14, "CREATE_POWERUP") != 0) continue;
            std::string rest = l.substr(14), pname;
            size_t a = rest.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            rest = rest.substr(a);
            if (rest[0] == '"') {
                size_t b = rest.find('"', 1);
                if (b == std::string::npos) continue;
                pname = rest.substr(1, b - 1);
                rest = rest.substr(b + 1);
            } else {
                size_t b = rest.find_first_of(" \t");
                pname = rest.substr(0, b);
                rest = b == std::string::npos ? "" : rest.substr(b);
            }
            // first run of three numbers = position
            std::istringstream is(rest);
            std::string tok;
            std::vector<float> run;
            while (is >> tok && run.size() < 3) {
                char* endp = nullptr;
                float v = strtof(tok.c_str(), &endp);
                if (endp && *endp == 0 && endp != tok.c_str()) run.push_back(v);
                else run.clear();
            }
            if (run.size() < 3 || pname.empty()) continue;
            int id = -1;
            for (size_t k = 0; k < pupTable.items.size(); k++)
                if (vfs::lower(pupTable.items[k].name) == vfs::lower(pname)) { id = (int)k; break; }
            Vec3f pos = fixX({run[0], run[1], run[2]});
            int cat = id >= 0 ? c.missionItem : c.mission;  // named items vs. objective markers
            out.addMarker(cat, pos, 0, pname + (id >= 0 ? " (mission item)" : " (mission objective)"));
            placed.push_back({id, cat, pos});
        }
    }
}

void CarmaGame::addPowerupMeshes(Scene& out, const std::vector<Placed>& placed) {
    // In-game icon meshes (spinning). The id -> icon table is hard-coded in the game executable, so the
    // assignment below follows the groups of the powerup string table.
    struct Icon { int geom = -1, halo = -1; Vec3f center; };
    std::map<std::string, Icon> geoms;
    for (const Placed& pl : placed) {
        if (pl.id < 0) continue;  // mission objective markers have no in-game mesh
        std::string icon = pupTable.iconFor(pl.id);
        auto g = geoms.find(icon);
        if (g == geoms.end()) {
            Icon ic;
            out.moverGeoms.emplace_back();
            out.moverGeoms.emplace_back();  // the halo (corona) quad, if the icon has one
            int idx = (int)out.moverGeoms.size() - 2;
            dst = &out.moverGeoms[idx];
            coronaDst = &out.moverGeoms[idx + 1];
            coronaFound = false;
            int mc = 0;
            emitHie(icon, mc, false, MIR_X);
            dst = nullptr;
            coronaDst = nullptr;
            bool halo = coronaFound && !out.moverGeoms[idx + 1].batches.empty();
            if (!halo) out.moverGeoms.pop_back();
            if (mc == 0) { out.moverGeoms.resize(idx); idx = -1; halo = false; }
            ic.geom = idx;
            if (halo) { ic.halo = idx + 1; ic.center = coronaCenter; }
            g = geoms.emplace(icon, ic).first;
        }
        if (g->second.geom < 0) continue;
        Mover m;
        m.geom = g->second.geom;
        m.cat = pl.cat;
        m.fixed = true;
        m.powerupMesh = true;
        m.path = {pl.pos};
        m.phase = (float)(pl.id % 7);
        out.movers.push_back(std::move(m));
        if (g->second.halo >= 0) {
            Mover h;
            h.geom = g->second.halo;
            h.scaleGeom = g->second.geom;
            h.cat = pl.cat;
            h.fixed = true;
            h.billboard = true;
            h.anchor = g->second.center;
            h.powerupMesh = true;
            h.path = {pl.pos};
            out.movers.push_back(std::move(h));
        }
        out.cats[pl.cat].hasMesh = true;
    }
}

void CarmaGame::addPowerups(const std::string& name, const KeyFile& race, bool listed, Scene& out, const Cats& c) {
    std::string pt;
    bool havePup = readRacePup(race, pt);
    if (!havePup && !listed) havePup = findPup(name, pt);  // not listed in races.txt: find it by naming convention
    loadPupTable();
    std::vector<Placed> placed;
    if (havePup) readPupFile(pt, out, c, placed);
    readMissionScripts(name, race, listed, out, c, placed);
    addPowerupMeshes(out, placed);
}

void CarmaGame::addCheckpoints(const KeyFile& kf, Scene& out, const Cats& c) {
    // checkpoints (special volumes: 8 corners each)
    std::string volName = kf.str("SPECIAL_VOLUMES"), volHdr = kf.str("SPECIAL_VOLUMES_HEADER");
    if (!volName.empty()) {
        std::vector<uint8_t> vd;
        if (readNamed(volName, vd)) {
            std::map<int, std::string> names;
            std::string ht;
            if (!volHdr.empty() && readNamedText(volHdr, ht))
                for (const std::string& l : splitLines(ht)) {
                    std::istringstream is(l);
                    std::string d, nm;
                    int idx;
                    if ((is >> d >> nm >> idx) && d == "#define") names[idx] = nm;
                }
            Reader r(vd);
            uint32_t n = r.u32();
            if (r.ok && n % 8 == 0 && r.left() >= (size_t)n * 12) {
                for (uint32_t b = 0; b < n / 8; b++) {
                    Vec3f p[8], center;
                    for (auto& v : p) { v = fixX(r.vec3()); center += v * 0.125f; }
                    static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                    for (auto& ed : e) out.addLine(c.check, p[ed[0]], p[ed[1]]);
                    std::string nm = names.count((int)b) ? names[(int)b] : "VOLUME" + std::to_string(b);
                    out.addMarker(c.check, center, 0, nm);
                }
            }
        }
    }
}

void CarmaGame::addRoutes(const KeyFile& race, bool listed, const Cats& c) {
    std::string opp = race.str("OPPONENT_PATHS"), drone = race.str("DRONE_PATHS");
    if (!listed && !endsWith(vfs::lower(opp), ".hie")) opp = findAiPathHie();
    if (!listed && !endsWith(vfs::lower(drone), ".hie")) drone = findDronePathHie();
    if (endsWith(vfs::lower(opp), ".hie")) emitPaths(opp.substr(0, opp.size() - 4), c.ai, c.race, c.startLine, c.drone, false);
    if (endsWith(vfs::lower(drone), ".hie")) emitPaths(drone.substr(0, drone.size() - 4), c.ai, c.race, c.startLine, c.drone, true);
}

void CarmaGame::addMovers(const KeyFile& kf, Scene& out) {
    int cMove = out.addCategory("movers", "Moving objects (path followers, behemoths, lifts)", rgb(0x7fd0ff), SHAPE_DIAMOND, 1.0f, false, true);
    int cAnimPath = out.addCategory("animpath", "Routes of animated props (behemoths, lifts)", rgb(0xffc46b), SHAPE_DIAMOND, 1.0f, false, false, true);
    int cFPath = out.addCategory("followerpath", "Paths of moving objects", rgb(0xff9ad5), SHAPE_DIAMOND, 1.0f, false, false, true);
    std::string pf = kf.str("PATH_FOLLOWERS");
    if (!pf.empty()) loadFollowers(pf, out, cMove, cFPath);
    std::string ap = kf.str("ANIMATED_PROPS");
    if (!ap.empty()) loadAnimProps(ap, out, cMove, cAnimPath);
}

void CarmaGame::addPeds(const KeyFile& kf, Scene& out, const Cats& c) {
    // PEDS_DESCRIPTOR names the descriptor whose "Ped Placement File" lists the pedestrians; levels that comment it
    // out (Bridge) have none.
    std::string pt, pn, pdesc = kf.str("PEDS_DESCRIPTOR"), pdText;
    if (!pdesc.empty() && readNamedText(pdesc, pdText))
        for (const std::string& l : splitLines(pdText)) {
            auto q = quotedStrings(stripComment(l));
            if (!q.empty() && endsWith(vfs::lower(q[0]), ".txt")) { pn = q[0]; break; }
        }
    if (!pn.empty() && readNamedText(pn, pt)) {
        for (const std::string& l : splitLines(pt)) {
            if (l.empty() || l.compare(0, 2, "//") == 0) continue;
            std::istringstream is(l);
            float v[7];
            int k = 0;
            while (k < 7 && (is >> v[k])) k++;
            if (k == 7)
                out.addMarker(c.ped, fixX({v[3], v[4], v[5]}), fixA(v[6] * 0.0174533f), "Pedestrian (skin " + std::to_string((int)v[1]) + ")");
        }
    }
}

void CarmaGame::addRespawns(const KeyFile& kf, const std::string& name, Scene& out, const Cats& c) {
    std::string rt;
    std::string rname = kf.str("RESPAWN_POINTS");  // the level descriptor names the file (spelling varies per level)
    if ((!rname.empty() && readNamedText(rname, rt)) || readNamedText(name + "_RespawnPoints.txt", rt) ||
        readNamedText(vfs::baseOf(baseDir) + "_RespawnPoints.txt", rt)) {
        for (const std::string& l : splitLines(rt)) {
            if (l.empty() || l.compare(0, 2, "//") == 0) continue;
            std::istringstream is(l);
            float v[4] = {0, 0, 0, 0};
            int k = 0;
            while (k < 4 && (is >> v[k])) k++;
            if (k >= 3) out.addMarker(c.respawn, fixX({v[0], v[1], v[2]}), k == 4 ? fixA(v[3]) : 0, "Respawn point");
        }
    }
}

// The level's ambient and sun light, for the shaded meshes (the minigame's car).
void CarmaGame::readLighting(const KeyFile& kf, Scene& out) {
    float amb, sun[3], sc3[3];
    if (kf.nums("AMBIENT_LIGHT", &amb, 1)) out.lighting.ambient = Vec3f(amb, amb, amb);
    if (kf.nums("SUN_VECTOR", sun, 3)) out.lighting.sunDir = normalize(fixX(Vec3f(-sun[0], -sun[1], -sun[2])));
    if (kf.nums("SUN_LIGHT_COLOUR", sc3, 3)) out.lighting.sunColor = {sc3[0], sc3[1], sc3[2]};
}

// The level's point lights: a marker layer, and the lights themselves for the shaded meshes.
void CarmaGame::addLightMarkers(const KeyFile& kf, Scene& out) {
    int cLight = out.addCategory("light", "Lights (in-game)", rgb(0xfff3a0), SHAPE_DIAMOND, 0.7f, false, true);
    std::string lname = kf.str("LIGHTS_DESCRIPTOR"), lt;
    if (!lname.empty() && readNamedText(lname, lt)) {
        bool first = true;
        for (const std::string& l : splitLines(lt)) {
            if (l.empty() || l.compare(0, 2, "//") == 0) continue;
            std::istringstream is(l);
            if (first) { first = false; float cnt; if (!(is >> cnt)) break; if (l.find('.') == std::string::npos && l.find('x') == std::string::npos && std::count(l.begin(), l.end(), ' ') == 0) continue; }
            float x, y, z, r0, r1;
            std::string col;
            if (!(is >> x >> y >> z >> r0 >> r1 >> col)) continue;
            uint32_t c = (uint32_t)strtoul(col.c_str(), nullptr, 16);
            PointLight pl;
            pl.pos = fixX({x, y, z});
            pl.inner = r0;
            pl.outer = std::max(r1, r0 + 0.01f);
            const Color3f lc = Color3f::from3b(Color3b::from_rgb(c));
            pl.color = Vec3f(lc.r, lc.g, lc.b);
            out.lighting.lights.push_back(pl);
            char hex[16];
            snprintf(hex, sizeof hex, "#%06x", c & 0xffffff);
            out.addMarker(cLight, pl.pos, 0, std::string("Light ") + hex + " (radius " + std::to_string((int)r0) + "-" + std::to_string((int)r1) + ")");
        }
    }
}

void CarmaGame::addProps(const KeyFile& kf, Scene& out) {
    int cProps = out.addCategory("props", "Destructible & movable props", rgb(0x9aa0a6), SHAPE_CUBE, 1.0f, false, true);
    out.moverGeoms.emplace_back();
    int propsGeom = (int)out.moverGeoms.size() - 1;
    int instances = 0;

    std::string brk = kf.str("BREAKABLES_DESCRIPTOR"), bt;
    if (!brk.empty() && readNamedText(brk, bt)) {
        for (const std::string& l : splitLines(bt)) {
            auto q = quotedStrings(stripComment(l));
            if (q.empty() || !endsWith(vfs::lower(q[0]), ".hie")) continue;
            dst = &out.moverGeoms[propsGeom];
            int mc = 0;
            emitHie(q[0].substr(0, q[0].size() - 4), mc);  // matrices already place the pieces in the level
            dst = nullptr;
            instances += mc > 0 ? 1 : 0;
            break;
        }
    }

    std::string mo = kf.str("MOVABLE_OBJECTS"), mt;
    if (!mo.empty() && readNamedText(mo, mt)) {
        std::map<std::string, std::unique_ptr<BatchSet>> cache;
        std::map<std::string, float> coms;
        for (const std::string& raw : splitLines(mt)) {
            std::string l = stripComment(raw);
            if (l.empty() || l[0] != '"') continue;
            size_t e = l.find('"', 1);
            if (e == std::string::npos) continue;
            std::string hie = l.substr(1, e - 1);
            float v[7];
            if (sscanf(l.c_str() + e + 1, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) continue;
            auto it = cache.find(vfs::lower(hie));
            if (it == cache.end()) {
                auto bs = std::make_unique<BatchSet>();
                dst = bs.get();
                int mc = 0;
                float com = 0;
                emitHie(hie, mc, false, MIR_NONE, &com);
                dst = nullptr;
                if (mc == 0) bs.reset();
                it = cache.emplace(vfs::lower(hie), std::move(bs)).first;
                coms[vfs::lower(hie)] = com;
            }
            if (!it->second) continue;
            // The descriptor places the object's centre of mass (the .dcol holds its height); the mesh sits on y = 0.
            float com = coms[vfs::lower(hie)];
            // position + rotation quaternion (x, y, z, w)
            float qx = v[3], qy = v[4], qz = v[5], qw = v[6];
            float r[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                             {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                             {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
            std::vector<SceneVertex> vs;
            std::vector<uint32_t> fl;
            for (const Batch& b : it->second->batches) {
                vs = b.verts;
                for (SceneVertex& sv : vs) {
                    float x = sv.x, y = sv.y - com, z = sv.z;
                    Vec3f w = fixX({r[0][0] * x + r[0][1] * y + r[0][2] * z + v[0], r[1][0] * x + r[1][1] * y + r[1][2] * z + v[1],
                                   r[2][0] * x + r[2][1] * y + r[2][2] * z + v[2]});
                    sv.x = w.x; sv.y = w.y; sv.z = w.z;
                }
                const auto* idx = b.idx.data();
                if (kMirrorX) {
                    fl.assign(b.idx.begin(), b.idx.end());
                    for (size_t i = 0; i + 2 < fl.size(); i += 3) std::swap(fl[i + 1], fl[i + 2]);
                    idx = fl.data();
                }
                out.moverGeoms[propsGeom].appendIndexed(b.texture, b.blend, vs.data(), vs.size(), idx, b.idx.size(), b.twoSided);
            }
            instances++;
        }
    }

    if (instances > 0 && !out.moverGeoms[propsGeom].batches.empty()) {
        Mover m;
        m.geom = propsGeom;
        m.cat = cProps;
        m.fixed = true;
        m.staticModel = true;
        m.path = {Vec3f()};
        out.movers.push_back(std::move(m));
        out.cats[cProps].count = instances;
    } else {
        out.moverGeoms.pop_back();
    }
}

std::string CarmaGame::summary(const Scene& out, int meshCount) const {
    char buf[512];
    std::string extra;
    if (missingTex) {
        extra += ", " + std::to_string(missingTex) + " textures missing (";
        for (size_t i = 0; i < missingTexNames.size(); i++) extra += (i ? ", " : "") + missingTexNames[i];
        extra += ")";
    }
    for (size_t i = 0; i < missing.size() && i < 3; i++) extra += (i ? ", " : ", skipped: ") + missing[i];
    snprintf(buf, sizeof buf, "%zu triangles from %d meshes, %d textures%s", out.triCount, meshCount, (int)out.textures.size(), extra.c_str());
    return buf;
}
}  // namespace carma
