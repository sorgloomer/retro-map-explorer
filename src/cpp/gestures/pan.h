// Panning in orbit mode: the surface the camera slides on while a pan gesture grabs a point.
#pragma once
#include "../scene/camera.h"

// Panning model. A pan gesture grabs the point P (the pivot). While the button is down the camera only translates, so its
// orientation is constant, and the camera position is computed backwards: the mouse position gives the direction d of
// the view ray that has to pass through P, and the camera sits where the line P - t d meets a "pan surface". The result
// is a function of the mouse position only (the surface is built once, when the gesture starts), so returning the cursor
// to a spot always returns the camera to the same place, whatever path the cursor took.
//
// The surface is a curve in the vertical plane that contains the camera's horizontal forward direction, extruded along
// the camera's right axis (so the horizontal screen coordinate of P simply follows the mouse). In that side view, with
// h = horizontal distance forward and y = height, and the start camera C at the origin:
//
//   top section     a horizontal half line y = Yh, running forward from T1 (horizontal panning, like sliding on the ground)
//   bottom section  a half line T2 + s (-u) running down and back, u = the camera's up vector: it lies in the plane
//                   perpendicular to the view direction (screen-parallel panning, uniform gain, changes the height)
//   arc             a circle of radius R, tangent to both lines at T1 and T2, centre O = T1 - (0, R) = T2 + R f
//
// T1 is where the ray from P at depression angle DX meets the top line, T2 where the ray at DM meets the bottom line:
// view rays steeper than DX pan horizontally, rays shallower than DM pan parallel to the screen, rays in between sweep
// the arc, so the transition is smooth. The start camera lies on the curve, which fixes its three free numbers
// (Yh, the offset c of the bottom line, R) together with the two ray conditions: on the top section when the pivot's own
// ray is steeper than DX (Yh = 0), on the bottom section when it is shallower than DM (c = 0), otherwise on the arc
// (|C - O| = R, a quadratic). Conditions for T1, T2 and for the arc are all linear in (O, R) apart from that last one.
struct PanSurface {
    bool valid = false;
    double phi = 0;            // camera pitch below the horizon
    double hx = 0, hz = 0;     // horizontal forward direction
    double fh = 0, fy = 0;     // view direction in the side plane: (cos phi, -sin phi)
    double hp = 0, yp = 0;     // pivot in the side plane
    double Yh = 0, c = 0;      // top line height, bottom line offset along f
    double Oh = 0, Oy = 0, R = 0;
};

// Builds the pan surface for the camera and the grabbed point as they are when the gesture starts (valid stays false when
// there is none, e.g. for a steep nearby surface: panning is then purely parallel to the screen).
PanSurface computePanSurface(const Camera& cam, const Vec3f& pivot, float pivotNormalY, float sceneRadius);

// Camera position that puts the grabbed point under the view ray `d`. `cam` is the camera of the gesture (its orientation
// does not change while panning) and `start` its position when the gesture began. The result depends on `d` only.
Vec3f panCameraPosition(const PanSurface& ps, const Camera& cam, const Vec3f& start, const Vec3f& pivot, const Vec3f& d);
