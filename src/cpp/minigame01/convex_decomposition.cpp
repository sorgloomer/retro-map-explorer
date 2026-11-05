#include "minigame01/convex_decomposition.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include "box3d/box3d.h"

namespace minigame01 {

namespace {

constexpr float kWeldDistance = 2e-3f;         // points this near each other (of the longest side) are one
constexpr size_t kMaxExactVertices = 100;     // a polyhedron with more vertices than this is made of boxes
constexpr double kFlatness = 8e-3;           // how far (of the longest side) a point may be in front of a face of a convex solid: a face that is not flat is not a concavity
constexpr double kFlatCosine = 0.9976;        // two faces that make an angle of less than 4 degrees are one
constexpr int kMaxCuts = 120;                 // the most planes that are tried on one polyhedron (then it is a box after all)
constexpr int kMaxDepth = 14;                 // the most cuts one on top of the other
constexpr size_t kCandidatesPerStep = 24;     // the planes that are tried, the worst concave edge first
constexpr size_t kMaxMergeVertices = 128;     // the most points of two pieces together whose union is tried
constexpr double kMergeTolerance = 2e-3;      // the union of two pieces is convex when its hull is at most this part larger than the two

struct D3 {
    double x = 0, y = 0, z = 0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double norm(D3 a) { return std::sqrt(dot(a, a)); }
D3 unit(D3 a) { const double l = norm(a); return l > 0 ? a * (1.0 / l) : a; }
D3 toD(const Vec3f& v) { return {v.x, v.y, v.z}; }
Vec3f toF(const D3& v) { return Vec3f((float)v.x, (float)v.y, (float)v.z); }

typedef std::array<uint32_t, 3> Tri;

// A closed surface: points, and triangles wound so that their normals point out.
struct Mesh {
    std::vector<D3> p;
    std::vector<Tri> t;
};

double signedVolume(const Mesh& m) {
    double volume = 0;
    for (const Tri& t : m.t) volume += dot(m.p[t[0]], cross(m.p[t[1]], m.p[t[2]])) / 6.0;
    return volume;
}

// The mesh without the triangles that have collapsed, and without the points that are left without a triangle. (Triangles that are only very thin stay:
// they keep the edges of their neighbours whole.)
Mesh cleaned(const Mesh& m) {
    Mesh out;
    std::vector<int> index(m.p.size(), -1);
    for (Tri t : m.t) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        for (int k = 0; k < 3; k++) {
            if (index[t[k]] < 0) { index[t[k]] = (int)out.p.size(); out.p.push_back(m.p[t[k]]); }
            t[k] = (uint32_t)index[t[k]];
        }
        out.t.push_back(t);
    }
    return out;
}

// Whether a surface is the surface of a convex solid: all its points are on one side of the plane of each of its triangles.
bool isConvexMesh(const Mesh& m, double tolerance, double areaEpsilon) {
    for (const Tri& t : m.t) {
        const D3 c = cross(m.p[t[1]] - m.p[t[0]], m.p[t[2]] - m.p[t[0]]);
        if (norm(c) < areaEpsilon) continue;
        const D3 n = unit(c);
        for (const D3& v : m.p)
            if (dot(n, v - m.p[t[0]]) > tolerance) return false;   // (the normals point out: nothing is in front of a face)
    }
    return true;
}

// Whether every edge of a surface is the edge of exactly two triangles: it is the surface of a solid.
bool isClosed(const std::vector<uint32_t>& triangles) {
    std::map<std::pair<uint32_t, uint32_t>, int> uses;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        for (int k = 0; k < 3; k++) {
            const uint32_t a = triangles[t + k], b = triangles[t + (k + 1) % 3];
            uses[std::make_pair(std::min(a, b), std::max(a, b))]++;
        }
    for (const auto& u : uses)
        if (u.second != 2) return false;
    return true;
}

float hullVolume(const std::vector<Vec3f>& points) {
    std::vector<b3Vec3> in;
    for (const Vec3f& p : points) in.push_back({p.x, p.y, p.z});
    if (in.size() < 4) return 0;
    b3HullData* hull = b3CreateHull(in.data(), (int)in.size(), (int)kMaxMergeVertices);
    if (!hull) return 0;
    const float volume = b3ComputeHullMass(hull, 1.0f).mass;   // (the mass at a density of 1)
    b3DestroyHull(hull);
    return std::max(0.0f, volume);
}

// The corners of a box around some points: the smaller of the box along the axes and the one along their principal axes.
std::vector<Vec3f> boxAround(const std::vector<Vec3f>& points) {
    D3 mean;
    for (const Vec3f& p : points) mean = mean + toD(p);
    mean = mean * (1.0 / (double)points.size());
    // covariance, and its eigenvectors by Jacobi rotations
    double a[3][3] = {}, v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const Vec3f& p : points) {
        const D3 d = toD(p) - mean;
        const double c[3] = {d.x, d.y, d.z};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) a[i][j] += c[i] * c[j];
    }
    for (int sweep = 0; sweep < 30; sweep++) {
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                if (std::fabs(a[p][q]) < 1e-18) continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; k++) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; k++) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    D3 axes[2][3] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{v[0][0], v[1][0], v[2][0]}, {v[0][1], v[1][1], v[2][1]}, {v[0][2], v[1][2], v[2][2]}}};
    double bestVolume = 1e300;
    std::vector<Vec3f> best;
    for (int which = 0; which < 2; which++) {
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (const Vec3f& p : points)
            for (int k = 0; k < 3; k++) {
                const double s = dot(toD(p), axes[which][k]);
                lo[k] = std::min(lo[k], s);
                hi[k] = std::max(hi[k], s);
            }
        const double volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
        if (volume >= bestVolume) continue;
        bestVolume = volume;
        best.clear();
        for (int corner = 0; corner < 8; corner++) {
            D3 p;
            for (int k = 0; k < 3; k++) p = p + axes[which][k] * ((corner >> k) & 1 ? hi[k] : lo[k]);
            best.push_back(toF(p));
        }
    }
    return best;
}

