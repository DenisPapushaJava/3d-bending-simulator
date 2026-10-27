#include "bending/application/BendingDrawing.h"
#include <algorithm>
#include <cmath>

namespace bending::application {
std::optional<double> drawingScale(const BendingDrawing &drawing) noexcept {
    double fit = 100;
    std::array<DrawingPoint, 3> available{{{134, 48}, {134, 48}, {104, 48}}};
    if (drawingProfilesCoverBends(drawing))
        available[0].x = 112; // Compact overview: keep room for dimensions at the left frame.
    for (std::size_t i = 0; i < available.size(); ++i) {
        const auto &v = drawing.views[i];
        double w = v.maximum.x - v.minimum.x;
        const double h = v.maximum.y - v.minimum.y;
        if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0 || v.paths.empty())
            return {};
        if (v.omittedX) {
            if (!std::isfinite((*v.omittedX)[0]) || !std::isfinite((*v.omittedX)[1]) ||
                (*v.omittedX)[0] <= v.minimum.x || (*v.omittedX)[1] >= v.maximum.x ||
                (*v.omittedX)[0] >= (*v.omittedX)[1])
                return {};
            w -= (*v.omittedX)[1] - (*v.omittedX)[0];
        }
        fit = std::min({fit, (available[i].x - (v.omittedX ? 8 : 0)) / w, available[i].y / h});
    }
    return standardDrawingScale(fit);
}
bool drawingProfilesCoverBends(const BendingDrawing &drawing) noexcept {
    if (drawing.bends.empty() || drawing.details.empty())
        return false;
    for (std::size_t i = 0; i < drawing.bends.size(); ++i)
        if (std::none_of(drawing.details.begin(), drawing.details.end(), [&](const auto &detail) {
                return std::any_of(detail.view.bendNotes.begin(), detail.view.bendNotes.end(),
                                   [i](const auto &note) { return note.second == i; });
            }))
            return false;
    return true;
}
std::optional<double> standardDrawingScale(double fit) noexcept {
    if (!std::isfinite(fit) || fit <= 0)
        return {};
    constexpr std::array scales{100., 50., 40.,     20., 10.,  5.,    4.,      2.5,    2.,
                                1.,   .5,  .4,      .25, .2,   .1,    1. / 15, .05,    .04,
                                .025, .02, 1. / 75, .01, .005, .0025, .002,    .00125, .001};
    for (const double scale : scales)
        if (scale <= fit + 1e-12)
            return scale;
    return {};
}
} // namespace bending::application
