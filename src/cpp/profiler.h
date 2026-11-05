// Timers for the frame: how long the parts of it take (milliseconds on the CPU, and on the GPU where the browser has the timer queries), as a running
// average and a peak. A section can be timed many times in a frame (the physics steps): the times are added up for the frame. The debug panel shows them.
#pragma once
#include <emscripten.h>
#include <algorithm>

namespace prof {

enum Section {
    FRAME,               // the whole of a frame of the page's loop
    FRAME_INTERVAL,      // the time from the start of one frame to the start of the next (the page shows it as frames per second)
    INPUT,               // reading the keys and the gamepad
    GAME_UPDATE,         // the minigame's update: everything below up to the camera
    MOVERS,              // the kinematic bodies of the moving objects: set where they go
    VEHICLE,             // the car's step: the wheels' casts and forces, and the physics world
    WORLD_STEP,          // ... of which the Box3D world step (the contacts, the solver, the objects)
    PROPS_SYNC,          // the movable objects: the meshes follow their bodies
    SKID,                // the skid marks
    EFFECTS,             // flame, dust
    CAMERA,              // the chase camera (with its ray against the level)
    RENDER,              // the whole of rendering the frame (the commands: the GPU works on them after)
    SHADOW_LEVEL,        // ... of which the shadow maps of the level, when they are made again
    SHADOW_MOVING,       // ... of which the shadows of the car and the objects, every frame
    RENDER_LEVEL,        // ... of which the draw calls of the level (the picture)
    RENDER_MOVERS,       // ... of which the draw calls of the movers (the car, the objects, the powerups, the effects)
    RENDER_MARKS,        // ... of which the markers and the lines
    GPU_SHADOWS,         // on the GPU: the shadow maps
    GPU_MAIN,            // on the GPU: the picture
    COUNT_DRAWS,         // (counts, not times) the draw calls of a frame
    COUNT_INSTANCED,     // ... of which instanced ones (one call for many objects)
    COUNT_MOVERS_DRAWN,  // the movers that were drawn
    COUNT_MOVERS_CULLED, // the movers that were out of view, and not drawn
    SECTION_COUNT
};

inline const char* name(Section s) {
    static const char* names[SECTION_COUNT] = {"frame (CPU work in the loop)", "frame interval", "input", "game update", "moving objects (kinematic)", "car step", "  b3World_Step", "objects follow bodies", "skid marks",
                                               "effects", "camera", "render (CPU commands)", "  shadow maps of the level", "  shadows of car and objects", "  level draw calls", "  mover draw calls", "  markers and lines",
                                               "GPU shadow maps", "GPU picture", "# draw calls", "#   instanced", "# movers drawn", "# movers out of view"};
    return names[s];
}

class Profiler {
public:
    static Profiler& get() { static Profiler p; return p; }
    // Adds time to a section of this frame.
    void add(Section s, double ms) { thisFrame_[s] += ms; used_[s] = true; }
    // A measurement that is not part of a frame (the GPU's, which come in later): straight into the average.
    void sample(Section s, double ms) { fold(s, ms); }
    // The end of a frame: what the sections took in it goes into the averages.
    void endFrame() {
        for (int s = 0; s < SECTION_COUNT; s++) {
            if (used_[s]) { fold((Section)s, thisFrame_[s]); thisFrame_[s] = 0; used_[s] = false; }
            else if (seen_[s] && s < GPU_SHADOWS && s != FRAME_INTERVAL) fold((Section)s, 0.0);   // (a part that is not there this frame took no time: the averages fall)
        }
    }
    bool seen(Section s) const { return seen_[s]; }
    double average(Section s) const { return average_[s]; }
    double peak(Section s) const { return peak_[s]; }

private:
    void fold(Section s, double ms) {
        if (!seen_[s]) { seen_[s] = true; average_[s] = ms; peak_[s] = ms; return; }
        average_[s] += (ms - average_[s]) * 0.08;
        peak_[s] = std::max(ms, peak_[s] * 0.985);
    }
    double thisFrame_[SECTION_COUNT] = {};
    bool used_[SECTION_COUNT] = {};
    bool seen_[SECTION_COUNT] = {};
    double average_[SECTION_COUNT] = {}, peak_[SECTION_COUNT] = {};
};

// Times what is left of the scope.
struct Scope {
    Section section;
    double start;
    explicit Scope(Section s) : section(s), start(emscripten_get_now()) {}
    ~Scope() { Profiler::get().add(section, emscripten_get_now() - start); }
};

}  // namespace prof
