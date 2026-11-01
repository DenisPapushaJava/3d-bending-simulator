#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <AIS_ColoredShape.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
namespace bending::viewer {
// The same shape presentation plus local, unpickable surface decals. No CAD operations.
class CollisionPresentation final : public AIS_ColoredShape {
  public:
    CollisionPresentation(const TopoDS_Shape &, const std::vector<cad::CollisionHighlightPatch> &,
                          bool machineVisible, bool floorVisible,
                          const std::vector<gp_Pnt> &floorContacts);

  protected:
    void Compute(const Handle(PrsMgr_PresentationManager) &, const Handle(Prs3d_Presentation) &,
                 Standard_Integer) override;

  private:
    std::vector<Handle(Graphic3d_ArrayOfTriangles)> patches_;
    Handle(Graphic3d_ArrayOfPoints) floorContacts_;
};
} // namespace bending::viewer
