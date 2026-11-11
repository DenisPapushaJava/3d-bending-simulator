#pragma once

#include "bending/application/Document.h"
#include <vector>

namespace bending::application {
struct FlatCutVertex {
    PointMm point;
    double bulge = 0; // tan(signed XY arc sweep / 4) to the next vertex.
};
struct FlatCutLoop {
    std::vector<FlatCutVertex> vertices;
    PointMm centre{};
    double radiusMm = 0; // Positive for a complete circle.
};
struct FlatCutProfile {
    std::vector<FlatCutLoop> loops; // Exterior first, then all through openings.
    std::size_t blindOpenings = 0;
    bool midThickness = false;
};
} // namespace bending::application
