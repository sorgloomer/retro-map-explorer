// Re-Volt (Acclaim 1999): levels/<name>/<name>.w + instances (.fin / .prm) + annotation files.
// Native coordinates: +X right, +Y DOWN, +Z forward. We convert to Y-up by negating Y.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include "loader/binio.h"
#include "game.h"
#include "minigame01/revolt_car.h"
#include "vfs.h"

namespace {

struct RvPoly {
    int16_t type = 0, tex = -1;
    uint16_t idx[4] = {0, 0, 0, 0};
    uint32_t col[4] = {0, 0, 0, 0};
    float uv[4][2] = {};
};
struct RvVert { Vec3 pos; };
struct RvMesh {
    std::vector<RvPoly> polys;
    std::vector<RvVert> verts;
};

bool readMesh(Reader& r, RvMesh& m, bool worldHeader) {
    if (worldHeader) r.skip(12 + 4 + 24);
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
        v.pos = r.vec3();
        r.skip(12);  // normal
    }
    return r.ok;
}

// Row-major 3x3 rotation (rows are the local axes) + translation.
struct RvXform {
    Vec3 r0{1, 0, 0}, r1{0, 1, 0}, r2{0, 0, 1}, t;
    Vec3 apply(const Vec3& v) const { return r0 * v.x + r1 * v.y + r2 * v.z + t; }
};

inline Vec3 toWorld(const Vec3& v) { return {v.x, -v.y, v.z}; }

struct Loader {
    Scene& sc;
    std::string dir, rdir, stem;
    bool reversed = false;
    int pageTex[26];
    std::map<std::string, std::shared_ptr<RvMesh>> prmCache;
    BatchSet* dstSet = nullptr;  // when set, emit() writes into this set instead of the level

    void tri(int tex, Blend bl, const SceneVertex& a, const SceneVertex& b, const SceneVertex& c) {
        if (!dstSet) { sc.addTriangle(tex, bl, a, b, c); return; }
        SceneVertex v[3] = {a, b, c};
        uint32_t idx[3] = {0, 1, 2};
        dstSet->appendIndexed(tex, bl, v, 3, idx, 3);
    }

    explicit Loader(Scene& s) : sc(s) { for (int& p : pageTex) p = -2; }

    std::string pick(const std::string& name) {  // reversed-aware lookup
        if (reversed) {
            int i = vfs::findIn(rdir, name);
            if (i >= 0) return vfs::files()[i].path;
        }
        int i = vfs::findIn(dir, name);
        return i >= 0 ? vfs::files()[i].path : "";
    }

    int pageTexture(int page) {
        if (page < 0 || page >= 26) return -1;
        if (pageTex[page] != -2) return pageTex[page];
        pageTex[page] = -1;
        std::string name = stem + (char)('a' + page) + ".bmp";
        int fi = vfs::findIn(dir, name);
        std::vector<uint8_t> data;
        Image img;
        if (fi >= 0 && vfs::read(fi, data) && decodeBMP(data.data(), data.size(), img)) {
            // Re-Volt colour-keys pure black: those texels are see-through (leaves, fences, translucent decals).
            for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
                if (img.rgba[i] == 0 && img.rgba[i + 1] == 0 && img.rgba[i + 2] == 0) {
                    img.rgba[i + 3] = 0;
                    img.hasAlpha = true;
                }
            pageTex[page] = sc.addTexture(std::move(img));
        }
        return pageTex[page];
    }

    void emit(const RvMesh& m, const RvXform* xf, bool colorTint) {
        (void)colorTint;
        auto vert = [&](const RvPoly& p, int k, Blend bl) {
            const RvVert& rv = m.verts[std::min<size_t>(p.idx[k], m.verts.size() - 1)];
            Vec3 w = toWorld(xf ? xf->apply(rv.pos) : rv.pos);
            uint32_t c = p.col[k];
            SceneVertex v;
            v.x = w.x; v.y = w.y; v.z = w.z;
            v.r = (c >> 16) & 255; v.g = (c >> 8) & 255; v.b = c & 255;
            v.a = bl == BLEND_OPAQUE || bl == BLEND_CUTOUT ? 255 : (uint8_t)(c >> 24 ? c >> 24 : 128);
            if (bl == BLEND_ADD) v.a = 255;
            v.u = p.uv[k][0];
            v.v = p.uv[k][1];
            return v;
        };
        for (const RvPoly& p : m.polys) {
            Blend bl = BLEND_OPAQUE;
            if (p.type & 0x100) bl = BLEND_ADD;
            else if (p.type & 0x4) bl = BLEND_ALPHA;
            bool quad = p.type & 1;
            int tex = p.tex >= 0 ? pageTexture(p.tex) : -1;
            if (tex >= 0 && bl == BLEND_OPAQUE && sc.textures[tex].hasAlpha) bl = BLEND_CUTOUT;
            SceneVertex a = vert(p, 0, bl), b = vert(p, 1, bl), c = vert(p, 2, bl);
            tri(tex, bl, a, b, c);
            if (quad) tri(tex, bl, a, c, vert(p, 3, bl));
        }
    }

