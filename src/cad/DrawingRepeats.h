#pragma once
#include "bending/application/BendingDrawing.h"
#include <TopoDS_Shape.hxx>

namespace bending::cad {
struct RepeatedDrawingSection {
    application::DrawingView overview;
    application::DrawingView unit;
    std::size_t count;
};
[[nodiscard]] std::optional<RepeatedDrawingSection>
repeatedDrawingSection(const TopoDS_Shape &flat, const application::DrawingView &actual,
                       const application::DrawingProfile &profile,
                       const std::vector<application::RecognizedBend> &bends, double tolerance,
                       std::stop_token stop);
void compactRepeatedFlat(application::DrawingFlatPattern &flat, double tolerance);
} // namespace bending::cad
