// Game-independent description of a level: textured triangle batches plus annotation
// markers (starts, pickups, paths, zones...). Loaders fill it, the renderer draws it.
#pragma once
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "../loader/image.h"
#include "../math/vmath.h"
#include "../math/vec2.h"
#include "math/color.h"

struct SceneVertex {
    float x, y, z;
    uint8_t r, g, b, a;
    float u, v;

    SceneVertex() = default;

    SceneVertex(const Vec3f& pos, const Color4b& col, const Vec2f& tex)
        : x(pos.x), y(pos.y), z(pos.z),
          r(col.r), g(col.g), b(col.b), a(col.a),
          u(tex.x), v(tex.y)
    {}
};


enum Blend : uint8_t { BLEND_OPAQUE = 0, BLEND_CUTOUT = 1, BLEND_ALPHA = 2, BLEND_ADD = 3 };

struct Batch {
    int texture = -1;  // index into Scene::textures, -1 = untextured
    Blend blend = BLEND_OPAQUE;
    bool twoSided = false;  // material flag 1: no back-face culling (foliage, thin objects)
    std::vector<SceneVertex> verts;
    std::vector<uint32_t> idx;
};

// A texture whose image changes over time (the games' texture animation scripts). Batches keep using `texture`; the renderer
// shows frames[floor(time * fps) % frames.size()], all indices into Scene::textures.
struct TexAnim {
    int texture = -1;
    std::vector<int> frames;
    float fps = 30;
};

enum MarkerShape : uint8_t { SHAPE_DIAMOND = 0, SHAPE_ARROW = 1, SHAPE_CUBE = 2, SHAPE_SPHERE = 3 };

struct Category {
    std::string id;
    std::string name;
    Color3b color = Color3b::from_rgb(0xffffff);
    MarkerShape shape = SHAPE_DIAMOND;
    float size = 1.0f;       // relative marker size
    bool beam = false;       // draw a vertical beam above each marker
    bool visible = true;
    int count = 0;           // number of markers / segments
    bool isLine = false;     // category consists of line segments only
    bool hasMesh = false;    // markers of this category can be replaced by in-game meshes (powerups)
};

struct Marker {
    int cat = 0;
    Vec3f pos;
    float yaw = 0;  // radians, direction in the XZ plane (0 = +Z)
    std::string label;
};

struct Line {
    int cat = 0;
    Vec3f a, b;
};

// A set of batches keyed by (texture, blend mode).
struct BatchSet {
    std::vector<Batch> batches;

    Batch& batchFor(int tex, Blend blend, bool twoSided = false) {
        uint64_t key = ((uint64_t)(uint32_t)(tex + 1) << 9) | (twoSided ? 256u : 0u) | blend;
        auto it = batchIndex.find(key);
        if (it != batchIndex.end()) return batches[it->second];
        batchIndex[key] = (int)batches.size();
        batches.emplace_back();
        batches.back().texture = tex;
        batches.back().blend = blend;
        batches.back().twoSided = twoSided;
        return batches.back();
    }
    void appendIndexed(int tex, Blend blend, const SceneVertex* v, size_t nv, const uint32_t* idx, size_t ni, bool twoSided = false) {
        Batch& bt = batchFor(tex, blend, twoSided);
        auto base = static_cast<uint32_t>(bt.verts.size());
        bt.verts.insert(bt.verts.end(), v, v + nv);
        for (size_t i = 0; i < ni; i++) bt.idx.push_back(base + idx[i]);
    }

private:
    std::unordered_map<uint64_t, int> batchIndex;
};

// A mesh (in its own local space) travelling along a polyline, e.g. helicopters or energy rings.
// One step of an animated prop (lift, behemoth, generator wheel): move by `trans` and turn by `rotDeg` about `axis` over `dur` seconds.
struct AnimKey { float dur = 0; Vec3f trans; Vec3f axis; float rotDeg = 0; };

struct Mover {
    int geom = 0;  // index into Scene::moverGeoms
    int cat = 0;
    std::string name;
    std::vector<Vec3f> path;
    std::vector<float> cum;  // cumulative length at each path point
    float length = 0;
    float speed = 0;  // units per second
    float phase = 0;  // start offset as a fraction of the path length (fixed objects: spin offset in radians)
    bool fixed = false;        // stands still at path[0] and spins (powerup icons)
    bool powerupMesh = false;  // only drawn when "in-game powerup meshes" is on
    bool billboard = false;    // always faces the camera (powerup halos); anchored at path[0] + anchor * scale
    Vec3f anchor;               // billboard centre in the model space of the icon it belongs to
    int scaleGeom = -1;        // geometry whose size decides the on-screen scale (the icon, for its halo)
    std::vector<AnimKey> keys; // looping keyframes of an animated prop
    Vec3f pivot;                // keyframe rotations turn about this point
    bool staticModel = false;  // geometry is already in world space (merged props): draw with an identity transform
    bool external = false;     // `model` is set from outside every frame (the minigame's car); `cat` may be -1
    bool hidden = false;       // not drawn (collected pickups, the parked car)
    Mat4 model = Mat4::identity();

