#include "minigame01/convex_decomposition.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "box3d/box3d.h"

namespace minigame01 {

namespace {

constexpr int kMaxSamplesPerVoxel = 3;
constexpr size_t kMeasureHullPoints = 1500;   // the most points that the hull that measures a piece is made of (the rest is left out evenly)
constexpr size_t kEvaluationHullPoints = 400; // ... and the hulls that compare the planes
constexpr int kMaxEvaluations = 300;          // the most hulls that a decomposition makes to compare planes: then it is as far as it gets (an object whose
                                              // surface is not quite what a solid is can make the algorithm try forever to make pieces of it)
constexpr int kSplitsPerAxis = 12;            // the planes that are tried along every axis (fewer when the piece is thin)
constexpr int kEvaluationHullVertices = 24;   // the hulls that compare the planes need not be exact
constexpr int kMeasureHullVertices = 48;      // the hull that says whether a piece is convex enough

struct Grid {
    int nx = 0, ny = 0, nz = 0;
    Vec3f origin;
    float cell = 1;
    std::vector<uint8_t> state;                  // 0: empty, 1: the surface is in it, 2: inside
    std::vector<float> weight;                   // how much of a voxel is the object
    std::vector<std::vector<Vec3f>> samples;     // points of the surface in each voxel
    std::vector<std::vector<Vec3f>> corners;     // the corners of the triangles in each voxel (they are what the hulls are made of: always kept)
    int index(int x, int y, int z) const { return (z * ny + y) * nx + x; }
    void coordinates(int i, int& x, int& y, int& z) const { x = i % nx; y = (i / nx) % ny; z = i / (nx * ny); }
};

float hullVolume(const std::vector<b3Vec3>& points, int maxVertices) {
    if (points.size() < 4) return 0;
    b3HullData* hull = b3CreateHull(points.data(), (int)points.size(), maxVertices);
    if (!hull) return 0;
    const float volume = b3ComputeHullMass(hull, 1.0f).mass;   // (the mass at a density of 1)
    b3DestroyHull(hull);
    return std::max(0.0f, volume);
}

// Whether a point is inside a closed surface: a ray from it crosses the surface an odd number of times (the direction is a slanted one, so that
// it does not run along the edges of boxes).
bool isInside(const Vec3f& origin, const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles) {
    const Vec3f direction(1.0f, 0.00137f, 0.00071f);
    int crossings = 0;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        if (triangles[t] >= vertices.size() || triangles[t + 1] >= vertices.size() || triangles[t + 2] >= vertices.size()) continue;
        const Vec3f& a = vertices[triangles[t]];
        const Vec3f e1 = vertices[triangles[t + 1]] - a, e2 = vertices[triangles[t + 2]] - a;
        const Vec3f p = cross(direction, e2);
        const float determinant = dot(e1, p);
        if (std::fabs(determinant) < 1e-12f) continue;
        const float inverse = 1.0f / determinant;
        const Vec3f toOrigin = origin - a;
        const float u = dot(toOrigin, p) * inverse;
        if (u < 0.0f || u > 1.0f) continue;
        const Vec3f q = cross(toOrigin, e1);
        const float v = dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f) continue;
        if (dot(e2, q) * inverse > 0.0f) crossings++;
    }
    return (crossings & 1) != 0;
}

// A piece: some voxels, and how much its convex hull has that it does not.
struct Piece {
    std::vector<int> voxels;
    float volume = 0;        // of the voxels
    float hull = 0;          // of the hull of its surface
    float excess() const { return std::max(0.0f, hull - volume); }
};

// The points of the surface in some voxels, for a hull that is only compared: the corners of the triangles, and the samples between them, left
// out evenly when they are more than a hull of that many points is worth.
void pointsOf(const Grid& grid, const std::vector<int>& voxels, std::vector<b3Vec3>& out, size_t limit) {
    out.clear();
    size_t samples = 0;
    for (int v : voxels) {
        for (const Vec3f& p : grid.corners[v]) out.push_back({p.x, p.y, p.z});
        samples += grid.samples[v].size();
    }
    const size_t room = limit > out.size() ? limit - out.size() : 0;
    const size_t stride = std::max<size_t>(1, (samples + room) / std::max<size_t>(1, room));
    size_t counter = 0;
    for (int v : voxels)
        for (const Vec3f& p : grid.samples[v])
            if (counter++ % stride == 0) out.push_back({p.x, p.y, p.z});
}

