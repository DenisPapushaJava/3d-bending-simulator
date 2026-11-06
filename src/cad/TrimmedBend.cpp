#include "TrimmedBend.h"
#include "ObliqueBendEnd.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFill.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeCircle.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <TColStd_HArray1OfReal.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <gp_Vec2d.hxx>
#include <stdexcept>
#include <vector>

namespace bending::cad {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw TrimmedBendError(message);
}
// Both radial boundaries must use the angular coordinate as their parameter.
// Keeping arbitrary STEP spline parameters would twist the ruled end between them.
Handle(Geom2d_BSplineCurve) angularCurve(const Handle(Geom2d_Curve) & curve, double first,
                                         double last, double tolerance, bool &reversed) {
    const auto a = curve->Value(first), b = curve->Value(last);
    reversed = b.X() < a.X();
    const double lo = std::min(a.X(), b.X()), hi = std::max(a.X(), b.X());
    const auto value = [&](double u) {
        double left = first, right = last;
        for (int i = 0; i < 55; ++i) {
            const double mid = (left + right) / 2;
            if ((curve->Value(mid).X() < u) != reversed)
                left = mid;
            else
                right = mid;
        }
        return gp_Pnt2d(u, curve->Value((left + right) / 2).Y());
    };
    for (int count = 17; count <= 513; count = count * 2 - 1) {
        Handle(TColgp_HArray1OfPnt2d) points = new TColgp_HArray1OfPnt2d(1, count);
        Handle(TColStd_HArray1OfReal) parameters = new TColStd_HArray1OfReal(1, count);
        for (int i = 1; i <= count; ++i) {
            const double u = lo + (hi - lo) * (i - 1) / (count - 1);
            points->SetValue(i, value(u));
            parameters->SetValue(i, u);
        }
        Geom2dAPI_Interpolate interpolate(points, parameters, false, 1e-10);
        interpolate.Perform();
        require(interpolate.IsDone(), "Cannot parameterize a trimmed bend end");
        const auto result = interpolate.Curve();
        bool accurate = true;
        for (int i = 0; i + 1 < count && accurate; ++i)
            for (double fraction : {.25, .5, .75}) {
                const double u = lo + (hi - lo) * (i + fraction) / (count - 1);
                accurate &= result->Value(u).Distance(value(u)) <= tolerance / 100;
            }
        if (accurate)
            return result;
    }
    throw std::runtime_error("Trimmed end angular approximation did not converge");
}
std::array<TopoDS_Face, 2> cylinders(const TopoDS_Shape &solid) {
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) {
        const auto face = TopoDS::Face(e.Current());
        if (BRepAdaptor_Surface(face).GetType() == GeomAbs_Cylinder)
            faces.push_back(face);
    }
    require(faces.size() == 2, "A trimmed bend must have exactly two cylindrical surfaces");
    std::sort(faces.begin(), faces.end(), [](const auto &a, const auto &b) {
        return BRepAdaptor_Surface(a).Cylinder().Radius() <
               BRepAdaptor_Surface(b).Cylinder().Radius();
    });
    return {faces[0], faces[1]};
}
} // namespace
TopoDS_Shape mapTrimmedBend(const TopoDS_Shape &solid, const std::function<gp_Pnt(gp_Pnt)> &map,
                            double scale, double tolerance, TopoDS_Face *inner,
                            std::stop_token stop, bool *nonRadialEnd) {
    if (nonRadialEnd)
        *nonRadialEnd = false;
    const auto source = cylinders(solid);
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(solid, TopAbs_EDGE, edges);
    std::vector<TopoDS_Edge> mapped(static_cast<std::size_t>(edges.Extent()) + 1);
    std::vector<Handle(Geom2d_BSplineCurve)> angularEdges(mapped.size());
    BRepBuilderAPI_Sewing sewing(tolerance, true, true, true, false);
    for (const auto &face : source) {
        require(!stop.stop_requested(), "Trimmed bend cancelled");
        BRepAdaptor_Surface surface(face);
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const auto first = map(surface.Value(u0, 0));
        const auto middle = map(surface.Value((u0 + u1) / 2, 0));
        const auto last = map(surface.Value(u1, 0));
        const gp_Dir axis(gp_Vec(first, map(surface.Value(u0, 1))));
        Handle(Geom_Surface) target;
        double uScale = scale;
        if (scale == 0) {
            const gp_Dir across(gp_Vec(first, last));
            target =
                new Geom_Plane(gp_Ax3(first, gp_Dir(gp_Vec(across).Crossed(gp_Vec(axis))), across));
            uScale = first.Distance(last) / (u1 - u0);
        } else {
            const GC_MakeCircle circle(first, middle, last);
            require(circle.IsDone(), "Cannot map a trimmed cylindrical surface");
            const auto center = circle.Value()->Location();
            if (gp_Vec(center, first).Crossed(gp_Vec(center, middle)).Dot(gp_Vec(axis)) < 0)
                uScale = -scale;
            target = new Geom_CylindricalSurface(
                gp_Ax3(center, axis, gp_Dir(gp_Vec(center, first))), circle.Value()->Radius());
        }
        BRepBuilderAPI_MakeWire wire;
        for (BRepTools_WireExplorer e(BRepTools::OuterWire(face), face); e.More(); e.Next()) {
            const auto edge = e.Current();
            double firstParameter, lastParameter;
            const auto curve = BRep_Tool::CurveOnSurface(edge, face, firstParameter, lastParameter);
            require(!curve.IsNull(), "Missing trimmed bend boundary parameters");
            bool reversed = false;
            const bool angular =
                std::abs(curve->Value(lastParameter).X() - curve->Value(firstParameter).X()) > 1e-8;
            const auto pc =
                angular ? angularCurve(curve, firstParameter, lastParameter, tolerance, reversed)
                        : Handle(Geom2d_BSplineCurve)::DownCast(
                              Geom2dConvert::CurveToBSplineCurve(
                                  new Geom2d_TrimmedCurve(curve, firstParameter, lastParameter))
                                  ->Copy());
            if (angular)
                angularEdges[static_cast<std::size_t>(edges.FindIndex(edge))] =
                    Handle(Geom2d_BSplineCurve)::DownCast(pc->Copy());
            for (int i = 1; i <= pc->NbPoles(); ++i) {
                auto pole = pc->Pole(i);
                pole.SetX((pole.X() - u0) * uScale);
                pc->SetPole(i, pole);
            }
            // Preserve analytic rails/circular end arcs. Turning these into 3D splines
            // needlessly makes every subsequent occupied-volume intersection harder.
            const auto start = pc->Value(pc->FirstParameter());
            const auto end = pc->Value(pc->LastParameter());
            const gp_Vec2d chord(start, end);
            bool linear = chord.Magnitude() > 1e-10;
            for (int i = 1; i <= pc->NbPoles() && linear; ++i)
                linear =
                    std::abs(chord.Crossed(gp_Vec2d(start, pc->Pole(i)))) / chord.Magnitude() <=
                    std::min(1e-8, tolerance / 100);
            Handle(Geom2d_Curve) boundary(pc.get());
            if (linear)
                boundary = new Geom2d_TrimmedCurve(new Geom2d_Line(start, gp_Dir2d(chord)), 0,
                                                   chord.Magnitude());
            auto result = BRepBuilderAPI_MakeEdge(boundary, target).Edge();
            require(BRepLib::BuildCurves3d(result, std::min(1e-7, tolerance / 100)),
                    "Cannot build a mapped trimmed bend edge");
            if (reversed)
                result.Reverse();
            mapped[static_cast<std::size_t>(edges.FindIndex(edge))] = result;
            if (edge.Orientation() == TopAbs_REVERSED)
                result.Reverse();
            wire.Add(result);
        }
        require(wire.IsDone(), "Cannot close a mapped trimmed bend boundary");
        auto result = BRepBuilderAPI_MakeFace(target, wire.Wire(), true).Face();
        result.Orientation(face.Orientation());
        if (inner && face.IsSame(source[0]))
            *inner = result;
        sewing.Add(result);
    }
    for (TopExp_Explorer f(solid, TopAbs_FACE); f.More(); f.Next()) {
        require(!stop.stop_requested(), "Trimmed bend cancelled");
        if (f.Current().IsSame(source[0]) || f.Current().IsSame(source[1]))
            continue;
        std::vector<int> boundary;
        for (TopExp_Explorer e(f.Current(), TopAbs_EDGE); e.More(); e.Next()) {
            const int i = edges.FindIndex(e.Current());
            if (!mapped[static_cast<std::size_t>(i)].IsNull())
                boundary.push_back(i);
        }
        if (boundary.size() != 2) {
            if (nonRadialEnd)
                *nonRadialEnd = true;
            sewing.Add(mapObliqueBendEnd(TopoDS::Face(f.Current()), edges, mapped, source, map,
                                         tolerance, stop));
            continue;
        }
        const auto &firstCurve = angularEdges[static_cast<std::size_t>(boundary[0])];
        const auto &secondCurve = angularEdges[static_cast<std::size_t>(boundary[1])];
        if (!firstCurve.IsNull() && !secondCurve.IsNull()) {
            if (std::abs(firstCurve->FirstParameter() - secondCurve->FirstParameter()) >= 1e-5 ||
                std::abs(firstCurve->LastParameter() - secondCurve->LastParameter()) >= 1e-5) {
                if (nonRadialEnd)
                    *nonRadialEnd = true;
                sewing.Add(mapObliqueBendEnd(TopoDS::Face(f.Current()), edges, mapped, source, map,
                                             tolerance, stop));
                continue;
            }
            for (int sample = 0; sample <= 32; ++sample) {
                const double u =
                    firstCurve->FirstParameter() +
                    (firstCurve->LastParameter() - firstCurve->FirstParameter()) * sample / 32;
                if (nonRadialEnd &&
                    std::abs(firstCurve->Value(u).Y() - secondCurve->Value(u).Y()) > tolerance / 10)
                    *nonRadialEnd = true;
            }
        }
        auto a = mapped[static_cast<std::size_t>(boundary[0])];
        auto b = mapped[static_cast<std::size_t>(boundary[1])];
        const auto start = [](const TopoDS_Edge &e) {
            return BRep_Tool::Pnt(TopExp::FirstVertex(e, true));
        };
        const auto end = [](const TopoDS_Edge &e) {
            return BRep_Tool::Pnt(TopExp::LastVertex(e, true));
        };
        if (start(a).Distance(start(b)) + end(a).Distance(end(b)) >
            start(a).Distance(end(b)) + end(a).Distance(start(b)))
            b.Reverse();
        sewing.Add(BRepFill::Face(a, b));
    }
    sewing.Perform();
    const auto shell = sewing.SewedShape();
    require(shell.ShapeType() == TopAbs_SHELL && shell.Closed(),
            "Mapped trimmed bend is not one closed shell");
    auto result = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shell)).Solid();
    require(BRepLib::OrientClosedSolid(result) && BRepCheck_Analyzer(result).IsValid(),
            "Invalid mapped trimmed bend solid");
    return result;
}
} // namespace bending::cad