    void finish() {
        cum.assign(path.size(), 0.0f);
        for (size_t i = 1; i < path.size(); i++) cum[i] = cum[i - 1] + length3(path[i] - path[i - 1]);
        length = cum.empty() ? 0.0f : cum.back();
    }
    static float length3(const Vec3f& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    // Position and direction at distance s (0 <= s < length).
    bool sample(float s, Vec3f& pos, Vec3f& dir) const {
        if (path.size() < 2 || length <= 0) return false;
        size_t lo = 0, hi = cum.size() - 1;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (cum[mid] <= s) lo = mid; else hi = mid;
        }
        float seg = std::max(1e-5f, cum[lo + 1] - cum[lo]);
        float t = (s - cum[lo]) / seg;
        pos = path[lo] + (path[lo + 1] - path[lo]) * t;
        dir = (path[lo + 1] - path[lo]) * (1.0f / seg);
        return true;
    }
};

struct Scene : BatchSet {
    bool cullBackfaces = false;  // the game draws single-sided faces (back faces are culled)
    // Powerup icons are all about the same size, so the viewer does not measure the meshes: hardcoded per game (model units).
    float pickupRadius = 1.5f;   // bounding sphere of an icon mesh around its origin
    float pickupCentreY = 1.0f;  // height of the icon's visual centre above its origin
    float pickupBottomY = 0.0f;  // height of its bottom above its origin: the point icons grow from when they are scaled up
    std::vector<BatchSet> moverGeoms;
    std::vector<Mover> movers;
    std::string title;
    std::string info;                 // short multi-line description shown in the UI
    Vec3f background{0.08f, 0.1f, 0.14f};
    std::vector<Image> textures;
    std::vector<TexAnim> texAnims;
    std::vector<Category> cats;
    std::vector<Marker> markers;
    std::vector<Line> lines;
    Vec3f bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
    bool hasCameraStart = false;
    Vec3f cameraPos;
    float cameraYaw = 0, cameraPitch = 0;

    int addCategory(
        const std::string& id,
        const std::string& name,
        const Color3b& color,
        MarkerShape shape = SHAPE_DIAMOND,
        float size = 1.0f,
        bool beam = false,
        bool visible = true,
        bool isLine = false
    ) {
        Category c;
        c.id = id;
        c.name = name;
        c.color = color;
        c.shape = shape;
        c.size = size;
        c.beam = beam;
        c.visible = visible;
        c.isLine = isLine;
        cats.push_back(c);
        return (int)cats.size() - 1;
    }
    void addMarker(int cat, const Vec3f& p, float yaw, const std::string& label) {
        Marker m;
        m.cat = cat; m.pos = p; m.yaw = yaw; m.label = label;
        markers.push_back(std::move(m));
        cats[cat].count++;
    }
    void addLine(int cat, const Vec3f& a, const Vec3f& b) {
        lines.push_back({cat, a, b});
        cats[cat].count++;
    }
    // Oriented box outline: centre, three axis vectors already scaled by the half extents.
    void addBox(int cat, const Vec3f& c, const Vec3f& ax, const Vec3f& ay, const Vec3f& az) {
        Vec3f p[8];
        for (int i = 0; i < 8; i++)
            p[i] = c + ax * ((i & 1) ? 1.f : -1.f) + ay * ((i & 2) ? 1.f : -1.f) + az * ((i & 4) ? 1.f : -1.f);
        static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& ed : e) addLine(cat, p[ed[0]], p[ed[1]]);
    }
    void extend(const Vec3f& p) {
        bmin = vmin(bmin, p);
        bmax = vmax(bmax, p);
    }

    int addTexture(Image&& img) {
        textures.push_back(std::move(img));
        return (int)textures.size() - 1;
    }

    // Appends one triangle (world space) to the proper batch and grows the bounds.
    void addTriangle(int tex, Blend blend, const SceneVertex& a, const SceneVertex& b, const SceneVertex& c) {
        Batch& bt = batchFor(tex, blend);
        uint32_t base = (uint32_t)bt.verts.size();
        bt.verts.push_back(a);
        bt.verts.push_back(b);
        bt.verts.push_back(c);
        bt.idx.push_back(base);
        bt.idx.push_back(base + 1);
        bt.idx.push_back(base + 2);
        triCount++;
        extend({a.x, a.y, a.z});
        extend({b.x, b.y, b.z});
        extend({c.x, c.y, c.z});
    }
    // Appends an indexed mesh whose vertices are already in world space.
    void addIndexed(int tex, Blend blend, const SceneVertex* v, size_t nv, const uint32_t* idx, size_t ni, bool twoSided = false) {
        appendIndexed(tex, blend, v, nv, idx, ni, twoSided);
        triCount += ni / 3;
        for (size_t i = 0; i < nv; i++) extend({v[i].x, v[i].y, v[i].z});
    }
    size_t triCount = 0;
};
