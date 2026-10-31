// Loads the minigame's car (the Eagle) from Carmageddon TDR 2000's Cars/Eagle4 folder: the hierarchy named by the car
// descriptor, its LOD 1 body and the four wheels, each in car space.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <sstream>
#include "carma/carma_game.h"
#include "vfs.h"

namespace carma {

namespace {

const char* const kCarDescriptor = "Eagle4descriptor.txt";

// The quoted value on the line after the comment line that contains `label`.
std::string descriptorValue(const std::vector<std::string>& lines, const char* label) {
    for (size_t i = 0; i + 1 < lines.size(); i++) {
        if (lines[i].compare(0, 2, "//") != 0 || lines[i].find(label) == std::string::npos) continue;
        for (size_t j = i + 1; j < lines.size() && j < i + 4; j++) {
            auto q = quotedStrings(stripComment(lines[j]));
            if (!q.empty()) return q[0];
        }
    }
    return "";
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

bool CarmaGame::canDrive() { return locate(kCarDescriptor, "", "") >= 0; }

bool CarmaGame::loadCarModel(Scene& scene, minigame01::CarModel& out) {
    int di = locate(kCarDescriptor, "", "");
    if (di < 0) return false;
    sc = &scene;  // textures of the car are added to the scene that is being played (possibly another game's)
    texCache.clear();
    const std::string dpath = vfs::files()[di].path, dir = vfs::dirOf(dpath);
    std::string dtext;
    if (!vfs::readText(dpath, dtext)) return false;
    std::vector<std::string> dlines = splitLines(dtext);
    const std::string hieName = descriptorValue(dlines, "Hierarchy filename"), headerName = descriptorValue(dlines, "Header filename");
    if (hieName.empty()) return false;

    // archives of the car folder
    std::vector<std::unique_ptr<Pak>> opened;
    openDirPaks(dir, opened);
    Pak* pk = nullptr;
    for (auto& p : opened) {
        Pak* a = adopt(std::move(p));
        if (!pk && a->find(hieName) >= 0) pk = a;
    }
    if (!pk) return false;

    Hie hie;
    std::vector<uint8_t> data;
    if (!pk->extract(pk->find(hieName), data) || !hie.parse(std::string((const char*)data.data(), data.size()))) return false;
    std::vector<CMesh> meshes;
    for (const std::string& mn : hie.meshNames) {
        int me = pk->find(mn);
        if (me < 0 || !pk->extract(me, data) || !parseMshs(data, meshes)) return false;
    }

    // node names: "#define NAME index" lines of the header
    std::vector<std::string> names(hie.nodes.size());
    if (!headerName.empty()) {
        int hi = vfs::findIn(dir, headerName);
        std::string ht;
        if (hi >= 0 && vfs::readText(vfs::files()[hi].path, ht))
            for (const std::string& l : splitLines(ht)) {
                std::istringstream is(l);
                std::string d, nm;
                int idx;
                if ((is >> d >> nm >> idx) && d == "#define" && idx >= 0 && idx < (int)names.size()) names[idx] = vfs::lower(nm);
            }
    }

    std::vector<int> texOf(hie.textures.size(), -2);
    std::vector<SceneVertex> vs;
    std::vector<uint32_t> flipped;
    auto emitMesh = [&](const CMesh& m, const Mat4f& mat, int texNode, int matNode, BatchSet& dst) {
        int tex = -1;
        if (texNode >= 0 && texNode < (int)hie.textures.size()) {
            if (texOf[texNode] == -2) texOf[texNode] = textureFor(hie.textures[texNode], pk);
            tex = texOf[texNode];
        }
        vs.resize(m.nv());
        bool translucent = false;
        for (size_t i = 0; i < m.nv(); i++) {
            const float* f = &m.v[i * 9];
            Vec3f w = mat.transformPoint({f[0], f[1], f[2]});
            SceneVertex& v = vs[i];
            v.x = -w.x; v.y = w.y; v.z = w.z;  // the viewer mirrors X
            v.r = (uint8_t)(std::min(std::max(f[5], 0.f), 1.f) * 255.f);
            v.g = (uint8_t)(std::min(std::max(f[4], 0.f), 1.f) * 255.f);
            v.b = (uint8_t)(std::min(std::max(f[3], 0.f), 1.f) * 255.f);
            v.a = (uint8_t)(std::min(std::max(f[6], 0.f), 1.f) * 255.f);
            if (v.a < 250) translucent = true;
            v.u = f[7];
            v.v = f[8];
        }
        Blend bl = translucent ? BLEND_ALPHA : BLEND_OPAQUE;
        if (!translucent && tex >= 0 && scene.textures[tex].hasAlpha) bl = scene.textures[tex].partialAlpha ? BLEND_ALPHA : BLEND_CUTOUT;
        if (!translucent)
            for (auto& v : vs) v.a = 255;
        int flags = matNode >= 0 && matNode < (int)hie.matFlags.size() ? hie.matFlags[matNode] : 0;
        if (flags & 8) {
            bl = BLEND_ADD;
            for (auto& v : vs) v.a = 255;
        }
        flipped.assign(m.idx.begin(), m.idx.end());
        for (size_t i = 0; i + 2 < flipped.size(); i += 3) std::swap(flipped[i + 1], flipped[i + 2]);  // the mirror reverses the winding
        // the file's vertex normals, moved and mirrored like the vertices
        std::vector<Vec3f> ns;
        if (m.n.size() == m.nv() * 3) {
            ns.resize(m.nv());
            for (size_t i = 0; i < m.nv(); i++) {
                Vec3f d = mat.transformDirection({m.n[i * 3], m.n[i * 3 + 1], m.n[i * 3 + 2]});
                float l = length(d);
                ns[i] = l > 1e-9f ? Vec3f(-d.x / l, d.y / l, d.z / l) : Vec3f(0, 0, 0);
            }
        }
        dst.appendIndexed(tex, bl, vs.data(), vs.size(), flipped.data(), flipped.size(), (flags & 1) != 0, ns.empty() ? nullptr : ns.data());
    };

    // wheels (their own nodes) are drawn separately from the chassis
    auto wheelSlot = [&](const std::string& nm) -> int {  // 0..3 = FL FR RL RR, -1 = not a wheel root
        static const char* roots[4] = {"_wheel_fl", "_wheel_fr", "_wheel_rl", "_wheel_rr"};
        for (int i = 0; i < 4; i++) {
            size_t n = strlen(roots[i]);
            if (nm.size() >= n && nm.compare(nm.size() - n, n, roots[i]) == 0) return i;
        }
        return -1;
    };

    std::function<void(int, const Mat4f&, int, int, BatchSet&, int)> walkNodes = [&](int first, const Mat4f& m0, int tex0, int mat0, BatchSet& dst, int depth) {
        if (depth > 40) return;
        for (int n = first; n >= 0 && n < (int)hie.nodes.size(); n = hie.nodes[n].sibling) {
            const Hie::Node& nd = hie.nodes[n];
            const std::string& nm = names[n];
            if (contains(nm, "_lod23") || contains(nm, "_lod33") || contains(nm, "master_wheel") || contains(nm, "piviots")) continue;
            Mat4f m = m0;
            int tex = tex0, mat = mat0;
            if (nd.type == 1 && nd.index >= 0 && nd.index < (int)hie.mats.size()) m = Mat4f::mul(m0, hie.mats[nd.index].m);
            else if (nd.type == 2) tex = nd.index;
            else if (nd.type == 5) mat = nd.index;
            else if (nd.type == 3 && nd.index >= 0 && nd.index < (int)meshes.size()) emitMesh(meshes[nd.index], m0, tex0, mat0, dst);
            if (nd.child >= 0) walkNodes(nd.child, m, tex, mat, dst, depth + 1);
        }
    };

    bool haveWheel[4] = {false, false, false, false};
    // find the wheel roots first, then build the chassis from everything else
    for (size_t n = 0; n < hie.nodes.size(); n++) {
        int slot = wheelSlot(names[n]);
        const Hie::Node& nd = hie.nodes[n];
        if (slot < 0 || nd.type != 1 || nd.index < 0 || nd.index >= (int)hie.mats.size()) continue;
        const Mat4f& wm = hie.mats[nd.index].m;
        out.wheelPos[slot] = Vec3f(-wm.m[12], wm.m[13], wm.m[14]);
        haveWheel[slot] = true;
        int axle = slot < 2 ? 0 : 2;  // the left wheel's mesh serves both wheels of an axle
        if (nd.child >= 0 && out.wheel[axle].batches.empty()) walkNodes(nd.child, Mat4f::identity(), -1, -1, out.wheel[axle], 0);
    }
    if (!(haveWheel[0] && haveWheel[1] && haveWheel[2] && haveWheel[3])) return false;

    // the chassis: skip the wheel roots (handled above)
    std::function<void(int, const Mat4f&, int, int, int)> body = [&](int first, const Mat4f& m0, int tex0, int mat0, int depth) {
        if (depth > 40) return;
        for (int n = first; n >= 0 && n < (int)hie.nodes.size(); n = hie.nodes[n].sibling) {
            const Hie::Node& nd = hie.nodes[n];
            const std::string& nm = names[n];
            if (wheelSlot(nm) >= 0 || contains(nm, "_lod23") || contains(nm, "_lod33") || contains(nm, "master_wheel") || contains(nm, "piviots") ||
                contains(nm, "_wheel_"))
                continue;
            Mat4f m = m0;
            int tex = tex0, mat = mat0;
            if (nd.type == 1 && nd.index >= 0 && nd.index < (int)hie.mats.size()) m = Mat4f::mul(m0, hie.mats[nd.index].m);
            else if (nd.type == 2) tex = nd.index;
            else if (nd.type == 5) mat = nd.index;
            else if (nd.type == 3 && nd.index >= 0 && nd.index < (int)meshes.size()) emitMesh(meshes[nd.index], m0, tex0, mat0, out.body);
            if (nd.child >= 0) body(nd.child, m, tex, mat, depth + 1);
        }
    };
    body(0, Mat4f::identity(), -1, -1, 0);
    if (out.body.batches.empty()) return false;

    auto bounds = [](const BatchSet& s, Vec3f& lo, Vec3f& hi) {
        lo = Vec3f(1e30f, 1e30f, 1e30f);
        hi = Vec3f(-1e30f, -1e30f, -1e30f);
        for (const Batch& b : s.batches)
            for (const SceneVertex& v : b.verts) {
                lo = vmin(lo, Vec3f(v.x, v.y, v.z));
                hi = vmax(hi, Vec3f(v.x, v.y, v.z));
            }
    };
    bounds(out.body, out.boundsMin, out.boundsMax);
    Vec3f wlo, whi;
    out.wheel[1] = out.wheel[0];
    out.wheel[3] = out.wheel[2];
    bounds(out.wheel[0], wlo, whi);
    out.wheelRadius = std::max(0.2f, std::min(0.6f, 0.5f * (whi.y - wlo.y)));
    out.wheelWidth = whi.x - wlo.x;
    // each axle's own radius from its mesh (the rear wheels are bigger than the front ones)
    for (int axle = 0; axle < 2; axle++) {
        bounds(out.wheel[axle * 2], wlo, whi);
        const float r = std::max(0.2f, std::min(0.6f, 0.5f * (whi.y - wlo.y)));
        out.meshWheelRadii[axle * 2] = out.meshWheelRadii[axle * 2 + 1] = r;
    }

    // The descriptor's wheel blocks: after the comment "Offset instance node" comes the wheel's node name (..._FL, _FR, _RL, _RR),
    // and further down its radius (m), spring stiffness (N/m) and damper (N s/m), each on the line after its comment.
    {
        auto numberAfter = [&](size_t i, float& v) {
            for (size_t j = i + 1; j < dlines.size() && j < i + 4; j++) {
                std::istringstream is(stripComment(dlines[j]));
                if (is >> v) return true;
            }
            return false;
        };
        int wheel = -1;
        float springSum = 0, damperSum = 0;
        int springCount = 0, damperCount = 0;
        for (size_t i = 0; i < dlines.size(); i++) {
            if (dlines[i].compare(0, 2, "//") != 0) continue;
            float v;
            if (contains(dlines[i], "Offset instance node")) {
                wheel = -1;
                for (size_t j = i + 1; j < dlines.size() && j < i + 4; j++) {
                    auto q = quotedStrings(stripComment(dlines[j]));
                    if (q.empty()) continue;
                    const std::string nm = vfs::lower(q[0]);
                    static const char* tails[4] = {"_wheel_fl", "_wheel_fr", "_wheel_rl", "_wheel_rr"};
                    for (int k = 0; k < 4; k++)
                        if (nm.size() >= strlen(tails[k]) && nm.compare(nm.size() - strlen(tails[k]), strlen(tails[k]), tails[k]) == 0) wheel = k;
                    break;
                }
            } else if (wheel >= 0 && contains(dlines[i], "Radius (m)") && numberAfter(i, v) && v > 0.05f) {
                out.fileWheelRadii[wheel] = v;
            } else if (contains(dlines[i], "spring stiffness") && numberAfter(i, v) && v > 0) {
                springSum += v;
                springCount++;
            } else if (contains(dlines[i], "shock absorber damping") && numberAfter(i, v) && v > 0) {
                damperSum += v;
                damperCount++;
            }
        }
        // the car has one spring and one damper setting in the physics: the mean of its wheels'
        if (springCount > 0) out.filesuspensionSpringK = springSum / (float)springCount;
        if (damperCount > 0) out.fileDamper = damperSum / (float)damperCount;
    }
    out.name = "Eagle";

    // The chassis .dcol the descriptor names ("Shadow dcol filename") starts with the car's mass in kg (the kerb weight of its
    // text file), then a width, height and length (m) of the box the inertia comes from, a margin, and at byte 20 a vector
    // (x, y, z) that TDR2000.exe (body constructor 0x51E180) stores in the body and adds to every part position to make it
    // relative to the centre of mass. So the centre of mass is the negative of it, in metres from the car's origin (which is at
    // axle height): (0, -0.2, 0.1) for the Eagle puts it 0.2 m above the origin and 0.1 m towards the rear.
    const std::string dcolName = descriptorValue(dlines, "Shadow dcol filename");
    const int dcolEntry = dcolName.empty() ? -1 : pk->find(dcolName);
    std::vector<uint8_t> dcol;
    if (dcolEntry >= 0 && pk->extract(dcolEntry, dcol) && dcol.size() >= 36) {
        float f[9];
        memcpy(f, dcol.data(), sizeof f);
        if (f[0] > 1.0f) out.mass = f[0];
        out.centreOfMassOffset = Vec3f(f[5], -f[6], -f[7]);  // the negative vector; x is mirrored like the rest of the car, so it stays
        out.hasCentreOfMass = true;
        // The chassis hull follows the header: a count at byte 60, then that many points (x, y, z), in the car's own space.
        // The fractions are measured against its box, which is the physics shape: the body mesh also has the roof spike.
        uint32_t points = 0;
        if (dcol.size() >= 64) memcpy(&points, &dcol[60], 4);
        if (points >= 4 && points <= 4096 && dcol.size() >= 64 + (size_t)points * 12) {
            Vec3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (uint32_t i = 0; i < points; i++) {
                float p[3];
                memcpy(p, &dcol[64 + (size_t)i * 12], 12);
                const Vec3f v(-p[0], p[1], p[2]);  // mirrored like the rest of the car
                lo = vmin(lo, v);
                hi = vmax(hi, v);
            }
            out.hullBoxMin = lo;
            out.hullBoxMax = hi;
            out.hasHullBox = true;
        }
    }
    return true;
}

}  // namespace carma
