#include "revolt_car.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include "../binio.h"
#include "../vfs.h"

namespace minigame01 {

namespace {

struct Poly {
    int16_t type = 0, tex = -1;
    uint16_t idx[4] = {};
    uint32_t col[4] = {};
    float uv[4][2] = {};
};
struct Prm {
    std::vector<Poly> polys;
    std::vector<Vec3> verts;
};

// .prm: [polygon count u16][vertex count u16], polygons (60 bytes: type, tex, 4 indices, 4 colours, 4 uvs), vertices (position + normal).
bool readPrm(const std::vector<uint8_t>& data, Prm& m) {
    Reader r(data);
    uint32_t pc = r.u16(), vc = r.u16();
    if (!r.ok || r.left() < (size_t)pc * 60 + (size_t)vc * 24) return false;
    m.polys.resize(pc);
    for (auto& p : m.polys) {
        p.type = r.i16();
        p.tex = r.i16();
        for (auto& i : p.idx) i = r.u16();
        for (auto& c : p.col) c = r.u32();
        for (auto& uv : p.uv) { uv[0] = r.f32(); uv[1] = r.f32(); }
    }
    m.verts.resize(vc);
    for (auto& v : m.verts) {
        v = r.vec3();
        r.skip(12);
    }
    return r.ok;
}

std::string baseName(std::string path) {
    for (char& c : path) if (c == '\\') c = '/';
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// The car folder to use: Adeon when present (a well-behaved all-rounder), otherwise the first one.
std::string findCarParameters() {
    std::string first, adeon;
    for (const auto& e : vfs::files()) {
        const std::string& l = e.lower;
        static const std::string kParams = "/parameters.txt";
        if (l.size() < kParams.size() || l.compare(l.size() - kParams.size(), kParams.size(), kParams) != 0) continue;
        if (l.find("/cars/") == std::string::npos) continue;
        std::string dir = vfs::dirOf(e.path);
        if (vfs::findIn(dir, "body.prm") < 0) continue;
        if (first.empty()) first = e.path;
        if (vfs::lower(vfs::baseOf(dir)) == "adeon") adeon = e.path;
    }
    return adeon.empty() ? first : adeon;
}

}  // namespace

bool revoltCanDrive() { return !findCarParameters().empty(); }

bool loadRevoltCar(Scene& scene, CarModel& out) {
    std::string ppath = findCarParameters();
    std::string text;
    if (ppath.empty() || !vfs::readText(ppath, text)) return false;
    const std::string dir = vfs::dirOf(ppath);

    // Parameters.txt: MODEL n "file", TPAGE "file", WHEEL n { Offset1 x y z ... Radius r }
    std::string models[5], tpage;
    Vec3 wheelOffset[4];
    float wheelRadius = 10.0f;
    int wheel = -1;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t sc = line.find(';');
        if (sc != std::string::npos) line.resize(sc);
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        for (char& c : key) c = (char)toupper((unsigned char)c);
        if (key == "MODEL") {
            int n;
            std::string f;
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            ls >> n;
            if (n >= 0 && n < 5 && q1 != std::string::npos && q2 > q1) models[n] = baseName(line.substr(q1 + 1, q2 - q1 - 1));
        } else if (key == "TPAGE") {
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            if (q1 != std::string::npos && q2 > q1) tpage = baseName(line.substr(q1 + 1, q2 - q1 - 1));
        } else if (key == "WHEEL") {
            ls >> wheel;
            if (wheel < 0 || wheel > 3) wheel = -1;
        } else if (key == "OFFSET1" && wheel >= 0) {
            ls >> wheelOffset[wheel].x >> wheelOffset[wheel].y >> wheelOffset[wheel].z;
        } else if (key == "RADIUS" && wheel >= 0) {
            ls >> wheelRadius;
        } else if (key == "}") {
            wheel = -1;
        }
    }

    int tex = -1;
    {
        int fi = vfs::findIn(dir, tpage);
        std::vector<uint8_t> data;
        Image img;
        if (fi >= 0 && vfs::read(fi, data) && decodeBMP(data.data(), data.size(), img)) tex = scene.addTexture(std::move(img));
    }

    auto loadModel = [&](const std::string& name, Prm& m) {
        int fi = vfs::findIn(dir, name);
        std::vector<uint8_t> data;
        return fi >= 0 && vfs::read(fi, data) && readPrm(data, m);
    };
    Prm parts[5];
    for (int i = 0; i < 5; i++)
        if (!loadModel(models[i], parts[i])) return false;

    // metres: size the car like a 4.3 m car, whatever the game's units are
    float zmin = 1e30f, zmax = -1e30f;
    for (const Vec3& v : parts[0].verts) { zmin = std::min(zmin, v.z); zmax = std::max(zmax, v.z); }
    const float s = std::max(1.0f, (zmax - zmin) / 4.3f);
    out.unitsPerMetre = s;

    auto emit = [&](const Prm& m, BatchSet& dst) {
        for (const Poly& p : m.polys) {
            Blend bl = BLEND_OPAQUE;
            if (p.type & 0x100) bl = BLEND_ADD;
            else if (p.type & 0x4) bl = BLEND_ALPHA;
            int n = (p.type & 1) ? 4 : 3;
            SceneVertex v[4];
            for (int k = 0; k < n; k++) {
                Vec3 pos = m.verts[std::min<size_t>(p.idx[k], m.verts.size() - 1)];
                uint32_t c = p.col[k];
                v[k].x = pos.x / s; v[k].y = -pos.y / s; v[k].z = pos.z / s;  // y is down in Re-Volt
                v[k].r = (c >> 16) & 255; v[k].g = (c >> 8) & 255; v[k].b = c & 255;
                v[k].a = bl == BLEND_OPAQUE || bl == BLEND_ADD ? 255 : (uint8_t)(c >> 24 ? c >> 24 : 128);
                v[k].u = p.uv[k][0];
                v[k].v = p.uv[k][1];
            }
            uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
            dst.appendIndexed(p.tex >= 0 ? tex : -1, bl, v, n, idx, n == 4 ? 6 : 3, true);
        }
    };
    emit(parts[0], out.body);
    for (int i = 0; i < 4; i++) {
        emit(parts[1 + i], out.wheel[i]);
        out.wheelPos[i] = Vec3(wheelOffset[i].x, -wheelOffset[i].y, wheelOffset[i].z) * (1.0f / s);
    }
    out.wheelRadius = wheelRadius / s;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const Batch& b : out.body.batches)
        for (const SceneVertex& v : b.verts) {
            lo = vmin(lo, Vec3(v.x, v.y, v.z));
            hi = vmax(hi, Vec3(v.x, v.y, v.z));
        }
    out.boundsMin = lo;
    out.boundsMax = hi;
    out.wheelWidth = 0.25f;
    out.name = vfs::baseOf(dir);
    return true;
}

}  // namespace minigame01
