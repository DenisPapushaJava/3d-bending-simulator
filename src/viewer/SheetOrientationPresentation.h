#pragma once
#include "bending/cad/occt/SheetOrientation.h"
#include <AIS_ColoredShape.hxx>
#include <AIS_InteractiveObject.hxx>
#include <Quantity_Color.hxx>
#include <vector>

namespace bending::viewer {
[[nodiscard]] Quantity_Color sheetSideColor(std::size_t side);
void applySheetSideColors(const Handle(AIS_ColoredShape) &, const cad::SheetOrientationVisual &);
[[nodiscard]] std::vector<Handle(AIS_InteractiveObject)>
sheetEdgeMarker(const cad::SheetEdgeMarker &, double textHeight);
} // namespace bending::viewer
