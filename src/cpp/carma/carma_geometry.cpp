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

int CarmaGame::textureFor(const std::string& name, Pak* preferred) {
    std::string key = vfs::lower(name);
    auto it = texCache.find(key);
    if (it != texCache.end()) return it->second;
    int result = -1;
    // An animated texture shows its script's frames: its own (static) image is never used.
    auto script = texScripts.find(key);
    if (script != texScripts.end()) {
        texCache[key] = -1;  // a script that lists its own texture must not recurse
        TexAnim anim;
        anim.fps = script->second.fps;
        for (const std::string& f : script->second.frames) {
            int fi = textureFor(f, preferred);
            if (fi >= 0) anim.frames.push_back(fi);
        }
        if (!anim.frames.empty()) {
            anim.texture = sc->addTexture(Image(sc->textures[anim.frames[0]]));  // what the batches' blending is derived from
            result = anim.texture;
            sc->texAnims.push_back(std::move(anim));
            texCache[key] = result;
            return result;
        }
    }
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

bool CarmaGame::loadHie(const std::string& base, Pak*& pk, Hie& hie) {
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

void CarmaGame::emitHie(const std::string& base, int& meshCount, bool water, int mir, float* centreOfMass) {
    Pak* pk = nullptr;
    Hie hie;
    if (!loadHie(base, pk, hie)) return;
    std::vector<uint8_t> data;
    if (centreOfMass && !hie.collNames.empty()) {
        int ce = pk->find(hie.collNames[0]);
        std::vector<uint8_t> cd;
        if (ce >= 0 && pk->extract(ce, cd) && cd.size() >= 28) memcpy(centreOfMass, &cd[24], 4);
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
            Vec3f w = corona ? Vec3f(f[0], f[1], f[2]) : st.m.transformPoint({f[0], f[1], f[2]});
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
        // The water surface is seen from below as well (swimming under it, the car underwater): always two sided.
        bool two = water || (st.mat >= 0 && st.mat < (int)hie.matFlags.size() && (hie.matFlags[st.mat] & 1));
        if (corona) coronaDst->appendIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), true);
        else if (dst) dst->appendIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), two);
        else sc->addIndexed(tex, bl, vs.data(), vs.size(), idx, m.idx.size(), two);
        meshCount++;
    });
}

void CarmaGame::emitPaths(const std::string& hieBase, int catAI, int catRace, int catStart, int catDrone, bool drone) {
    Pak* pk = nullptr;
    Hie hie;
    if (!loadHie(hieBase, pk, hie)) return;
    std::vector<uint8_t> data;
    std::vector<std::vector<Vec3f>> lines;
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

bool CarmaGame::loadPathPoints(const std::string& base, std::vector<Vec3f>& pts) {
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
    std::vector<std::vector<Vec3f>> lines;
    for (const std::string& ln : hie.lineNames) {
        int le = pk->find(ln);
        if (le < 0 || !pk->extract(le, data)) continue;
        parseLins(data, lines);
    }
    bool got = false;
    walk(hie, [&](const Hie::Node& n, const State& st) {
        if (got || n.type != 6 || n.index < 0 || n.index >= (int)lines.size()) return;
        for (const Vec3f& p : lines[n.index]) pts.push_back(fixX(st.m.transformPoint(p)));
        got = true;
    });
    return got && pts.size() >= 2;
}

}  // namespace carma
