#include "bending/core/ToolGeometry.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>
namespace bending::core {
namespace {
constexpr double pi = std::numbers::pi, tolerance = 1.e-5;
double radius(const DrawingEdge &e) {
    return std::hypot(e.start.x - e.center->x, e.start.y - e.center->y);
}
bool contains(const DrawingEdge &e, double angle) {
    const double start = std::atan2(e.start.y - e.center->y, e.start.x - e.center->x);
    double distance = std::fmod((angle - start) * (e.sweepRadians > 0 ? 1 : -1) + 4 * pi, 2 * pi);
    return distance <= std::abs(e.sweepRadians) + 1.e-8 || 2 * pi - distance < 1.e-8;
}
struct Bounds {
    double minY, maxY, area;
};
Bounds bounds(const ToolDrawing &d) {
    Bounds b{1.e100, -1.e100, 0};
    for (const auto &e : d.edges) {
        b.minY = std::min({b.minY, e.start.y, e.end.y});
        b.maxY = std::max({b.maxY, e.start.y, e.end.y});
        if (e.center) {
            const double r = radius(e);
            if (contains(e, -pi / 2))
                b.minY = std::min(b.minY, e.center->y - r);
            if (contains(e, pi / 2))
                b.maxY = std::max(b.maxY, e.center->y + r);
            b.area += e.center->x * (e.end.y - e.start.y) - e.center->y * (e.end.x - e.start.x) +
                      r * r * e.sweepRadians;
        } else
            b.area += e.start.x * e.end.y - e.end.x * e.start.y;
    }
    return b;
}
bool tangentNeighbours(const ToolDrawing &d, std::size_t i) {
    const auto &arc = d.edges[i];
    for (const auto j : {(i + d.edges.size() - 1) % d.edges.size(), (i + 1) % d.edges.size()}) {
        const auto &line = d.edges[j];
        if (line.center)
            return false;
        const auto p = j == (i + 1) % d.edges.size() ? arc.end : arc.start;
        const double dx = line.end.x - line.start.x, dy = line.end.y - line.start.y;
        if (std::abs(dx * (p.x - arc.center->x) + dy * (p.y - arc.center->y)) >
            tolerance * std::hypot(dx, dy))
            return false;
    }
    return true;
}
struct DieFeature {
    ToolGeometry geometry;
    double opening, angle;
};
std::vector<DieFeature> dieFeatures(const ToolDrawing &d) {
    const auto db = bounds(d);
    std::vector<std::size_t> lips;
    for (std::size_t i = 0; i < d.edges.size(); ++i) {
        const auto &e = d.edges[i];
        if (e.center && e.sweepRadians * db.area > 0 && contains(e, pi / 2) &&
            std::abs(e.center->y + radius(e) - db.maxY) <= tolerance &&
            std::abs(e.sweepRadians) >= pi / 6 - 1.e-6 &&
            std::abs(e.sweepRadians) <= 5 * pi / 12 + 1.e-6 && tangentNeighbours(d, i))
            lips.push_back(i);
    }
    std::vector<DieFeature> candidates;
    for (auto l : lips)
        for (auto r : lips) {
            const auto &left = d.edges[l], &right = d.edges[r];
            const double maxTilt = std::abs(left.sweepRadians);
            const double rad = radius(left), span = (right.center->x - left.center->x) / 2;
            if (std::abs(std::abs(right.sweepRadians) - maxTilt) > 1.e-6 || span <= tolerance ||
                std::abs(rad - radius(right)) > tolerance ||
                std::abs(left.center->y - right.center->y) > tolerance ||
                !contains(left, pi / 2 - maxTilt) || !contains(right, pi / 2 + maxTilt))
                continue;
            const double opening = 2 * (span - rad * std::tan(maxTilt / 2));
            if (opening < 1 || opening > 1000)
                continue;
            const auto width = [&](std::size_t i, bool isLeft) {
                double result = 0;
                const double centre = d.edges[i].center->x;
                for (const auto j :
                     {(i + d.edges.size() - 1) % d.edges.size(), (i + 1) % d.edges.size()}) {
                    const auto &e = d.edges[j];
                    if (!e.center && std::abs(e.start.y - db.maxY) < tolerance &&
                        std::abs(e.end.y - db.maxY) < tolerance)
                        result = std::max(result, isLeft ? centre - std::min(e.start.x, e.end.x)
                                                         : std::max(e.start.x, e.end.x) - centre);
                }
                return result;
            };
            const double lw = width(l, true), rw = width(r, false);
            if (lw <= tolerance || rw <= tolerance)
                continue;
            ToolGeometry candidate{};
            candidate.dieTopCentre = {(left.center->x + right.center->x) / 2, db.maxY};
            candidate.leftShoulderEdge = l;
            candidate.rightShoulderEdge = r;
            candidate.shoulderHalfSpanMm = span;
            candidate.shoulderRadiusMm = rad;
            candidate.leftSupportWidthMm = lw;
            candidate.rightSupportWidthMm = rw;
            candidate.hasDie = true;
            candidates.push_back({candidate, opening, pi - 2 * maxTilt});
        }
    return candidates;
}
} // namespace
std::optional<ToolGeometry> drawingToolGeometry(const ToolAssignment &a) {
    if (!validToolPreset(a.punch) || !validToolPreset(a.die) || !a.punch.parametersConfirmed ||
        !a.die.parametersConfirmed || a.punch.kind != ToolKind::Punch ||
        a.die.kind != ToolKind::Die || a.groove >= a.die.openingsMm.size())
        return {};
    ToolGeometry geometry{};
    if (a.punch.drawing) {
        const auto &p = *a.punch.drawing;
        if (!p.millimeters)
            return {};
        const auto pb = bounds(p);
        std::vector<std::size_t> noses;
        for (std::size_t i = 0; i < p.edges.size(); ++i) {
            const auto &e = p.edges[i];
            if (e.center && e.sweepRadians * pb.area > 0 && contains(e, -pi / 2) &&
                std::abs(radius(e) - *a.punch.noseRadiusMm) <= tolerance &&
                std::abs(e.center->y - radius(e) - pb.minY) <= tolerance &&
                std::abs(std::abs(e.sweepRadians) - (pi - a.punch.angleRadians)) < 1.e-6 &&
                tangentNeighbours(p, i))
                noses.push_back(i);
        }
        if (noses.size() != 1)
            return {};
        const auto &nose = p.edges[noses[0]];
        geometry.punchTip = {nose.center->x, nose.center->y - radius(nose)};
        geometry.noseEdge = noses[0];
        geometry.hasPunch = true;
    }
    if (!a.die.drawing)
        return geometry.hasPunch ? std::optional{geometry} : std::nullopt;
    const auto drawing = activeDieDrawing(a.die, a.groove);
    if (!drawing || !drawing->millimeters)
        return {};
    const auto settings = dieGroove(a.die, a.groove);
    auto candidates = dieFeatures(*drawing);
    std::erase_if(candidates, [&](const auto &candidate) {
        return std::abs(candidate.opening - a.die.openingsMm[a.groove]) > tolerance ||
               std::abs(candidate.angle - settings->angleRadians) > 1.e-6;
    });
    if (candidates.size() != 1)
        return {};
    auto result = candidates.front().geometry;
    result.punchTip = geometry.punchTip;
    result.noseEdge = geometry.noseEdge;
    result.hasPunch = geometry.hasPunch;
    return result;
}
std::vector<DetectedDieGroove> detectDieGrooves(const ToolDrawing &drawing) {
    std::vector<DetectedDieGroove> result;
    if (!drawing.millimeters || !validToolDrawing(drawing))
        return result;
    for (double rotation : {0., pi / 2, pi, -pi / 2}) {
        const auto candidates = dieFeatures(rotateToolDrawing(drawing, rotation));
        if (candidates.size() == 1)
            result.push_back({candidates.front().opening, {candidates.front().angle, rotation}});
    }
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.openingMm < b.openingMm; });
    return result;
}

} // namespace bending::core
