#include "gestures/gestures.h"
#include <emscripten.h>
#include <algorithm>
#include <cmath>

namespace {

// Two-finger gestures normally pivot on the midpoint between the fingers. When the second finger touches down at least this
// long after the first one, the first finger (which was already holding a spot on the map) is the pivot instead.
constexpr double kFirstFingerPivotDelayMs = 500.0;

Vec2i clientPos(const EmscriptenMouseEvent* e) { return {e->clientX, e->clientY}; }

}  // namespace

void GestureHandler::install() {
    emscripten_set_mousedown_callback("#canvas", this, EM_TRUE,
        [](int, const EmscriptenMouseEvent* e, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->mouseDown(e); });
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE,
        [](int, const EmscriptenMouseEvent*, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->mouseUp(); });
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE,
        [](int, const EmscriptenMouseEvent* e, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->mouseMove(e); });
    emscripten_set_mouseleave_callback("#canvas", this, EM_TRUE,
        [](int, const EmscriptenMouseEvent*, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->mouseLeave(); });
    emscripten_set_wheel_callback("#canvas", this, EM_TRUE,
        [](int, const EmscriptenWheelEvent* e, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->wheel(e); });
    auto touchCb = [](int type, const EmscriptenTouchEvent* e, void* u) -> EM_BOOL { return static_cast<GestureHandler*>(u)->touch(type, e); };
    emscripten_set_touchstart_callback("#canvas", this, EM_TRUE, touchCb);
    emscripten_set_touchmove_callback("#canvas", this, EM_TRUE, touchCb);
    emscripten_set_touchend_callback("#canvas", this, EM_TRUE, touchCb);
    emscripten_set_touchcancel_callback("#canvas", this, EM_TRUE, touchCb);
}

EM_BOOL GestureHandler::mouseDown(const EmscriptenMouseEvent* e) {
    if (!target.enabled()) return EM_FALSE;
    isTouch = false;
    isDragging = true;
    dragButton = e->button;
    setPointer(clientPos(e));
    if (target.orbitMode()) {
        target.pickPivot(pos);
        target.beginPan();
    }
    return EM_TRUE;
}

EM_BOOL GestureHandler::mouseUp() {
    isDragging = false;
    dragButton = -1;
    return EM_FALSE;
}

EM_BOOL GestureHandler::mouseMove(const EmscriptenMouseEvent* e) {
    if (!target.enabled()) return EM_FALSE;
    // Never preventDefault here: the browser needs mouse moves to drag the page's sliders.
    if (isTouch && e->movementX == 0 && e->movementY == 0) return EM_FALSE;
    isTouch = false;
    setPointer(clientPos(e));  // the canvas fills the window, so client == canvas CSS coordinates
    if (isDragging) {
        if (target.orbitMode() && dragButton == 0) {
            target.panTo(pos);  // left drag: grab the ground and slide it
        } else {
            target.rotateView(Vec2f((float)e->movementX, (float)e->movementY) * 0.0035f);
            if (target.orbitMode()) target.orbitReposition();  // right drag: orbit around the pivot
        }
    }
    return EM_FALSE;
}

EM_BOOL GestureHandler::mouseLeave() {
    pointerKnown = false;
    return EM_FALSE;
}

EM_BOOL GestureHandler::wheel(const EmscriptenWheelEvent* e) {
    if (!target.enabled()) return EM_FALSE;
    float amount = e->deltaY > 0 ? -1.0f : 1.0f;  // wheel up = forward / closer
    Vec2i at = clientPos(&e->mouse);
    if (target.orbitMode()) setPointer(at);
    target.wheelZoom(at, amount);
    return EM_TRUE;
}

// ---- touch: 1 finger = look (fly) / pan (orbit); 2 fingers = pinch zoom + drag (orbit: rotate, fly: slide)
void GestureHandler::collectTouches(const EmscriptenTouchEvent* e, bool ending) {
    touches.clear();
    for (int i = 0; i < e->numTouches; i++) {
        const EmscriptenTouchPoint& t = e->touches[i];
        if (ending && t.isChanged) continue;
        touches.push_back({t.identifier, Vec2f((float)t.clientX, (float)t.clientY)});
    }
}