    std::shared_ptr<RvMesh> loadPrm(const std::string& name) {
        std::string key = vfs::lower(name);
        auto it = prmCache.find(key);
        if (it != prmCache.end()) return it->second;
        std::shared_ptr<RvMesh> res;
        int fi = vfs::findIn(dir, name + ".prm");
        std::vector<uint8_t> data;
        if (fi >= 0 && vfs::read(fi, data)) {
            Reader r(data);
            auto m = std::make_shared<RvMesh>();
            if (readMesh(r, *m, false)) res = m;
        }
        prmCache[key] = res;
        return res;
    }
};

struct Inf {
    std::string name;
    bool hasStart = false, hasStartRev = false;
    Vec3 start, startRev;
    float rot = 0, rotRev = 0.5f;
    Vec3 fog{80, 144, 192};
    bool hasFog = false;
};

Inf parseInf(const std::string& text) {
    Inf inf;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t sc = line.find(';');
        if (sc != std::string::npos) line.resize(sc);
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        for (char& c : key) c = (char)toupper((unsigned char)c);
        if (key == "NAME") {
            size_t q1 = line.find('\''), q2 = line.rfind('\'');
            if (q1 != std::string::npos && q2 > q1) inf.name = line.substr(q1 + 1, q2 - q1 - 1);
        } else if (key == "STARTPOS") {
            ls >> inf.start.x >> inf.start.y >> inf.start.z; inf.hasStart = true;
        } else if (key == "STARTPOSREV") {
            ls >> inf.startRev.x >> inf.startRev.y >> inf.startRev.z; inf.hasStartRev = true;
        } else if (key == "STARTROT") {
            ls >> inf.rot;
        } else if (key == "STARTROTREV") {
            ls >> inf.rotRev;
        } else if (key == "FOGCOLOR") {
            ls >> inf.fog.x >> inf.fog.y >> inf.fog.z; inf.hasFog = true;
        }
    }
    return inf;
}

class RevoltGame : public Game {
public:
    const char* id() const override { return "revolt"; }
    const char* name() const override { return "Re-Volt"; }
    const char* dropHint() const override { return "Re-Volt game folder (the one containing levels/)"; }
    bool canDrive() override { return minigame01::revoltCanDrive(); }
    float unitsPerMetre() const override { return 28.6f; }  // an Adeon is 123 units long: about 4.3 m
    bool loadCarModel(Scene& scene, minigame01::CarModel& out) override { return minigame01::loadRevoltCar(scene, out); }

    // Level directories: <anything>/levels/<x>/<x>.w
    std::vector<std::string> levelDirs() {
        std::vector<std::string> dirs;
        for (const auto& e : vfs::files()) {
            if (e.lower.size() < 3 || e.lower.compare(e.lower.size() - 2, 2, ".w") != 0) continue;
            std::string d = vfs::dirOf(e.path);
            std::string parent = vfs::lower(vfs::baseOf(vfs::dirOf(d)));
            if (parent != "levels") continue;
            if (vfs::lower(vfs::baseOf(d)) != vfs::lower(vfs::stemOf(e.path))) continue;
            dirs.push_back(d);
        }
        return dirs;
    }

    bool detect() override { return !levelDirs().empty(); }

