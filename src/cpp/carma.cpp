// Carmageddon TDR 2000 (Torus Games, 2000).
//   ASSETS/Tracks/<track>/<track>.txt            level descriptor (text)
//   ASSETS/Tracks/<track>/**/<name>.dir + .pak   archives: names in a prefix-compressed trie (.dir),
//                                                 payloads zlib-compressed and header-obfuscated (.pak)
//   <name>.hie    text scene graph: matrices / textures / meshes / paths
//   <name>.mshs   binary meshes (position, RGBA colour, uv, triangle list)
//   <name>_NxN_32.tga / _8.tga   textures
//   .lins         spline point lists, .pup powerups, _VOL.scol checkpoint volumes
// Native coordinates are Y-up; no conversion except an optional X mirror (kMirrorX).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <unordered_map>
#include "binio.h"
#include "game.h"
#include "inflate.h"
#include "vfs.h"

namespace {

constexpr bool kMirrorX = true;  // the game is left-handed: mirror X so the maps read like in-game

// ---------------------------------------------------------------- text helpers
std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string l = t.substr(p, e - p);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        size_t s = 0;
        while (s < l.size() && (l[s] == ' ' || l[s] == '\t')) s++;
        out.push_back(l.substr(s));
        p = e + 1;
    }
    return out;
}

std::vector<std::string> quotedStrings(const std::string& line) {
    std::vector<std::string> r;
    size_t p = 0;
    while (true) {
        size_t a = line.find('"', p);
        if (a == std::string::npos) break;
        size_t b = line.find('"', a + 1);
        if (b == std::string::npos) break;
        r.push_back(line.substr(a + 1, b - a - 1));
        p = b + 1;
    }
    return r;
}

std::string stripComment(const std::string& l) {
    bool inq = false;
    for (size_t i = 0; i + 1 < l.size(); i++) {
        if (l[i] == '"') inq = !inq;
        if (!inq && l[i] == '/' && l[i + 1] == '/') return l.substr(0, i);
    }
    return l;
}

bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// key -> remaining text of the first line that starts with the key (comments removed).
struct KeyFile {
    std::vector<std::pair<std::string, std::string>> items;
    void parse(const std::string& text) {
        for (const std::string& raw : splitLines(text)) {
            std::string l = stripComment(raw);
            size_t sp = 0;
            while (sp < l.size() && l[sp] != ' ' && l[sp] != '\t') sp++;
            if (sp == 0) continue;
            std::string key = l.substr(0, sp);
            size_t v = sp;
            while (v < l.size() && (l[v] == ' ' || l[v] == '\t')) v++;
            items.push_back({key, l.substr(v)});
        }
    }
    const std::string* get(const char* key) const {
        for (auto& it : items)
            if (it.first == key) return &it.second;
        return nullptr;
    }
    std::string str(const char* key) const {
        const std::string* v = get(key);
        if (!v) return "";
        auto q = quotedStrings(*v);
        return q.empty() ? *v : q[0];
    }
    bool nums(const char* key, float* out, int n) const {
        const std::string* v = get(key);
        if (!v) return false;
        std::istringstream is(*v);
        for (int i = 0; i < n; i++)
            if (!(is >> out[i])) return false;
        return true;
    }
};

// ---------------------------------------------------------------- pak / dir
struct DirEntry {
    std::string name;
    uint32_t off = 0, size = 0;
};

struct Pak {
    std::string base;
    int pakFile = -1;
    std::vector<DirEntry> entries;
    std::unordered_map<std::string, int> byName;  // lower-case name
    std::unordered_map<std::string, int> textures;  // lower-case texture name -> entry of best .tga

    bool open(int dirFile, int pakFileIdx, const std::string& baseName) {
        base = baseName;
        pakFile = pakFileIdx;
        std::vector<uint8_t> d;
        if (!vfs::read(dirFile, d)) return false;
        parseDir(d);
        for (size_t i = 0; i < entries.size(); i++) byName[vfs::lower(entries[i].name)] = (int)i;
        indexTextures();
        return true;
    }

    void parseDir(const std::vector<uint8_t>& d) {
        char fn[512];
        bool rb[512] = {false};
        int i = 0, sbase = 0;
        size_t p = 0;
        while (p + 1 < d.size()) {
            int code;
            do {
                if (p + 1 >= d.size() || i >= 500) return;
                fn[i] = (char)d[p];
                code = d[p + 1];
                p += 2;
                if (code == 136) sbase = i;
                if (code == 192) rb[i] = true;
                i++;
            } while (!(code & 8));
            if (p + 8 > d.size()) return;
            DirEntry e;
            e.name.assign(fn, i);
            memcpy(&e.off, &d[p], 4);
            memcpy(&e.size, &d[p + 4], 4);
            p += 8;
            entries.push_back(std::move(e));
            if (code == 72) {
            } else if (code == 136) {
                i = sbase;
            } else if (code == 200) {
                if (i > 0) rb[i - 1] = true;
            } else {
                int j = -1;
                for (int k = 511; k >= 0; k--)
                    if (rb[k]) { j = k; break; }
                if (j >= 0) rb[j] = false;
                i = j < 0 ? 0 : j;
            }
        }
    }

    // "<name>_<w>x<h>_<bits>.tga"  ->  name (prefer 32 bit variants)
    void indexTextures() {
        std::unordered_map<std::string, int> bits;
        for (size_t i = 0; i < entries.size(); i++) {
            std::string n = vfs::lower(entries[i].name);
            if (!endsWith(n, ".tga")) continue;
            n.resize(n.size() - 4);
            size_t u1 = n.find_last_of('_');
            if (u1 == std::string::npos) continue;
            int b = atoi(n.c_str() + u1 + 1);
            n.resize(u1);
            size_t u2 = n.find_last_of('_');
            if (u2 == std::string::npos) continue;
            bool plainNumber = n.find_first_not_of("0123456789", u2 + 1) == std::string::npos;
            n.resize(u2);
            if (plainNumber) {  // "<name>_<w>_<h>_<bits>" spelling: the width comes before the height
                size_t u3 = n.find_last_of('_');
                if (u3 != std::string::npos && n.find_first_not_of("0123456789", u3 + 1) == std::string::npos) n.resize(u3);
            }
            auto it = bits.find(n);
            if (it == bits.end() || b > it->second) {
                bits[n] = b;
                textures[n] = (int)i;
            }
        }
    }

    int find(const std::string& name) const {
        auto it = byName.find(vfs::lower(name));
        return it == byName.end() ? -1 : it->second;
    }

    bool extract(int e, std::vector<uint8_t>& out) const {
        const DirEntry& de = entries[e];
        std::vector<uint8_t> raw;
        if (!vfs::readRange(pakFile, de.off, de.size, raw)) return false;
        if (raw.size() >= 8 && (raw[0] ^ raw[1]) == 0x7a && (raw[2] ^ raw[3]) == 0x0e) {
            uint32_t exp = (raw[4] ^ raw[7]) | ((raw[5] ^ raw[7]) << 8) | ((raw[6] ^ raw[7]) << 16);
            return inflateZlib(raw.data() + 8, raw.size() - 8, out, exp);
        }
        out = std::move(raw);
        return true;
    }
};

// ---------------------------------------------------------------- hie
struct Hie {
    struct Mat { Mat4 m; std::string name; };
    struct Node { int type, index, child, sibling; };
    std::vector<std::string> textures, meshNames, lineNames, collNames;
    std::vector<int> matFlags;  // flags of the "materials" section; bit 8 = additive (glowing) surfaces
    std::vector<Mat> mats;
    std::vector<Node> nodes;

    static bool section(const std::vector<std::string>& L, const char* key, std::vector<std::string>& items) {
        std::string head = std::string("// Number of ") + key;
        size_t k = 0;
        for (; k < L.size(); k++)
            if (L[k].compare(0, head.size(), head) == 0) break;
        if (k + 1 >= L.size()) return false;
        items.clear();
        for (size_t i = k + 2; i < L.size(); i++) {
            if (L[i].empty()) continue;
            if (L[i].compare(0, 2, "//") == 0) {
                if (L[i].compare(0, 13, "// Number of ") == 0) break;
                continue;
            }
            items.push_back(L[i]);
        }
        return true;
    }

    static std::string unquote(const std::string& s) {
        size_t a = s.find('"'), b = s.rfind('"');
        return (a != std::string::npos && b > a) ? s.substr(a + 1, b - a - 1) : s;
    }