// A plane that cuts a polyhedron, and how much the cut is worth.
struct Cut {
    D3 n;
    double offset = 0;
    double score = 0;
};

// The cuts to try: for each concave edge (the two faces at it bend outwards: a vertex of one is in front of the other), the plane through the edge
// and the vertex that is the closest to the bisector of the inside angle at the edge. That plane halves the angle of the solid that is more than
// a half turn. The edges with the most to them come first.
std::vector<Cut> candidateCuts(const Mesh& m, double tolerance, double areaEpsilon) {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;   // the triangles of an edge
    for (size_t i = 0; i < m.t.size(); i++)
        for (int k = 0; k < 3; k++) {
            const uint32_t a = m.t[i][k], b = m.t[i][(k + 1) % 3];
            edges[std::make_pair(std::min(a, b), std::max(a, b))].push_back((uint32_t)i);
        }
    struct Concave {
        uint32_t a, b;
        int bisector;     // the vertex that is the closest to the bisector of the angle inside, or -1
        D3 n1, n2;
        double score;
    };
    std::vector<Concave> found;
    std::vector<Cut> cuts;
    for (const auto& edge : edges) {
        if (edge.second.size() != 2) continue;
        const Tri& t1 = m.t[edge.second[0]];
        const Tri& t2 = m.t[edge.second[1]];
        uint32_t a = 0, b = 0, o1 = 0, o2 = 0;
        for (int k = 0; k < 3; k++) {
            const uint32_t x = t1[k], y = t1[(k + 1) % 3];
            if (std::min(x, y) == edge.first.first && std::max(x, y) == edge.first.second) { a = x; b = y; o1 = t1[(k + 2) % 3]; }
        }
        for (int k = 0; k < 3; k++)
            if (t2[k] != a && t2[k] != b) o2 = t2[k];
        const D3 c1 = cross(m.p[t1[1]] - m.p[t1[0]], m.p[t1[2]] - m.p[t1[0]]), c2 = cross(m.p[t2[1]] - m.p[t2[0]], m.p[t2[2]] - m.p[t2[0]]);
        if (norm(c1) < areaEpsilon || norm(c2) < areaEpsilon) continue;   // (a sliver has no say)
        const D3 n1 = unit(c1), n2 = unit(c2);
        const double h1 = dot(n1, m.p[o2] - m.p[a]), h2 = dot(n2, m.p[o1] - m.p[a]);
        const double height = std::max(h1, h2);
        if (height <= tolerance) continue;   // (it is not concave)
        if (dot(n1, n2) > kFlatCosine) continue;   // (the faces are nearly one: a quad that is not quite flat, not a concavity)
        const D3 e = m.p[b] - m.p[a];
        const double length = norm(e);
        if (length < 1e-12) continue;
        const D3 direction = e * (1.0 / length);
        auto across = [&](const D3& v) { D3 w = v - m.p[a]; return w - direction * dot(w, direction); };
        const D3 sum = unit(across(m.p[o1])) + unit(across(m.p[o2]));   // (to the middle of the angle that is empty)
        if (norm(sum) < 1e-9) continue;
        const D3 inside = unit(sum) * -1.0;
        int best = -1;
        double bestCosine = 0;
        for (size_t v = 0; v < m.p.size(); v++) {
            if (v == a || v == b) continue;
            const D3 w = across(m.p[v]);
            const double l = norm(w);
            if (l < tolerance) continue;
            const double cosine = dot(w, inside) / l;
            if (cosine > bestCosine) { bestCosine = cosine; best = (int)v; }
        }
        found.push_back({a, b, best, n1, n2, length * height});
    }
    std::sort(found.begin(), found.end(), [](const Concave& x, const Concave& y) { return x.score > y.score; });
    // whether the segment from the middle of an edge to a vertex is inside the solid: it goes into the solid at the edge, and no face is in its way
    auto visible = [&](const Concave& edge, uint32_t w) {
        if (w == edge.a || w == edge.b) return false;
        const D3 middle = (m.p[edge.a] + m.p[edge.b]) * 0.5;
        const D3 to = m.p[w] - middle;
        if (dot(to, edge.n1) > tolerance && dot(to, edge.n2) > tolerance) return false;   // (it goes into the empty angle)
        for (const Tri& t : m.t) {
            if (t[0] == w || t[1] == w || t[2] == w) continue;
            if ((t[0] == edge.a || t[1] == edge.a || t[2] == edge.a) && (t[0] == edge.b || t[1] == edge.b || t[2] == edge.b)) continue;   // (the faces at the edge)
            const D3 e1 = m.p[t[1]] - m.p[t[0]], e2 = m.p[t[2]] - m.p[t[0]];
            const D3 pv = cross(to, e2);
            const double det = dot(e1, pv);
            if (std::fabs(det) < 1e-18) continue;
            const D3 tv = middle - m.p[t[0]];
            const double u = dot(tv, pv) / det;
            const D3 qv = cross(tv, e1);
            const double v = dot(to, qv) / det;
            const double s = dot(e2, qv) / det;
            if (u > 1e-6 && v > 1e-6 && u + v < 1 - 1e-6 && s > 1e-6 && s < 1 - 1e-6) return false;
        }
        return true;
    };
    // for each concave edge: first the planes through it and a vertex of another concave edge that can be seen from it, then the bisector's
    auto planeThrough = [&](const Concave& edge, int w) {
        const D3 e = m.p[edge.b] - m.p[edge.a];
        const D3 normal = unit(cross(e, m.p[w] - m.p[edge.a]));
        if (norm(normal) < 0.5) return;
        Cut cut;
        cut.n = normal;
        cut.offset = dot(normal, m.p[edge.a]);
        cuts.push_back(cut);
    };
    for (const Concave& edge : found) {
        std::vector<uint32_t> seen;
        for (const Concave& other : found) {
            if (&other == &edge) continue;
            for (uint32_t w : {other.a, other.b})
                if (std::find(seen.begin(), seen.end(), w) == seen.end() && visible(edge, w)) seen.push_back(w);
        }
        for (uint32_t w : seen) planeThrough(edge, (int)w);
        if (edge.bisector >= 0) planeThrough(edge, edge.bisector);
    }
    return cuts;
}