// The volume of some voxels: the ones inside the surface count whole, the ones that the surface is in half (they are half in and half out
// on average; the whole of them would make a thick object much larger than it is), unless nothing is inside next to them (a thin object).
float volumeOf(const Grid& grid, const std::vector<int>& voxels) {
    float count = 0;
    for (int v : voxels) count += grid.weight[v];
    return count * grid.cell * grid.cell * grid.cell;
}

void measure(const Grid& grid, Piece& piece, int hullVertices, std::vector<b3Vec3>& scratch) {
    piece.volume = volumeOf(grid, piece.voxels);
    pointsOf(grid, piece.voxels, scratch, kMeasureHullPoints);
    piece.hull = hullVolume(scratch, hullVertices);
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

namespace {

// Whether a surface is the surface of a convex solid: all its points are on one side of the plane of each of its triangles.
bool isConvexSurface(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles, float tolerance) {
    std::vector<bool> used(vertices.size(), false);
    for (uint32_t i : triangles)
        if (i < vertices.size()) used[i] = true;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        if (triangles[t] >= vertices.size() || triangles[t + 1] >= vertices.size() || triangles[t + 2] >= vertices.size()) continue;
        const Vec3f& a = vertices[triangles[t]];
        Vec3f normal = cross(vertices[triangles[t + 1]] - a, vertices[triangles[t + 2]] - a);
        const float normalLength = length(normal);
        if (normalLength < 1e-12f) continue;
        normal = normal * (1.0f / normalLength);
        bool above = false, below = false;
        for (size_t v = 0; v < vertices.size(); v++) {
            if (!used[v]) continue;
            const float side = dot(normal, vertices[v] - a);
            if (side > tolerance) above = true;
            if (side < -tolerance) below = true;
            if (above && below) return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::vector<Vec3f>> decomposeIntoConvexPieces(const std::vector<Vec3f>& vertices, const std::vector<uint32_t>& triangles,
                                                          const ConvexDecompositionSettings& settings) {
    std::vector<std::vector<Vec3f>> result;
    if (vertices.size() < 4 || triangles.size() < 3) return result;
    Vec3f lo = vertices[0], hi = vertices[0];
    for (const Vec3f& v : vertices) {
        lo = Vec3f(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
        hi = Vec3f(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
    }
    const Vec3f size = hi - lo;
    const float longest = std::max(size.x, std::max(size.y, size.z));
    if (longest < 1e-6f) return result;
    if (isConvexSurface(vertices, triangles, 1e-3f * longest)) {   // (the pieces of a convex object: it is one)
        std::vector<bool> used(vertices.size(), false);
        std::vector<Vec3f> points;
        for (uint32_t i : triangles)
            if (i < vertices.size() && !used[i]) { used[i] = true; points.push_back(vertices[i]); }
        if (points.size() >= 3) result.push_back(std::move(points));
        return result;
    }

    if (settings.solid) {
        // A closed surface has its volume exactly (the triangles are wound the same way all over): when the hull of all its points is not larger than
        // that by more than the concavity allows, it is one piece, without the voxels.
        double volume = 0;
        std::vector<b3Vec3> hullPoints;
        std::vector<bool> used(vertices.size(), false);
        for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
            if (triangles[t] >= vertices.size() || triangles[t + 1] >= vertices.size() || triangles[t + 2] >= vertices.size()) continue;
            const Vec3f &a = vertices[triangles[t]], &b = vertices[triangles[t + 1]], &c = vertices[triangles[t + 2]];
            volume += (double)dot(a, cross(b, c)) / 6.0;
            for (uint32_t i : {triangles[t], triangles[t + 1], triangles[t + 2]})
                if (!used[i]) { used[i] = true; hullPoints.push_back({vertices[i].x, vertices[i].y, vertices[i].z}); }
        }
        const float hull = hullVolume(hullPoints, 64);
        if (hull > 0 && hull - (float)std::fabs(volume) <= settings.concavity * hull) {
            std::vector<Vec3f> points;
            for (const b3Vec3& p : hullPoints) points.push_back(Vec3f(p.x, p.y, p.z));
            result.push_back(std::move(points));
            return result;
        }
    }

    // --- the voxels: the surface (sampled finer than the voxels), and what the surface encloses (what the outside cannot reach)
    Grid grid;
    grid.cell = longest / (float)settings.resolution;
    // A voxel of margin all round (the outside is connected), and the grid shifted by parts of a voxel that have nothing to do with the object: its flat
    // faces would otherwise lie on the planes of the grid, and the voxels along them would all be counted in (or all out).
    grid.origin = lo - Vec3f(1.37f * grid.cell, 1.41f * grid.cell, 1.29f * grid.cell);
    grid.nx = (int)std::ceil(size.x / grid.cell) + 3;
    grid.ny = (int)std::ceil(size.y / grid.cell) + 3;
    grid.nz = (int)std::ceil(size.z / grid.cell) + 3;
    const size_t total = (size_t)grid.nx * grid.ny * grid.nz;
    grid.state.assign(total, 0);
    grid.samples.assign(total, std::vector<Vec3f>());
    grid.corners.assign(total, std::vector<Vec3f>());
    auto voxelOf = [&](const Vec3f& p) {
        const int x = std::min(grid.nx - 1, std::max(0, (int)std::floor((p.x - grid.origin.x) / grid.cell)));
        const int y = std::min(grid.ny - 1, std::max(0, (int)std::floor((p.y - grid.origin.y) / grid.cell)));
        const int z = std::min(grid.nz - 1, std::max(0, (int)std::floor((p.z - grid.origin.z) / grid.cell)));
        return grid.index(x, y, z);
    };
    for (size_t t = 0; t + 2 < triangles.size(); t += 3) {
        if (triangles[t] >= vertices.size() || triangles[t + 1] >= vertices.size() || triangles[t + 2] >= vertices.size()) continue;
        const Vec3f a = vertices[triangles[t]], b = vertices[triangles[t + 1]], c = vertices[triangles[t + 2]];
        const Vec3f e1 = b - a, e2 = c - a;
        const float reach = std::max(length(e1), length(e2));
        const int steps = std::min(400, std::max(1, (int)std::ceil(reach / (0.5f * grid.cell))));
        for (int i = 0; i <= steps; i++)
            for (int j = 0; j <= steps - i; j++) {
                const Vec3f p = a + e1 * ((float)i / (float)steps) + e2 * ((float)j / (float)steps);
                const int v = voxelOf(p);
                grid.state[v] = 1;
                if ((int)grid.samples[v].size() < kMaxSamplesPerVoxel) grid.samples[v].push_back(p);
            }
        for (const Vec3f& corner : {a, b, c}) grid.corners[voxelOf(corner)].push_back(corner);
    }
    {   // the outside: empty voxels that the margin reaches without crossing the surface; what is left empty is inside
        std::vector<uint8_t> outside(total, 0);
        std::vector<int> stack;
        stack.push_back(0);
        outside[0] = 1;
        while (!stack.empty()) {
            const int v = stack.back();
            stack.pop_back();
            int x, y, z;
            grid.coordinates(v, x, y, z);
            const int neighbours[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
            for (const auto& n : neighbours) {
                if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= grid.nx || n[1] >= grid.ny || n[2] >= grid.nz) continue;
                const int w = grid.index(n[0], n[1], n[2]);
                if (outside[w] || grid.state[w] == 1) continue;
                outside[w] = 1;
                stack.push_back(w);
            }
        }
        for (size_t v = 0; v < total; v++)
            if (grid.state[v] == 0 && !outside[v]) grid.state[v] = 2;
    }
    grid.weight.assign(total, 0.0f);
    for (size_t v = 0; v < total; v++) {
        if (grid.state[v] == 2) { grid.weight[v] = 1.0f; continue; }
        if (grid.state[v] == 0) continue;
        int x, y, z;
        grid.coordinates((int)v, x, y, z);
        const int neighbours[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
        bool insideNext = false;
        for (const auto& n : neighbours)
            if (n[0] >= 0 && n[1] >= 0 && n[2] >= 0 && n[0] < grid.nx && n[1] < grid.ny && n[2] < grid.nz && grid.state[grid.index(n[0], n[1], n[2])] == 2) insideNext = true;
        grid.weight[v] = insideNext ? 0.5f : 1.0f;
    }
    if (settings.solid) {   // a closed surface: a voxel with the surface in it is as much in as its middle is (counted over all of them, that is exact)
        for (size_t v = 0; v < total; v++) {
            if (grid.state[v] != 1) continue;
            int x, y, z;
            grid.coordinates((int)v, x, y, z);
            const Vec3f middle = grid.origin + Vec3f(((float)x + 0.5f) * grid.cell, ((float)y + 0.5f) * grid.cell, ((float)z + 0.5f) * grid.cell);
            grid.weight[v] = isInside(middle, vertices, triangles) ? 1.0f : 0.0f;
        }
    }

    // --- the pieces: all the voxels first, then the worst piece is split until the pieces are convex enough
    std::vector<Piece> pieces(1);
    for (size_t v = 0; v < total; v++)
        if (grid.state[v] != 0) pieces[0].voxels.push_back((int)v);
    std::vector<b3Vec3> scratch;
    measure(grid, pieces[0], kMeasureHullVertices, scratch);
    std::vector<bool> settled(1, false);
    int evaluations = 0;
    while ((int)pieces.size() < settings.maxPieces && evaluations < kMaxEvaluations) {
        int worst = -1;
        float worstExcess = 0;
        for (size_t p = 0; p < pieces.size(); p++) {
            if (settled[p]) continue;
            if (pieces[p].hull <= 0 || pieces[p].excess() <= settings.concavity * pieces[p].hull) { settled[p] = true; continue; }
            if (pieces[p].excess() > worstExcess) { worstExcess = pieces[p].excess(); worst = (int)p; }
        }
        if (worst < 0) break;
        const Piece piece = pieces[worst];   // (a copy: the list changes below)
        // the planes of the grid that cut the piece: the one that leaves the least that the hulls of the two halves have and the halves do not
        int bestAxis = -1, bestPlane = 0;
        float bestLeft = piece.excess() * 0.95f;   // (a plane must make it better than it is)
        std::vector<int> halfA, halfB;
        std::vector<b3Vec3> pointsA, pointsB;
        for (int axis = 0; axis < 3; axis++) {
            int low = 1 << 30, high = -1;
            for (int v : piece.voxels) {
                int c[3];
                grid.coordinates(v, c[0], c[1], c[2]);
                low = std::min(low, c[axis]);
                high = std::max(high, c[axis]);
            }
            if (high - low < 1) continue;
            const int stride = std::max(1, (high - low + 1) / kSplitsPerAxis);
            for (int plane = low + stride; plane <= high; plane += stride) {   // voxels below the plane are one half
                halfA.clear();
                halfB.clear();
                for (int v : piece.voxels) {
                    int c[3];
                    grid.coordinates(v, c[0], c[1], c[2]);
                    (c[axis] < plane ? halfA : halfB).push_back(v);
                }
                if (halfA.empty() || halfB.empty()) continue;
                pointsOf(grid, halfA, pointsA, kEvaluationHullPoints);
                pointsOf(grid, halfB, pointsB, kEvaluationHullPoints);
                evaluations += 2;
                const float left = std::max(0.0f, hullVolume(pointsA, kEvaluationHullVertices) - volumeOf(grid, halfA)) +
                                   std::max(0.0f, hullVolume(pointsB, kEvaluationHullVertices) - volumeOf(grid, halfB));
                if (left < bestLeft) { bestLeft = left; bestAxis = axis; bestPlane = plane; }
            }
        }
        if (bestAxis < 0) { settled[worst] = true; continue; }   // no plane helps: it stays as it is
        Piece a, b;
        for (int v : piece.voxels) {
            int c[3];
            grid.coordinates(v, c[0], c[1], c[2]);
            (c[bestAxis] < bestPlane ? a : b).voxels.push_back(v);
        }
        measure(grid, a, kMeasureHullVertices, scratch);
        measure(grid, b, kMeasureHullVertices, scratch);
        pieces[worst] = std::move(a);
        pieces.push_back(std::move(b));
        settled[worst] = false;
        settled.push_back(false);
    }

    // --- every piece as the points of its surface
    for (const Piece& piece : pieces) {
        std::vector<Vec3f> points;
        for (int v : piece.voxels) {
            points.insert(points.end(), grid.corners[v].begin(), grid.corners[v].end());
            points.insert(points.end(), grid.samples[v].begin(), grid.samples[v].end());
        }
        if (points.size() >= 4) result.push_back(std::move(points));
    }
    return result;
}

}  // namespace minigame01
