#pragma once
#include "bending/application/Document.h"
#include "bending/cad/occt/SheetOrientation.h"
#include <gp_Trsf.hxx>
#include <optional>
#include <span>
#include <stop_token>

namespace bending::cad {
[[nodiscard]] SheetOrientationData
classifySheetOrientation(const TopoDS_Shape &, const application::DocumentMetadata &,
                         std::stop_token, std::optional<SheetReference> reference = {});
[[nodiscard]] SheetReference movedSheetReference(const SheetReference &, const gp_Trsf &);
[[nodiscard]] SheetOrientationVisual sheetOrientationVisual(const TopoDS_Shape &,
                                                            const SheetOrientationData &);
[[nodiscard]] std::vector<SheetSide>
matchSheetSides(const TopoDS_Shape &, const TopoDS_Shape &source, const SheetOrientationData &);
void appendSheetSides(SheetOrientationVisual &, const TopoDS_Shape &,
                      const std::vector<SheetSide> &);
[[nodiscard]] std::optional<std::size_t>
sheetReferenceRegion(const SheetReference &, std::span<const TopoDS_Shape>, double tolerance);
} // namespace bending::cad