// The triangles of a polygon with the points `loop` in the plane with the normal n (the points go round counter-clockwise seen from the front), by
// cutting off its corners one at a time. False when it does not work (a loop that is a hole, for one).
bool triangulateLoop(const std::vector<D3>& pool, std::vector<uint32_t> loop, D3 n, double areaEpsilon, std::vector<Tri>& out) {
    const D3 helper = std::fabs(n.x) < 0.7 ? D3{1, 0, 0} : D3{0, 1, 0};
    const D3 u = unit(cross(n, helper));
    const D3 v = cross(n, u);
    auto at = [&](uint32_t i) { return std::make_pair(dot(pool[i], u), dot(pool[i], v)); };
    auto turn = [&](uint32_t a, uint32_t b, uint32_t c) {
        const auto pa = at(a), pb = at(b), pc = at(c);
        return (pb.first - pa.first) * (pc.second - pb.second) - (pb.second - pa.second) * (pc.first - pb.first);
    };
    int pass = 0;   // 0: corners that turn; 1: when there are none, the ones that are on a line (a flat triangle keeps the edges whole)
    while (loop.size() > 3) {
        bool clipped = false;
        const size_t count = loop.size();
        for (size_t i = 0; i < count && !clipped; i++) {
            const uint32_t a = loop[(i + count - 1) % count], b = loop[i], c = loop[(i + 1) % count];
            const double corner = turn(a, b, c);
            if (pass == 0 ? corner <= areaEpsilon : corner < -areaEpsilon) continue;
            bool empty = true;
            for (size_t k = 0; k < count && empty; k++) {
                const uint32_t q = loop[k];
                if (q == a || q == b || q == c) continue;
                const double d1 = turn(a, b, q), d2 = turn(b, c, q), d3 = turn(c, a, q);
                if (d1 > areaEpsilon && d2 > areaEpsilon && d3 > areaEpsilon) empty = false;
            }
            if (!empty) continue;
            out.push_back({a, b, c});
            loop.erase(loop.begin() + (long)i);
            clipped = true;
        }
        if (clipped) { pass = 0; continue; }
        if (pass == 0) { pass = 1; continue; }
        return false;
        /*
        // no corner to cut off: the points on a line may go
        bool removed = false;
        for (size_t i = 0; i < count && !removed; i++)
            if (std::fabs(turn(loop[(i + count - 1) % count], loop[i], loop[(i + 1) % count])) <= areaEpsilon) { loop.erase(loop.begin() + (long)i); removed = true; }
        if (!removed) return false;
        */
    }
    if (loop.size() == 3) out.push_back({loop[0], loop[1], loop[2]});
    return true;
}