    // Tracks grouped by "world", in the order the campaign presents them; battle / stunt arenas and the menu come last.
    struct Slot { const char* stem; const char* group; int order; };
    static const Slot* slotFor(const std::string& stemLower) {
        static const Slot slots[] = {
            {"nhood1", "Toys in the Hood", 10}, {"nhood2", "Toys in the Hood", 11},
            {"market1", "Supermarket", 20}, {"market2", "Supermarket", 21},
            {"toylite", "Toy World", 30}, {"toy2", "Toy World", 31},
            {"wild_west1", "Ghost Town", 40}, {"wild_west2", "Ghost Town", 41},
            {"muse1", "Museum", 50}, {"muse2", "Museum", 51},
            {"ship1", "Toytanic", 60}, {"ship2", "Toytanic", 61},
            {"garden1", "Botanical Garden", 70},
            {"bot_bat", "Battle tag & stunt arenas", 80}, {"markar", "Battle tag & stunt arenas", 81},
            {"muse_bat", "Battle tag & stunt arenas", 82}, {"nhood1_battle", "Battle tag & stunt arenas", 83},
            {"stunts", "Battle tag & stunt arenas", 84},
            {"frontend", "Menu & other", 90}, {"user000", "Menu & other", 91},
        };
        for (const Slot& sl : slots)
            if (stemLower == sl.stem) return &sl;
        return nullptr;
    }

