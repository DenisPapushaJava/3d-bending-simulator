#include "CollisionPresentation.h"
#include <Aspect_PolygonOffsetMode.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <algorithm>
#include <cmath>
namespace bending::viewer {
CollisionPresentation::CollisionPresentation(
    const TopoDS_Shape &shape, const std::vector<cad::CollisionHighlightPatch> &patches,
    bool machineVisible, bool floorVisible, const std::vector<gp_Pnt> &floorContacts,
    const std::vector<gp_Pnt> &backgaugeContacts)
    : AIS_ColoredShape(shape) {
    if (machineVisible && !backgaugeContacts.empty()) {
        gaugeContacts_ = new Graphic3d_ArrayOfPoints(static_cast<int>(backgaugeContacts.size()));
        for (const auto &point : backgaugeContacts)
            gaugeContacts_->AddVertex(point);
    }
    if (!floorContacts.empty()) {
        floorContacts_ = new Graphic3d_ArrayOfPoints(static_cast<int>(floorContacts.size()));
        for (auto point : floorContacts) {
            point.SetY(point.Y() + .2); // Render-only depth offset, not a clearance allowance.
            floorContacts_->AddVertex(point);
        }
    }
    for (const auto &patch : patches) {
        if (patch.vertices.empty() || (patch.bodyId == "floor" && !floorVisible) ||
            (patch.bodyId != "floor" && !machineVisible && patch.bodyId != "sheet" &&
             patch.bodyId != "die" && patch.bodyId != "punch"))
            continue;
        Handle(Graphic3d_ArrayOfTriangles) triangles =
            new Graphic3d_ArrayOfTriangles(static_cast<int>(patch.vertices.size()), 0, false, true);
        for (const auto &v : patch.vertices) {
            const auto index = triangles->AddVertex(v.point);
            triangles->SetVertexColor(
                index, Graphic3d_Vec4ub(242, 15, 10,
                                        static_cast<unsigned char>(
                                            std::lround(std::clamp(v.strength, 0.f, 1.f) * 250))));
        }
        patches_.push_back(triangles);
    }
}
void CollisionPresentation::Compute(const Handle(PrsMgr_PresentationManager) & manager,
                                    const Handle(Prs3d_Presentation) & presentation,
                                    Standard_Integer mode) {
    AIS_ColoredShape::Compute(manager, presentation, mode);
    if (patches_.empty() && floorContacts_.IsNull() && gaugeContacts_.IsNull())
        return;
    Handle(Graphic3d_AspectFillArea3d) aspect = new Graphic3d_AspectFillArea3d;
    aspect->SetInteriorStyle(Aspect_IS_SOLID);
    aspect->SetInteriorColor(Quantity_Color(Quantity_NOC_WHITE));
    aspect->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    aspect->SetAlphaMode(Graphic3d_AlphaMode_Blend);
    aspect->SetFaceCulling(Graphic3d_TypeOfBackfacingModel_DoubleSided);
    // Keep depth testing against other objects; offset just avoids surface z-fighting.
    aspect->SetPolygonOffsets(Aspect_POM_Fill, -1.f, -1.f);
    auto group = presentation->NewGroup();
    group->SetGroupPrimitivesAspect(aspect);
    for (const auto &patch : patches_)
        group->AddPrimitiveArray(patch, false);
    if (!gaugeContacts_.IsNull()) {
        auto markers = presentation->NewGroup();
        markers->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(
            Aspect_TOM_O_POINT, Quantity_Color(.27, .74, .63, Quantity_TOC_RGB), 2.0));
        markers->AddPrimitiveArray(gaugeContacts_, false);
    }
    if (!floorContacts_.IsNull()) {
        auto markers = presentation->NewGroup();
        markers->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(
            Aspect_TOM_O_POINT, Quantity_Color(.95, .04, .03, Quantity_TOC_RGB), 1.2));
        markers->AddPrimitiveArray(floorContacts_, false);
    }
}
} // namespace bending::viewer