// The two parts of a closed surface on the two sides of a plane (the one with the points of a negative distance, and the other), each closed with a
// cap over the cut. False when the cut is not one that this can close (the cut of the solid has a hole in it, or touches itself).
bool cutMesh(const Mesh& m, D3 n, double offset, double epsilon, double areaEpsilon, Mesh& negative, Mesh& positive) {
    std::vector<D3> pool = m.p;
    std::vector<double> distance(pool.size());
    for (size_t i = 0; i < pool.size(); i++) distance[i] = dot(n, pool[i]) - offset;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> crossings;
    // the point where an edge from the point a (in front by a distance of less than - epsilon) to the point b crosses the plane
    auto crossing = [&](uint32_t a, uint32_t b) -> uint32_t {
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        auto found = crossings.find(key);
        if (found != crossings.end()) return found->second;
        const double s = distance[a] / (distance[a] - distance[b]);
        pool.push_back(pool[a] + (pool[b] - pool[a]) * s);
        distance.push_back(0);
        crossings[key] = (uint32_t)pool.size() - 1;
        return (uint32_t)pool.size() - 1;
    };
    std::multimap<uint32_t, uint32_t> next;   // along the edge of the cap of the negative part (a point where the cut touches itself has several)
    auto addFan = [](std::vector<Tri>& out, std::vector<uint32_t>& poly) {
        std::vector<uint32_t> clean;
        for (uint32_t i : poly)
            if (clean.empty() || clean.back() != i) clean.push_back(i);
        while (clean.size() > 1 && clean.front() == clean.back()) clean.pop_back();
        for (size_t k = 1; k + 1 < clean.size(); k++) out.push_back({clean[0], clean[k], clean[k + 1]});
    };
    negative.t.clear();
    positive.t.clear();
    for (const Tri& t : m.t) {
        bool isNegative[3];
        int count = 0;
        for (int k = 0; k < 3; k++) { isNegative[k] = distance[t[k]] < -1e-9 * epsilon; count += isNegative[k]; }
        if (count == 0) { positive.t.push_back(t); continue; }
        if (count == 3) { negative.t.push_back(t); continue; }
        std::vector<uint32_t> negativePoly, positivePoly;
        uint32_t exitPoint = 0, entryPoint = 0;
        for (int k = 0; k < 3; k++) {
            const uint32_t a = t[k], b = t[(k + 1) % 3];
            (isNegative[k] ? negativePoly : positivePoly).push_back(a);
            if (isNegative[k] != isNegative[(k + 1) % 3]) {
                const uint32_t point = isNegative[k] ? crossing(a, b) : crossing(b, a);
                negativePoly.push_back(point);
                positivePoly.push_back(point);
                (isNegative[k] ? exitPoint : entryPoint) = point;
            }
        }
        addFan(negative.t, negativePoly);
        addFan(positive.t, positivePoly);
        if (entryPoint != exitPoint) {
            next.emplace(entryPoint, exitPoint);
        }
    }
    // the caps: the loops of the edge
    const D3 helper = std::fabs(n.x) < 0.7 ? D3{1, 0, 0} : D3{0, 1, 0};
    const D3 axisU = unit(cross(n, helper));
    const D3 axisV = cross(n, axisU);
    auto flat = [&](uint32_t i) { return std::make_pair(dot(pool[i], axisU), dot(pool[i], axisV)); };
    while (!next.empty()) {
        std::vector<uint32_t> loop;
        const uint32_t start = next.begin()->first;
        uint32_t at = start;
        uint32_t from = start;
        bool first = true;
        for (;;) {
            loop.push_back(at);
            auto range = next.equal_range(at);
            if (range.first == range.second) return false;   // (it does not close)
            auto step = range.first;
            if (!first && std::distance(range.first, range.second) > 1) {
                // several ways on: the one that turns the most to the left keeps the loops apart
                const auto pa = flat(from), pb = flat(at);
                double bestAngle = -10;
                for (auto it = range.first; it != range.second; ++it) {
                    const auto pc = flat(it->second);
                    const double angle = std::atan2((pb.first - pa.first) * (pc.second - pb.second) - (pb.second - pa.second) * (pc.first - pb.first),
                                                    (pb.first - pa.first) * (pc.first - pb.first) + (pb.second - pa.second) * (pc.second - pb.second));
                    if (angle > bestAngle) { bestAngle = angle; step = it; }
                }
            }
            const uint32_t to = step->second;
            next.erase(step);
            first = false;
            from = at;
            at = to;
            if (at == start) break;
        }
        double area = 0;
        for (size_t i = 0; i < loop.size(); i++) area += dot(n, cross(pool[loop[i]] - pool[loop[0]], pool[loop[(i + 1) % loop.size()]] - pool[loop[0]])) * 0.5;
        if (area < -areaEpsilon) return false;   // (a hole in the cut)
        std::vector<Tri> cap;
        if (area <= areaEpsilon) {
            // (a loop with no area still needs triangles, flat ones, or its edges are left open)
            for (size_t k = 1; k + 1 < loop.size(); k++) cap.push_back({loop[0], loop[k], loop[k + 1]});
        } else if (!triangulateLoop(pool, loop, n, areaEpsilon, cap)) {
            return false;
        }
        for (const Tri& c : cap) {
            negative.t.push_back(c);
            positive.t.push_back({c[0], c[2], c[1]});
        }
    }
    negative.p = pool;
    positive.p = pool;
    return true;
}

