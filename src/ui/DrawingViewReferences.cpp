#include "DrawingSheetInternal.h"
#include <algorithm>
#include <cmath>

namespace bending::ui::drawing {
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
    std::vector<application::DrawingPoint> removed;
    std::erase_if(view.labels, [&](const auto &label) {
        const auto detail = availability(drawing.details, DrawingViewKind::Detail, label.second);
        const auto profile = availability(drawing.profiles, DrawingViewKind::Profile, label.second);
        if ((!detail && !profile) || detail.value_or(false) || profile.value_or(false))
            return false;
        removed.push_back(label.first);
        return true;
    });
    const auto sameAnchor = [](auto a, auto b) {
        // The same association is used when classicalSource restores locators.
        return std::hypot(a.x - b.x, a.y - b.y) < 1.e-5;
    };
    std::erase_if(view.detailCircles, [&](const auto &circle) {
        const bool disabled = std::any_of(removed.begin(), removed.end(), [&](auto anchor) {
            return sameAnchor(anchor, circle.first);
        });
        return disabled &&
               std::none_of(view.labels.begin(), view.labels.end(), [&](const auto &label) {
                   return sameAnchor(label.first, circle.first);
               });
    });
    return view;
}
} // namespace bending::ui::drawing
