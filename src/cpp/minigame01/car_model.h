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
    float wheelRadius = 0.34f;      // one radius for all wheels, when nothing better is known
    float meshWheelRadii[4] = {0, 0, 0, 0};  // measured from each wheel's mesh (0: not measured)
    float fileWheelRadii[4] = {0, 0, 0, 0};  // from the car's own files (0: the files do not say)
    float fileSpringK = 0;          // suspension spring stiffness in N/m from the car's own files (0: not said)
    float fileDamper = 0;           // suspension damping in N s/m from the car's own files (0: not said)
    float wheelWidth = 0.25f;
    Vec3f boundsMin, boundsMax;  // of the chassis
    float mass = 0;                  // kg, from the car's own files; 0 when they do not say
    bool hasCentreOfMass = false;
    // Where the car's own files put the centre of mass, as fractions of the chassis box measured from its geometric centre
    // (car space: x towards the right, y up, z forward): z = -0.2 is a fifth of the length towards the rear, y = -0.1 is a tenth
    // of the height below the middle, x = 0 is no left-right bias.
    Vec3f centreOfMassFraction;
    // The chassis hull of the car's files (the physics shape, without the roof spike and the like), as a box in car space.
    bool hasHullBox = false;
    Vec3f hullBoxMin, hullBoxMax;
};

}  // namespace minigame01