struct Splitter {
    double tolerance = 0;     // how far a point may be in front of a face of a convex solid
    double epsilon = 0;       // the distance to a plane that is taken for none
    double areaEpsilon = 0;
    double volumeScale = 0;
    int cuts = 0;             // the cuts that are left to try
};

// The convex pieces of a closed surface: the surface is cut where it is concave until the parts are convex. False when it does not come out.
bool split(const Mesh& whole, Splitter& s, int depth, std::vector<Mesh>& out) {
    const Mesh m = cleaned(whole);
    const double volume = signedVolume(m);
    if (m.t.size() < 4 || volume <= 1e-9 * s.volumeScale) return true;   // (nothing there)
    if (isConvexMesh(m, s.tolerance, s.areaEpsilon)) { out.push_back(m); return true; }
    if (depth >= kMaxDepth) return false;
    std::vector<Cut> cuts = candidateCuts(m, s.tolerance, s.areaEpsilon);
    if (cuts.empty()) { out.push_back(m); return true; }   // (no concave edge: it is convex enough)
    if (cuts.size() > kCandidatesPerStep) cuts.resize(kCandidatesPerStep);
    for (const Cut& cut : cuts) {
        Mesh negative, positive;
        const bool okCut = cutMesh(m, cut.n, cut.offset, s.epsilon, s.areaEpsilon, negative, positive);
        if (!okCut) continue;
        const double a = signedVolume(negative), b = signedVolume(positive);
        if (a < 1e-5 * volume || b < 1e-5 * volume || std::fabs(a + b - volume) > 0.01 * volume) continue;   // (no cut: nothing on a side, or it went wrong)
        if (--s.cuts < 0) return false;
        const size_t before = out.size();
        if (split(negative, s, depth + 1, out) && split(positive, s, depth + 1, out)) return true;
        out.resize(before);
    }
    return false;
}

