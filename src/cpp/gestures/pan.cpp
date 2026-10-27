#include "gestures/pan.h"
#include <algorithm>
#include <cmath>

namespace {
// Screen-only panning of walls: the grabbed surface must be steeper than kWallPanMinSlope (degrees from horizontal) and the
// pivot closer to the camera than kWallPanMaxDistance (as a fraction of the scene radius).
const double kWallPanMinSlope = 60.0, kWallPanMaxDistance = 0.25;
const double kPanDM = 0.0, kPanDX = 0.04;
}  // namespace

// Builds the pan surface for the camera and pivot as they are now (ps.valid stays false when no surface exists).
PanSurface computePanSurface(const Camera& cam, const Vec3f& pivot, float pivotNormalY, float sceneRadius) {
    PanSurface ps;
    // Grabbing a steep surface (a wall, a tower) close to the camera pans purely parallel to the screen: no pan surface,
    // panTo then uses the screen-parallel solution.
    const double steepNormalY = std::cos(kWallPanMinSlope * 3.14159265358979 / 180.0);  // |n.y| of a wall at that slope
    if (pivotNormalY < steepNormalY && length(pivot - cam.pos) < kWallPanMaxDistance * sceneRadius) return ps;
    const double DM = std::asin(kPanDM), DX = std::asin(kPanDX);
    ps.phi = -cam.pitch;
    ps.hx = std::sin(cam.yaw);
    ps.hz = std::cos(cam.yaw);
    ps.fh = std::cos(ps.phi);
    ps.fy = -std::sin(ps.phi);
    Vec3f rel = pivot - cam.pos;
    ps.hp = rel.x * ps.hx + rel.z * ps.hz;
    ps.yp = rel.y;
    const double d0 = std::atan2(-ps.yp, ps.hp);  // depression of the pivot's own ray
    // E1: T1 = (Oh, Oy + R) on the ray from P with depression DX;  E2: T2 = (Oh - R cos phi, Oy + R sin phi) on the ray DM
    const double sM = std::sin(DX), cM = std::cos(DX), sm = std::sin(DM), cm = std::cos(DM);
    double A[3][3] = {{sM, cM, cM}, {sm, cm, -std::cos(ps.phi) * sm + std::sin(ps.phi) * cm}, {0, 0, 0}};
    double rhs[3] = {sM * ps.hp + cM * ps.yp, sm * ps.hp + cm * ps.yp, 0};
    auto det3 = [](double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    auto solve3 = [&](double m[3][3], double b[3], double x[3]) {
        double D = det3(m);
        if (std::fabs(D) < 1e-12) return false;
        for (int k = 0; k < 3; k++) {
            double t[3][3];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) t[i][j] = j == k ? b[i] : m[i][j];
            x[k] = det3(t) / D;
        }
        return true;
    };
    double x[3];
    if (d0 >= DX || d0 <= DM) {
        // third condition: Yh = Oy + R = 0 (camera on the top line) or c = O.f - R = 0 (camera on the bottom line)
        if (d0 >= DX) { A[2][0] = 0; A[2][1] = 1; A[2][2] = 1; }
        else { A[2][0] = ps.fh; A[2][1] = ps.fy; A[2][2] = -1; }
        if (!solve3(A, rhs, x) || x[2] <= 0) return ps;
    } else {
        // camera on the arc: (O, R) = (O0 + O1 R), |O|^2 = R^2
        double D = A[0][0] * A[1][1] - A[0][1] * A[1][0];
        if (std::fabs(D) < 1e-12) return ps;
        double b1 = rhs[0], b2 = rhs[1];
        double O0h = (b1 * A[1][1] - A[0][1] * b2) / D, O0y = (A[0][0] * b2 - A[1][0] * b1) / D;
        double O1h = (-A[0][2] * A[1][1] + A[0][1] * A[1][2]) / D, O1y = (-A[0][0] * A[1][2] + A[1][0] * A[0][2]) / D;
        double qa = O1h * O1h + O1y * O1y - 1.0, qb = 2 * (O0h * O1h + O0y * O1y), qc = O0h * O0h + O0y * O0y;
        double roots[2];
        int n = 0;
        if (std::fabs(qa) < 1e-12) { if (std::fabs(qb) > 1e-12) roots[n++] = -qc / qb; }
        else {
            double disc = qb * qb - 4 * qa * qc;
            if (disc >= 0) { double sq = std::sqrt(disc); roots[n++] = (-qb + sq) / (2 * qa); roots[n++] = (-qb - sq) / (2 * qa); }
        }
        bool ok = false;
        for (int i = 0; i < n && !ok; i++) {
            double R = roots[i];
            if (R <= 0) continue;
            double Oh = O0h + O1h * R, Oy = O0y + O1y * R;
            double ang = std::atan2(-Oy, -Oh);  // from the centre towards the start camera; must lie on the arc
            if (ang < 0) ang += 2 * 3.14159265358979;
            if (ang >= 1.5707963 - 1e-6 && ang <= 3.14159265358979 - ps.phi + 1e-6) { x[0] = Oh; x[1] = Oy; x[2] = R; ok = true; }
        }
        if (!ok) return ps;
    }
    ps.Oh = x[0]; ps.Oy = x[1]; ps.R = x[2];
    ps.Yh = ps.Oy + ps.R;
    ps.c = ps.Oh * ps.fh + ps.Oy * ps.fy - ps.R;
    ps.valid = true;
    return ps;
}

