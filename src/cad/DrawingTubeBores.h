#pragma once

#include "DrawingOpenings.h"

#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>

namespace bending::cad {
struct DrawingTubeWall;
std::optional<DrawingTubeBore>
drawingTubeBore(const DrawingOpening &, const DrawingTubeWall &,
                const TopTools_IndexedMapOfShape &faces,
                const TopTools_IndexedDataMapOfShapeListOfShape &neighbors, std::stop_token);
} // namespace bending::cad
