#pragma once
#include "bending/application/BendingDrawing.h"
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>

namespace bending::cad {
void prepareClassicalDrawing(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                             const std::vector<application::RecognizedBend> &bends,
                             const std::array<gp_Ax2, 3> &frames,
                             application::BendingDrawing &drawing, double tolerance,
                             std::size_t &budget, std::stop_token stop,
                             const std::vector<application::DrawingSectionEdit> &edits = {});
} // namespace bending::cad