    bool parse(const std::string& text) {
        std::vector<std::string> L;
        for (std::string& l : splitLines(text))
            if (!l.empty()) L.push_back(std::move(l));
        std::vector<std::string> it;
        if (section(L, "Collision Data", it)) for (auto& s : it) collNames.push_back(unquote(s));
        if (section(L, "lines", it)) for (auto& s : it) lineNames.push_back(unquote(s));
        if (section(L, "textures", it)) for (auto& s : it) textures.push_back(unquote(s));
        if (section(L, "meshes", it)) for (auto& s : it) meshNames.push_back(unquote(s));
        if (section(L, "materials", it))
            for (auto& s : it) {
                std::istringstream is(s);
                std::string tok, last;
                while (is >> tok) last = tok;
                matFlags.push_back(atoi(last.c_str()));
            }
        if (section(L, "matrices", it)) {
            for (size_t i = 0; i + 4 < it.size(); i += 5) {
                float v[16];
                bool good = true;
                for (int r = 0; r < 4; r++) {
                    std::string row = it[i + r];
                    for (char& c : row)
                        if (c == ';') c = ' ';
                    std::istringstream is(row);
                    for (int c = 0; c < 4; c++)
                        if (!(is >> v[r * 4 + c])) good = false;
                }
                if (!good) return false;
                Mat m;
                // The file uses the row-vector convention (translation in the 4th row). Mat4 is column-major,
                // so copying the rows verbatim stores the transpose, i.e. the column-vector form.
                for (int k = 0; k < 16; k++) m.m.m[k] = v[k];
                m.name = unquote(it[i + 4]);
                mats.push_back(m);
            }
        }
        if (section(L, "nodes", it)) {
            for (auto& s : it) {
                std::istringstream is(s);
                std::string a, b, c, d;
                if (!(is >> a >> b >> c >> d)) continue;
                nodes.push_back({atoi(a.c_str()), atoi(b.c_str()), c == "NULL" ? -1 : atoi(c.c_str()),
                                 d == "NULL" ? -1 : atoi(d.c_str())});
            }
        }
        return !nodes.empty();
    }
};

struct CMesh {
    std::vector<float> v;  // 9 floats per vertex: pos(3) rgba(4) uv(2)
    std::vector<uint32_t> idx;
    size_t nv() const { return v.size() / 9; }
};

bool parseMshs(const std::vector<uint8_t>& data, std::vector<CMesh>& out) {
    Reader r(data);
    while (r.left() >= 24) {
        uint32_t hdr = r.u32();
        uint32_t tris = hdr & 0xffff;
        if ((hdr >> 16) == 0) {
            // "NGon" meshes (water, laser pipes): [npoly][total verts][sphere 16] then per polygon
            // [k][face normal 12] + k * {pos 12, normal 12, rgba 16, uv 8}
            r.skip(4 + 16);
            CMesh m;
            for (uint32_t pi = 0; pi < tris; pi++) {
                uint32_t k = r.u32();
                r.skip(12);
                if (!r.ok || k < 3 || k > 256 || r.left() < (size_t)k * 48) return false;
                uint32_t base = (uint32_t)m.nv();
                for (uint32_t j = 0; j < k; j++) {
                    Vec3 pos = r.vec3();
                    r.skip(12);
                    float col[4] = {r.f32(), r.f32(), r.f32(), r.f32()};
                    float u = r.f32(), v = r.f32();
                    m.v.insert(m.v.end(), {pos.x, pos.y, pos.z, col[0], col[1], col[2], col[3], u, v});
                }
                for (uint32_t j = 1; j + 1 < k; j++) m.idx.insert(m.idx.end(), {base, base + j, base + j + 1});
            }
            if (!r.ok) return false;
            out.push_back(std::move(m));
            continue;
        }
        size_t h0 = r.pos - 4;  // start of this mesh header
        r.skip(16);
        uint32_t nv = r.u32();
        if (!r.ok || r.left() < (size_t)nv * 36 + (size_t)tris * 12) {
            // Third layout (gorilla, sharks): [hdr][nv][sphere 16][nv positions] then per triangle 132 bytes:
            // face normal 12, 3 indices, 3 * rgba, 3 * uv, 3 vertex normals. Some data trails the triangles.
            const uint8_t* b0 = data.data() + h0;
            uint32_t nv2;
            memcpy(&nv2, b0 + 4, 4);
            size_t need = 24 + (size_t)nv2 * 12 + (size_t)tris * 132;
            if (h0 + need > data.size() || nv2 == 0) return false;
            const float* pos = (const float*)(b0 + 24);
            const uint8_t* rec = b0 + 24 + (size_t)nv2 * 12;
            CMesh m;
            for (uint32_t t = 0; t < tris; t++, rec += 132) {
                uint32_t id[3];
                memcpy(id, rec + 12, 12);
                const float* rf = (const float*)(rec + 24);  // 3 * rgba, then 3 * uv
                for (int c = 0; c < 3; c++) {
                    uint32_t i = id[c] < nv2 ? id[c] : 0;
                    m.idx.push_back((uint32_t)m.nv());
                    m.v.insert(m.v.end(), {pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], rf[c * 4], rf[c * 4 + 1], rf[c * 4 + 2], rf[c * 4 + 3], rf[12 + c * 2], rf[12 + c * 2 + 1]});
                }
            }
            out.push_back(std::move(m));
            return true;
        }
        CMesh m;
        m.v.resize((size_t)nv * 9);
        memcpy(m.v.data(), r.p + r.pos, (size_t)nv * 36);
        r.skip((size_t)nv * 36);
        m.idx.resize((size_t)tris * 3);
        memcpy(m.idx.data(), r.p + r.pos, (size_t)tris * 12);
        r.skip((size_t)tris * 12);
        for (uint32_t& i : m.idx)
            if (i >= nv) i = 0;
        out.push_back(std::move(m));
    }
    return true;
}

bool parseLins(const std::vector<uint8_t>& data, std::vector<std::vector<Vec3>>& out) {
    // Single-spline ".lin" files are text: a point count followed by "x y z" lines.
    if (data.size() > 2 && data[0] >= '0' && data[0] <= '9' && data[1] != 0) {
        std::istringstream is(std::string((const char*)data.data(), data.size()));
        int n = 0;
        if (!(is >> n) || n < 0 || n > 100000) return false;
        std::vector<Vec3> pts;
        for (int i = 0; i < n; i++) {
            Vec3 p;
            if (!(is >> p.x >> p.y >> p.z)) break;
            pts.push_back(p);
        }
        if (!pts.empty()) out.push_back(std::move(pts));
        return true;
    }
    Reader r(data);
    while (r.left() >= 8) {
        uint32_t nl = r.u32();
        if (nl > 4096) return false;
        r.skip(nl);
        uint32_t np = r.u32();
        if (!r.ok || r.left() < (size_t)np * 12) return false;
        std::vector<Vec3> pts(np);
        for (auto& p : pts) p = r.vec3();
        out.push_back(std::move(pts));
    }
    return true;
}

inline Vec3 fixX(Vec3 v) { if (kMirrorX) v.x = -v.x; return v; }
inline float fixA(float a) { return kMirrorX ? -a : a; }  // heading angle (sin a, cos a) of a mirrored direction

// ---------------------------------------------------------------- level assembly
class CarmaGame : public Game {
public:
    const char* id() const override { return "carma"; }
    const char* name() const override { return "Carmageddon TDR 2000"; }
    const char* dropHint() const override { return "Carmageddon TDR 2000 folder (the one containing ASSETS/)"; }

    // Directories directly below a folder called "Tracks".
    std::vector<std::string> trackDirs() {
        std::vector<std::string> r;
        std::unordered_map<std::string, bool> seen;
        for (const auto& e : vfs::files()) {
            std::string d = vfs::dirOf(e.path);
            if (d.empty() || vfs::lower(vfs::baseOf(vfs::dirOf(d))) != "tracks") continue;
            if (seen.emplace(vfs::lower(d), true).second) r.push_back(d);
        }
        std::sort(r.begin(), r.end(), [](const std::string& a, const std::string& b) { return vfs::lower(a) < vfs::lower(b); });
        return r;
    }

    // Opens every <x>.dir/<x>.pak pair that sits directly in `dir`.
    void openDirPaks(const std::string& dir, std::vector<std::unique_ptr<Pak>>& into) {
        std::string pre = vfs::lower(dir) + "/";
        for (size_t i = 0; i < vfs::files().size(); i++) {
            const auto& e = vfs::files()[i];
            if (!endsWith(e.lower, ".dir") || e.lower.compare(0, pre.size(), pre) != 0) continue;
            if (e.lower.find('/', pre.size()) != std::string::npos) continue;
            int pk = vfs::findIn(dir, vfs::stemOf(e.path) + ".pak");
            if (pk < 0) continue;
            auto p = std::make_unique<Pak>();
            if (p->open((int)i, pk, vfs::stemOf(e.path))) into.push_back(std::move(p));
        }
    }

