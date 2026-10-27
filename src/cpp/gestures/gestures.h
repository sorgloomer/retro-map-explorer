// Mouse, wheel and touch gestures. The handler owns the pointer state (button, drag, fingers); what a gesture does to the
// camera is done by a GestureTarget, which the app implements.
#pragma once
#include <emscripten/html5.h>
#include <vector>
#include "math/vec2.h"

// What the gestures steer. Positions are CSS pixels on the canvas.
class GestureTarget {
public:
    virtual ~GestureTarget() = default;
    virtual bool enabled() const = 0;      // false while something else (the minigame) owns the input
    virtual bool orbitMode() const = 0;    // orbit around a pivot (true) or fly (false)
    virtual void pickPivot(VecI2 at) = 0;          // pivot of the next orbit / pan: what is under the point
    virtual void beginPan() = 0;                   // a pan starts from the current camera
    virtual void panTo(VecI2 at) = 0;              // slide the map so the pivot stays under the point
    virtual void rotateView(Vec2 angles) = 0;      // turn the view by these angles (radians: yaw, pitch)
    virtual void orbitReposition() = 0;            // put the camera back so the pivot keeps its place on screen
    virtual void wheelZoom(VecI2 at, float amount) = 0;  // amount > 0: closer
    virtual void pinchTilt(float dYaw, float dPitch, float zoom) = 0;  // two fingers: turn, tilt, scale the distance to the pivot
};

class GestureHandler {
public:
    explicit GestureHandler(GestureTarget& target) : target(target) {}

    // Registers the mouse, wheel and touch callbacks.
    void install();

    bool dragging() const { return isDragging; }
    bool hasPointer() const { return pointerKnown; }  // false while the pointer is away: pointer() means nothing then
    VecI2 pointer() const { return pos; }  // CSS pixels (any value, negative included, can be a real position)
    bool touchMode() const { return isTouch; }  // the last input came from a touch screen
    void cancelDrag() { isDragging = false; }

private:
    struct TouchPt { int id; Vec2 pos; };

    EM_BOOL mouseDown(const EmscriptenMouseEvent* e);
    EM_BOOL mouseUp();
    EM_BOOL mouseMove(const EmscriptenMouseEvent* e);
    EM_BOOL mouseLeave();
    EM_BOOL wheel(const EmscriptenWheelEvent* e);
    EM_BOOL touch(int type, const EmscriptenTouchEvent* e);
    void setPointer(VecI2 p) { pos = p; pointerKnown = true; }
    void collectTouches(const EmscriptenTouchEvent* e, bool ending);
    void beginGesture();

    GestureTarget& target;
    bool isDragging = false;
    VecI2 pos;
    bool pointerKnown = false;
    int dragButton = -1;       // mouse button that started the current drag
    bool isTouch = false;
    std::vector<TouchPt> touches;
    float gestureDist = 0, gestureAngle = 0;  // of the two fingers, when the gesture last changed
    Vec2 gestureCenter;                       // their midpoint
    float touchTravel = 0;     // how far the fingers moved since the first one went down (tap detection)
    double touchStart = 0;
    int firstTouchId = -1;     // finger that started the current touch gesture
    bool pivotOnFirst = false; // the two-finger gesture pivots on the first finger (the second one came late)
};
