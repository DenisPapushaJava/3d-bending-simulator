#include "DrawingSheetInternal.h"
#include <algorithm>

namespace bending::ui::drawing {
const application::DrawingView &LayoutContext::view(const application::BendingDrawing &drawing,
                                                    DrawingViewKey key) {
    const auto id = std::pair{key.kind, key.index};
    if (const auto it = views_.find(id); it != views_.end())
        return it->second;
    return views_.emplace(id, selectedView(drawing, key, choices)).first->second;
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
        return availability(drawing.details, DrawingViewKind::Detail, locator.designation) == false;
    });
    std::erase_if(view.labels, [&](const auto &label) {
        return availability(drawing.profiles, DrawingViewKind::Profile, label.second) == false;
    });
    return view;
}
} // namespace bending::ui::drawing
