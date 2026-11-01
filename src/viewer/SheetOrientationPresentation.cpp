#include "SheetOrientationPresentation.h"
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <algorithm>

namespace bending::viewer {
Quantity_Color sheetSideColor(std::size_t side) {
    return side == 0 ? Quantity_Color(0.40, 0.54, 0.65, Quantity_TOC_sRGB)
                     : Quantity_Color(0.68, 0.46, 0.43, Quantity_TOC_sRGB);
}
void applySheetSideColors(const Handle(AIS_ColoredShape) & shape,
                          const cad::SheetOrientationVisual &orientation) {
    for (std::size_t side = 0; side < 2; ++side)
        for (const auto &face : orientation.sides[side])
            shape->SetCustomColor(face, sheetSideColor(side));
}
std::vector<Handle(AIS_InteractiveObject)> sheetEdgeMarker(const cad::SheetEdgeMarker &edge,
                                                           double textHeight) {
    const auto a = edge.points[0], b = edge.points[1];
    gp_Vec along(a, b);
    if (along.Magnitude() < 1e-6 || edge.normal.SquareMagnitude() < .5)
        return {};
    const auto forward = along.Normalized();
    const auto across = edge.normal.Crossed(forward).Normalized();
    const double size = std::min(along.Magnitude() * .22, 5.);
    BRep_Builder builder;
    TopoDS_Compound lines;
    builder.MakeCompound(lines);
    builder.Add(lines, BRepBuilderAPI_MakeEdge(a, b).Edge());
    for (double sign : {-1., 1.})
        builder.Add(lines, BRepBuilderAPI_MakeEdge(
                               b, b.Translated(-forward * size + across * (sign * size * .45)))
                               .Edge());
    const Quantity_Color color(.96, .88, .68, Quantity_TOC_sRGB);
    Handle(AIS_Shape) arrow = new AIS_Shape(lines);
    arrow->SetColor(color);
    arrow->SetWidth(3);
    arrow->SetInfiniteState(true);
    arrow->SetZLayer(Graphic3d_ZLayerId_Topmost);
    Handle(AIS_TextLabel) label = new AIS_TextLabel;
    label->SetPosition(a.Translated(across * size));
    label->SetText(TCollection_ExtendedString("1"));
    label->SetFont(Font_NOF_SANS_SERIF);
    label->SetColor(color);
    label->SetHeight(textHeight);
    label->SetZoomable(false);
    label->SetDisplayType(Aspect_TODT_SUBTITLE);
    label->SetColorSubTitle(Quantity_Color(.125, .165, .21, Quantity_TOC_sRGB));
    label->SetInfiniteState(true);
    label->SetZLayer(Graphic3d_ZLayerId_Topmost);
    return {arrow, label};
}
} // namespace bending::viewer
