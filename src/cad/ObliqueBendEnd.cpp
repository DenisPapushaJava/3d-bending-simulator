#include "ObliqueBendEnd.h"
#include "TrimmedBend.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <GeomProjLib.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw TrimmedBendError(message);
}
using PointFunction = std::function<gp_Pnt(double)>;
Handle(Geom_BSplineCurve)
    interpolate(const PointFunction &value, double first, double last, int count) {
    Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, count);
    Handle(TColStd_HArray1OfReal) parameters = new TColStd_HArray1OfReal(1, count);
    for (int i = 1; i <= count; ++i) {
        const double at = first + (last - first) * (i - 1) / (count - 1);
        const auto point = value(at);
        require(std::isfinite(point.X()) && std::isfinite(point.Y()) && std::isfinite(point.Z()),
                "Non-finite mapped bend end");
        points->SetValue(i, point);
        parameters->SetValue(i, at);
    }
    GeomAPI_Interpolate fit(points, parameters, false, 1e-10);
    fit.Perform();
    require(fit.IsDone(), "Cannot interpolate a mapped bend end");
    return fit.Curve();
}
bool accurate(const Handle(Geom_BSplineCurve) & curve, const PointFunction &value, double first,
              double last, int count, double tolerance) {
    for (int i = 0; i + 1 < count; ++i)
        for (double fraction : {.25, .5, .75}) {
            const double at = first + (last - first) * (i + fraction) / (count - 1);
            if (curve->Value(at).Distance(value(at)) > tolerance)
                return false;
        }
    return true;
}
TopoDS_Wire mappedWire(const TopoDS_Face &face, const TopTools_IndexedMapOfShape &edges,
                       std::vector<TopoDS_Edge> &mapped, const std::function<gp_Pnt(gp_Pnt)> &map,
                       double precision, std::stop_token stop) {
    int wires = 0;
    for (TopExp_Explorer ex(face, TopAbs_WIRE); ex.More(); ex.Next())
        require(++wires <= 1, "Interior contours in an oblique cap are unsupported");
    require(wires == 1, "Interior contours in an oblique cap are unsupported");
    BRepBuilderAPI_MakeWire wire;
    int countEdges = 0;
    for (BRepTools_WireExplorer ex(BRepTools::OuterWire(face), face); ex.More(); ex.Next()) {
        require(!stop.stop_requested(), "Oblique bend end cancelled");
        require(++countEdges <= 64, "Oblique bend end boundary limit reached");
        const auto edge = ex.Current();
        const auto index = static_cast<std::size_t>(edges.FindIndex(edge));
        require(index != 0, "Missing oblique bend edge");
        if (mapped[index].IsNull()) {
            const BRepAdaptor_Curve source(edge, face);
            const double first = source.FirstParameter(), last = source.LastParameter();
            const PointFunction value = [&](double at) { return map(source.Value(at)); };
            Handle(Geom_BSplineCurve) curve;
            bool complete = false;
            for (int count = 17; count <= 513; count = count * 2 - 1) {
                require(!stop.stop_requested(), "Oblique bend edge cancelled");
                curve = interpolate(value, first, last, count);
                if (accurate(curve, value, first, last, count, precision)) {
                    complete = true;
                    break;
                }
            }
            require(complete, "Oblique bend edge approximation did not converge");
            mapped[index] = BRepBuilderAPI_MakeEdge(curve).Edge();
        }
        auto transformed = mapped[index];
        if (edge.Orientation() == TopAbs_REVERSED)
            transformed.Reverse();
        wire.Add(transformed);
    }
    require(wire.IsDone(), "Cannot close a mapped oblique end");
    return wire.Wire();
}
} // namespace
TopoDS_Face mapObliqueBendEnd(const TopoDS_Face &face, const TopTools_IndexedMapOfShape &edges,
                              std::vector<TopoDS_Edge> &mapped,
                              const std::array<TopoDS_Face, 2> &skins,
                              const std::function<gp_Pnt(gp_Pnt)> &map, double tolerance,
                              std::stop_token stop) {
    const BRepAdaptor_Surface cap(face);
    require(cap.GetType() == GeomAbs_Plane, "A compound bend end must be planar");
    const auto plane = cap.Plane();
    const auto cylinder = BRepAdaptor_Surface(skins[0]).Cylinder();
    const double innerRadius = cylinder.Radius();
    const double outerRadius = BRepAdaptor_Surface(skins[1]).Cylinder().Radius();
    const auto axis = gp_Vec(cylinder.Axis().Direction());
    const auto normal = gp_Vec(plane.Axis().Direction());
    const double axialNormal = normal.Dot(axis);
    const double precision = std::min(1e-7, tolerance / 100);
    const auto boundary = mappedWire(face, edges, mapped, map, precision, stop);
    if (std::abs(axialNormal) <= 1e-8) {
        BRepBuilderAPI_MakeFace make(boundary, true);
        require(make.IsDone(), "A mapped axial cap is not planar");
        auto result = make.Face();
        GProp_GProps properties;
        BRepGProp::SurfaceProperties(face, properties);
        const auto center = properties.CentreOfMass();
        const double h = 1e-5;
        const auto du =
            gp_Vec(map(center), map(center.Translated(gp_Vec(plane.XAxis().Direction()) * h)));
        const auto dv =
            gp_Vec(map(center), map(center.Translated(gp_Vec(plane.YAxis().Direction()) * h)));
        auto desired = du.Crossed(dv);
        if (face.Orientation() == TopAbs_REVERSED)
            desired.Reverse();
        const auto actual = BRepAdaptor_Surface(result).Plane().Axis().Direction();
        result.Orientation(desired.Dot(gp_Vec(actual)) > 0 ? TopAbs_FORWARD : TopAbs_REVERSED);
        require(BRepCheck_Analyzer(result).IsValid(), "Invalid mapped axial cap");
        return result;
    }
    double u0, u1, v0, v1;
    BRepTools::UVBounds(skins[0], u0, u1, v0, v1);
    const auto sourcePoint = [&](double angle, double radius) {
        const auto radial = gp_Vec(cylinder.XAxis().Direction()) * std::cos(angle) +
                            gp_Vec(cylinder.YAxis().Direction()) * std::sin(angle);
        auto point = cylinder.Location().Translated(radial * radius);
        const double axial = -gp_Vec(plane.Location(), point).Dot(normal) / axialNormal;
        return point.Translated(axis * axial);
    };
    // A planar cut is linear through the radius. The bending map preserves
    // that ruling; fit only angular variation, with common parameters.
    std::array<Handle(Geom_BSplineCurve), 2> curves;
    bool fitted = false;
    for (int count = 17; count <= 513; count = count * 2 - 1) {
        require(!stop.stop_requested(), "Oblique bend end cancelled");
        fitted = true;
        for (int side = 0; side < 2; ++side) {
            const double radius = side == 0 ? innerRadius : outerRadius;
            const PointFunction value = [&](double angle) {
                return map(sourcePoint(angle, radius));
            };
            curves[side] = interpolate(value, u0, u1, count);
            fitted &= accurate(curves[side], value, u0, u1, count, precision);
        }
        if (fitted)
            break;
    }
    require(fitted, "Oblique bend end approximation did not converge");
    require(curves[0]->Degree() == curves[1]->Degree() &&
                curves[0]->NbPoles() == curves[1]->NbPoles() &&
                curves[0]->NbKnots() == curves[1]->NbKnots(),
            "Incompatible oblique end rulings");
    for (int i = 1; i <= curves[0]->NbKnots(); ++i)
        require(curves[0]->Knot(i) == curves[1]->Knot(i) &&
                    curves[0]->Multiplicity(i) == curves[1]->Multiplicity(i),
                "Incompatible oblique end parameters");
    TColgp_Array2OfPnt poles(1, curves[0]->NbPoles(), 1, 2);
    for (int i = 1; i <= curves[0]->NbPoles(); ++i)
        for (int side = 0; side < 2; ++side)
            poles.SetValue(i, side + 1, curves[side]->Pole(i));
    TColStd_Array1OfReal vKnots(1, 2);
    vKnots.SetValue(1, 0);
    vKnots.SetValue(2, 1);
    TColStd_Array1OfInteger vMults(1, 2);
    vMults.Init(2);
    Handle(Geom_BSplineSurface) target =
        new Geom_BSplineSurface(poles, curves[0]->Knots(), vKnots, curves[0]->Multiplicities(),
                                vMults, curves[0]->Degree(), 1);
    BRep_Builder builder;
    TopoDS_Face result;
    builder.MakeFace(result, target, TopLoc_Location(), precision);
    for (BRepTools_WireExplorer ex(boundary); ex.More(); ex.Next()) {
        require(!stop.stop_requested(), "Oblique bend end cancelled");
        auto transformed = ex.Current();
        double first, last;
        const auto curve = BRep_Tool::Curve(transformed, first, last);
        require(!curve.IsNull(), "Missing mapped oblique bend curve");
        double reached = precision;
        const auto pc = GeomProjLib::Curve2d(curve, first, last, target, reached);
        require(!pc.IsNull() && reached <= precision,
                "Cannot project a mapped oblique boundary onto its end surface");
        builder.UpdateEdge(transformed, pc, result, precision);
        builder.Range(transformed, result, first, last);
        builder.SameParameter(transformed, false);
        builder.SameRange(transformed, false);
        BRepLib::SameRange(transformed, precision);
        BRepLib::SameParameter(transformed, precision);
        require(BRep_Tool::Tolerance(transformed) <= tolerance,
                "Mapped oblique boundary exceeds the comparison tolerance");
    }
    const double middle = (u0 + u1) / 2, radius = (innerRadius + outerRadius) / 2;
    const auto sourceU =
        gp_Vec(sourcePoint(middle - 1e-5, radius), sourcePoint(middle + 1e-5, radius));
    const auto sourceR = gp_Vec(sourcePoint(middle, innerRadius), sourcePoint(middle, outerRadius));
    double orientation = sourceU.Crossed(sourceR).Dot(normal);
    result = BRepBuilderAPI_MakeFace(target, boundary, true).Face();
    if (face.Orientation() == TopAbs_REVERSED)
        orientation = -orientation;
    result.Orientation(orientation > 0 ? TopAbs_FORWARD : TopAbs_REVERSED);
    require(BRepCheck_Analyzer(result).IsValid(), "Invalid mapped oblique end face");
    return result;
}
} // namespace bending::cad
