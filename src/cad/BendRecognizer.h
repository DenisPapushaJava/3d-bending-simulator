#pragma once

#include "bending/application/Document.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::cad {
enum class BendRecognitionPurpose { Simulation, DrawingMeasurements };
[[nodiscard]] application::BendAnalysis
recognizeBends(const TopoDS_Shape &shape, double maximumToleranceMm, std::stop_token stop,
               BendRecognitionPurpose purpose = BendRecognitionPurpose::Simulation);
} // namespace bending::cad