    static bool readFromPaks(const std::vector<std::unique_ptr<Pak>>& paks, const std::string& name, std::vector<uint8_t>& out) {
        for (const auto& p : paks) {
            int e = p->find(name);
            if (e >= 0 && p->extract(e, out)) return true;
        }
        return false;
    }

    bool detect() override {
        for (const std::string& d : trackDirs())
            if (!vfs::subdirs(d).empty() || vfs::findIn(d, vfs::baseOf(d) + ".txt") >= 0) return true;
        return false;
    }

    // Races_Strings_*.txt lists every race / mission of the campaign in the order the game presents them.
    struct RaceRow { std::string name, world, display; };
    std::vector<RaceRow> readRaceTable() {
        std::vector<RaceRow> rows;
        int i = locate("Races_Strings_UK.txt", "", "");
        if (i < 0) i = locate("Races_Strings_US.txt", "", "");
        std::string t;
        if (i < 0 || !vfs::readText(vfs::files()[i].path, t)) return rows;
        for (const std::string& l : splitLines(t)) {
            auto q = quotedStrings(stripComment(l));
            if (l.compare(0, 4, "RACE") == 0) {
                size_t a = l.find('"');
                size_t b = a == std::string::npos ? a : l.find('"', a + 1);
                if (b != std::string::npos) rows.push_back({l.substr(a + 1, b - a - 1), "", ""});
            } else if (!rows.empty() && !q.empty()) {
                if (l.compare(0, 5, "WORLD") == 0) rows.back().world = q[0];
                else if (l.compare(0, 12, "DISPLAY_NAME") == 0) rows.back().display = q[0];
            }
        }
        return rows;
    }

