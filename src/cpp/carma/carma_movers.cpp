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

void CarmaGame::loadFollowers(const std::string& fname, Scene& out, int cat, int pathCat) {
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
    std::map<std::string, std::vector<Vec3f>> pathCache;
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
            std::vector<Vec3f> pts;
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

void CarmaGame::loadAnimProps(const std::string& fname, Scene& out, int cat, int pathCat) {
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
        m.path = {Vec3f()};
        const Mat4& R = lastRootMat;
        m.pivot = fixX(Vec3f(R.m[12], R.m[13], R.m[14]));
        for (int k = 0; k < nkeys; k++) {
            const float* v = &nums[k * 8];
            AnimKey key;
            key.dur = v[0];
            key.trans = fixX(Vec3f(v[1], v[2], v[3]));
            // the axis is given in the hierarchy's own frame; mirroring turns a rotation about a into one about (ax, -ay, -az)
            Vec3f a(R.m[0] * v[4] + R.m[4] * v[5] + R.m[8] * v[6], R.m[1] * v[4] + R.m[5] * v[5] + R.m[9] * v[6],
                   R.m[2] * v[4] + R.m[6] * v[5] + R.m[10] * v[6]);
            key.axis = kMirrorX ? Vec3f(a.x, -a.y, -a.z) : a;
            key.rotDeg = v[7];
            m.keys.push_back(key);
        }
        // the route of the prop: from the centre of its geometry through the translations of its movements
        {
            Vec3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (const Batch& b : out.moverGeoms[m.geom].batches)
                for (const SceneVertex& v : b.verts) {
                    lo = Vec3f(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
                    hi = Vec3f(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
                }
            Vec3f p = (lo + hi) * 0.5f;
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
bool CarmaGame::pathAlignment(const std::string& pathBase, float m[16]) {
    std::string t;
    return readNamedText(pathBase + ".txt", t) && parseAlignmentMatrix(t, m);
}

int CarmaGame::followerSmoothIndex(const std::string& hieBase) {
    std::string t;
    return readNamedText(hieBase + ".txt", t) ? parseSmoothIndex(t) : 0;
}

}  // namespace carma