// The points of a polyhedron with those that are (nearly) at one place made one (the files have some that are doubled, or a hair apart, which would
// make the surface not close), and the triangles over them without those that have collapsed.
void weld(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles, float longest, std::vector<Vec3f>& points, std::vector<uint32_t>& welded) {
    const float tolerance = std::max(1e-6f, kWeldDistance * longest);
    std::vector<int> index(vertices.size(), -1);
    std::map<std::array<long long, 3>, uint32_t> known;   // (for big surfaces: the cell of a point)
    auto place = [&](uint32_t v) {
        if (index[v] >= 0) return;
        if (vertices.size() <= 256) {
            for (size_t j = 0; j < points.size(); j++)
                if (length(points[j] - vertices[v]) <= tolerance) { index[v] = (int)j; return; }
        } else {
            const std::array<long long, 3> key = {std::llround(vertices[v].x / tolerance), std::llround(vertices[v].y / tolerance), std::llround(vertices[v].z / tolerance)};
            auto found = known.find(key);
            if (found != known.end()) { index[v] = (int)found->second; return; }
            known.emplace(key, (uint32_t)points.size());
        }
        index[v] = (int)points.size();
        points.push_back(vertices[v]);
    };
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        if (triangles[t] >= vertices.size() || triangles[t + 1] >= vertices.size() || triangles[t + 2] >= vertices.size()) continue;
        for (int k = 0; k < 3; k++) place(triangles[t + k]);
        const uint32_t a = (uint32_t)index[triangles[t]], b = (uint32_t)index[triangles[t + 1]], c = (uint32_t)index[triangles[t + 2]];
        if (a == b || b == c || a == c) continue;
        welded.push_back(a); welded.push_back(b); welded.push_back(c);
    }
}

// Closes the holes of a surface (a face that a file leaves out): the edges that only one triangle has make loops, which are covered. Loops that do
// not make sense (an edge that has three triangles, a loop that branches, a big one) are left as they are.
void closeHoles(const std::vector<Vec3f>& points, std::vector<uint32_t>& triangles, float longest) {
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        for (int k = 0; k < 3; k++) directed[std::make_pair(triangles[t + k], triangles[t + (k + 1) % 3])]++;
    std::map<uint32_t, uint32_t> next;   // along the edge of the cover: against the direction of the edges of the surface
    for (const auto& e : directed) {
        if (e.second != 1) return;                                                    // (an edge with more than one triangle on one side)
        if (directed.count(std::make_pair(e.first.second, e.first.first))) continue;   // (it has its neighbour)
        if (!next.emplace(e.first.second, e.first.first).second) return;             // (a point where the hole touches itself)
    }
    std::vector<D3> pool;
    for (const Vec3f& p : points) pool.push_back(toD(p));
    while (!next.empty()) {
        std::vector<uint32_t> loop;
        const uint32_t start = next.begin()->first;
        uint32_t at = start;
        for (;;) {
            loop.push_back(at);
            auto step = next.find(at);
            if (step == next.end()) return;   // (it does not close)
            const uint32_t to = step->second;
            next.erase(step);
            at = to;
            if (at == start) break;
        }
        if (loop.size() < 3 || loop.size() > 16) continue;
        D3 normal;   // (Newell: the normal of the loop, the way it goes round)
        for (size_t i = 0; i < loop.size(); i++) normal = normal + cross(pool[loop[i]], pool[loop[(i + 1) % loop.size()]]);
        if (norm(normal) < 1e-12) continue;
        std::vector<Tri> cover;
        if (!triangulateLoop(pool, loop, unit(normal), 1e-9 * (double)longest * longest, cover)) continue;
        for (const Tri& c : cover) triangles.insert(triangles.end(), c.begin(), c.end());
    }
}