void GestureHandler::beginGesture() {
    size_t n = touches.size();
    isDragging = n > 0;
    if (n == 0) return;
    Vec2f sum;
    for (auto& t : touches) sum += t.pos;
    gestureCenter = sum * (1.0f / (float)n);
    if (n >= 2) {
        Vec2f d = touches[1].pos - touches[0].pos;
        gestureDist = std::max(1.0f, length(d));
        gestureAngle = std::atan2(d.y, d.x);
    }
    Vec2f pivotAt = gestureCenter;  // where the pivot is picked
    if (n >= 2 && pivotOnFirst)
        for (auto& t : touches)
            if (t.id == firstTouchId) pivotAt = t.pos;
    setPointer(Vec2i(gestureCenter));
    target.pickPivot(Vec2i(pivotAt));
    target.beginPan();
}

EM_BOOL GestureHandler::touch(int type, const EmscriptenTouchEvent* e) {
    if (!target.enabled()) return EM_FALSE;
    if (!isTouch) {
        isTouch = true;
        EM_ASM({ if (window.RME && window.RME.onTouchMode) window.RME.onTouchMode(); });
    }
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) {
        if (e->numTouches == 1) {
            touchStart = emscripten_get_now();
            touchTravel = 0;
            firstTouchId = e->touches[0].identifier;
            pivotOnFirst = false;
        } else if (e->numTouches == 2) {
            pivotOnFirst = emscripten_get_now() - touchStart >= kFirstFingerPivotDelayMs;
        }
        collectTouches(e, false);
        beginGesture();
    } else if (type == EMSCRIPTEN_EVENT_TOUCHMOVE) {
        size_t n = touches.size();
        std::vector<TouchPt> cur;
        for (int i = 0; i < e->numTouches; i++)
            cur.push_back({e->touches[i].identifier, Vec2f((float)e->touches[i].clientX, (float)e->touches[i].clientY)});
        if (cur.size() != n || n == 0) { collectTouches(e, false); beginGesture(); return EM_TRUE; }
        if (n == 1) {
            // one finger: drag the ground under the finger (horizontal pan, height unchanged)
            Vec2f moved = cur[0].pos - touches[0].pos;
            touchTravel += std::fabs(moved.x) + std::fabs(moved.y);
            target.panTo(Vec2i(cur[0].pos));
            setPointer(Vec2i(cur[0].pos));
        } else {
            // two fingers (like Google Earth): pinch = zoom, vertical drag = tilt, twist = rotate around the
            // vertical axis through the touched midpoint. The pivot keeps its on-screen position throughout.
            Vec2f center = (cur[0].pos + cur[1].pos) * 0.5f;
            Vec2f d = cur[1].pos - cur[0].pos;
            float dist = std::max(1.0f, length(d));
            float angle = std::atan2(d.y, d.x);
            float dAngle = angle - gestureAngle;
            const float pi = 3.14159265f;
            if (dAngle > pi) dAngle -= 2 * pi;
            if (dAngle < -pi) dAngle += 2 * pi;
            Vec2f centerMoved = center - gestureCenter;
            touchTravel += std::fabs(centerMoved.x) + std::fabs(centerMoved.y);
            float zoom = std::max(0.5f, std::min(2.0f, dist / gestureDist));  // spreading fingers = closer
            // the map follows the fingers: twist = turn the other way; swipe up = towards the horizon and beyond
            target.pinchTilt(-dAngle, -centerMoved.y * 0.005f, zoom);
            gestureCenter = center;
            gestureDist = dist;
            gestureAngle = angle;
        }
        touches = cur;
    } else {  // end / cancel
        bool tap = type == EMSCRIPTEN_EVENT_TOUCHEND && touches.size() == 1 && e->numTouches == 1 && touchTravel < 12.0f &&
                   emscripten_get_now() - touchStart < 400.0;
        Vec2f last = touches.empty() ? Vec2f() : touches[0].pos;
        collectTouches(e, true);
        beginGesture();
        if (touches.empty()) {
            isDragging = false;
            if (tap) setPointer(Vec2i(last));
            else pointerKnown = false;
        }
    }
    return EM_TRUE;
}
