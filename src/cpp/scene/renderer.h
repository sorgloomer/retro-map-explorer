// WebGL2 renderer: textured level geometry, instanced 3D markers and screen-space line ribbons.
#pragma once
#include <vector>
#include "scene.h"
#include "../math/color.h"
#include "../math/vmath.h"

struct RenderOptions {
    float exposure = 1.35f;
    float haloExposure = 1.35f;  // brightness of the powerup halos (camera-facing billboards), separate from the level's
    bool textured = true;
    float markerScale = 1.0f;   // size of the plain markers (not the powerups)
    float powerupScale = 1.0f;  // size factor of the powerups: their meshes and their diamonds
    float lineWidth = 3.0f;
    float xrayStrength = 1.0f;  // see-through strength: how visible occluded markers / lines / powerup meshes are
    bool showGeometry = true;
    bool sunLight = false;    // in-game global light (sun + ambient) when the scene provides it
    bool wireframe = false;   // level geometry as lines
    bool backFaces = false;   // draw back faces too (otherwise follow the game and cull them)
    bool pointLights = false;  // in-game point lights when the scene provides them
    bool beams = false;         // vertical beams above powerup markers
    bool powerupMeshes = true;   // draw powerups with their in-game meshes instead of markers

    bool xrayEnabled() const { return xrayStrength > 0; }  // false: the see-through passes are skipped
};

struct ViewParams {
    Mat4 view, proj, viewProj;
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
    void render(const ViewParams& vp, const RenderOptions& opt, int highlightMarker);
    int contextLost() const { return lost; }

private:
    struct GpuBatch {
        unsigned vao = 0, vbo = 0, ibo = 0;
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
    void uploadSet(const std::vector<Batch>& src, std::vector<GpuBatch>& dst);
    void freeSet(std::vector<GpuBatch>& set);
    void buildShapes();
    unsigned compile(const char* vs, const char* fs);
    void drawMarkers(const ViewParams& vp, const RenderOptions& opt, bool xrayPass, int highlight);
    void drawLines(const ViewParams& vp, const RenderOptions& opt, bool xrayPass);
    float iconScale(const Vec3f& at, const ViewParams& vp, const RenderOptions& opt) const;

    const Scene* scene = nullptr;
    int lost = 0;
    unsigned progWorld = 0, progMarker = 0, progLine = 0;
    unsigned lightTex = 0;
    int numLights = 0;
    std::vector<GpuBatch> batches;
    std::vector<std::vector<GpuBatch>> geomGpu;  // one per Scene::moverGeoms entry
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
