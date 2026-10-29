// Every tunable number of the minigame (car physics, rules, camera, keyboard feel) in one place. The page can read them as a
// JSON object and replace them (rme_game_params_json / rme_game_params_apply): each entry is "name": number, and an entry that
// is left out goes back to its default. Units: metres, seconds, kilograms, newtons, radians, unless said otherwise.
#pragma once
#include <string>

namespace minigame01 {

// X(name, default). The comment says what the number does; the JSON uses the same name.
#define MINIGAME_PARAMS(X) \
    /* --- mass and shape */ \
    X(mass, 0)                          /* kg; 0 = the car's own mass from its files, else defaultMass */ \
    X(defaultMass, 1250)                /* kg, when the car's files do not say and mass is 0 */ \
    X(centreOfMassFromFile, 1)          /* 1: use the centre of mass of the car's files when it has one; 0: the two numbers below */ \
    X(centreOfMassBoxFromFile, 1)       /* the file's centre of mass is fractions of a box from its centre (-0.5 = the very rear): 1: the chassis hull box of the car's files; 0: the whole body mesh's bounding box (it includes the roof spike) */ \
    X(centreOfMassLowering, 0.2)        /* the centre of mass of the car's files is moved down by this fraction of the chassis box height (0 = where the file says) */ \
    X(centreOfMassHeight, 0.40)       /* m above the road, when not taken from the car's files */ \
    X(centreOfMassFromRearAxle, 0.36)   /* between the axles when not taken from the files: 0 = on the rear axle, 1 = on the front axle */ \
    X(rollInertiaFactor, 1.5)           /* turns the roll (about the forward axis) inertia up or down */ \
    X(probeRadius, 0.3)                 /* m, radius of the spheres that make the chassis collide with walls and ceilings */ \
    /* --- world and time step */ \
    X(gravity, 9.81)                    /* m/s^2 */ \
    X(maxPhysicsStep, 0.006666667)      /* s: a frame is split into physics steps of at most this (1/150) */ \
    X(vehicleMaxFrameSeconds, 0.1)      /* s: the longest frame the vehicle simulates in one call */ \
    /* --- suspension: lengths from the upper mount down the car's up axis */ \
    X(suspRest, 0.15)                   /* wheel centre below the upper mount at rest */ \
    X(suspMin, 0)                    /* fully compressed (the bump stop) */ \
    X(suspMax, 0.30)                    /* fully extended (the wheel hangs in the air) */ \
    X(springFree, 0.33)                 /* free spring length (about 13 cm of preload at rest) */ \
    X(suspensionFromFile, 1)            /* 1: the spring stiffness and damping of the car's files when they say (the Eagle does); 0: the two below */ \
    X(wheelRadiusFromFile, 1)           /* 1: the wheel radii of the car's files (front and rear differ); 0: measured from the wheel meshes */ \
    X(springK, 30000)                   /* spring stiffness, N/m (when not taken from the car's files) */ \
    X(damper, 3300)                     /* damping, N s/m: resists the speed the wheel moves up or down at (when not taken from the files) */ \
    X(bumpStopZone, 0.03)               /* within this distance above suspMin the spring turns stiffer... */ \
    X(bumpStopStiffness, 4)             /* ...by this factor */ \
    /* --- tyres, engine and air */ \
    X(grip, 1.1)                      /* friction coefficient on the road: a tyre pushes with at most this times its load */ \
    X(slideGrip, 0.85)                  /* part of the grip left while a tyre slides sideways */ \
    X(engineTorqueScale, 0.3)           /* scales the engine's push (forward and reverse, not the afterburner): 1 = engineForce and enginePower as they are */ \
    X(engineForce, 40000)              /* N at the wheels at low speed (over three times the car's weight) */ \
    X(enginePower, 420000)              /* W: above the speed where force * speed reaches this, the force falls off as 1 / speed */ \
    X(brakeForce, 1)                    /* brake pedal fully down asks for this part of the friction limit */ \
    X(rollResist, 0.016)                /* rolling resistance, as a fraction of the tyre load */ \
    X(drag, 0.25)                       /* 0.5 * rho * Cd * A: air drag force is drag * speed^2, against the velocity */ \
    X(downforce, 0.9)                   /* N per (m/s)^2: presses the car onto the road, along its down axis */ \
    X(maxSpeed, 500)                    /* m/s (hard limit) */ \
    X(afterburnerForce, 36000)          /* N: about three times the car's weight, in the direction the car faces */ \
    X(kangarooSpeed, 8)                 /* m/s added upwards per jump (about 3 m high from rest) */ \
    X(rollCentre, 0.6)                 /* tyre forces act this far above the road: less body roll */ \
    /* --- steering */ \
    X(maxSteerAngle, 0.45)              /* radians of front wheel turn at a standstill (full lock) */ \
    X(steerFadeSpeed, 20)               /* m/s: around this speed the available steering angle has dropped to half... */ \
    X(steerFadeExponent, 1.6)           /* ...and this makes the drop steeper the faster the car goes */ \
    X(steerResponse, 9)                 /* 1/s: how quickly the wheels follow the steering input */ \
    /* --- driving details */ \
    X(minPedal, 0.01)                   /* a pedal below this counts as released */ \
    X(powerMinSpeed, 4)                 /* m/s: below this the engine force is not limited by its power */ \
    X(reverseForceFraction, 0.55)       /* reverse has this part of the engine force... */ \
    X(reversePowerFraction, 0.4)        /* ...and of the power */ \
    X(reversePowerMinSpeed, 3)          /* m/s, like powerMinSpeed for reverse */ \
    X(taperStartSpeed, 55)              /* m/s: from here the engine's push fades out... */ \
    X(taperLength, 15)                  /* ...completely over this many m/s more */ \
    X(handbrakeBrake, 0.85)             /* the handbrake brakes the rear wheels with this part of the friction limit */ \
    X(handbrakeGripLoss, 0.62)          /* and takes this part of their sideways grip away (the car slides) */ \
    X(rollResistWhileDriving, 0.3)      /* rolling resistance is scaled by this while the engine pushes */ \
    X(lateralStiffness, 0.85)           /* how much of the sideways speed a tyre tries to cancel in one step (below the limit) */ \
    /* --- rotation damping (1/s): the spin loses this part of its speed every second */ \
    X(airAngularDamping, 0.6)           /* no wheel touches the road */ \
    X(groundAngularDamping, 0.35)       /* at least one wheel does */ \
    /* --- drift measure */ \
    X(driftMinSpeed, 8)                 /* m/s: slower than this the car is never drifting */ \
    X(driftSlipDeadZone, 0.12)          /* radians of slip angle that is just cornering */ \
    X(driftSlipRange, 0.45)             /* radians more of slip angle for the measure to go from 0 to 1 */ \
    X(driftPointsFactor, 0.5)           /* score per (drift measure * m/s * second) */ \
    /* --- hull contacts (the chassis against the level) */ \
    X(floorNormalY, 0.7)                /* a contact whose normal points up at least this much counts as floor, else as a wall */ \
    X(wallBounce, 0.25)                 /* restitution against walls (floors do not bounce) */ \
    X(floorFriction, 0.9)               /* sliding friction on a floor contact, as a multiple of the normal impulse */ \
    X(wallFriction, 0.5)                /* and on a wall contact */ \
    X(hullPushOut, 0.7)                 /* how much of the penetration depth is corrected in one step... */ \
    X(hullMaxPushOut, 0.25)             /* ...but never more than this many metres */ \
    /* --- rules */ \
    X(roundSeconds, 90)                 /* s on the clock in Powerup Rush */ \
    X(pickupRadius, 3.4)                /* m: a powerup is collected this close to the car (horizontally)... */ \
    X(pickupReachY, 3.4)                /* ...and this close vertically */ \
    X(pointsPowerup, 50)                /* score of an ordinary powerup (also pedestrian ones) */ \
    X(pointsNotable, 150)               /* notable powerups and mission items */ \
    X(pointsTimeBonus, 50)              /* time bonus powerups, which also add timeBonusSeconds */ \
    X(timeBonusSeconds, 15) \
    X(pointsUpgrade, 500) \
    X(pointsCredit, 300) \
    X(pointsPickup, 100) \
    X(pointsStar, 250) \
    X(finishBonusPerSecond, 10)         /* score per second left when every powerup is collected */ \
    X(maxFrameSeconds, 0.05)            /* s: the longest frame the game simulates in one call */ \
    X(safeSpotSeconds, 0.7)             /* s between remembering a safe spot to return to after falling out of the world */ \
    X(stuckSeconds, 2.5)                /* s upside down and slow before the car is put back on its wheels */ \
    X(stuckMaxSpeed, 2)                 /* m/s: slower than this counts as slow for the above */ \
    X(worldFloorBelow, 60)              /* m below the level's lowest point: a car below it fell out of the world */ \
    /* --- camera */ \
    X(cameraDistance, 6.4)              /* m behind the car (chase camera)... */ \
    X(cameraHeight, 2.5)                /* ...and above it */ \
    X(cameraFarDistance, 11)            /* the same for the far camera (C) */ \
    X(cameraFarHeight, 5) \
    X(cameraSpeedDistance, 0.035)       /* m of extra distance per m/s of speed... */ \
    X(cameraSpeedCap, 40)               /* ...up to this speed */ \
    X(cameraYawFollow, 3.2)             /* 1/s: how quickly the camera turns behind the car */ \
    X(cameraPositionFollow, 7)          /* 1/s: how quickly it follows the wanted position */ \
    X(cameraLookAhead, 2.2)             /* m ahead of the car that it looks at */ \
    X(cameraLookHeight, 0.9)            /* m above the car that it looks at */ \
    X(cameraSlideMinSpeed, 5)           /* m/s: faster than this, the camera looks where the car goes when it slides... */ \
    X(cameraSlideBlend, 0.6)            /* ...by this part of the difference between heading and movement */ \
    X(cameraWallMargin, 0.4)            /* m kept between the camera and a wall behind the car */ \
    /* --- keyboard feel (1/s: how fast the digital keys move the analog value) */ \
    X(keySteerRise, 3.2)                /* steering towards a key that is held */ \
    X(keySteerReturn, 7)                /* steering back to the centre when no key is held */ \
    X(keySteerReverse, 9)               /* steering from one side to the other */ \
    X(keyAccelRise, 5) \
    X(keyAccelFall, 8) \
    X(keyBrakeRise, 7) \
    X(keyBrakeFall, 10) \
    X(gamepadDeadzone, 0.12)            /* the stick's dead zone, 0..1 */

struct Params {
#define X(name, def) float name = (float)(def);
    MINIGAME_PARAMS(X)
#undef X
};

// The parameters in effect (one set for the whole minigame).
Params& tuning();

// All parameters as a multi-line JSON object, one "name": value per line; `defaults`: the built-in values instead.
std::string paramsToJson(const Params& p);
// Reads a JSON object of "name": number entries. Entries that are left out get their default. False with a message
// (line number included) when the text is not valid or names an unknown parameter; `out` is untouched then.
bool paramsFromJson(const std::string& text, Params& out, std::string& error);

}  // namespace minigame01
