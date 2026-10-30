// The minigame's car for Re-Volt: a car folder (cars/<name>/Parameters.txt, body and wheel .prm models, car bitmap).
#pragma once
#include "minigame01/car_model.h"

namespace minigame01 {

// Re-Volt levels are made for radio controlled cars: a car of 0.43 m (an Adeon is 123 units long) is 286 units per real metre.
constexpr float kRevoltUnitsPerRealMetre = 286.0f;

bool revoltCanDrive();                                // the file set holds at least one car
bool loadRevoltCar(Scene& scene, CarModel& out);     // adds the car's texture to `scene`

}  // namespace minigame01
