// Driving controls for the minigame: keyboard and gamepad folded into one set of analog controls.
//   keyboard: W / Up accelerate, S / Down brake and reverse, A D / Left Right steer, Space handbrake,
//             R put the car back on its wheels, C camera, Enter new round, Esc exit
//   touch:    on-screen stick (steer, gas / brake) and buttons (handbrake, upright, camera) drawn by the page
//   gamepad:  left stick / d-pad steer, RT accelerate, LT brake and reverse, A handbrake, Y reset car,
//             X camera, Start new round, Back exit (standard mapping)
#pragma once

namespace minigame01 {

struct Controls {
    float accel = 0, brake = 0, steer = 0, handbrake = 0;
    bool reset = false;         // edge: true for the one update after the button went down
    bool restart = false;       // edge
    bool toggleCamera = false;  // edge
};

class Input {
public:
    // Feeds a keyboard event (code = KeyboardEvent.code). True when the key belongs to the minigame.
    bool key(const char* code, bool down, bool repeat);
    // Feeds the on-screen stick: x = steering (-1 left .. 1 right), y = throttle (1 gas .. -1 brake / reverse).
    void touchStick(float x, float y);
    // Feeds an on-screen button (see Touch); the handbrake stays on until released, the others fire on press.
    enum Touch { T_HANDBRAKE, T_RESET, T_CAMERA, T_RESTART, T_EXIT };
    void touch(int control, bool down);
    // Samples the gamepads and returns the controls for a frame of `dt` seconds.
    Controls update(float dt);
    // True once after Esc or the gamepad Back button was pressed.
    bool exitRequested();
    void releaseAll();
    bool gamepadConnected() const { return pad; }

private:
    bool held[8] = {};  // forward, back, left, right, handbrake, ... (see input.cpp)
    float touchSteer = 0, touchThrottle = 0;  // on-screen stick
    bool touchHandbrake = false;
    bool edgeReset = false, edgeRestart = false, edgeCamera = false, exitFlag = false;
    float kbSteer = 0, kbAccel = 0, kbBrake = 0;
    bool pad = false;
    bool prevButtons[16] = {};
};

}  // namespace minigame01
