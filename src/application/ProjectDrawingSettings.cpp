#include "bending/application/ProjectDrawingSettings.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace bending::application {
namespace {
bool validGroup(const std::vector<std::size_t> &ids) {
    return !ids.empty() && ids.size() <= 512 && ids.back() < 512 &&
           std::is_sorted(ids.begin(), ids.end()) &&
           std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}
} // namespace

bool validProjectDrawingSettings(const ProjectDrawingSettings &s) {
    if ((s.name && s.name->size() > 240) || s.designation.size() > 192 || s.material.size() > 240 ||
        s.execution.size() > 240 || s.quantity < 0 || s.quantity > 1000000 || s.quarterTurns < -1 ||
        s.quarterTurns > 3 ||
        (s.baseFace && (*s.baseFace == 0 || *s.baseFace > std::numeric_limits<int>::max())) ||
        s.flatSide < DrawingFlatSide::Automatic || s.flatSide > DrawingFlatSide::Opposite ||
        s.sectionEdits.size() > 128 || s.views.size() > 1024)
        return false;
    for (auto i = s.sectionEdits.begin(); i != s.sectionEdits.end(); ++i) {
        if (!validGroup(i->bends) ||
            (i->fraction &&
             (!std::isfinite(*i->fraction) || *i->fraction <= 0 || *i->fraction >= 1)) ||
            std::any_of(s.sectionEdits.begin(), i,
                        [&](const auto &e) { return e.bends == i->bends; }))
            return false;
    }
    for (auto i = s.views.begin(); i != s.views.end(); ++i) {
        const auto &id = i->identity;
        if (id.kind < DrawingViewKind::Projection || id.kind > DrawingViewKind::Isometric ||
            id.index >= 1024 || id.groups.size() > 128 ||
            !std::all_of(id.groups.begin(), id.groups.end(), validGroup) ||
            (i->scale && standardDrawingScale(*i->scale) != i->scale) ||
            std::any_of(s.views.begin(), i, [&](const auto &v) { return v.identity == id; }))
            return false;
    }
    return true;
}
} // namespace bending::application
