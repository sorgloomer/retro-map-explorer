#include "carma/carma_paths.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include "carma/carma_formats.h"
#include "carma/carma_text.h"

namespace carma {

bool parseAlignmentMatrix(const std::string& t, float m[16]) {
    std::string body;
    for (const std::string& l : splitLines(t)) body += stripComment(l) + " ";
    size_t a = body.find("OPT_ALIGNMENT_MATRIX44");
    if (a == std::string::npos) return false;
    std::istringstream is(body.substr(a + 22));
    for (int i = 0; i < 16; i++)
        if (!(is >> m[i])) return false;
    return true;
}

int parseSmoothIndex(const std::string& t) {
    for (const std::string& l : splitLines(t)) {
        std::string c = stripComment(l);
        size_t a = c.find("OPT_SMOOTH_INDEX");
        if (a != std::string::npos) return std::max(0, atoi(c.c_str() + a + 16));
    }
    return 0;
}

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

std::vector<Vec3f> catmullRom(const std::vector<Vec3f>& p, int n) {
    size_t cnt = p.size();
    if (cnt < 3 || n < 2) return p;
    bool closed = Mover::length3(p.front() - p.back()) < 1.0f;
    if (closed) cnt--;
    auto at = [&](long i) {
        if (closed) return p[(size_t)(((i % (long)cnt) + (long)cnt) % (long)cnt)];
        return p[(size_t)std::min<long>(std::max<long>(i, 0), (long)cnt - 1)];
    };
    std::vector<Vec3f> out;
    size_t segs = closed ? cnt : cnt - 1;
    out.reserve(segs * n + 1);
    for (size_t i = 0; i < segs; i++) {
        Vec3f p0 = at((long)i - 1), p1 = at((long)i), p2 = at((long)i + 1), p3 = at((long)i + 2);
        for (int k = 0; k < n; k++) {
            float t = (float)k / (float)n, t2 = t * t, t3 = t2 * t;
            out.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f);
        }
    }
    out.push_back(closed ? p.back() : at((long)cnt - 1));
    return out;
}

}  // namespace carma
