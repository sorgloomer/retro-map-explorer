// Path follower helpers: options files (OPT_*), model alignment and spline smoothing.
#pragma once
#include <string>
#include <vector>
#include "../scene/scene.h"

namespace carma {

// OPT_ALIGNMENT_MATRIX44 of a path's options file: a 4x4 matrix (rows as written, translation in the last column).
bool parseAlignmentMatrix(const std::string& optionsText, float m[16]);

// OPT_SMOOTH_INDEX of a follower's options file, 0 when it has none.
int parseSmoothIndex(const std::string& optionsText);

// Applies the alignment to a follower's model (column vectors: p' = A p + t), then the mirror of the maps (Z, see kMirrorX).
void alignGeometry(BatchSet& g, const float m[16]);

// Uniform Catmull-Rom through the points, `n` samples per segment. Paths whose ends meet are treated as closed loops.
std::vector<Vec3> catmullRom(const std::vector<Vec3>& p, int n);

}  // namespace carma
