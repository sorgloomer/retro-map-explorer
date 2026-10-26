// Minigame 01 - "Powerup Rush": drive the Eagle through the level you were viewing and collect its powerups before
// the clock runs out. Time bonuses add seconds, upgrades and credits are worth the most, and sliding earns drift points.
// The level's own triangles are the road (CollisionWorld), the car is a raycast vehicle (Vehicle).
#pragma once
#include <vector>
#include "../camera.h"
#include "../scene.h"
#include "car_model.h"
#include "collision_world.h"
#include "input.h"
#include "vehicle.h"

namespace minigame01 {

struct Hud {
    float speedKmh = 0;
    int score = 0;
    int driftScore = 0;
    float timeLeft = 0;
    int collected = 0, total = 0;
    int state = 0;           // 0 = playing, 1 = time is up, 2 = everything collected, 3 = free drive (no clock, no end)
    float messageAge = 99;   // seconds since the last pickup message
    char message[64] = {};
};

class MiniGame {
public:
    bool prepared() const { return isPrepared; }
    // Adds the car to the scene (once per scene) and builds the collision world from the level.
    bool prepare(Scene& scene, const CarModel& car);
    // Forgets everything about the current scene (call when a new level replaced it).
    void forget();

    // Puts the car on the start line and begins a round; freeDrive: no time limit, the round never ends.
    void start(Scene& scene, const Vec3& sceneCenter, bool freeDrive);
    void stop(Scene& scene);                              // hides the car and brings the pickups back
    bool active() const { return running; }

    void update(float dt, const Controls& controls, Scene& scene);
    Camera camera() const { return cam; }
    // World units per metre of the level (the physics runs in metres; Re-Volt's units are much smaller than a metre).
    float unitsPerMetre() const { return scale; }
    const Hud& hud() const { return hud_; }
    const Input& inputRef() const { return input; }
    Input& inputRef() { return input; }

private:
    struct Pickup {
        Vec3 pos;
        int points = 50;
        float seconds = 0;
        std::vector<int> movers;  // the icon and its halo
        bool taken = false;
        const char* label = "Powerup";
    };

    void newRound(Scene& scene);
    void placeCar(const Vec3& groundPos, float yaw);
    enum class Spawn { Start, Here, Safe };
    void respawn(Scene& scene, Spawn where);
    void collect(Scene& scene);
    void updateCamera(float dt, bool snap);
    void syncMovers(Scene& scene);
    void makePickupIcons(Scene& scene);  // levels without pickup meshes: spinning gems at their pickup markers
    bool groundBelow(const Vec3& from, Vec3& hit) const;

    bool isPrepared = false, running = false, freeDrive = false;
    float scale = 1.0f;
    std::vector<int> iconCats;        // categories whose markers are replaced by gem movers while playing
    std::vector<char> iconCatShown;   // their visibility in the viewer
    CollisionWorld world;
    CarModel car;
    Vehicle vehicle;
    Input input;
    int carMover[5] = {-1, -1, -1, -1, -1};  // body, FL, FR, RL, RR
    std::vector<Pickup> pickups;
    Hud hud_;
    Vec3 startPos;
    float startYaw = 0;
    Vec3 safePos;
    float safeYaw = 0, safeTimer = 0, stuckTimer = 0, worldFloor = -1000.0f;
    Camera cam;
    float camYaw = 0;
    Vec3 camPos;
    int camMode = 0;
    float driftAccum = 0;
};

}  // namespace minigame01