// The pieces of a closed polyhedron: cut where it is concave, then the pieces that make a convex whole again are joined.
bool decomposeByCutting(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles, float longest, std::vector<std::vector<Vec3f>>& out) {
    Mesh whole;
    for (const Vec3f& v : vertices) whole.p.push_back(toD(v));
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) whole.t.push_back({triangles[t], triangles[t + 1], triangles[t + 2]});
    double volume = signedVolume(whole);
    if (volume < 0) {
        for (Tri& t : whole.t) std::swap(t[1], t[2]);
        volume = -volume;
    }
    const double scale = (double)longest * longest * longest;
    if (volume < 1e-9 * scale) return false;
    Splitter splitter;
    splitter.tolerance = kFlatness * longest;
    splitter.epsilon = 1e-3 * longest;
    splitter.areaEpsilon = 1e-9 * (double)longest * longest;
    splitter.volumeScale = scale;
    splitter.cuts = kMaxCuts;
    std::vector<Mesh> parts;
    if (!split(whole, splitter, 0, parts) || parts.empty()) return false;
    double sum = 0;
    struct Piece { std::vector<Vec3f> points; double volume; D3 lo, hi; int id; };
    std::vector<Piece> pieces;
    int nextId = 0;
    for (const Mesh& part : parts) {
        Piece piece;
        piece.volume = signedVolume(part);
        sum += piece.volume;
        piece.lo = piece.hi = part.p[0];
        for (const D3& q : part.p) {
            piece.points.push_back(toF(q));
            piece.lo = {std::min(piece.lo.x, q.x), std::min(piece.lo.y, q.y), std::min(piece.lo.z, q.z)};
            piece.hi = {std::max(piece.hi.x, q.x), std::max(piece.hi.y, q.y), std::max(piece.hi.z, q.z)};
        }
        piece.id = nextId++;
        pieces.push_back(std::move(piece));
    }
    if (std::fabs(sum - volume) > 0.03 * volume) return false;   // (the pieces must make the polyhedron)
    // join the pieces whose union is convex, the pair that loses nothing and is the largest first, else the one that loses the least
    std::map<std::pair<int, int>, double> excess;   // per pair of ids: how much larger the hull is than the two, or -1 for no
    const double slack = splitter.epsilon;
    while (pieces.size() > 1) {
        int bestA = -1, bestB = -1;
        double bestScore = 1e300;
        for (size_t i = 0; i < pieces.size(); i++)
            for (size_t j = i + 1; j < pieces.size(); j++) {
                const Piece& a = pieces[i];
                const Piece& b = pieces[j];
                if (a.lo.x > b.hi.x + slack || b.lo.x > a.hi.x + slack || a.lo.y > b.hi.y + slack || b.lo.y > a.hi.y + slack ||
                    a.lo.z > b.hi.z + slack || b.lo.z > a.hi.z + slack) continue;   // (they do not touch)
                const auto key = std::make_pair(std::min(a.id, b.id), std::max(a.id, b.id));
                auto found = excess.find(key);
                double value;
                if (found != excess.end()) {
                    value = found->second;
                } else {
                    value = -1;
                    std::vector<Vec3f> together = a.points;
                    together.insert(together.end(), b.points.begin(), b.points.end());
                    const double both = a.volume + b.volume;
                    const double hull = hullVolume(together);
                    if (hull > 0 && hull <= both * (1 + kMergeTolerance) + 1e-7 * scale) value = std::max(0.0, hull - both) / both;
                    excess[key] = value;
                }
                const double score = value < 0 ? 1e300 : value < 1e-6 ? -(a.volume + b.volume) : value;
                if (value >= 0 && score < bestScore) { bestScore = score; bestA = (int)i; bestB = (int)j; }
            }
        if (bestA < 0) break;
        Piece merged;
        merged.points = pieces[bestA].points;
        merged.points.insert(merged.points.end(), pieces[bestB].points.begin(), pieces[bestB].points.end());
        merged.volume = pieces[bestA].volume + pieces[bestB].volume;
        merged.lo = {std::min(pieces[bestA].lo.x, pieces[bestB].lo.x), std::min(pieces[bestA].lo.y, pieces[bestB].lo.y), std::min(pieces[bestA].lo.z, pieces[bestB].lo.z)};
        merged.hi = {std::max(pieces[bestA].hi.x, pieces[bestB].hi.x), std::max(pieces[bestA].hi.y, pieces[bestB].hi.y), std::max(pieces[bestA].hi.z, pieces[bestB].hi.z)};
        merged.id = nextId++;
        pieces.erase(pieces.begin() + bestB);
        pieces.erase(pieces.begin() + bestA);
        pieces.push_back(std::move(merged));
    }
    // the pieces as points, those at one place (the cuts make a few) once
    const double cell = std::max(1e-6, 1e-5 * (double)longest);
    for (Piece& piece : pieces) {
        std::map<std::array<long long, 3>, bool> seen;
        std::vector<Vec3f> unique;
        for (const Vec3f& p : piece.points)
            if (seen.emplace(std::array<long long, 3>{std::llround(p.x / cell), std::llround(p.y / cell), std::llround(p.z / cell)}, true).second) unique.push_back(p);
        out.push_back(std::move(unique));
    }
    return true;
}

}  // namespace