    std::vector<LevelInfo> listLevels() override {
        struct Entry { int order; std::string stem; std::vector<LevelInfo> infos; };
        std::vector<Entry> entries;
        for (const std::string& d : levelDirs()) {
            std::string stem = vfs::baseOf(d);
            std::string title = stem;
            std::string text;
            int fi = vfs::findIn(d, stem + ".inf");
            if (fi >= 0 && vfs::readText(vfs::files()[fi].path, text)) {
                Inf inf = parseInf(text);
                if (!inf.name.empty()) title = inf.name;
            }
            const Slot* sl = slotFor(vfs::lower(stem));
            std::string group = sl ? sl->group : "Other levels";
            Entry e;
            e.order = sl ? sl->order : 99;
            e.stem = vfs::lower(stem);
            e.infos.push_back({d, title, group});
            for (const std::string& sd : vfs::subdirs(d))
                if (vfs::lower(sd) == "reversed") e.infos.push_back({d + "|rev", title + " (reversed)", group});
            entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.order != b.order ? a.order < b.order : a.stem < b.stem;
        });
        std::vector<LevelInfo> out;
        for (auto& e : entries)
            for (auto& li : e.infos) out.push_back(std::move(li));
        return out;
    }

    bool load(const std::string& levelId, Scene& sc, std::string& err) override {
        Loader L(sc);
        std::string id = levelId;
        if (id.size() > 4 && id.compare(id.size() - 4, 4, "|rev") == 0) {
            L.reversed = true;
            id.resize(id.size() - 4);
        }
        L.dir = id;
        L.rdir = id + "/reversed";
        L.stem = vfs::baseOf(id);

        std::string text;
        int fi = vfs::findIn(L.dir, L.stem + ".inf");
        Inf inf;
        if (fi >= 0 && vfs::readText(vfs::files()[fi].path, text)) inf = parseInf(text);
        sc.title = (inf.name.empty() ? L.stem : inf.name) + (L.reversed ? " (reversed)" : "");
        if (inf.hasFog)
            sc.background = {inf.fog.x / 255.f * 0.8f, inf.fog.y / 255.f * 0.8f, inf.fog.z / 255.f * 0.8f};

        // --- world geometry
        std::vector<uint8_t> data;
        fi = vfs::findIn(L.dir, L.stem + ".w");
        if (fi < 0 || !vfs::read(fi, data)) { err = "cannot read world file"; return false; }
        {
            Reader r(data);
            int32_t meshCount = r.i32();
            if (!r.ok || meshCount < 0 || meshCount > 100000) { err = "bad .w file"; return false; }
            for (int i = 0; i < meshCount; i++) {
                RvMesh m;
                if (!readMesh(r, m, true)) { err = "truncated .w file"; return false; }
                L.emit(m, nullptr, false);
            }
        }

        // --- instances
        fi = vfs::findIn(L.dir, L.stem + ".fin");
        std::string finPath = L.pick(L.stem + ".fin");
        int instCount = 0, instMissing = 0;
        if (!finPath.empty() && vfs::read(vfs::find(finPath), data)) {
            Reader r(data);
            int32_t n = r.i32();
            for (int i = 0; i < n && r.ok; i++) {
                size_t base = r.pos;
                std::string name = r.fixedString(9);
                RvXform xf;
                r.pos = base + 24;
                xf.t = r.vec3();
                xf.r0 = r.vec3(); xf.r1 = r.vec3(); xf.r2 = r.vec3();
                if (!r.ok) break;
                auto mesh = L.loadPrm(name);
                if (!mesh) { instMissing++; continue; }
                L.emit(*mesh, &xf, true);
                instCount++;
            }
        }

        // --- markers
        int cStart = sc.addCategory("start", "Start position", 0x39ff7a, SHAPE_ARROW, 2.2f, true);
        int cPick = sc.addCategory("pickup", "Pickups", 0xffd23f, SHAPE_DIAMOND, 1.0f, true);
        int cStar = sc.addCategory("star", "Star pickups (practice)", 0xff8a1f, SHAPE_DIAMOND, 1.1f, true);
        int cPath = sc.addCategory("path", "Race line (position nodes)", 0x37c8ff, SHAPE_DIAMOND, 1.0f, false, true, true);
        int cLight = sc.addCategory("light", "Lights", 0xfff3a0, SHAPE_DIAMOND, 0.7f, false, false);
        int cZone = sc.addCategory("zone", "Track zones", 0x66e0a3, SHAPE_CUBE, 0.6f, false, false, true);
        int cTrig = sc.addCategory("trigger", "Triggers (arrows / reposition)", 0xff7ad9, SHAPE_CUBE, 0.6f, false, false, true);
        int cField = sc.addCategory("field", "Force fields", 0xa88bff, SHAPE_DIAMOND, 1.0f, true, false);
        int cMirror = sc.addCategory("mirror", "Mirror planes", 0xbfefff, SHAPE_CUBE, 0.6f, false, false, true);
        int cObj = sc.addCategory("object", "Other objects (.fob)", 0xff8a3d, SHAPE_CUBE, 0.8f, false, false);

        float twoPi = 6.2831853f;
        if (L.reversed ? inf.hasStartRev : inf.hasStart) {
            Vec3 p = toWorld(L.reversed ? inf.startRev : inf.start);
            float rot = (L.reversed ? inf.rotRev : inf.rot) * twoPi;
            sc.addMarker(cStart, p, rot, L.reversed ? "Start position (reversed)" : "Start position");
            sc.hasCameraStart = true;
            sc.cameraPos = p + Vec3(std::sin(rot), 0, std::cos(rot)) * -900.0f + Vec3(0, 700, 0);
            sc.cameraYaw = rot;
            sc.cameraPitch = -0.45f;
        }

        auto readFile = [&](const std::string& ext, std::vector<uint8_t>& out) {
            std::string pth = L.pick(L.stem + ext);
            return !pth.empty() && vfs::read(vfs::find(pth), out);
        };

        // objects (.fob): 56 byte records
        if (readFile(".fob", data)) {
            Reader r(data);
            int32_t n = r.i32();
            for (int i = 0; i < n && r.has(56); i++) {
                size_t b = r.pos;
                uint32_t type = r.u32();
                r.pos = b + 20;
                Vec3 p = toWorld(r.vec3());
                r.pos = b + 56;
                if (type == 30) sc.addMarker(cPick, p, 0, "Pickup");
                else if (type == 50) sc.addMarker(cStar, p, 0, "Practice star (type 50)");
                else sc.addMarker(cObj, p, 0, "Object (type " + std::to_string(type) + ")");
            }
        }
        // position nodes (.pan)
        if (readFile(".pan", data)) {
            Reader r(data);
            int32_t n = r.i32();
            r.skip(8);
            std::vector<Vec3> pts;
            std::vector<std::vector<int>> links;
            for (int i = 0; i < n && r.has(48); i++) {
                Vec3 p = toWorld(r.vec3());
                r.skip(4);
                int32_t prev[4], next[4];
                for (auto& v : prev) v = r.i32();
                for (auto& v : next) v = r.i32();
                pts.push_back(p);
                links.push_back({next[0], next[1], next[2], next[3]});
            }
            for (size_t i = 0; i < pts.size(); i++)
                for (int nx : links[i])
                    if (nx >= 0 && nx < (int)pts.size()) sc.addLine(cPath, pts[i], pts[nx]);
        }
        // zones (.taz): 64 byte records: id, pos, 3x3 matrix, size
        auto readBoxes = [&](const std::string& ext, int hdr, int cat, const char* label) {
            if (!readFile(ext, data)) return;
            Reader r(data);
            int32_t n = r.i32();
            int stride = hdr + 12 + 36 + 12;
            for (int i = 0; i < n && r.has(stride); i++) {
                size_t b = r.pos;
                int32_t a = r.i32();
                int32_t bb = hdr == 8 ? r.i32() : 0;
                Vec3 pos = r.vec3();
                Vec3 m0 = r.vec3(), m1 = r.vec3(), m2 = r.vec3();
                Vec3 size = r.vec3();
                r.pos = b + stride;
                Vec3 c = toWorld(pos);
                Vec3 ax = toWorld(m0 * size.x), ay = toWorld(m1 * size.y), az = toWorld(m2 * size.z);
                sc.addBox(cat, c, ax, ay, az);
                std::string text = std::string(label) + " #" + std::to_string(i);
                if (hdr == 8) text += " (type " + std::to_string(a) + "/" + std::to_string(bb) + ")";
                sc.addMarker(cat, c, 0, text);
            }
        };
        readBoxes(".taz", 4, cZone, "Track zone");
        readBoxes(".tri", 8, cTrig, "Trigger");
        // lights (.lit): pos, reach, ...
        if (readFile(".lit", data)) {
            Reader r(data);
            int32_t n = r.i32();
            for (int i = 0; i < n && r.has(72); i++) {
                size_t b = r.pos;
                Vec3 p = toWorld(r.vec3());
                float reach = r.f32();
                r.pos = b + 72;
                sc.addMarker(cLight, p, 0, "Light (reach " + std::to_string((int)reach) + ")");
            }
        }
        // force fields (.fld)
        if (readFile(".fld", data)) {
            Reader r(data);
            int32_t n = r.i32();
            for (int i = 0; i < n && r.has(100); i++) {
                size_t b = r.pos;
                int32_t type = r.i32();
                Vec3 p = toWorld(r.vec3());
                r.pos = b + 100;
                sc.addMarker(cField, p, 0, "Force field (type " + std::to_string(type) + ")");
            }
        }
        // mirrors (.rim): u16 count, 92 byte records
        {
            std::string pth = L.pick(L.stem + ".rim");
            if (!pth.empty() && vfs::read(vfs::find(pth), data)) {
                Reader r(data);
                int n = r.i16();
                for (int i = 0; i < n && r.has(92); i++) {
                    size_t b = r.pos;
                    r.skip(4 + 12 + 4 + 24);
                    Vec3 q[4];
                    for (auto& v : q) v = toWorld(r.vec3());
                    r.pos = b + 92;
                    for (int k = 0; k < 4; k++) sc.addLine(cMirror, q[k], q[(k + 1) % 4]);
                    sc.addMarker(cMirror, (q[0] + q[2]) * 0.5f, 0, "Mirror plane");
                }
            }
        }

        // In-game pickup / star models: models/pickup.m and star.m have the same layout as a .prm.
        sc.pickupRadius = 46.0f;  // both models measure about this around their origin, centred on it
        sc.pickupCentreY = 0.0f;
        sc.pickupBottomY = -38.0f;  // the pickup reaches down to -42, the star to -35
        {
            std::string models = vfs::dirOf(vfs::dirOf(L.dir)) + "/models/";
            auto loadModel = [&](const char* file) {
                int mi = vfs::find(models + file);
                std::vector<uint8_t> md;
                if (mi < 0 || !vfs::read(mi, md)) return -1;
                Reader pr(md);
                RvMesh pm;
                if (!readMesh(pr, pm, false)) return -1;
                sc.moverGeoms.emplace_back();
                L.dstSet = &sc.moverGeoms.back();
                L.emit(pm, nullptr, false);
                L.dstSet = nullptr;
                return (int)sc.moverGeoms.size() - 1;
            };
            int pickGeom = loadModel("pickup.m"), starGeom = loadModel("star.m");
            for (size_t k = 0; k < sc.markers.size(); k++) {
                int c = sc.markers[k].cat;
                if (c != cPick && c != cStar) continue;
                int gi = (c == cStar && starGeom >= 0) ? starGeom : pickGeom;
                if (gi < 0) continue;
                Mover mv;
                mv.geom = gi;
                mv.cat = c;
                mv.fixed = true;
                mv.powerupMesh = true;
                mv.path = {sc.markers[k].pos};
                sc.movers.push_back(std::move(mv));
                sc.cats[c].hasMesh = true;
            }
        }

        char buf[256];
        snprintf(buf, sizeof buf, "%zu triangles, %d model instances%s", sc.triCount, instCount,
                 instMissing ? (" (" + std::to_string(instMissing) + " models missing)").c_str() : "");
        sc.info = buf;
        return true;
    }
};

}  // namespace

Game* makeRevolt() { return new RevoltGame(); }
