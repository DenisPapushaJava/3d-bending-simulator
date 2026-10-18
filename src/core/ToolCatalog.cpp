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
bool validToolPreset(const ToolPreset &t) {
    const double degrees = t.angleRadians * 180 / std::numbers::pi;
    if (!text(t.id, 128) || !text(t.manufacturer, 128) || !text(t.code, 128) ||
        !text(t.source, 2048) || !text(t.notes, 4096, true) || !size(t.lengthMm, 1, 100000) ||
        (t.heightMm && !size(*t.heightMm, .1, 100000))) {
        return false;
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
    if (!validToolPreset(a.punch) || !validToolPreset(a.die) || a.punch.kind != ToolKind::Punch ||
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