std::vector<SurfaceComponent> splitSurfaceIntoComponents(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles) {
    const size_t count = triangles.size() / 3;
    auto valid = [&](size_t t) { return triangles[3 * t] < vertices.size() && triangles[3 * t + 1] < vertices.size() && triangles[3 * t + 2] < vertices.size(); };
    std::vector<int> parent(count);
    for (size_t i = 0; i < count; i++) parent[i] = (int)i;
    auto find = [&](int i) { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
    // two triangles are one part when they share an edge (an edge is two vertices): polyhedra that touch each other at a vertex are two
    std::map<std::pair<uint32_t, uint32_t>, int> edgeOwner;
    for (size_t t = 0; t < count; t++) {
        if (!valid(t)) continue;
        for (int k = 0; k < 3; k++) {
            const uint32_t a = triangles[3 * t + k], b = triangles[3 * t + (k + 1) % 3];
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto it = edgeOwner.emplace(key, (int)t);
            if (!it.second) parent[find((int)t)] = find(it.first->second);
        }
    }
    std::vector<int> componentOf(count, -1);   // the component of the root of a triangle
    std::vector<SurfaceComponent> components;
    std::vector<std::vector<int>> newIndex;    // per component: a vertex in its points
    for (size_t t = 0; t < count; t++) {
        if (!valid(t)) continue;
        const int root = find((int)t);
        if (componentOf[root] < 0) {
            componentOf[root] = (int)components.size();
            components.emplace_back();
            newIndex.emplace_back(vertices.size(), -1);
        }
        SurfaceComponent& component = components[componentOf[root]];
        std::vector<int>& index = newIndex[componentOf[root]];
        for (int k = 0; k < 3; k++) {
            const uint32_t v = triangles[3 * t + k];
            if (index[v] < 0) {
                index[v] = (int)component.points.size();
                component.points.push_back(vertices[v]);
            }
            component.triangles.push_back((uint32_t)index[v]);
        }
    }
    return components;
}

std::vector<std::vector<Vec3f>> decomposeIntoConvexPieces(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles) {
    std::vector<std::vector<Vec3f>> result;
    if (vertices.empty()) return result;
    Vec3f lo = vertices[0], hi = vertices[0];
    for (const Vec3f& v : vertices) {
        lo = Vec3f(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
        hi = Vec3f(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
    }
    const Vec3f size = hi - lo;
    const float longest = std::max(size.x, std::max(size.y, size.z));
    if (longest < 1e-6f) return result;
    for (const SurfaceComponent& whole : splitSurfaceIntoComponents(vertices, triangles)) {
        // the points of a polyhedron that are at one place are one (the files have some that are doubled, which would make the surface not close)
        SurfaceComponent component;
        weld(whole.points, whole.triangles, longest, component.points, component.triangles);
        if (component.points.size() < 3) continue;
        closeHoles(component.points, component.triangles, longest);
        Mesh mesh;
        for (const Vec3f& v : component.points) mesh.p.push_back(toD(v));
        for (size_t t = 0; t + 2 < component.triangles.size(); t += 3) mesh.t.push_back({component.triangles[t], component.triangles[t + 1], component.triangles[t + 2]});
        // (a convex polyhedron is one piece; the test does not need the triangles to be wound the same way)
        bool convex = true;
        for (const Tri& t : mesh.t) {
            const D3 n = unit(cross(mesh.p[t[1]] - mesh.p[t[0]], mesh.p[t[2]] - mesh.p[t[0]]));
            if (norm(n) < 0.5) continue;
            bool above = false, below = false;
            for (const D3& v : mesh.p) {
                const double side = dot(n, v - mesh.p[t[0]]);
                if (side > kFlatness * longest) above = true;
                if (side < -kFlatness * longest) below = true;
            }
            if (above && below) { convex = false; break; }
        }
        std::vector<std::vector<Vec3f>> pieces;
        if (convex) {
            result.push_back(component.points);
        } else if (component.points.size() <= kMaxExactVertices && isClosed(component.triangles) && decomposeByCutting(component.points, component.triangles, longest, pieces)) {
            for (std::vector<Vec3f>& piece : pieces) result.push_back(std::move(piece));
        } else {
            result.push_back(boxAround(component.points));   // too complex, or not a solid, or it did not work: a box
        }
    }
    return result;
}

}  // namespace minigame01
