#pragma once

#include "bending/application/Thickness.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::application {
struct BendAnalysis;
}
namespace bending::cad {
// Reads unpublished geometry only. Face indices belong to this exact import revision.
application::ThicknessAnalysis recognizeThickness(const TopoDS_Shape &shape, double toleranceMm,
                                                  std::stop_token stop,
                                                  const application::BendAnalysis *bends = nullptr);
} // namespace bending::cad
