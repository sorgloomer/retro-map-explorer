// The drivable car as a game provides it: meshes in car space plus the few dimensions the physics needs.
// Car space follows the viewer's world: +x right, +y up, +z forward, in metres (the mirror of the original data is already applied).
// The meshes are in metres; the minigame scales them by unitsPerMetre when it draws them in the level.
#pragma once
#include <string>
#include "../scene/scene.h"

namespace minigame01 {

struct CarModel {
    std::string name;
    BatchSet body;       // chassis, windows, driver...
    BatchSet wheel[4];   // one per wheel (front left, front right, rear left, rear right), centred on the axle
    Vec3f wheelPos[4];    // rest position of the wheel centres, same order
    float unitsPerMetre = 1;  // size of a metre in the game's own units: the model is already in metres, the level is not
    float wheelRadius = 0.34f;
    float wheelWidth = 0.25f;
    Vec3f boundsMin, boundsMax;  // of the chassis
};

}  // namespace minigame01
