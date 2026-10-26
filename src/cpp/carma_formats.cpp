#include "carma_formats.h"
#include <cstring>
#include <sstream>
#include "binio.h"
#include "carma_text.h"

namespace carma {

bool Hie::section(const std::vector<std::string>& L, const char* key, std::vector<std::string>& items) {
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

std::string Hie::unquote(const std::string& s) {
    size_t a = s.find('"'), b = s.rfind('"');
    return (a != std::string::npos && b > a) ? s.substr(a + 1, b - a - 1) : s;
}

bool Hie::parse(const std::string& text) {
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
            // Car files hold many meshes back to back; single-mesh files (gorilla, sharks) may carry other data after the triangles.
            size_t next = h0 + need;
            uint32_t nextHdr;
            if (next + 24 <= data.size() && (memcpy(&nextHdr, data.data() + next, 4), nextHdr >> 16) == (hdr >> 16)) {
                r.pos = next;
                continue;
            }
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

}  // namespace carma
