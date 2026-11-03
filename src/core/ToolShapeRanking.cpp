#include "bending/core/ToolGeometry.h"
#include "bending/core/ToolSelection.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace bending::core {
namespace {
std::optional<std::vector<DrawingPoint>> polygon(const ToolDrawing &drawing, DrawingPoint origin,
                                                 bool reversed = false) {
    std::vector<DrawingPoint> result;
    for (const auto &e : drawing.edges) {
        const auto count =
            e.center ? std::max(1, static_cast<int>(
                                       std::ceil(std::abs(e.sweepRadians) * 36 / std::numbers::pi)))
                     : 1;
        if (result.size() + static_cast<std::size_t>(count) > 4096)
            return {};
        const double start =
            e.center ? std::atan2(e.start.y - e.center->y, e.start.x - e.center->x) : 0;
        const double radius =
            e.center ? std::hypot(e.start.x - e.center->x, e.start.y - e.center->y) : 0;
        for (int i = 0; i < count; ++i) {
            const double angle = start + e.sweepRadians * i / count;
            const DrawingPoint p = e.center ? DrawingPoint{e.center->x + radius * std::cos(angle),
                                                           e.center->y + radius * std::sin(angle)}
                                            : e.start;
            result.push_back({(p.x - origin.x) * (reversed ? -1 : 1), p.y - origin.y});
        }
    }
    return result;
}
bool inside(const std::vector<DrawingPoint> &poly, DrawingPoint p) {
    bool result = false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto a = poly[i], b = poly[(i + 1) % poly.size()];
        const double dx = b.x - a.x, dy = b.y - a.y;
        if (dx * dx + dy * dy < 1.e-18)
            continue;
        const double dot = (p.x - a.x) * dx + (p.y - a.y) * dy;
        if (dot >= 0 && dot <= dx * dx + dy * dy &&
            std::abs(dx * (p.y - a.y) - dy * (p.x - a.x)) <= 1.e-6 * std::hypot(dx, dy))
            return false; // Intended/tangential contact is not a body-interior sample.
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
            result = !result;
    }
    return result;
}
DrawingPoint mix(DrawingPoint a, DrawingPoint b, double f) {
    return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}
} // namespace
std::optional<double> toolShapeRisk(const ToolAssignment &a, std::span<const ToolFlangeHint> hints,
                                    double thickness, std::stop_token stop) {
    if (hints.empty() || hints.size() > 256 || !std::isfinite(thickness) || thickness <= 0 ||
        !a.useDrawingGeometry || !a.punch.drawing || !a.die.drawing ||
        a.punch.drawing->edges.size() > 2048 || a.die.drawing->edges.size() > 2048 ||
        stop.stop_requested())
        return {};
    if (!toolParameters(a))
        return {};
    const auto geometry = drawingToolGeometry(a);
    const auto die = activeDieDrawing(a.die, a.groove);
    if (!geometry || !geometry->hasPunch || !geometry->hasDie || !die)
        return {};
    const auto punchPoly = polygon(*a.punch.drawing, geometry->punchTip, a.punchReversed);
    const auto diePoly = polygon(*die, geometry->dieTopCentre);
    if (!punchPoly || !diePoly ||
        (punchPoly->size() + diePoly->size()) * hints.size() * 102 > 2000000)
        return {};
    std::map<std::pair<std::size_t, std::size_t>, double> pairs;
    for (const auto &hint : hints) {
        if (stop.stop_requested())
            return {};
        if (hint.bendIndex == hint.neighbourBendIndex ||
            !std::all_of(hint.section.begin(), hint.section.end(), [](auto p) {
                return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= 1.e5 &&
                       std::abs(p.y) <= 1.e5;
            }))
            return {};
        double area = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto p = hint.section[i], q = hint.section[(i + 1) % 4];
            area += p.x * q.y - q.x * p.y;
        }
        if (std::abs(area) < 1.e-8)
            return {};
        double best = 1;
        for (const double side : {-1., 1.}) {
            unsigned hits = 0;
            for (unsigned i = 0; i <= 16; ++i)
                for (unsigned j = 0; j <= 2; ++j) {
                    auto p = mix(mix(hint.section[0], hint.section[1], i / 16.),
                                 mix(hint.section[3], hint.section[2], i / 16.), j / 2.);
                    p.x *= side;
                    hits +=
                        inside(*punchPoly, p) || inside(*diePoly, {p.x, p.y + thickness}) ? 1u : 0u;
                }
            best = std::min(best, hits / 51.);
        }
        const auto key = std::minmax(hint.bendIndex, hint.neighbourBendIndex);
        const auto [found, inserted] = pairs.emplace(key, best);
        if (!inserted)
            found->second = std::min(found->second, best); // Either bend can be performed first.
    }
    double risk = 0;
    for (const auto &[key, value] : pairs) {
        (void)key;
        risk = std::max(risk, value);
    }
    return risk;
}
} // namespace bending::core
