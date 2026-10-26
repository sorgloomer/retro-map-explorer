// Application shell: game registry, camera + input, main loop and the JS-facing API.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "camera.h"
#include "game.h"
#include "minigame01/minigame.h"
#include "pan.h"
#include "raycast.h"
#include "renderer.h"
#include "vfs.h"

namespace {

struct App {
    Renderer renderer;
    std::vector<std::unique_ptr<Game>> games;
    std::unique_ptr<Scene> scene, retired;
    Camera cam;
    RenderOptions opt;
    float speedMul = 1.0f;
    double animTime = 0;      // seconds of animation time
    float animScale = 1.0f;   // 0 = frozen
    float sceneRadius = 1000.0f;
    float farDist = 30000.0f;
    Vec3 sceneCenter;
    bool keys[256] = {};  // indexed by our own key enum
    bool dragging = false;
    int mouseX = -1, mouseY = -1;  // CSS pixels
    int hover = -1;
    bool pointerLocked = false;
    int camMode = 0;  // 0 = fly, 1 = orbit around a clicked pivot
    Vec3 pivot;
    int dragButton = -1;     // mouse button that started the current drag
    float pivotNormalY = 1.0f;  // |y| of the unit normal of the surface the pivot was picked on (1 = floor, 0 = wall)
    Vec3 panStart;  // camera position when the current pan gesture began
    bool touchMode = false;  // last input came from a touch screen
    struct TouchPt { int id; float x, y; };
    std::vector<TouchPt> touches;
    float gestureDist = 0, gestureAngle = 0;
    float gestureCx = 0, gestureCy = 0;
    float touchTravel = 0;
    double touchStart = 0;
    Vec3 orbitOff;  // pivot position in camera space (right, up, forward) - keeps it fixed on screen while orbiting
    std::string json;  // return buffer for strings handed to JS
    std::string lastError;
    int canvasW = 0, canvasH = 0;
    double lastTime = 0;
    int hoverShown = -2;
    // driving minigame (src/cpp/minigame01)
    minigame01::MiniGame mg;
    bool gameMode = false;
    Game* currentGame = nullptr;  // the game of the loaded level
    RenderOptions viewerOpt;      // options and camera of the viewer, restored when the minigame ends
    Camera viewerCam;
    int viewerCamMode = 0;
    int hudTick = 0;
} app;

enum Key { K_W, K_A, K_S, K_D, K_UP, K_DOWN, K_FAST, K_SLOW, K_COUNT };

int keyFromCode(const char* code) {
    if (!strcmp(code, "KeyW") || !strcmp(code, "ArrowUp")) return K_W;
    if (!strcmp(code, "KeyA") || !strcmp(code, "ArrowLeft")) return K_A;
    if (!strcmp(code, "KeyS") || !strcmp(code, "ArrowDown")) return K_S;
    if (!strcmp(code, "KeyD") || !strcmp(code, "ArrowRight")) return K_D;
    if (!strcmp(code, "KeyE") || !strcmp(code, "Space")) return K_UP;
    if (!strcmp(code, "KeyQ") || !strcmp(code, "KeyC")) return K_DOWN;
    if (!strcmp(code, "ShiftLeft") || !strcmp(code, "ShiftRight")) return K_FAST;
    if (!strcmp(code, "AltLeft") || !strcmp(code, "AltRight")) return K_SLOW;
    return -1;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// Frames the level using percentile bounds so huge skydomes / stray geometry don't push the camera away.
void frameCamera() {
    Scene* s = app.scene.get();
    if (!s) return;
    Vec3 c, ext(1000, 1000, 1000);
    float fullR = 1000.0f;
    if (s->triCount > 0) {
        std::vector<float> xs, ys, zs;
        size_t total = 0;
        for (const Batch& b : s->batches) total += b.verts.size();
        size_t step = std::max<size_t>(1, total / 200000);
        size_t k = 0;
        for (const Batch& b : s->batches)
            for (const SceneVertex& v : b.verts)
                if (k++ % step == 0) { xs.push_back(v.x); ys.push_back(v.y); zs.push_back(v.z); }
        auto pct = [](std::vector<float>& a, float q) {
            size_t i = std::min(a.size() - 1, (size_t)(q * (float)a.size()));
            std::nth_element(a.begin(), a.begin() + i, a.end());
            return a[i];
        };
        Vec3 lo(pct(xs, 0.02f), pct(ys, 0.02f), pct(zs, 0.02f));
        Vec3 hi(pct(xs, 0.98f), pct(ys, 0.98f), pct(zs, 0.98f));
        c = (lo + hi) * 0.5f;
        ext = hi - lo;
        fullR = length(s->bmax - s->bmin) * 0.5f;
    }
    float r = std::max(1.0f, length(ext) * 0.5f);
    app.sceneCenter = c;
    app.sceneRadius = r;
    app.farDist = std::max(r * 30.0f, fullR * 3.0f);
    app.cam.pos = c + Vec3(0, r * 0.9f, -r * 0.8f);
    app.cam.yaw = 0;
    app.cam.pitch = -std::atan2(0.9f, 0.8f);
}

void jumpToStart() {
    Scene* s = app.scene.get();
    if (!s || !s->hasCameraStart) return;
    app.cam.pos = s->cameraPos;
    app.cam.yaw = s->cameraYaw;
    app.cam.pitch = s->cameraPitch;
}

struct Basis { Vec3 r, u, f; };
Basis camBasis() {
    Basis b;
    b.f = app.cam.forward();
    b.r = normalize(cross(Vec3(0, 1, 0), b.f));
    b.u = cross(b.f, b.r);
    return b;
}

// Places the camera so the pivot keeps its camera-space offset (and therefore its on-screen position).
void orbitReposition() {
    Basis b = camBasis();
    app.cam.pos = app.pivot - (b.r * app.orbitOff.x + b.u * app.orbitOff.y + b.f * app.orbitOff.z);
}

// Ray through a canvas pixel (CSS coordinates).
Vec3 mouseRay(int mx, int my) { return viewRay(app.cam, app.canvasW, app.canvasH, mx, my); }

// Sets the orbit pivot to what is under (mx, my); falls back to the ground below the cursor / scene centre.
void pickPivot(int mx, int my) {
    bool culling = app.scene && app.scene->cullBackfaces && !app.opt.backFaces && !app.opt.wireframe;
    PivotPick pick = pickPivotPoint(app.scene.get(), culling, app.cam, app.canvasW, app.canvasH, mx, my, app.sceneCenter, app.sceneRadius);
    app.pivot = pick.point;
    app.pivotNormalY = pick.normalY;
    Basis b = camBasis();
    Vec3 rel = app.pivot - app.cam.pos;
    app.orbitOff = {dot(rel, b.r), dot(rel, b.u), std::max(app.sceneRadius * 0.002f, dot(rel, b.f))};
}

// Keeps orbitOff consistent after the camera moved without turning (panning).
void refreshOrbitOffset() {
    Basis b = camBasis();
    Vec3 rel = app.pivot - app.cam.pos;
    app.orbitOff = {dot(rel, b.r), dot(rel, b.u), dot(rel, b.f)};
}

PanSurface panSurf;  // the surface of the pan gesture in progress (see pan.h)

// Starts a pan gesture: remembers the camera and builds the surface it will move on.
void beginPan() {
    app.panStart = app.cam.pos;
    panSurf = computePanSurface(app.cam, app.pivot, app.pivotNormalY, app.sceneRadius);
}

void panTo(int mx, int my) {
    app.cam.pos = panCameraPosition(panSurf, app.cam, app.panStart, app.pivot, mouseRay(mx, my));
    refreshOrbitOffset();
}

// Pitch limit of the camera in radians (a little short of straight up / down), for the mouse and for touch tilting alike.
constexpr float kMaxPitch = 1.55f;

void rotateView(float dx, float dy) {
    app.cam.yaw += dx;
    app.cam.pitch = std::max(-kMaxPitch, std::min(kMaxPitch, app.cam.pitch - dy));
}

void setCameraMode(int mode) {
    app.camMode = mode;
    if (mode == 1) pickPivot(app.canvasW / 2, app.canvasH / 2);
    EM_ASM({ if (window.RME && window.RME.onCameraMode) window.RME.onCameraMode($0); }, mode);
}

void notifyHover(const char* text, const char* cat, int x, int y) {
    EM_ASM({
        if (window.RME && window.RME.onHover) window.RME.onHover($0 ? UTF8ToString($0) : null, $1 ? UTF8ToString($1) : null, $2, $3);
    }, text, cat, x, y);
}

void updateHover(const ViewParams& vp) {
    Scene* s = app.scene.get();
    int best = -1;
    if (s && app.mouseX >= 0 && !app.dragging) {
        float pickR = app.touchMode ? 30.0f : 14.0f;
        float bestScore = 1e30f;
        float pixW = 2.0f * std::tan(0.525f) / (float)std::max(1, app.canvasH);  // world units per pixel at distance 1
        float sx = (float)vp.width / (float)std::max(1, app.canvasW), sy = (float)vp.height / (float)std::max(1, app.canvasH);
        (void)sx; (void)sy;
        for (size_t i = 0; i < s->markers.size(); i++) {
            const Marker& m = s->markers[i];
            if (!s->cats[m.cat].visible) continue;
            const float* a = vp.viewProj.m;
            float x = a[0] * m.pos.x + a[4] * m.pos.y + a[8] * m.pos.z + a[12];
            float y = a[1] * m.pos.x + a[5] * m.pos.y + a[9] * m.pos.z + a[13];
            float w = a[3] * m.pos.x + a[7] * m.pos.y + a[11] * m.pos.z + a[15];
            if (w <= 0.01f) continue;
            float px = (x / w * 0.5f + 0.5f) * (float)app.canvasW;
            float py = (1.0f - (y / w * 0.5f + 0.5f)) * (float)app.canvasH;
            float dx = px - app.mouseX, dy = py - app.mouseY;
            float d = dx * dx + dy * dy;
            // Hit radius follows the drawn size: close up (world-sized markers / meshes) it grows with the zoom.
            const Category& cat = s->cats[m.cat];
            float cd = std::max(1e-3f, length(m.pos - app.cam.pos));
            float sizeW = std::max(app.sceneRadius * 0.0035f * app.opt.markerScale, cd * pixW * 7.0f * app.opt.markerScale) *
                          cat.size * (cat.isLine ? 0.5f : 1.0f);
            if (cat.hasMesh && app.opt.powerupMeshes) sizeW = std::max(sizeW, 4.0f);
            float r = std::max(pickR, sizeW / (cd * pixW) * 1.4f);
            float score = d / (r * r);
            if (score < 1.0f && score < bestScore) { bestScore = score; best = (int)i; }
        }
    }
    app.hover = best;
    if (best != app.hoverShown) {
        app.hoverShown = best;
        if (best < 0) notifyHover(nullptr, nullptr, 0, 0);
        else notifyHover(s->markers[best].label.c_str(), s->cats[s->markers[best].cat].name.c_str(), app.mouseX, app.mouseY - (app.touchMode ? 70 : 0));
    }
}

void stopGame() {
    if (!app.gameMode) return;
    if (app.scene) app.mg.stop(*app.scene);
    app.gameMode = false;
    app.opt = app.viewerOpt;
    app.cam = app.viewerCam;
    app.camMode = app.viewerCamMode;
    for (bool& k : app.keys) k = false;
    EM_ASM({ if (window.RME && window.RME.onGameMode) window.RME.onGameMode(0); });
}

// Sends the minigame's heads-up display values to the page.
void pushGameHud() {
    const minigame01::Hud& h = app.mg.hud();
    EM_ASM({
        if (window.RME && window.RME.onGameHud) window.RME.onGameHud($0, $1, $2, $3, $4, $5, $6, UTF8ToString($7), $8);
    }, (double)h.speedKmh, h.score, h.driftScore, (double)h.timeLeft, h.collected, h.total, h.state, h.message, (double)h.messageAge);
}

void frame() {
    double now = emscripten_get_now();
    float dt = app.lastTime > 0 ? (float)std::min(0.1, (now - app.lastTime) / 1000.0) : 0.016f;
    app.lastTime = now;

    double cw, ch;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    float dpr = (float)emscripten_get_device_pixel_ratio();
    int w = std::max(1, (int)(cw * dpr)), h = std::max(1, (int)(ch * dpr));
    int curW, curH;
    emscripten_get_canvas_element_size("#canvas", &curW, &curH);
    if (curW != w || curH != h) emscripten_set_canvas_element_size("#canvas", w, h);
    app.canvasW = (int)cw;
    app.canvasH = (int)ch;

    if (app.gameMode) {
        minigame01::Controls ctl = app.mg.inputRef().update(dt);
        if (app.mg.inputRef().exitRequested()) stopGame();
        else if (app.scene) {
            app.mg.update(dt, ctl, *app.scene);
            app.cam = app.mg.camera();
            if (++app.hudTick % 3 == 0) pushGameHud();
        }
    }

    // movement
    float speed = app.sceneRadius * 0.3f * app.speedMul;
    if (app.keys[K_FAST]) speed *= 4.0f;
    if (app.keys[K_SLOW]) speed *= 0.2f;
    Vec3 f(std::sin(app.cam.yaw), 0, std::cos(app.cam.yaw));
    Vec3 r(std::cos(app.cam.yaw), 0, -std::sin(app.cam.yaw));
    Vec3 mv;
    if (app.gameMode) { app.keys[K_W] = app.keys[K_A] = app.keys[K_S] = app.keys[K_D] = app.keys[K_UP] = app.keys[K_DOWN] = false; }
    // Fly mode: W/S follow the view direction (changes height); orbit mode: horizontal like a walking pan.
    Vec3 fwdMove = app.camMode == 0 ? app.cam.forward() : f;
    if (app.keys[K_W]) mv += fwdMove;
    if (app.keys[K_S]) mv += fwdMove * -1.0f;
    if (app.keys[K_D]) mv += r;
    if (app.keys[K_A]) mv += r * -1.0f;
    if (app.keys[K_UP]) mv += Vec3(0, 1, 0);
    if (app.keys[K_DOWN]) mv += Vec3(0, -1, 0);
    app.cam.pos += mv * (speed * dt);
    if (app.camMode == 1) app.pivot += mv * (speed * dt);

    ViewParams vp;
    vp.width = w;
    vp.height = h;
    vp.fovY = kFovY;
    app.animTime += dt * app.animScale;
    vp.time = (float)std::fmod(app.animTime, 100000.0);
    vp.showPivot = app.dragging && (app.camMode == 1 || app.touchMode);
    vp.pivot = app.pivot;
    vp.eye = app.cam.pos;
    vp.view = Mat4::lookAt(app.cam.pos, app.cam.forward(), Vec3(0, 1, 0));
    // the chase camera sits close to the car: a much nearer clipping plane than the viewer's
    float zNear = app.gameMode ? 0.4f * app.mg.unitsPerMetre() : nearPlane(app.sceneRadius);
    float zFar = app.gameMode ? std::min(app.farDist, 12000.0f * app.mg.unitsPerMetre()) : app.farDist;
    vp.proj = Mat4::perspective(vp.fovY, (float)w / (float)h, zNear, zFar);
    vp.viewProj = Mat4::mul(vp.proj, vp.view);
    if (app.gameMode) app.hover = -1;
    else updateHover(vp);
    app.renderer.render(vp, app.opt, app.hover);
}

// ----- input
EM_BOOL onKey(int type, const EmscriptenKeyboardEvent* e, void*) {
    if (e->ctrlKey || e->metaKey) return EM_FALSE;
    if (app.gameMode) return app.mg.inputRef().key(e->code, type == EMSCRIPTEN_EVENT_KEYDOWN, e->repeat != 0) ? EM_TRUE : EM_FALSE;
    int k = keyFromCode(e->code);
    bool down = type == EMSCRIPTEN_EVENT_KEYDOWN;
    if (k >= 0) {
        app.keys[k] = down;
        return EM_TRUE;
    }
    if (down && !e->repeat) {
        if (!strcmp(e->code, "KeyR")) { frameCamera(); if (app.camMode == 1) pickPivot(app.canvasW / 2, app.canvasH / 2); return EM_TRUE; }
        if (!strcmp(e->code, "KeyT")) { jumpToStart(); return EM_TRUE; }
        if (!strcmp(e->code, "KeyV")) { setCameraMode(1 - app.camMode); return EM_TRUE; }
    }
    return EM_FALSE;
}

EM_BOOL onBlur(int, const EmscriptenFocusEvent*, void*) {
    for (bool& k : app.keys) k = false;
    return EM_FALSE;
}

EM_BOOL onMouseDown(int, const EmscriptenMouseEvent* e, void*) {
    if (app.gameMode) return EM_FALSE;
    app.touchMode = false;
    app.dragging = true;
    app.dragButton = e->button;
    app.mouseX = e->clientX;
    app.mouseY = e->clientY;
    if (app.camMode == 1) {
        pickPivot(e->clientX, e->clientY);
        beginPan();
    }
    return EM_TRUE;
}
EM_BOOL onMouseUp(int, const EmscriptenMouseEvent*, void*) {
    app.dragging = false;
    app.dragButton = -1;
    return EM_FALSE;
}
EM_BOOL onMouseMove(int, const EmscriptenMouseEvent* e, void*) {
    if (app.gameMode) return EM_FALSE;
    // Never preventDefault here: the browser needs mouse moves to drag the page's sliders.
    if (app.touchMode && e->movementX == 0 && e->movementY == 0) return EM_FALSE;
    app.touchMode = false;
    app.mouseX = e->clientX;  // the canvas fills the window, so client == canvas CSS coordinates
    app.mouseY = e->clientY;
    if (app.dragging || app.pointerLocked) {
        if (app.camMode == 1 && app.dragButton == 0) {
            panTo(e->clientX, e->clientY);  // left drag: grab the ground and slide it
        } else {
            rotateView((float)e->movementX * 0.0035f, (float)e->movementY * 0.0035f);
            if (app.camMode == 1) orbitReposition();  // right drag: orbit around the pivot
        }
    }
    return EM_FALSE;
}
EM_BOOL onMouseLeave(int, const EmscriptenMouseEvent*, void*) {
    app.mouseX = -1;
    return EM_FALSE;
}
EM_BOOL onWheel(int, const EmscriptenWheelEvent* e, void*) {
    if (app.gameMode) return EM_FALSE;
    float amount = e->deltaY > 0 ? -1.0f : 1.0f;  // wheel up = forward / closer
    if (app.camMode == 1) {
        // Zoom towards whatever is under the cursor right now (not the last clicked pivot).
        int mx = e->mouse.clientX, my = e->mouse.clientY;
        app.mouseX = mx;
        app.mouseY = my;
        pickPivot(mx, my);
        float k = amount > 0 ? 0.88f : 1.0f / 0.88f;
        if (app.orbitOff.z * k >= app.sceneRadius * 0.002f || k > 1.0f) app.orbitOff = app.orbitOff * k;
        orbitReposition();
    } else {
        app.cam.pos += app.cam.forward() * (amount * app.sceneRadius * 0.06f * app.speedMul);
    }
    return EM_TRUE;
}
// ---- touch: 1 finger = look (fly) / pan (orbit); 2 fingers = pinch zoom + drag (orbit: rotate, fly: slide)
void collectTouches(const EmscriptenTouchEvent* e, bool ending) {
    app.touches.clear();
    for (int i = 0; i < e->numTouches; i++) {
        const EmscriptenTouchPoint& t = e->touches[i];
        if (ending && t.isChanged) continue;
        app.touches.push_back({t.identifier, (float)t.clientX, (float)t.clientY});
    }
}

void beginGesture() {
    size_t n = app.touches.size();
    app.dragging = n > 0;
    if (n == 0) return;
    float cx = 0, cy = 0;
    for (auto& t : app.touches) { cx += t.x; cy += t.y; }
    cx /= n; cy /= n;
    app.gestureCx = cx;
    app.gestureCy = cy;
    if (n >= 2) {
        app.gestureDist = std::max(1.0f, std::hypot(app.touches[0].x - app.touches[1].x, app.touches[0].y - app.touches[1].y));
        app.gestureAngle = std::atan2(app.touches[1].y - app.touches[0].y, app.touches[1].x - app.touches[0].x);
    }
    app.mouseX = (int)cx;
    app.mouseY = (int)cy;
    pickPivot((int)cx, (int)cy);
    beginPan();
}

EM_BOOL onTouch(int type, const EmscriptenTouchEvent* e, void*) {
    if (app.gameMode) return EM_FALSE;
    if (!app.touchMode) {
        app.touchMode = true;
        EM_ASM({ if (window.RME && window.RME.onTouchMode) window.RME.onTouchMode(); });
    }
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) {
        if (e->numTouches == 1) { app.touchStart = emscripten_get_now(); app.touchTravel = 0; }
        collectTouches(e, false);
        beginGesture();
    } else if (type == EMSCRIPTEN_EVENT_TOUCHMOVE) {
        size_t n = app.touches.size();
        std::vector<App::TouchPt> cur;
        for (int i = 0; i < e->numTouches; i++)
            cur.push_back({e->touches[i].identifier, (float)e->touches[i].clientX, (float)e->touches[i].clientY});
        if (cur.size() != n || n == 0) { collectTouches(e, false); beginGesture(); return EM_TRUE; }
        if (n == 1) {
            // one finger: drag the ground under the finger (horizontal pan, height unchanged)
            app.touchTravel += std::fabs(cur[0].x - app.touches[0].x) + std::fabs(cur[0].y - app.touches[0].y);
            panTo((int)cur[0].x, (int)cur[0].y);
            app.mouseX = (int)cur[0].x;
            app.mouseY = (int)cur[0].y;
        } else {
            // two fingers (like Google Earth): pinch = zoom, vertical drag = tilt, twist = rotate around the
            // vertical axis through the touched midpoint. The pivot keeps its on-screen position throughout.
            float cx = (cur[0].x + cur[1].x) * 0.5f, cy = (cur[0].y + cur[1].y) * 0.5f;
            float dist = std::max(1.0f, std::hypot(cur[0].x - cur[1].x, cur[0].y - cur[1].y));
            float angle = std::atan2(cur[1].y - cur[0].y, cur[1].x - cur[0].x);
            float dAngle = angle - app.gestureAngle;
            const float pi = 3.14159265f;
            if (dAngle > pi) dAngle -= 2 * pi;
            if (dAngle < -pi) dAngle += 2 * pi;
            float dy = cy - app.gestureCy;
            app.touchTravel += std::fabs(cx - app.gestureCx) + std::fabs(dy);
            app.cam.yaw -= dAngle;  // map follows the fingers
            app.cam.pitch = std::max(-kMaxPitch, std::min(kMaxPitch, app.cam.pitch - dy * 0.005f));  // swipe up = towards the horizon and beyond
            float zoom = std::max(0.5f, std::min(2.0f, dist / app.gestureDist));  // spreading fingers = closer
            Vec3 off = app.orbitOff * (1.0f / zoom);
            if (off.z >= app.sceneRadius * 0.002f || zoom < 1.0f) app.orbitOff = off;
            orbitReposition();
            app.gestureCx = cx;
            app.gestureCy = cy;
            app.gestureDist = dist;
            app.gestureAngle = angle;
        }
        app.touches = cur;
    } else {  // end / cancel
        bool tap = type == EMSCRIPTEN_EVENT_TOUCHEND && app.touches.size() == 1 && e->numTouches == 1 && app.touchTravel < 12.0f &&
                   emscripten_get_now() - app.touchStart < 400.0;
        double lastX = app.touches.empty() ? -1 : app.touches[0].x, lastY = app.touches.empty() ? -1 : app.touches[0].y;
        collectTouches(e, true);
        beginGesture();
        if (app.touches.empty()) {
            app.dragging = false;
            if (tap) { app.mouseX = (int)lastX; app.mouseY = (int)lastY; }
            else { app.mouseX = -1; }
        }
    }
    return EM_TRUE;
}

EM_BOOL onContextMenu(int, const EmscriptenMouseEvent*, void*) { return EM_TRUE; }

void initGames() {
    app.games.emplace_back(makeRevolt());
    app.games.emplace_back(makeCarma());
}

}  // namespace

