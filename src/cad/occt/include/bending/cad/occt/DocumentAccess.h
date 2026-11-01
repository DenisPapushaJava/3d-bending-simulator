#pragma once

#include "bending/cad/CadDocument.h"
#include "bending/cad/occt/SheetOrientation.h"
#include <TopoDS_Shape.hxx>
#include <optional>
#include <stop_token>

namespace bending::cad {
// Native bridge for CAD algorithms/presentation. Borrowed shapes remain immutable.
// Derived route states are prepared privately in the CAD worker, never in the viewer.
class OcctDocumentAccess final {
  public:
    [[nodiscard]] static const TopoDS_Shape &shape(const CadDocument &document);
    [[nodiscard]] static const TopoDS_Shape &simulationShape(const CadDocument &document);
    [[nodiscard]] static const application::BendAnalysis &
    simulationBends(const CadDocument &document);
    [[nodiscard]] static const TopoDS_Shape &routeSkin(const CadDocument &);
    [[nodiscard]] static bool isRouteState(const CadDocument &);
    [[nodiscard]] static const SheetOrientationData &sheetOrientation(const CadDocument &);
    [[nodiscard]] static const std::vector<SheetSide> &routeSkinSides(const CadDocument &);
    // Internal derived state, never published as the imported/session document.
    [[nodiscard]] static std::shared_ptr<const CadDocument>
    routeState(const CadDocument &, TopoDS_Shape, TopoDS_Shape, std::stop_token,
               std::optional<SheetReference> = {}, std::vector<SheetSide> = {});
};
} // namespace bending::cad
