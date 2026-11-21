#pragma once

#include "bending/application/BendingDrawing.h"
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>

namespace bending::cad {
// Uses the immutable mesh already prepared during STEP import.
[[nodiscard]] std::optional<application::DrawingIsometricFill>
makeDrawingIsometricFill(const TopoDS_Shape &, const gp_Ax2 &, const application::DrawingView &,
                         std::stop_token);
} // namespace bending::cad
