#include "minigame01/input.h"
#include "minigame01/params.h"
#include <emscripten/html5.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace minigame01 {

namespace {

enum Held { H_FWD, H_BACK, H_LEFT, H_RIGHT, H_HAND, H_AFTERBURNER, H_SIDE_THRUST };

float approach(float v, float target, float rate, float dt) {
    float d = target - v;
    float step = rate * dt;
    return std::fabs(d) <= step ? target : v + (d > 0 ? step : -step);
}

float deadzone(float v, float dz) {
    float a = std::fabs(v);
    if (a <= dz) return 0.0f;
    a = (a - dz) / (1.0f - dz);
    return (v < 0 ? -1.0f : 1.0f) * a * (0.35f + 0.65f * a);  // soft centre: fine control near the middle
}

}  // namespace

bool Input::key(const char* code, bool down, bool repeat) {
    if (!strcmp(code, "KeyW") || !strcmp(code, "ArrowUp")) { held[H_FWD] = down; return true; }
    if (!strcmp(code, "KeyS") || !strcmp(code, "ArrowDown")) { held[H_BACK] = down; return true; }
    if (!strcmp(code, "KeyA") || !strcmp(code, "ArrowLeft")) { held[H_LEFT] = down; return true; }
    if (!strcmp(code, "KeyD") || !strcmp(code, "ArrowRight")) { held[H_RIGHT] = down; return true; }
    if (!strcmp(code, "Space")) { held[H_HAND] = down; return true; }
    if (!strcmp(code, "Digit1") || !strcmp(code, "Numpad1")) { held[H_AFTERBURNER] = down; return true; }
    if (!strcmp(code, "Digit2") || !strcmp(code, "Numpad2")) { if (down && !repeat) edgeKangaroo = true; return true; }
    if (!strcmp(code, "Digit8") || !strcmp(code, "Numpad8")) { if (down && !repeat) edgeFlip = true; return true; }
    if (!strcmp(code, "Digit9") || !strcmp(code, "Numpad9")) { held[H_SIDE_THRUST] = down; return true; }
    if (!strcmp(code, "KeyR")) { if (down && !repeat) edgeReset = true; return true; }
    if (!strcmp(code, "KeyC")) { if (down && !repeat) edgeCamera = true; return true; }
    if (!strcmp(code, "Enter")) { if (down && !repeat) edgeRestart = true; return true; }
    if (!strcmp(code, "Escape")) { if (down && !repeat) exitFlag = true; return true; }
    return false;
}

void Input::touchStick(float x, float y) {
    touchSteer = std::max(-1.0f, std::min(1.0f, x));
    touchThrottle = std::max(-1.0f, std::min(1.0f, y));
}

void Input::touch(int control, bool down) {
    switch (control) {
        case T_HANDBRAKE: touchHandbrake = down; break;
        case T_RESET: if (down) edgeReset = true; break;
        case T_CAMERA: if (down) edgeCamera = true; break;
        case T_RESTART: if (down) edgeRestart = true; break;
        case T_EXIT: if (down) exitFlag = true; break;
        default: break;
    }
}

void Input::releaseAll() {
    std::memset(held, 0, sizeof held);
    touchSteer = touchThrottle = 0;
    touchHandbrake = false;
    kbSteer = kbAccel = kbBrake = 0;
}

bool Input::exitRequested() {
    bool v = exitFlag;
    exitFlag = false;
    return v;
}

Controls Input::update(float dt) {
    const Params& P = tuning();
    Controls c;
    // keyboard: digital keys ramp up and fall back, so steering and throttle are not on / off
    float steerT = (held[H_RIGHT] ? 1.0f : 0.0f) - (held[H_LEFT] ? 1.0f : 0.0f);
    kbSteer = approach(kbSteer, steerT, steerT == 0 ? P.keySteerReturn : (steerT * kbSteer < 0 ? P.keySteerReverse : P.keySteerRise), dt);
    kbAccel = approach(kbAccel, held[H_FWD] ? 1.0f : 0.0f, held[H_FWD] ? P.keyAccelRise : P.keyAccelFall, dt);
    kbBrake = approach(kbBrake, held[H_BACK] ? 1.0f : 0.0f, held[H_BACK] ? P.keyBrakeRise : P.keyBrakeFall, dt);
    c.steer = kbSteer;
    c.accel = kbAccel;
    c.brake = kbBrake;
    c.handbrake = (held[H_HAND] || touchHandbrake) ? 1.0f : 0.0f;
    // touch stick: analog, so a small push is a gentle input
    float ts = deadzone(touchSteer, 0.08f);
    if (std::fabs(ts) > std::fabs(c.steer)) c.steer = ts;
    c.accel = std::max(c.accel, deadzone(std::max(touchThrottle, 0.0f), 0.08f));
    c.brake = std::max(c.brake, deadzone(std::max(-touchThrottle, 0.0f), 0.08f));
    c.reset = edgeReset;
    c.restart = edgeRestart;
    c.toggleCamera = edgeCamera;
    c.afterburner = held[H_AFTERBURNER];
    c.kangaroo = edgeKangaroo;
    c.sideThrust = held[H_SIDE_THRUST];
    c.flip = edgeFlip;
    edgeReset = edgeRestart = edgeCamera = edgeKangaroo = edgeFlip = false;

    // gamepad (the first connected one)
    pad = false;
    if (emscripten_sample_gamepad_data() == EMSCRIPTEN_RESULT_SUCCESS) {
        int n = emscripten_get_num_gamepads();
        for (int i = 0; i < n; i++) {
            EmscriptenGamepadEvent g;
            if (emscripten_get_gamepad_status(i, &g) != EMSCRIPTEN_RESULT_SUCCESS || !g.connected) continue;
            pad = true;
            auto axis = [&](int a) { return a < g.numAxes ? (float)g.axis[a] : 0.0f; };
            auto btn = [&](int b) { return b < g.numButtons ? (float)g.analogButton[b] : 0.0f; };
            auto down = [&](int b) { return b < g.numButtons && g.digitalButton[b]; };
            float steer = deadzone(axis(0), P.gamepadDeadzone);
            if (down(14)) steer = -1.0f;
            if (down(15)) steer = 1.0f;
            if (std::fabs(steer) > std::fabs(c.steer)) c.steer = steer;
            float rt = btn(7), lt = btn(6);
            if (down(7) && rt < 0.5f) rt = 1.0f;
            if (down(6) && lt < 0.5f) lt = 1.0f;
            if (rt > c.accel) c.accel = rt;
            if (lt > c.brake) c.brake = lt;
            if (down(0)) c.handbrake = 1.0f;
            bool now[16];
            for (int b = 0; b < 16; b++) now[b] = down(b);
            if (now[3] && !prevButtons[3]) c.reset = true;
            if (now[2] && !prevButtons[2]) c.toggleCamera = true;
            if (now[9] && !prevButtons[9]) c.restart = true;
            if (now[8] && !prevButtons[8]) exitFlag = true;
            std::memcpy(prevButtons, now, sizeof now);
            break;
        }
    }
    c.steer = std::max(-1.0f, std::min(1.0f, c.steer));
    return c;
}

}  // namespace minigame01