extern "C" {

// Rebuilds the file index; call after the set of available files changed.
EMSCRIPTEN_KEEPALIVE void rme_files_changed() { vfs::refresh(); }

// JSON: {"games":[{"id","name","hint","detected","levels":[{"id","name","group"}]}]}
EMSCRIPTEN_KEEPALIVE const char* rme_catalog() {
    std::string j = "{\"games\":[";
    for (size_t g = 0; g < app.games.size(); g++) {
        Game* game = app.games[g].get();
        if (g) j += ",";
        bool det = game->detect();
        j += "{\"id\":\"" + std::string(game->id()) + "\",\"name\":\"" + jsonEscape(game->name()) + "\",\"hint\":\"" +
             jsonEscape(game->dropHint()) + "\",\"detected\":" + (det ? "true" : "false") + ",\"levels\":[";
        if (det) {
            auto levels = game->listLevels();
            for (size_t i = 0; i < levels.size(); i++) {
                if (i) j += ",";
                j += "{\"id\":\"" + jsonEscape(levels[i].id) + "\",\"name\":\"" + jsonEscape(levels[i].name) + "\",\"group\":\"" +
                     jsonEscape(levels[i].group) + "\"}";
            }
        }
        j += "]}";
    }
    j += "]}";
    app.json = j;
    return app.json.c_str();
}

// Loads a level; returns 1 on success. Suspends while files are read.
EMSCRIPTEN_KEEPALIVE int rme_load(const char* gameId, const char* levelId) {
    Game* game = nullptr;
    for (auto& g : app.games)
        if (!strcmp(g->id(), gameId)) game = g.get();
    if (!game) { app.lastError = "unknown game"; return 0; }
    auto s = std::make_unique<Scene>();
    std::string err;
    if (!game->load(levelId, *s, err)) {
        app.lastError = err.empty() ? "failed to load level" : err;
        return 0;
    }
    stopGame();
    app.mg.forget();
    app.currentGame = game;
    app.retired = std::move(app.scene);
    app.scene = std::move(s);
    frameCamera();
    if (app.camMode == 1) pickPivot(app.canvasW / 2, app.canvasH / 2);
    app.renderer.setScene(app.scene.get(), app.sceneRadius);
    app.retired.reset();
    app.hoverShown = -2;
    return 1;
}

// ---- driving minigame (see src/cpp/minigame01)
// The game whose car the minigame prefers (any game's levels can be driven with it), else null.
Game* featuredCarGame() {
    for (auto& g : app.games)
        if (g->featuredCar() && g->canDrive()) return g.get();
    return nullptr;
}

// 1 when the loaded level can be played: a car is available, the featured one from any game or the level's own.
EMSCRIPTEN_KEEPALIVE int rme_game_can_play() { return app.scene && app.currentGame && (featuredCarGame() || app.currentGame->canDrive()) ? 1 : 0; }

// Starts the minigame in the loaded level; returns 1 on success. Suspends while the car is read.
// mode 0 = Powerup Rush (timed), 1 = free drive.
EMSCRIPTEN_KEEPALIVE int rme_game_start(int mode) {
    if (!app.scene || !app.currentGame) { app.lastError = "no level loaded"; return 0; }
    if (app.gameMode) return 1;
    if (!app.mg.prepared()) {
        minigame01::CarModel car;
        Game* provider = featuredCarGame();
        if (!provider) provider = app.currentGame;
        if (!provider->loadCarModel(*app.scene, car)) { app.lastError = "cannot load the car"; return 0; }
        if (provider != app.currentGame) car.unitsPerMetre = app.currentGame->unitsPerMetre();  // fit the car to this game's scale
        if (!app.mg.prepare(*app.scene, car)) { app.lastError = "this level has no solid ground to drive on"; return 0; }
        app.renderer.setScene(app.scene.get(), app.sceneRadius);  // uploads the car
    }
    app.viewerOpt = app.opt;
    app.viewerCam = app.cam;
    app.viewerCamMode = app.camMode;
    app.opt.powerupMeshes = true;   // the pickups are the in-game meshes
    app.opt.wireframe = false;
    app.opt.showGeometry = true;
    app.opt.textured = true;
    app.dragging = false;
    app.mg.start(*app.scene, app.sceneCenter, mode == 1);
    app.gameMode = true;
    app.hoverShown = -2;
    notifyHover(nullptr, nullptr, 0, 0);
    EM_ASM({ if (window.RME && window.RME.onGameMode) window.RME.onGameMode(1); });
    pushGameHud();
    return 1;
}

EMSCRIPTEN_KEEPALIVE void rme_game_stop() { stopGame(); }

EMSCRIPTEN_KEEPALIVE const char* rme_last_error() { return app.lastError.c_str(); }

// JSON: {"title","info","categories":[{"name","color","count","visible","line"}]}
EMSCRIPTEN_KEEPALIVE const char* rme_scene_info() {
    Scene* s = app.scene.get();
    std::string j = "{";
    if (s) {
        j += std::string("\"lighting\":") + (s->hasLighting ? "true" : "false") + ",";
        bool pm = false;
        for (const Category& c : s->cats) pm |= c.hasMesh;
        j += std::string("\"canCull\":") + (s->cullBackfaces ? "true" : "false") + ",";
        bool animated = false;
        for (const Mover& mv : s->movers) animated |= !mv.staticModel && !mv.external;
        j += std::string("\"hasMovers\":") + (animated ? "true" : "false") + ",";
        j += std::string("\"hasPowerupMeshes\":") + (pm ? "true" : "false") + ",";
        j += "\"title\":\"" + jsonEscape(s->title) + "\",\"info\":\"" + jsonEscape(s->info) + "\",\"categories\":[";
        for (size_t i = 0; i < s->cats.size(); i++) {
            const Category& c = s->cats[i];
            if (i) j += ",";
            char col[16];
            snprintf(col, sizeof col, "#%06x", c.color);
            // Box / line categories also own a centre marker: report markers when there are any, else segments.
            int markers = 0;
            for (const Marker& m : s->markers) markers += m.cat == (int)i;
            j += "{\"id\":\"" + jsonEscape(c.id) + "\",\"name\":\"" + jsonEscape(c.name) + "\",\"color\":\"" + col + "\",\"count\":" + std::to_string(markers ? markers : c.count) +
                 ",\"visible\":" + (c.visible ? "true" : "false") + "}";
        }
        j += "]";
    }
    j += "}";
    app.json = j;
    return app.json.c_str();
}

EMSCRIPTEN_KEEPALIVE void rme_set_category_visible(int cat, int visible) {
    if (app.scene && cat >= 0 && cat < (int)app.scene->cats.size()) app.scene->cats[cat].visible = visible != 0;
}

EMSCRIPTEN_KEEPALIVE void rme_set_option(const char* name, double v) {
    std::string n = name;
    if (n == "exposure") app.opt.exposure = (float)v;
    else if (n == "textured") app.opt.textured = v != 0;
    else if (n == "shade") app.opt.shade = (float)v;
    else if (n == "markerScale") app.opt.markerScale = (float)v;
    else if (n == "lineWidth") app.opt.lineWidth = (float)v;
    else if (n == "xray") app.opt.xray = v != 0;
    else if (n == "geometry") app.opt.showGeometry = v != 0;
    else if (n == "sun") app.opt.sunLight = v != 0;
    else if (n == "points") app.opt.pointLights = v != 0;
    else if (n == "powerupMeshes") app.opt.powerupMeshes = v != 0;
    else if (n == "wireframe") app.opt.wireframe = v != 0;
    else if (n == "beams") app.opt.beams = v != 0;
    else if (n == "backFaces") app.opt.backFaces = v != 0;
    else if (n == "animSpeed") app.animScale = (float)std::max(0.0, v);
    else if (n == "speed") app.speedMul = (float)v;
}

EMSCRIPTEN_KEEPALIVE void rme_camera_command(int cmd) {
    if (cmd == 0) frameCamera();
    else if (cmd == 1) jumpToStart();
    else if (cmd == 2 && app.scene) {  // top-down
        app.cam.pos = app.sceneCenter + Vec3(0, app.sceneRadius * 1.6f, 0);
        app.cam.pitch = -kMaxPitch;
    }
    if (app.camMode == 1) pickPivot(app.canvasW / 2, app.canvasH / 2);
}

EMSCRIPTEN_KEEPALIVE void rme_set_camera_mode(int mode) { setCameraMode(mode ? 1 : 0); }

// Debug helper: place the camera explicitly.
EMSCRIPTEN_KEEPALIVE void rme_set_camera(double x, double y, double z, double yaw, double pitch) {
    app.cam.pos = Vec3((float)x, (float)y, (float)z);
    app.cam.yaw = (float)yaw;
    app.cam.pitch = (float)pitch;
    if (app.camMode == 1) pickPivot(app.canvasW / 2, app.canvasH / 2);
}

EMSCRIPTEN_KEEPALIVE double rme_anim_time() { return app.animTime; }
EMSCRIPTEN_KEEPALIVE void rme_set_anim_time(double t) { app.animTime = t < 0 ? 0 : t; }

// "x,y,z,yaw,pitch" - used by the page to store / share the view.
EMSCRIPTEN_KEEPALIVE const char* rme_camera_state() {
    char buf[160];
    snprintf(buf, sizeof buf, "%.2f,%.2f,%.2f,%.4f,%.4f", app.cam.pos.x, app.cam.pos.y, app.cam.pos.z, app.cam.yaw, app.cam.pitch);
    app.json = buf;
    return app.json.c_str();
}

}  // extern "C"

int main() {
    if (!app.renderer.init("#canvas")) {
        EM_ASM({ if (window.RME && window.RME.onFatal) window.RME.onFatal('WebGL2 is not available in this browser.'); });
        return 1;
    }
    initGames();
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onKey);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onKey);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onBlur);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, onMouseDown);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onMouseUp);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, onMouseMove);
    emscripten_set_mouseleave_callback("#canvas", nullptr, EM_TRUE, onMouseLeave);
    emscripten_set_wheel_callback("#canvas", nullptr, EM_TRUE, onWheel);
    emscripten_set_touchstart_callback("#canvas", nullptr, EM_TRUE, onTouch);
    emscripten_set_touchmove_callback("#canvas", nullptr, EM_TRUE, onTouch);
    emscripten_set_touchend_callback("#canvas", nullptr, EM_TRUE, onTouch);
    emscripten_set_touchcancel_callback("#canvas", nullptr, EM_TRUE, onTouch);
    EM_ASM({ if (window.RME && window.RME.onReady) window.RME.onReady(); });
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
