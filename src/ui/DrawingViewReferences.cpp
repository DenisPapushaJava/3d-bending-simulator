#include "DrawingSheetInternal.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace bending::ui::drawing {
const application::DrawingView &LayoutContext::view(const application::BendingDrawing &drawing,
                                                    DrawingViewKey key) {
    const auto id = std::pair{key.kind, key.index};
    if (const auto it = views_.find(id); it != views_.end())
        return it->second;
    auto view = key.kind == DrawingViewKind::Flat ? preparedView(drawing, key)
                                                  : selectedView(drawing, key, choices);
    if (application::supportsPaperViewRotation(key.kind)) {
        int turns = 0;
        if (choices)
            if (const auto found =
                    std::find_if(choices->begin(), choices->end(),
                                 [&](const auto &choice) { return choice.key == key; });
                found != choices->end())
                turns = found->paperQuarterTurns;
        const bool mirror =
            key.kind == DrawingViewKind::Flat &&
            DrawingSheet::flatOrientation(drawing.flatPattern->info, flatSide).reversed;
        orientPaperView(view, mirror, turns, this);
    }
    return views_.emplace(id, std::move(view)).first->second;
}
application::DrawingView selectedView(const application::BendingDrawing &drawing,
                                      DrawingViewKey key,
                                      const std::vector<DrawingViewChoice> *choices) {
    auto view = preparedView(drawing, key);
    if (!choices)
        return view;
    const auto visible = [&](DrawingViewKind kind, std::size_t index) {
        return std::any_of(choices->begin(), choices->end(), [&](const auto &choice) {
            return choice.visible && choice.key == DrawingViewKey{kind, index};
        });
    };
    if (key.kind == DrawingViewKind::Auxiliary && !view.opposedTubeBores.empty()) {
        std::set<std::size_t> alreadyShown;
        const auto addPairs = [&](const application::DrawingView &candidate) {
            for (const auto &bore : candidate.opposedTubeBores)
                alreadyShown.insert(bore.pair);
        };
        for (std::size_t i = 0; i < 3; ++i)
            if (visible(DrawingViewKind::Projection, i))
                addPairs(drawing.classicalViews ? drawing.classicalViews->at(i) : drawing.views[i]);
        for (std::size_t i = 0; i < key.index; ++i)
            if (visible(DrawingViewKind::Auxiliary, i))
                addPairs(drawing.auxiliaryViews[i].view);
        std::vector<bool> covered(view.holeDiameters.size());
        for (auto &bore : view.opposedTubeBores) {
            bore.coveredOnAnotherView = alreadyShown.contains(bore.pair);
            covered[bore.diameterIndex] = bore.coveredOnAnotherView;
        }
        if (std::any_of(covered.begin(), covered.end(), [](bool found) { return found; }))
            std::erase_if(view.holeDimensions, [&](const auto &dimension) {
                if (dimension.pitchIntervals)
                    return false;
                const bool horizontal = std::abs(dimension.end.x - dimension.start.x) >
                                        std::abs(dimension.end.y - dimension.start.y);
                const auto sharesCoordinate = [&](application::DrawingPoint at) {
                    return std::abs(horizontal ? at.x - dimension.end.x : at.y - dimension.end.y) <
                           1e-5;
                };
                bool onlyCovered = false;
                for (std::size_t i = 0; i < view.holeDiameters.size(); ++i) {
                    if (!sharesCoordinate(view.holeDiameters[i].first))
                        continue;
                    if (!covered[i])
                        return false;
                    onlyCovered = true;
                }
                return onlyCovered && std::none_of(view.openingAxes.begin(), view.openingAxes.end(),
                                                   [&](const auto &axes) {
                                                       return sharesCoordinate(axes.center);
                                                   });
            });
    }
    // Match actual destinations, never a guessed letter/prefix. Unrelated notes
    // keep their meaning; omitted choices have the same scope as unchecked ones.
    const auto availability = [&](const auto &destinations, DrawingViewKind kind,
                                  const std::string &designation) -> std::optional<bool> {
        bool known = false;
        for (std::size_t i = 0; i < destinations.size(); ++i)
            if (destinations[i].designation == designation) {
                known = true;
                if (visible(kind, i))
                    return true;
            }
        return known ? std::optional{false} : std::nullopt;
    };
    std::erase_if(view.sectionMarkers, [&](const auto &marker) {
        return availability(drawing.sections, DrawingViewKind::Section, marker.designation) ==
               false;
    });
    std::erase_if(view.viewArrows, [&](const auto &arrow) {
        return availability(drawing.auxiliaryViews, DrawingViewKind::Auxiliary,
                            arrow.designation) == false;
    });
    std::erase_if(view.detailLocators, [&](const auto &locator) {
        if (availability(drawing.details, DrawingViewKind::Detail, locator.designation) == false)
            return true;
        const auto end = std::find_if(
            drawing.auxiliaryViews.begin(), drawing.auxiliaryViews.end(), [&](const auto &view) {
                return view.tubeEnd && view.designation == locator.designation;
            });
        return end != drawing.auxiliaryViews.end() &&
               !visible(DrawingViewKind::Auxiliary,
                        static_cast<std::size_t>(end - drawing.auxiliaryViews.begin()));
    });
    std::erase_if(view.labels, [&](const auto &label) {
        return availability(drawing.profiles, DrawingViewKind::Profile, label.second) == false;
    });
    return view;
}
} // namespace bending::ui::drawing
