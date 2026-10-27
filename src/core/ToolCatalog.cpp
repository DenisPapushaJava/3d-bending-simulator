#include "bending/core/ToolCatalog.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace bending::core {
namespace {
bool size(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
bool text(const std::string &value, std::size_t maximum, bool empty = false) {
    return (empty || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string::npos;
}
} // namespace
bool validToolDrawing(const ToolDrawing &d) {
    if (!text(d.sourceFile, 256) || d.sha256.size() != 64 || d.edges.size() < 2 ||
        d.edges.size() > 512 || !std::all_of(d.sha256.begin(), d.sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        return false;
    const auto point = [](DrawingPoint p) {
        return size(p.x, -1000000, 1000000) && size(p.y, -1000000, 1000000);
    };
    const auto distance = [](DrawingPoint a, DrawingPoint b) {
        return std::hypot(a.x - b.x, a.y - b.y);
    };
    for (std::size_t i = 0; i < d.edges.size(); ++i) {
        const auto &e = d.edges[i];
        if (!point(e.start) || !point(e.end) || distance(e.start, e.end) < 1e-8 ||
            distance(e.end, d.edges[(i + 1) % d.edges.size()].start) > 1e-5 ||
            !std::isfinite(e.sweepRadians))
            return false;
        if (e.center) {
            const double r = distance(e.start, *e.center);
            if (!point(*e.center) || !size(r, 1e-6, 1000000) ||
                !size(std::abs(e.sweepRadians), 1e-9, 2 * std::numbers::pi - 1e-9) ||
                std::abs(r - distance(e.end, *e.center)) > 1e-5)
                return false;
            const double angle =
                std::atan2(e.start.y - e.center->y, e.start.x - e.center->x) + e.sweepRadians;
            if (distance(e.end, {e.center->x + r * std::cos(angle),
                                 e.center->y + r * std::sin(angle)}) > 1e-5)
                return false;
        } else if (e.sweepRadians != 0)
            return false;
    }
    return true;
}
bool validToolPreset(const ToolPreset &t) {
    const double degrees = t.angleRadians * 180 / std::numbers::pi;
    if (!text(t.id, 128) || !text(t.manufacturer, 128) || !text(t.code, 128) ||
        !text(t.source, 2048) || !text(t.notes, 4096, true) ||
        !size(t.lengthMm, t.parametersConfirmed ? 1 : 0, 100000) ||
        (t.drawing && !validToolDrawing(*t.drawing)) ||
        (t.heightMm && !size(*t.heightMm, .1, 100000))) {
        return false;
    }
    if (!t.parametersConfirmed) {
        return (t.kind == ToolKind::Punch || t.kind == ToolKind::Die) &&
               size(degrees, 0, 120 + 1e-9) &&
               (!t.noseRadiusMm || size(*t.noseRadiusMm, .1, 250)) && t.openingsMm.size() <= 16 &&
               std::all_of(t.openingsMm.begin(), t.openingsMm.end(),
                           [](double v) { return size(v, 1, 1000); });
    }
    if (t.kind == ToolKind::Punch) {
        return size(degrees, 10 - 1e-9, 120 + 1e-9) && t.noseRadiusMm &&
               size(*t.noseRadiusMm, .1, 250) && t.openingsMm.empty();
    }
    if (t.kind != ToolKind::Die || !size(degrees, 30 - 1e-9, 120 + 1e-9) || t.noseRadiusMm ||
        t.openingsMm.empty() || t.openingsMm.size() > 16) {
        return false;
    }
    std::set<double> openings;
    return std::all_of(t.openingsMm.begin(), t.openingsMm.end(), [&](double opening) {
        return size(opening, 1, 1000) && openings.insert(opening).second;
    });
}
bool validToolCatalog(const ToolCatalog &catalog) {
    if (catalog.empty() || catalog.size() > 512) {
        return false;
    }
    std::set<std::string> ids;
    return std::all_of(catalog.begin(), catalog.end(), [&](const auto &tool) {
        return validToolPreset(tool) && ids.insert(tool.id).second;
    });
}
std::optional<PressTools> toolParameters(const ToolAssignment &a) {
    if (!validToolPreset(a.punch) || !validToolPreset(a.die) || !a.punch.parametersConfirmed ||
        !a.die.parametersConfirmed || a.punch.kind != ToolKind::Punch ||
        a.die.kind != ToolKind::Die || a.groove >= a.die.openingsMm.size() ||
        !text(a.machineName, 256, true)) {
        return {};
    }
    PressTools result{a.die.openingsMm[a.groove],
                      a.die.angleRadians,
                      a.punch.angleRadians,
                      *a.punch.noseRadiusMm,
                      a.die.lengthMm,
                      a.clearanceMm,
                      a.punch.lengthMm};
    return validPressTools(result) ? std::optional{result} : std::nullopt;
}
} // namespace bending::core
