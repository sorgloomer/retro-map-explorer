// The minigame's car for Re-Volt: a car folder (cars/<name>/Parameters.txt, body and wheel .prm models, car bitmap).
#pragma once
#include "car_model.h"

namespace minigame01 {

bool revoltCanDrive();                                // the file set holds at least one car
bool loadRevoltCar(Scene& scene, CarModel& out);     // adds the car's texture to `scene`

}  // namespace minigame01
