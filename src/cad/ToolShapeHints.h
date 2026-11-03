#pragma once
#include "bending/application/Document.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::cad {
// Optional source-only numerical projections. No Boolean, meshing or import failure
// for unsupported geometry; original face IDs and axial overlap are required.
std::vector<core::ToolFlangeHint>
toolShapeHints(const TopoDS_Shape &, const application::BendAnalysis &, std::stop_token);
} // namespace bending::cad
