// WebGL2 renderer: textured level geometry, instanced 3D markers and screen-space line ribbons.
#pragma once
#include <vector>
#include "scene.h"
#include "../math/color.h"
#include "../math/transform.h"

struct RenderOptions {
    float exposure = 1.35f;
    float haloExposure = 1.35f;  // brightness of the powerup halos (camera-facing billboards), separate from the level's
    bool textured = true;
    float markerScale = 1.0f;   // size of the plain markers (not the powerups)
    float powerupScale = 1.0f;  // size factor of the powerups: their meshes and their diamonds
    float lineWidth = 3.0f;
    float xrayStrength = 1.0f;  // see-through strength: how visible occluded markers / lines / powerup meshes are
    bool showGeometry = true;
    bool showCollision = false;  // the game's collision mesh as a translucent overlay (when the scene has one)
    bool wireframe = false;  // level geometry as lines
    bool backFaces = false;   // draw back faces too (otherwise follow the game and cull them)
    bool beams = false;         // vertical beams above powerup markers
    bool powerupMeshes = true;   // draw powerups with their in-game meshes instead of markers

    bool xrayEnabled() const { return xrayStrength > 0; }  // false: the see-through passes are skipped
};

struct ViewParams {
    Mat4f view, proj, viewProj;
    Vec3f eye;
    int width = 1, height = 1;
    float fovY = 1.0f;
    float time = 0;  // seconds, drives moving objects
    bool showPivot = false;  // draw the orbit / pan pivot marker
    Vec3f pivot;
};

class Renderer {
public:
    bool init(const char* canvasSelector);
    void setScene(const Scene* scene, float radius);  // uploads GPU data; radius = typical level size
    void uploadCollision();  // after Scene::ensureCollision filled the overlay
    void render(const ViewParams& vp, const RenderOptions& opt, int highlightMarker);
    int contextLost() const { return lost; }
    // The point lights that lit the car in the last frame: how many reached it (some may not be used) and how many the shader takes.
    static constexpr int kMaxLights = 8;
    int lightsInRange() const { return lightsInRange_; }
    // Their colours as 0xRRGGBB, nearest first (the shader uses the first kMaxLights).
    const std::vector<unsigned>& lightColours() const { return lightColours_; }

private:
    int lightsInRange_ = 0;
    std::vector<unsigned> lightColours_;
    struct GpuBatch {
        unsigned vao = 0, vbo = 0, ibo = 0;
        unsigned nbo = 0;  // vertex normals (attribute 3), only for lit sets
        int count = 0;
        int tex = -1;  // gpu texture handle index (or -1)
        Blend blend = BLEND_OPAQUE;
        bool twoSided = false;
        unsigned lineIbo = 0;  // edge list for the wireframe view (built on demand)
        int lineCount = 0;
        const Batch* src = nullptr;
    };
    struct CatLines {
        int firstLine = 0, lineCount = 0;
        int firstBeam = 0, beamCount = 0;
        int firstMarker = 0, markerCount = 0;
    };

    void clearScene();
    void drawPivot(const ViewParams& vp, const RenderOptions& opt);
    unsigned pivotBuf = 0;
    void uploadSet(const std::vector<Batch>& src, std::vector<GpuBatch>& dst, bool lit = false);
    void freeSet(std::vector<GpuBatch>& set);
    void buildShapes();
    unsigned compile(const char* vs, const char* fs);
    void drawMarkers(const ViewParams& vp, const RenderOptions& opt, bool xrayPass, int highlight);
    void drawLines(const ViewParams& vp, const RenderOptions& opt, bool xrayPass);
    float iconScale(const Vec3f& at, const ViewParams& vp, const RenderOptions& opt) const;

    const Scene* scene = nullptr;
    int lost = 0;
    unsigned progWorld = 0, progLit = 0, progMarker = 0, progLine = 0, progDepth = 0;
    unsigned progWorldInst = 0, progDepthInst = 0, progLitInst = 0;   // the same with the model matrix per instance (attributes 4 to 7), for many objects of a geometry in one call
    struct GeomBounds { Vec3f centre; float radius = 0; bool valid = false; };   // a sphere round a geometry (its own space)
    std::vector<GeomBounds> geomBounds_;
    std::vector<unsigned> geomInst_;   // per geometry: the buffer of the model matrices of its instances
    unsigned instanceBuffer(int geom);
    // the shadow maps (depth only): the sun's, seen from the sun over the area round the car, and one from straight above for the ambient occlusion
    // Two cascades of the sun's: a near one (a 56 m square, finest) and a far one (twenty times as wide), and the occlusion map from above. The level is
    // in all of them, and does not move: it is drawn into a map of its own (the "static" ones) only when the car has moved on to another cell of a
    // grid; each frame the near maps start as a copy of that, and the objects that move are drawn on top.
    static constexpr int kNearSize = 2048, kFarSize = 2048, kAoSize = 1024;
    struct DepthTarget { unsigned fbo = 0, tex = 0; };
    DepthTarget nearStatic_, near_, far_, aoStatic_, ao_;
    bool nearValid_ = false, farValid_ = false, aoValid_ = false;
    int nearKey_[3] = {0, 0, 0}, farKey_[3] = {0, 0, 0}, aoKey_[3] = {0, 0, 0};
    Vec3f keySun_;
    void makeDepthTarget(DepthTarget& target, int size);
    // GPU timers (EXT_disjoint_timer_query_webgl2, when the browser has it): 0 the shadow maps, 1 the picture. A few queries each, read when they are ready.
    bool gpuOk_ = false;
    unsigned gpuQuery_[2][4] = {};
    int gpuHead_[2] = {0, 0}, gpuPending_[2] = {0, 0};
    bool gpuActive_[2] = {false, false};
    void gpuBegin(int which);
    void gpuEnd(int which);
    void gpuCollect();
    std::vector<GpuBatch> batches;
    std::vector<GpuBatch> collisionGpu;
    std::vector<std::vector<GpuBatch>> geomGpu;  // one per Scene::moverGeoms entry
    std::vector<uint32_t> geomRevision;          // the BatchSet::revision of each that is on the GPU
    std::vector<unsigned> textures;
    std::vector<int> texFrame;  // texture shown for each scene texture: itself, or the current frame of its animation

    // marker meshes
    struct Shape { unsigned vao = 0, vbo = 0; int count = 0; unsigned wvao = 0, wvbo = 0; int wcount = 0; };  // w*: triangle edges as lines
    Shape shapes[4];
    unsigned markerInst = 0;
    std::vector<int> markerOrder;  // marker index per instance slot
    std::vector<int> markerSlot;   // instance slot per marker
    unsigned lineVbo = 0, lineVao = 0;
    std::vector<CatLines> catRanges;
    float sceneRadius = 1000.0f;
    float markerBase = 1.0f;
};
