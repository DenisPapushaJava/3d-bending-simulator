#pragma once

#include "bending/application/Document.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::cad {
[[nodiscard]] application::BendAnalysis
recognizeBends(const TopoDS_Shape &shape, double maximumToleranceMm, std::stop_token stop);
}