Vec3f panCameraPosition(const PanSurface& ps, const Camera& cam, const Vec3f& start, const Vec3f& pivot, const Vec3f& d) {
    const Vec3f S = start, P = pivot;
    const double DM = std::asin(kPanDM), DX = std::asin(kPanDX);
    // the ray in the side plane: d_h forward, d_y up; from P back towards the camera the direction is v = -d
    const double dh = d.x * ps.hx + d.z * ps.hz, dy = d.y;
    const double vh = -dh, vy = -dy;
    const double delta = std::atan2(-dy, dh);  // depression of the ray
    bool have = false;
    double t = 0;
    auto hitTop = [&]() { if (vy > 1e-9) { t = (ps.Yh - ps.yp) / vy; have = true; } };
    auto hitBottom = [&]() {
        double den = vh * ps.fh + vy * ps.fy;
        if (std::fabs(den) > 1e-9) { t = (ps.c - (ps.hp * ps.fh + ps.yp * ps.fy)) / den; have = true; }
    };
    if (ps.valid) {
        if (delta >= DX) hitTop();
        else if (delta <= DM) hitBottom();
        else {
            double wh = ps.hp - ps.Oh, wy = ps.yp - ps.Oy;
            double qa = vh * vh + vy * vy, qb = 2 * (wh * vh + wy * vy), qc = wh * wh + wy * wy - ps.R * ps.R;
            double disc = qb * qb - 4 * qa * qc;
            if (disc >= 0 && qa > 1e-12) {
                double sq = std::sqrt(disc);
                double best = -1;
                for (double tt : {(-qb + sq) / (2 * qa), (-qb - sq) / (2 * qa)}) {
                    double xh = ps.hp + tt * vh - ps.Oh, xy = ps.yp + tt * vy - ps.Oy;
                    double ang = std::atan2(xy, xh);
                    if (ang < 0) ang += 2 * 3.14159265358979;
                    if (tt > 0 && ang >= 1.5707963 - 1e-6 && ang <= 3.14159265358979 - ps.phi + 1e-6) best = std::max(best, tt);
                }
                if (best > 0) { t = best; have = true; }
            }
            if (!have) { if (delta > 0.5 * (DM + DX)) hitTop(); else hitBottom(); }  // no clean intersection: nearer end
        }
    }
    if (!have) {  // no usable surface: slide parallel to the screen
        Vec3f f = cam.forward();
        double den = dot(d, f);
        if (den < 1e-4) return cam.pos;
        t = dot(P - S, f) / den;
    }
    if (t <= 0) return cam.pos;
    return P - d * (float)t;
}
