#pragma once

#include "bending/application/FlatCutProfile.h"
#include <cmath>
#include <numeric>

namespace bending::application {
struct FlatPatternInfo;
}
namespace bending::ui {
struct FlatDxfFrame {
    double cosine = 1, sine = 0;
    double x = 0, y = 0;

    [[nodiscard]] application::PointMm map(const application::PointMm &p) const {
        return {p.x * cosine + p.y * sine + x, p.y * cosine - p.x * sine + y, p.z};
    }
};
[[nodiscard]] FlatDxfFrame flatDxfFrame(const application::FlatPatternInfo &);

struct CutArc {
    application::PointMm centre;
    double radius;
    double sweep; // Signed radians in the DXF XY plane.
};

inline CutArc cutArc(const application::PointMm &start, const application::PointMm &end,
                     double bulge) {
    const auto dx = end.x - start.x, dy = end.y - start.y;
    const auto length = std::hypot(dx, dy);
    const auto offset = (1 / bulge - bulge) / 4;
    return {{std::midpoint(start.x, end.x) - dy * offset,
             std::midpoint(start.y, end.y) + dx * offset, 0},
            std::hypot(length / 2, length * offset),
            4 * std::atan(bulge)};
}
} // namespace bending::ui