    std::vector<LevelInfo> listLevels() override {
        struct Entry { int order; std::string key; LevelInfo info; };
        std::vector<Entry> entries;
        std::vector<RaceRow> table = readRaceTable();
        for (const std::string& d : trackDirs()) {
            std::string nm = vfs::baseOf(d);
            std::vector<uint8_t> data;
            int li = vfs::findIn(d, nm + ".txt");
            bool ok = li >= 0 && vfs::read(li, data);
            if (!ok) {
                std::vector<std::unique_ptr<Pak>> paks;
                openDirPaks(d, paks);
                ok = readFromPaks(paks, nm + ".txt", data);
            }
            if (!ok) continue;
            KeyFile kf;
            kf.parse(std::string((const char*)data.data(), data.size()));
            if (!kf.get("STATIC_MESH_DESCRIPTOR")) continue;
            std::string title = kf.str("NAME");
            if (title.empty()) title = nm;
            std::string lnm = vfs::lower(nm), group;
            int order = 1000;
            for (size_t k = 0; k < table.size(); k++)
                if (vfs::lower(table[k].name) == lnm) {
                    order = (int)k;
                    group = table[k].world.empty() ? nm : table[k].world;
                    if (!table[k].display.empty()) title += " \xe2\x80\x93 " + table[k].display;
                    break;
                }
            {
                std::string lt = vfs::lower(title);
                if (order != 1000 && lt.find("multiplayer") != std::string::npos) { order = 4000 + order; group = "Multiplayer & arenas"; }
                else if (order != 1000 && lt.find("free driving") != std::string::npos) { order = 2000 + order; group = "Free driving"; }
            }
            if (order == 1000) {
                bool addon = lnm.compare(0, 5, "1920s") == 0 || lnm.compare(0, 4, "hell") == 0 || lnm.compare(0, 14, "super speedway") == 0;
                bool freeDrive = vfs::lower(title).find("free driving") != std::string::npos;
                if (addon) { group = "Add-on levels"; order = 3000; }
                else if (freeDrive) { group = "Free driving"; order = 2000; }
                else { group = "Multiplayer & arenas"; order = 4000; }
            }
            entries.push_back({order, lnm, {d, title, group}});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.order != b.order ? a.order < b.order : a.key < b.key;
        });
        std::vector<LevelInfo> out;
        for (auto& e : entries) out.push_back(std::move(e.info));
        return out;
    }

    // ---- file resolution
    std::unordered_map<std::string, std::vector<int>> byBase;
    void indexBases() {
        if (!byBase.empty()) return;
        for (size_t i = 0; i < vfs::files().size(); i++) byBase[vfs::lower(vfs::baseOf(vfs::files()[i].path))].push_back((int)i);
    }
    // Finds loose file `name`, preferring files under `first`, then `second`.
    int locate(const std::string& name, const std::string& first, const std::string& second) {
        indexBases();
        auto it = byBase.find(vfs::lower(name));
        if (it == byBase.end()) return -1;
        std::string f1 = vfs::lower(first) + "/", f2 = vfs::lower(second) + "/";
        int best = -1, bestScore = -1;
        for (int i : it->second) {
            const std::string& lp = vfs::files()[i].lower;
            int score = lp.compare(0, f1.size(), f1) == 0 ? 3 : (!second.empty() && lp.compare(0, f2.size(), f2) == 0) ? 2 : 1;
            if (score > bestScore) { best = i; bestScore = score; }
        }
        return best;
    }

    // ---- per-load state
    Scene* sc = nullptr;
    BatchSet* coronaDst = nullptr;  // when set, meshes under a "corona" matrix go here, centred on that matrix (billboard halos)
    Vec3 coronaCenter;
    bool coronaFound = false;
    Mat4 lastRootMat = Mat4::identity();  // first matrix of the hierarchy emitted last
    BatchSet* dst = nullptr;  // when set, emitHie writes into this (local space) instead of the level
    std::string trackDir, baseDir;
    std::vector<std::unique_ptr<Pak>> paks;     // everything opened so far (stable pointers)
    std::vector<Pak*> trackPaks;                // archives directly inside the track / base folder
    std::unordered_map<std::string, int> texCache;
    int missingTex = 0;
    std::vector<std::string> missingTexNames;
    std::vector<std::string> missing;

    Pak* adopt(std::unique_ptr<Pak> p) {
        paks.push_back(std::move(p));
        return paks.back().get();
    }

    // Reads a descriptor-like file: loose copy near the track, else inside the track archives, else anywhere.
    bool readNamed(const std::string& name, std::vector<uint8_t>& out) {
        int i = locate(name, trackDir, baseDir);
        if (i >= 0) {
            const std::string& lp = vfs::files()[i].lower;
            std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
            bool near = lp.compare(0, t.size(), t) == 0 || lp.compare(0, b.size(), b) == 0;
            if (near && vfs::read(i, out)) return true;
        }
        for (Pak* p : trackPaks) {
            int e = p->find(name);
            if (e >= 0 && p->extract(e, out)) return true;
        }
        return i >= 0 && vfs::read(i, out);
    }
    bool readNamedText(const std::string& name, std::string& out) {
        std::vector<uint8_t> d;
        if (!readNamed(name, d)) return false;
        out.assign((const char*)d.data(), d.size());
        return true;
    }

    // The archive that holds `<base>.hie`: one of the track archives, or a stand-alone <base>.dir/.pak.
    Pak* pakForHie(const std::string& base) {
        std::string hie = base + ".hie";
        for (Pak* p : trackPaks)
            if (p->find(hie) >= 0) return p;
        for (auto& p : paks)
            if (vfs::lower(p->base) == vfs::lower(base)) return p.get();
        int d = locate(base + ".dir", trackDir, baseDir);
        int k = locate(base + ".pak", trackDir, baseDir);
        if (d < 0 || k < 0) return nullptr;
        auto p = std::make_unique<Pak>();
        if (!p->open(d, k, base)) return nullptr;
        return adopt(std::move(p));
    }

    int textureFor(const std::string& name, Pak* preferred) {
        std::string key = vfs::lower(name);
        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;
        int result = -1;
        std::vector<Pak*> order;
        if (preferred) order.push_back(preferred);
        for (auto& p : paks)
            if (p.get() != preferred) order.push_back(p.get());
        for (Pak* p : order) {
            auto t = p->textures.find(key);
            if (t == p->textures.end()) continue;
            std::vector<uint8_t> data;
            Image img;
            if (p->extract(t->second, data) && decodeTGA(data.data(), data.size(), img)) {
                int tx = p->find(name + ".tx");  // TTEX descriptor: blend mode word at offset 12
                std::vector<uint8_t> td;
                if (tx >= 0 && p->extract(tx, td) && td.size() >= 16 && memcmp(td.data(), "TTEX", 4) == 0) {
                    uint32_t bm;
                    memcpy(&bm, &td[12], 4);
                    img.blendMode = bm <= 2 ? (uint8_t)bm : 0;
                }
                result = sc->addTexture(std::move(img));
                break;
            }
        }
        if (result < 0) { missingTex++; if (missingTexNames.size() < 4) missingTexNames.push_back(name); }
        texCache[key] = result;
        return result;
    }

    struct State {
        Mat4 m = Mat4::identity();
        int tex = -1;
        int attr = -1;
        int mat = -1;
    };

    // Walks the scene graph; `leaf` is called for every mesh (type 3) and spline (type 6) node.
    template <class F>
    void walk(const Hie& h, F&& leaf) {
        if (h.nodes.empty()) return;
        struct Item { int node; State st; };
        std::vector<Item> stack;
        stack.push_back({0, State()});
        std::vector<char> seen(h.nodes.size(), 0);
        while (!stack.empty()) {
            Item it = stack.back();
            stack.pop_back();
            if (it.node < 0 || it.node >= (int)h.nodes.size() || seen[it.node]) continue;
            seen[it.node] = 1;
            const Hie::Node& n = h.nodes[it.node];
            State cur = it.st;
            switch (n.type) {
                case 1:
                    if (n.index >= 0 && n.index < (int)h.mats.size()) {
                        cur.m = Mat4::mul(it.st.m, h.mats[n.index].m);
                        cur.attr = n.index;
                    }
                    break;
                case 2: cur.tex = n.index; break;
                case 5: cur.mat = n.index; break;
                case 3:
                case 6: leaf(n, cur); break;
                default: break;
            }
            if (n.sibling >= 0) stack.push_back({n.sibling, it.st});
            if (n.child >= 0) stack.push_back({n.child, cur});
        }
    }

    bool loadHie(const std::string& base, Pak*& pk, Hie& hie) {
        pk = pakForHie(base);
        if (!pk) { missing.push_back(base); return false; }
        std::vector<uint8_t> data;
        int he = pk->find(base + ".hie");
        if (he < 0 || !pk->extract(he, data)) { missing.push_back(base + ".hie"); return false; }
        if (!hie.parse(std::string((const char*)data.data(), data.size()))) {
            missing.push_back(base + ".hie (unreadable)");
            return false;
        }
        return true;
    }

    enum { MIR_WORLD, MIR_NONE, MIR_X, MIR_Z };  // how emitHie mirrors the geometry (see kMirrorX)

    // mir: MIR_WORLD for level geometry; MIR_X / MIR_Z mirror model-space meshes (icons / path followers, whose forward is +X);
    // MIR_NONE leaves the mesh raw. com receives the height of the centre of mass from the .dcol (movable objects).
    void emitHie(const std::string& base, int& meshCount, bool water = false, int mir = MIR_WORLD, float* com = nullptr) {
        Pak* pk = nullptr;
        Hie hie;
        if (!loadHie(base, pk, hie)) return;
        std::vector<uint8_t> data;
        if (com && !hie.collNames.empty()) {
            int ce = pk->find(hie.collNames[0]);
            std::vector<uint8_t> cd;
            if (ce >= 0 && pk->extract(ce, cd) && cd.size() >= 28) memcpy(com, &cd[24], 4);
        }
        const bool flip = kMirrorX && mir != MIR_NONE;
        if (!hie.mats.empty()) lastRootMat = hie.mats[0].m;
        std::vector<uint32_t> flipped;
        std::vector<CMesh> meshes;
        for (const std::string& mn : hie.meshNames) {
            int me = pk->find(mn);
            if (me < 0 || !pk->extract(me, data) || !parseMshs(data, meshes)) { missing.push_back(mn); return; }
        }
        std::vector<int> texOf(hie.textures.size(), -2);
        std::vector<SceneVertex> vs;
        walk(hie, [&](const Hie::Node& n, const State& st) {
            if (n.type != 3 || n.index < 0 || n.index >= (int)meshes.size()) return;
            const CMesh& m = meshes[n.index];
            int tex = -1;
            if (st.tex >= 0 && st.tex < (int)hie.textures.size()) {
                if (texOf[st.tex] == -2) texOf[st.tex] = textureFor(hie.textures[st.tex], pk);
                tex = texOf[st.tex];
            }
            vs.resize(m.nv());
            bool translucent = false;
            bool corona = coronaDst && st.attr >= 0 && vfs::lower(hie.mats[st.attr].name).find("corona") != std::string::npos;
            if (corona) {
                coronaCenter = st.m.transformPoint({0, 0, 0});
                if (flip) coronaCenter.x = -coronaCenter.x;
                coronaFound = true;
            }
            for (size_t i = 0; i < m.nv(); i++) {
                const float* f = &m.v[i * 9];
                Vec3 w = corona ? Vec3(f[0], f[1], f[2]) : st.m.transformPoint({f[0], f[1], f[2]});
                if (flip) {
                    if (mir == MIR_Z) w.z = -w.z;
                    else w.x = -w.x;
                }
                SceneVertex& v = vs[i];
                v.x = w.x; v.y = w.y; v.z = w.z;
                // the stored colour channels are in B, G, R order (D3D style)
                v.r = (uint8_t)(std::min(std::max(f[5], 0.f), 1.f) * 255.f);
                v.g = (uint8_t)(std::min(std::max(f[4], 0.f), 1.f) * 255.f);
                v.b = (uint8_t)(std::min(std::max(f[3], 0.f), 1.f) * 255.f);
                v.a = (uint8_t)(std::min(std::max(f[6], 0.f), 1.f) * 255.f);
                if (v.a < 250) translucent = true;
                v.u = f[7];
                v.v = f[8];
            }
            Blend bl = translucent ? BLEND_ALPHA : BLEND_OPAQUE;
            if (!translucent && tex >= 0 && sc->textures[tex].hasAlpha)
                bl = sc->textures[tex].partialAlpha ? BLEND_ALPHA : BLEND_CUTOUT;  // glass / lasers vs. fences / leaves
            if (!translucent)
                for (auto& v : vs) v.a = 255;
            if (water) {  // water / laser pipes: half transparent
                bl = BLEND_ALPHA;
                for (auto& v : vs) v.a = 150;
            }
            // Material flag bit 8 marks glowing surfaces (lasers, lens flares, neon, the moon): draw them additively.
            // Everything else that is see-through (glass, domes, shadows) is ordinary alpha blending / alpha testing.
            bool additive = st.mat >= 0 && st.mat < (int)hie.matFlags.size() && (hie.matFlags[st.mat] & 8);
            if (additive) {
                bl = BLEND_ADD;
                for (auto& v : vs) v.a = 255;
            }
            const auto* idx = m.idx.data();
            if (flip) {  // a mirror reverses the winding
                flipped.assign(m.idx.begin(), m.idx.end());
                for (size_t i = 0; i + 2 < flipped.size(); i += 3) std::swap(flipped[i + 1], flipped[i + 2]);
                idx = flipped.data();
            }
            // Material flag 1 is "double sided": leaves, thin boards and the like are drawn without culling.
            bool two = st.mat >= 0 && st.mat < (int)hie.matFlags.size() && (hie.matFlags[st.mat] & 1);
            if (corona) coronaDst->appendIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), true);
            else if (dst) dst->appendIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), two);
            else sc->addIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), two);
            meshCount++;
        });
    }

    // Paths: .lins + hie placed with parent matrices.
    void emitPaths(const std::string& hieBase, int catAI, int catRace, int catStart, int catDrone, bool drone) {
        Pak* pk = nullptr;
        Hie hie;
        if (!loadHie(hieBase, pk, hie)) return;
        std::vector<uint8_t> data;
        std::vector<std::vector<Vec3>> lines;
        for (const std::string& ln : hie.lineNames) {
            int le = pk->find(ln);
            if (le < 0 || !pk->extract(le, data)) continue;
            parseLins(data, lines);
        }
        walk(hie, [&](const Hie::Node& n, const State& st) {
            if (n.type != 6 || n.index < 0 || n.index >= (int)lines.size()) return;
            std::string attr = st.attr >= 0 ? vfs::lower(hie.mats[st.attr].name) : "";
            int cat = drone ? catDrone : catAI;
            if (!drone) {
                if (attr.find("startfinish") != std::string::npos) cat = catStart;
                else if (attr.find("rl") != std::string::npos) cat = catRace;
            }
            const auto& pts = lines[n.index];
            for (size_t i = 0; i + 1 < pts.size(); i++)
                sc->addLine(cat, fixX(st.m.transformPoint(pts[i])), fixX(st.m.transformPoint(pts[i + 1])));
        });
    }

    // Polyline of the first spline of a path hierarchy, in world space.
    bool loadPathPoints(const std::string& base, std::vector<Vec3>& pts) {
        // ASSETS/ModifiedSplines/<path>.bin holds the game's final version of a follower spline: plain x,y,z floats.
        // The .lin inside the archive is the raw export (still in the object space of the model it was drawn on).
        {
            std::string assets = vfs::dirOf(vfs::dirOf(trackDir));
            int bi = locate(base + ".bin", assets + "/ModifiedSplines", "");
            if (bi >= 0 && vfs::files()[bi].lower.find("/modifiedsplines/") != std::string::npos) {
                std::vector<uint8_t> bd;
                if (vfs::read(bi, bd) && bd.size() >= 24) {
                    Reader r(bd);
                    while (r.left() >= 12) pts.push_back(fixX(r.vec3()));
                    if (pts.size() >= 2) return true;
                    pts.clear();
                }
            }
        }
        Pak* pk = nullptr;
        Hie hie;
        if (!loadHie(base, pk, hie)) return false;
        std::vector<uint8_t> data;
        std::vector<std::vector<Vec3>> lines;
        for (const std::string& ln : hie.lineNames) {
            int le = pk->find(ln);
            if (le < 0 || !pk->extract(le, data)) continue;
            parseLins(data, lines);
        }
        bool got = false;
        walk(hie, [&](const Hie::Node& n, const State& st) {
            if (got || n.type != 6 || n.index < 0 || n.index >= (int)lines.size()) return;
            for (const Vec3& p : lines[n.index]) pts.push_back(fixX(st.m.transformPoint(p)));
            got = true;
        });
        return got && pts.size() >= 2;
    }

    // ---- powerup metadata: PowerupStrings_*.txt lists every powerup in id order, grouped by "// apo", "// car", "// peds"...
    struct PupInfo { std::string group, name; };
    std::vector<PupInfo> pupTable;
    void loadPupTable() {
        pupTable.clear();
        std::string t;
        if (!readNamedText("PowerupStrings_UK.txt", t) && !readNamedText("PowerupStrings_US.txt", t)) return;
        std::string group;
        for (const std::string& l : splitLines(t)) {
            if (l.empty()) continue;
            if (l.compare(0, 2, "//") == 0) {
                std::string w = l.substr(2);
                while (!w.empty() && w[0] == ' ') w.erase(0, 1);
                bool word = !w.empty();
                for (char c : w) word &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                if (word) group = vfs::lower(w);
                continue;
            }
            if (l[0] != '"') continue;
            auto q = quotedStrings(l);
            pupTable.push_back({group, q.empty() ? "" : q[0]});
        }
    }
    // 0 = generic, 1 = special ability, 2 = pedestrian powerup, 3 = permanent upgrade, 4 = credits
    int classifyPowerup(int id, const std::string& label) {
        std::string g, nm = vfs::lower(label);
        if (id >= 0 && id < (int)pupTable.size()) { g = pupTable[id].group; nm = vfs::lower(pupTable[id].name); }
        auto has = [&](const char* w) { return nm.find(w) != std::string::npos; };
        if (g == "money" || has("credits") || has("loads of cash")) return 4;
        if ((g == "apo" && id >= 1 && id <= 20) || has("extra armour") || has("extra power") || has("extra offensive") ||
            has("extra everything") || has("extra slots") || has("bonus slots") || has("bonus armour") || has("bonus power") ||
            has("bonus offensive") || has("max armour") || has("max power") || has("max offensive") || has("max everything"))
            return 3;
        if (id == 88 || g == "peds" || has("pedestrian") || has("peds ")) return 2;
        if (g == "time" || has("time bonus") || has("bonus time") || has("extra time")) return 5;
        static const char* special[] = {"afterburner", "hot rod", "invulnerab", "underwater", "wall climb", "solid granite", "kangaroo",
                                        "mega-turbo", "turbo nutter", "nuclear bomb", "slaughter mortar", "hovercraft"};
        for (const char* w : special)
            if (has(w)) return 1;
        return 0;
    }
    // Icon hierarchy for a powerup id. The id -> icon table is hard-coded in the game executable; this follows what
    // the game visibly does: permanent upgrades have one icon per kind (armour / power / offensive / everything),
    // cash, pedestrian, time, random and the mission item pieces have their own, everything else is the wrench.
    std::string iconFor(int id) {
        std::string g;
        if (id >= 0 && id < (int)pupTable.size()) g = pupTable[id].group;
        if (id == 100) return "BombPiececube1";
        if (id == 101) return "Oil_DrumDrum_null";
        if (id == 102) return "fuseFuse_NULL";
        if (id == 103) return "EnginePartobj3";
        if (id == 104) return "DingablesMoneyBagPowerup";
        if (id == 105) return "DingablesArtilleryShellPow";
        if (g == "apo" && id >= 1 && id <= 20) {
            static const char* kinds[] = {"newIconsHELMET", "newIconsENGINE", "newIconsFIST", "newIconsAPOall"};
            return kinds[(id - 1) % 4];  // armour, power, offensive, everything (and their double / max / slot variants)
        }
        if (id == 88 || g == "peds") return "newIconsPEDSIGN";
        if (g == "money") return "newIconsWADOCASH";
        if (g == "time") return "newIconsTIME";
        if (g == "random") return "newIconsRANDOM";
        return "newIconsSPANNER";
    }

    // ---- discovery by naming convention, for installs where the RaceInfo files are missing
    // Powerup file for this level: "<level>.pup" or one whose name carries the level's suffix ("race1", "mission2").
    bool findPup(const std::string& levelName, std::string& text) {
        std::string ln = vfs::lower(levelName), suffix;
        size_t us = ln.find('_');
        if (us != std::string::npos) suffix = ln.substr(us + 1);
        auto score = [&](const std::string& file) {
            std::string stem = vfs::lower(file);
            if (endsWith(stem, ".pup")) stem.resize(stem.size() - 4);
            size_t sl = stem.find_last_of('/');
            if (sl != std::string::npos) stem = stem.substr(sl + 1);
            if (stem == ln) return 3;
            if (!suffix.empty() && stem.find(suffix) != std::string::npos) return 2;
            // Missions, free driving and arenas often ship a "<Track>_Race1.pup" that serves them all; races need their own.
            if (suffix.compare(0, 4, "race") != 0) return 1;
            return 0;
        };
        int best = 0;
        Pak* bestPak = nullptr;
        std::string bestName;
        int bestLoose = -1;
        for (Pak* p : trackPaks)
            for (const DirEntry& e : p->entries)
                if (endsWith(vfs::lower(e.name), ".pup")) {
                    int sc = score(e.name);
                    if (sc > best) { best = sc; bestPak = p; bestName = e.name; bestLoose = -1; }
                }
        std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
        for (size_t i = 0; i < vfs::files().size(); i++) {
            const auto& f = vfs::files()[i];
            if (!endsWith(f.lower, ".pup") || (f.lower.compare(0, t.size(), t) != 0 && f.lower.compare(0, b.size(), b) != 0)) continue;
            int sc = score(f.path);
            if (sc > best) { best = sc; bestPak = nullptr; bestLoose = (int)i; }
        }
        std::vector<uint8_t> d;
        if (bestLoose >= 0) { if (!vfs::read(bestLoose, d)) return false; }
        else if (bestPak) { int e = bestPak->find(bestName); if (e < 0 || !bestPak->extract(e, d)) return false; }
        else return findSiblingPup(suffix.compare(0, 4, "race") == 0, text);
        text.assign((const char*)d.data(), d.size());
        return true;
    }

    // Non-race levels (missions, free driving) use the powerups of the base track, which ship with its Race 1.
    bool findSiblingPup(bool isRace, std::string& text) {
        if (isRace) return false;
        std::string parent = vfs::dirOf(baseDir), want = vfs::lower(vfs::baseOf(baseDir)) + "_race1", sib;
        for (const std::string& sd : vfs::subdirs(parent))
            if (vfs::lower(sd) == want) sib = parent + "/" + sd;
        if (sib.empty()) return false;
        std::string pre = vfs::lower(sib) + "/";
        std::vector<uint8_t> d;
        for (size_t i = 0; i < vfs::files().size(); i++) {  // loose .pup
            const auto& f = vfs::files()[i];
            if (endsWith(f.lower, ".pup") && f.lower.compare(0, pre.size(), pre) == 0 && vfs::read((int)i, d)) {
                text.assign((const char*)d.data(), d.size());
                return true;
            }
        }
        std::vector<std::unique_ptr<Pak>> tmp;  // .pup inside the sibling's archives
        openDirPaks(sib, tmp);
        for (auto& p : tmp)
            for (size_t e = 0; e < p->entries.size(); e++)
                if (endsWith(vfs::lower(p->entries[e].name), ".pup") && p->extract((int)e, d)) {
                    text.assign((const char*)d.data(), d.size());
                    return true;
                }
        return false;
    }

    // AI path hierarchy ("<name>.hie" + "<name>.lins") inside one of the track's own archives.
    std::string findAiPathHie() {
        for (Pak* p : trackPaks)
            for (const DirEntry& e : p->entries) {
                std::string n = e.name;
                if (!endsWith(vfs::lower(n), ".lins")) continue;
                std::string stem = n.substr(0, n.size() - 5);
                if (p->find(stem + ".hie") >= 0) return stem + ".hie";
            }
        return "";
    }

    // Drone / traffic path archive ("...Drone...Path....dir") anywhere in the track or base folder.
    std::string findDronePathHie() {
        std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
        for (const auto& f : vfs::files()) {
            if (!endsWith(f.lower, ".dir") || (f.lower.compare(0, t.size(), t) != 0 && f.lower.compare(0, b.size(), b) != 0)) continue;
            std::string stem = vfs::stemOf(f.path), ls = vfs::lower(stem);
            if (ls.find("drone") != std::string::npos && ls.find("path") != std::string::npos) return stem + ".hie";
        }
        return "";
    }

    // PATH_FOLLOWERS: meshes that travel along splines (helicopters, energy rings, boats...).
    //   @name / "mesh.hie" / "path.hie" / "anim" / speed sway swayMag animTime / startTime / frames / frameNulls
    // OPT_ALIGNMENT_MATRIX44 of a path's options file: a 4x4 matrix (rows as written, translation in the last column).
    bool pathAlignment(const std::string& pathBase, float m[16]) {
        std::string t;
        if (!readNamedText(pathBase + ".txt", t)) return false;
        std::string body;
        for (const std::string& l : splitLines(t)) body += stripComment(l) + " ";
        size_t a = body.find("OPT_ALIGNMENT_MATRIX44");
        if (a == std::string::npos) return false;
        std::istringstream is(body.substr(a + 22));
        for (int i = 0; i < 16; i++)
            if (!(is >> m[i])) return false;
        return true;
    }

    // Applies the alignment to a follower's model (column vectors: p' = A p + t), then the mirror of the maps (Z, see MIR_Z).
    void alignGeometry(BatchSet& g, const float m[16]) {
        float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
        bool flip = kMirrorX != (det < 0);
        for (Batch& b : g.batches) {
            for (SceneVertex& v : b.verts) {
                float x = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
                float y = m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7];
                float z = m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11];
                v.x = x; v.y = y; v.z = kMirrorX ? -z : z;
            }
            if (flip)
                for (size_t i = 0; i + 2 < b.idx.size(); i += 3) std::swap(b.idx[i + 1], b.idx[i + 2]);
        }
    }

    // OPT_SMOOTH_INDEX of a follower's options file, 0 when it has none.
    int followerSmoothIndex(const std::string& hieBase) {
        std::string t;
        if (!readNamedText(hieBase + ".txt", t)) return 0;
        for (const std::string& l : splitLines(t)) {
            std::string c = stripComment(l);
            size_t a = c.find("OPT_SMOOTH_INDEX");
            if (a != std::string::npos) return std::max(0, atoi(c.c_str() + a + 16));
        }
        return 0;
    }

    // Uniform Catmull-Rom through the points, `n` samples per segment. Paths whose ends meet are treated as closed loops.
    static std::vector<Vec3> catmullRom(const std::vector<Vec3>& p, int n) {
        size_t cnt = p.size();
        if (cnt < 3 || n < 2) return p;
        bool closed = Mover::length3(p.front() - p.back()) < 1.0f;
        if (closed) cnt--;
        auto at = [&](long i) {
            if (closed) return p[(size_t)(((i % (long)cnt) + (long)cnt) % (long)cnt)];
            return p[(size_t)std::min<long>(std::max<long>(i, 0), (long)cnt - 1)];
        };
        std::vector<Vec3> out;
        size_t segs = closed ? cnt : cnt - 1;
        out.reserve(segs * n + 1);
        for (size_t i = 0; i < segs; i++) {
            Vec3 p0 = at((long)i - 1), p1 = at((long)i), p2 = at((long)i + 1), p3 = at((long)i + 2);
            for (int k = 0; k < n; k++) {
                float t = (float)k / (float)n, t2 = t * t, t3 = t2 * t;
                out.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f);
            }
        }
        out.push_back(closed ? p.back() : at((long)cnt - 1));
        return out;
    }

    void loadFollowers(const std::string& fname, Scene& out, int cat, int pathCat) {
        std::string txt;
        if (!readNamedText(fname, txt)) { missing.push_back("followers file " + fname); return; }
        struct Entry { std::string name, mesh, path; float speed = 0, start = 0; };
        std::vector<Entry> list;
        Entry cur;
        int stage = -1;
        for (const std::string& l : splitLines(txt)) {
            if (l.empty() || l.compare(0, 2, "//") == 0) continue;
            if (l[0] == '@') {
                if (stage >= 0) list.push_back(cur);
                cur = Entry();
                cur.name = l.substr(1);
                stage = 0;
                continue;
            }
            if (stage < 0) continue;
            auto q = quotedStrings(l);
            if (stage == 0) cur.mesh = q.empty() ? "" : q[0];
            else if (stage == 1) cur.path = q.empty() ? "" : q[0];
            else if (stage == 3) cur.speed = (float)atof(l.c_str());
            else if (stage == 4) cur.start = (float)atof(l.c_str());
            stage++;
        }
        if (stage >= 0) list.push_back(cur);

        auto stem = [](std::string s) {
            if (endsWith(vfs::lower(s), ".hie")) s.resize(s.size() - 4);
            return s;
        };
        std::map<std::string, int> geomIdx;
        std::map<std::string, std::vector<Vec3>> pathCache;
        std::set<std::string> drawnPaths;
        for (const Entry& e : list) {
            std::string mb = stem(e.mesh), pb = stem(e.path);
            if (mb.empty() || pb.empty() || vfs::lower(mb) == "none" || vfs::lower(pb) == "none" || e.speed <= 0) {
                if (missing.size() < 6) missing.push_back("follower " + e.name + " (incomplete entry)");
                continue;
            }
            // The path's options file (<path hierarchy>.txt) may hold OPT_ALIGNMENT_MATRIX44: how the model sits on the path
            // (the Stuka is modelled nose along +Z, but the path frame has +X forward).
            float al[16];
            bool aligned = pathAlignment(pb, al);
            std::string gkey = vfs::lower(mb);
            if (aligned)
                for (float f : al) gkey += "|" + std::to_string(f);
            auto gi = geomIdx.find(gkey);
            if (gi == geomIdx.end()) {
                out.moverGeoms.emplace_back();
                dst = &out.moverGeoms.back();
                int mc = 0;
                emitHie(mb, mc, false, aligned ? MIR_NONE : MIR_Z);
                dst = nullptr;
                int idx = (int)out.moverGeoms.size() - 1;
                if (mc == 0) { out.moverGeoms.pop_back(); idx = -1; }
                else if (aligned) alignGeometry(out.moverGeoms.back(), al);
                gi = geomIdx.emplace(gkey, idx).first;
            }
            if (gi->second < 0) {
                if (missing.size() < 6) missing.push_back("follower mesh " + mb);
                continue;
            }
            // The follower's options file (<hierarchy>.txt) has OPT_SMOOTH_INDEX: the game flies a smooth curve through the
            // stored polyline, which is modelled here as a Catmull-Rom spline with that many samples per segment.
            int smooth = followerSmoothIndex(mb);
            std::string pkey = vfs::lower(pb) + "#" + std::to_string(smooth);
            auto pi = pathCache.find(pkey);
            if (pi == pathCache.end()) {
                std::vector<Vec3> pts;
                loadPathPoints(pb, pts);
                if (smooth > 1) pts = catmullRom(pts, smooth);
                pi = pathCache.emplace(pkey, std::move(pts)).first;
            }
            if (pi->second.size() < 2) {
                if (missing.size() < 6) missing.push_back("follower path " + pb);
                continue;
            }
            if (drawnPaths.insert(pkey).second)
                for (size_t k = 0; k + 1 < pi->second.size(); k++) out.addLine(pathCat, pi->second[k], pi->second[k + 1]);
            Mover m;
            m.geom = gi->second;
            m.cat = cat;
            m.name = e.name;
            m.path = pi->second;
            m.speed = e.speed;
            m.phase = e.start;
            m.finish();
            out.movers.push_back(std::move(m));
            out.cats[cat].count++;
        }
    }

    // Animated props: lifts, generator wheels and the Police State behemoths. Each descriptor names a hierarchy and a
    // looping list of movements (duration, translation, rotation axis, rotation in degrees).
    void loadAnimProps(const std::string& fname, Scene& out, int cat, int pathCat) {
        std::string list;
        if (!readNamedText(fname, list)) return;
        for (const std::string& raw : splitLines(list)) {
            auto q = quotedStrings(stripComment(raw));
            if (q.empty() || !endsWith(vfs::lower(q[0]), ".txt")) continue;
            std::string dt;
            if (!readNamedText(q[0], dt)) { if (missing.size() < 6) missing.push_back(q[0]); continue; }
            std::string hieName;
            int stage = 0, nkeys = -1;
            std::vector<float> nums;
            for (const std::string& l2 : splitLines(dt)) {
                std::string l = stripComment(l2);
                auto qq = quotedStrings(l);
                if (!qq.empty()) {
                    if (stage == 0) hieName = qq[0];
                    stage++;
                    continue;
                }
                if (stage < 3) continue;
                std::istringstream is(l);
                float f;
                while (is >> f) nums.push_back(f);
                if (nkeys < 0 && !nums.empty()) { nkeys = (int)nums[0]; nums.erase(nums.begin()); }
                if (nkeys >= 0 && (int)nums.size() >= nkeys * 8) break;
            }
            if (!endsWith(vfs::lower(hieName), ".hie") || nkeys <= 0 || (int)nums.size() < nkeys * 8) continue;
            hieName.resize(hieName.size() - 4);
            out.moverGeoms.emplace_back();
            dst = &out.moverGeoms.back();
            int mc = 0;
            emitHie(hieName, mc);
            dst = nullptr;
            if (mc == 0) { out.moverGeoms.pop_back(); continue; }
            Mover m;
            m.geom = (int)out.moverGeoms.size() - 1;
            m.cat = cat;
            m.name = hieName;
            m.path = {Vec3()};
            const Mat4& R = lastRootMat;
            m.pivot = fixX(Vec3(R.m[12], R.m[13], R.m[14]));
            for (int k = 0; k < nkeys; k++) {
                const float* v = &nums[k * 8];
                AnimKey key;
                key.dur = v[0];
                key.trans = fixX(Vec3(v[1], v[2], v[3]));
                // the axis is given in the hierarchy's own frame; mirroring turns a rotation about a into one about (ax, -ay, -az)
                Vec3 a(R.m[0] * v[4] + R.m[4] * v[5] + R.m[8] * v[6], R.m[1] * v[4] + R.m[5] * v[5] + R.m[9] * v[6],
                       R.m[2] * v[4] + R.m[6] * v[5] + R.m[10] * v[6]);
                key.axis = kMirrorX ? Vec3(a.x, -a.y, -a.z) : a;
                key.rotDeg = v[7];
                m.keys.push_back(key);
            }
            // the route of the prop: from the centre of its geometry through the translations of its movements
            {
                Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
                for (const Batch& b : out.moverGeoms[m.geom].batches)
                    for (const SceneVertex& v : b.verts) {
                        lo = Vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
                        hi = Vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
                    }
                Vec3 p = (lo + hi) * 0.5f;
                for (const AnimKey& k : m.keys) {
                    if (Mover::length3(k.trans) < 0.5f) continue;
                    out.addLine(pathCat, p, p + k.trans);
                    p = p + k.trans;
                }
            }
            out.movers.push_back(std::move(m));
            out.cats[cat].count++;
        }
    }

    bool load(const std::string& levelId, Scene& out, std::string& err) override {
        sc = &out;
        out.cullBackfaces = true;  // the game draws single-sided faces
        paks.clear();
        trackPaks.clear();
        texCache.clear();
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
        {
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
                if (vfs::lower(sd) == "pathfollowers" || vfs::lower(sd) == "powerups" || vfs::lower(sd) == "movableobjects" || vfs::lower(sd) == "drones") {
                    std::vector<std::unique_ptr<Pak>> sp;
                    openDirPaks(root + "/" + sd, sp);
                    for (auto& p : sp) trackPaks.push_back(adopt(std::move(p)));
                }
        }

        std::string text;
        if (!readNamedText(name + ".txt", text)) { err = "cannot read track descriptor"; return false; }
        KeyFile kf;
        kf.parse(text);
        // Every level descriptor is complete on its own: keys it leaves out or comments out are simply off (no inheritance).
        out.title = kf.str("NAME");
        if (out.title.empty()) out.title = name;
        float col[3];
        if (kf.nums("FOG_TRACK_COLOUR", col, 3)) out.background = {col[0] * 0.9f, col[1] * 0.9f, col[2] * 0.9f};

        KeyFile race;
        {
            std::string rt;
            if (readNamedText(name + "_RaceInfo.txt", rt)) race.parse(rt);
        }

        // --- geometry
        std::vector<std::string> hies;
        {
            std::string md;
            if (!readNamedText(kf.str("STATIC_MESH_DESCRIPTOR"), md)) { err = "cannot read mesh descriptor"; return false; }
            for (const std::string& raw : splitLines(md)) {
                auto q = quotedStrings(stripComment(raw));
                if (q.empty()) continue;
                const std::string& first = q[0];
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
        int meshCount = 0;
        for (const std::string& h : hies) emitHie(h, meshCount, vfs::lower(h).find("water") != std::string::npos);
        if (sc->triCount == 0) {
            err = "no geometry found (" + std::to_string(hies.size()) + " hierarchies";
            for (const std::string& m : missing) err += ", missing " + m;
            err += ")";
            return false;
        }

        // --- annotations
        int cStart = out.addCategory("start", "Start position", 0x39ff7a, SHAPE_ARROW, 2.0f, true);
        int cPup = out.addCategory("powerup", "Powerups", 0xffb870, SHAPE_DIAMOND, 1.0f, true);
        int cSpecial = out.addCategory("special", "Special powerups", 0xe07b00, SHAPE_DIAMOND, 1.15f, true);
        int cPedPup = out.addCategory("pedpowerup", "Pedestrian powerups", 0xffe600, SHAPE_DIAMOND, 1.0f, true);
        int cTime = out.addCategory("timepowerup", "Time bonuses", 0x8fd8ff, SHAPE_DIAMOND, 1.0f, true);
        int cUpgrade = out.addCategory("upgrade", "Permanent Upgrades", 0xff2a2a, SHAPE_DIAMOND, 1.2f, true);
        int cCredit = out.addCategory("credit", "Credits", 0x35e04a, SHAPE_DIAMOND, 1.0f, true);
        int cMissionItem = out.addCategory("missionitem", "Mission items", 0x35c7e8, SHAPE_DIAMOND, 1.0f, true);
        int cMission = out.addCategory("mission", "Mission objectives", 0xff4fd8, SHAPE_CUBE, 1.0f, true);
        int cCheck = out.addCategory("checkpoint", "Checkpoints", 0xff5a5a, SHAPE_CUBE, 1.0f, false, true, true);
        int cRace = out.addCategory("raceline", "Race line", 0x37c8ff, SHAPE_DIAMOND, 1.0f, false, true, true);
        int cStartLine = out.addCategory("startline", "AI race-line start (path flag)", 0xffffff, SHAPE_DIAMOND, 1.0f, false, false, true);
        int cAI = out.addCategory("aipath", "Opponent roam paths", 0x4a7dff, SHAPE_DIAMOND, 1.0f, false, false, true);
        int cDrone = out.addCategory("drone", "Drone paths (traffic, trains)", 0xc27aff, SHAPE_DIAMOND, 1.0f, false, false, true);
        int cPed = out.addCategory("ped", "Pedestrians", 0x1f7a2e, SHAPE_DIAMOND, 0.6f, false, false);
        int cRespawn = out.addCategory("respawn", "Respawn points", 0x7af0ff, SHAPE_ARROW, 1.2f, false, false);

        float sp[3], ang;
        if (kf.nums("START_POS", sp, 3)) {
            float a = 0;
            if (kf.nums("START_ANGLE", &ang, 1)) a = ang;
            Vec3 p = fixX({sp[0], sp[1], sp[2]});
            out.addMarker(cStart, p, fixA(a), "Start position");
            out.hasCameraStart = true;
            out.cameraYaw = fixA(a);
            out.cameraPitch = -0.5f;
            out.cameraPos = p + Vec3(std::sin(fixA(a)), 0, std::cos(fixA(a))) * -60.0f + Vec3(0, 50, 0);
        }

        // powerups (.pup text: name comment, then "type x y z group respawn")
        {
            std::string pupName = race.str("POWERUPS");
            if (pupName.empty()) pupName = kf.str("POWERUPS");
            std::string pt;
            bool havePup = !pupName.empty() && readNamedText(pupName, pt);
            if (!havePup) havePup = findPup(name, pt);  // most installs have no RaceInfo: find it by naming convention
            loadPupTable();
            struct Placed { int id; int cat; Vec3 pos; };
            std::vector<Placed> placed;
            if (havePup) {
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
                        Vec3 p = fixX({nums[1], nums[2], nums[3]});
                        std::string lb = label.empty() ? "Powerup" : label;
                        int cat = cPup;
                        switch (classifyPowerup(id, lb)) {
                            case 1: cat = cSpecial; break;
                            case 2: cat = cPedPup; break;
                            case 3: cat = cUpgrade; break;
                            case 4: cat = cCredit; break;
                            case 5: cat = cTime; break;
                            default: break;
                        }
                        out.addMarker(cat, p, 0, lb + " (id " + std::to_string(id) + ")");
                        placed.push_back({id, cat, p});
                        nums.clear();
                        label.clear();
                    }
                }
            }  // end if (havePup)

            // Missions spawn their items with script lines: CREATE_POWERUP "<name>" [flags] x y z ...
            {
                std::string lname = vfs::lower(name);
                for (Pak* p : trackPaks)
                    for (size_t ei = 0; ei < p->entries.size(); ei++) {
                        std::string en = vfs::lower(p->entries[ei].name);
                        if (!endsWith(en, "_script.txt") || en.find(lname) == std::string::npos) continue;
                        std::vector<uint8_t> sd;
                        if (!p->extract((int)ei, sd)) continue;
                        for (const std::string& raw : splitLines(std::string((const char*)sd.data(), sd.size()))) {
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
                            for (size_t k = 0; k < pupTable.size(); k++)
                                if (vfs::lower(pupTable[k].name) == vfs::lower(pname)) { id = (int)k; break; }
                            Vec3 pos = fixX({run[0], run[1], run[2]});
                            int cat = id >= 0 ? cMissionItem : cMission;  // named items vs. objective markers
                            out.addMarker(cat, pos, 0, pname + (id >= 0 ? " (mission item)" : " (mission objective)"));
                            placed.push_back({id, cat, pos});
                        }
                    }
            }

            {
                // In-game icon meshes (spinning). The id -> icon table is hard-coded in the game executable, so the
                // assignment below follows the groups of the powerup string table.
                struct Icon { int geom = -1, halo = -1; Vec3 center; };
                std::map<std::string, Icon> geoms;
                for (const Placed& pl : placed) {
                    if (pl.id < 0) continue;  // mission objective markers have no in-game mesh
                    std::string icon = iconFor(pl.id);
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
        }

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
                        Vec3 p[8], c;
                        for (auto& v : p) { v = fixX(r.vec3()); c += v * 0.125f; }
                        static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                        for (auto& ed : e) out.addLine(cCheck, p[ed[0]], p[ed[1]]);
                        std::string nm = names.count((int)b) ? names[(int)b] : "VOLUME" + std::to_string(b);
                        out.addMarker(cCheck, c, 0, nm);
                    }
                }
            }
        }

        // paths
        std::string opp = race.str("OPPONENT_PATHS"), drone = race.str("DRONE_PATHS");
        if (!endsWith(vfs::lower(opp), ".hie")) opp = findAiPathHie();
        if (!endsWith(vfs::lower(drone), ".hie")) drone = findDronePathHie();
        if (endsWith(vfs::lower(opp), ".hie")) emitPaths(opp.substr(0, opp.size() - 4), cAI, cRace, cStartLine, cDrone, false);
        if (endsWith(vfs::lower(drone), ".hie")) emitPaths(drone.substr(0, drone.size() - 4), cAI, cRace, cStartLine, cDrone, true);

        // animated path followers (helicopters, energy rings, ...)
        {
            int cMove = out.addCategory("movers", "Moving objects (path followers, behemoths, lifts)", 0x7fd0ff, SHAPE_DIAMOND, 1.0f, false, true);
            int cAnimPath = out.addCategory("animpath", "Routes of animated props (behemoths, lifts)", 0xffc46b, SHAPE_DIAMOND, 1.0f, false, false, true);
            int cFPath = out.addCategory("followerpath", "Paths of moving objects", 0xff9ad5, SHAPE_DIAMOND, 1.0f, false, false, true);
            std::string pf = kf.str("PATH_FOLLOWERS");
            if (!pf.empty()) loadFollowers(pf, out, cMove, cFPath);
            std::string ap = kf.str("ANIMATED_PROPS");
            if (!ap.empty()) loadAnimProps(ap, out, cMove, cAnimPath);
        }

        // pedestrians: "<type> <skin> <anim> x y z dir"
        {
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
                        out.addMarker(cPed, fixX({v[3], v[4], v[5]}), fixA(v[6] * 0.0174533f), "Pedestrian (skin " + std::to_string((int)v[1]) + ")");
                }
            }
        }

        // respawn points: free-form text, try "x y z [angle]" lines
        {
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
                    if (k >= 3) out.addMarker(cRespawn, fixX({v[0], v[1], v[2]}), k == 4 ? fixA(v[3]) : 0, "Respawn point");
                }
            }
        }

        // in-game lighting: ambient, sun and the point lights of LIGHTS_DESCRIPTOR
        {
            float amb, sun[3], sc3[3];
            out.hasLighting = true;
            if (kf.nums("AMBIENT_LIGHT", &amb, 1)) out.ambient = amb;
            if (kf.nums("SUN_VECTOR", sun, 3)) out.sunDir = normalize(fixX(Vec3(-sun[0], -sun[1], -sun[2])));
            if (kf.nums("SUN_LIGHT_COLOUR", sc3, 3)) out.sunColor = {sc3[0], sc3[1], sc3[2]};
            int cLight = out.addCategory("light", "Lights (in-game)", 0xfff3a0, SHAPE_DIAMOND, 0.7f, false, true);
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
                    pl.color = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
                    out.lights.push_back(pl);
                    char hex[16];
                    snprintf(hex, sizeof hex, "#%06x", c & 0xffffff);
                    out.addMarker(cLight, pl.pos, 0, std::string("Light ") + hex + " (radius " + std::to_string((int)r0) + "-" + std::to_string((int)r1) + ")");
                }
            }
        }

        // Destructible / movable props (trees, light poles, benches, parked cars) placed by MOVABLE_OBJECTS, plus the
        // breakable fence / glass hierarchy. Everything is merged into one static mesh so it costs a single draw.
        {
            int cProps = out.addCategory("props", "Destructible & movable props", 0x9aa0a6, SHAPE_CUBE, 1.0f, false, true);
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
                            Vec3 w = fixX({r[0][0] * x + r[0][1] * y + r[0][2] * z + v[0], r[1][0] * x + r[1][1] * y + r[1][2] * z + v[1],
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
                m.path = {Vec3()};
                out.movers.push_back(std::move(m));
                out.cats[cProps].count = instances;
            } else {
                out.moverGeoms.pop_back();
            }
        }

        char buf[512];
        std::string extra;
        if (missingTex) {
            extra += ", " + std::to_string(missingTex) + " textures missing (";
            for (size_t i = 0; i < missingTexNames.size(); i++) extra += (i ? ", " : "") + missingTexNames[i];
            extra += ")";
        }
        for (size_t i = 0; i < missing.size() && i < 3; i++) extra += (i ? ", " : ", skipped: ") + missing[i];
        snprintf(buf, sizeof buf, "%zu triangles from %d meshes, %d textures%s", out.triCount, meshCount, (int)out.textures.size(), extra.c_str());
        out.info = buf;
        return true;
    }
};

}  // namespace

Game* makeCarma() { return new CarmaGame(); }
