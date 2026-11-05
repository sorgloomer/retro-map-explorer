// Where a moving object of a scene is at a time: the animated props (behemoths, lifts: looping translate / rotate steps) and the path followers.
// The renderer draws them there, and the minigame's physics puts their (kinematic) bodies there. False for the movers that do not move that way.
#pragma once
#include "scene/scene.h"

bool moverPoseAt(const Mover& m, float time, Mat4f& out);
