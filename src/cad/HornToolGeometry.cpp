#include "HornToolGeometry.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bending::cad {
namespace {
void check(bool ok, std::stop_token stop, const char *message) {
    if (stop.stop_requested())
        throw std::runtime_error("Horn geometry cancelled");
    if (!ok)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
Bnd_Box bounds(const TopoDS_Shape &s) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(s, box, false, false);
    if (box.IsVoid() || box.IsOpen())
        throw std::runtime_error("Invalid horn geometry bounds");
    return box;
}
template <class Op>
TopoDS_Shape boolean(const TopoDS_Shape &a, const TopoDS_Shape &b, std::stop_token stop) {
    check(true, stop, "");
    Op op;
    TopTools_ListOfShape args, tools;
    args.Append(a);
    tools.Append(b);
    op.SetArguments(args);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.SetRunParallel(false);
    op.SetFuzzyValue(1e-7);
    op.Build();
    check(op.IsDone() && !op.HasErrors() && BRepCheck_Analyzer(op.Shape()).IsValid(), stop,
          "Cannot validate horn geometry Boolean");
    return op.Shape();
}
} // namespace
TopoDS_Shape cutHornSection(const TopoDS_Shape &source, const core::ToolSegment &s, double tip,
                            double start, std::stop_token stop) {
    check(s.horn && core::validHornRelief(*s.horn, s.lengthMm) &&
              (s.shape == core::SegmentShape::LeftHorn || s.shape == core::SegmentShape::RightHorn),
          stop, "Missing or invalid horn relief dimensions");
    const auto h = *s.horn;
    const auto box = bounds(source);
    const auto lo = box.CornerMin(), hi = box.CornerMax();
    const double root = tip + core::hornRootHeight(h), top = hi.Y() + 1;
    check(std::isfinite(tip) && std::isfinite(start) && root < hi.Y() - 1e-7, stop,
          "Horn relief reaches the top of the punch profile");
    const auto at = [&](double axial, double y) {
        return gp_Pnt(lo.X() - 1, y,
                      s.shape == core::SegmentShape::LeftHorn ? start + axial
                                                              : start + s.lengthMm - axial);
    };
    const double straight = std::max(0., h.projectionMm - h.radiusMm);
    const double angle = std::acos(std::max(0., 1 - h.projectionMm / h.radiusMm));
    const auto outer = at(0, tip + h.tipHeightMm);
    const auto arcStart = at(straight, tip + h.tipHeightMm);
    const auto inner = at(h.projectionMm, root);
    const auto mid = at(h.projectionMm - h.radiusMm + h.radiusMm * std::cos(angle / 2),
                        root - h.radiusMm * std::sin(angle / 2));
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    if (straight > 0)
        line(outer, arcStart);
    wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(arcStart, mid, inner).Value()).Edge());
    line(inner, at(h.projectionMm, top));
    line(at(h.projectionMm, top), at(0, top));
    line(at(0, top), outer);
    check(wire.IsDone(), stop, "Cannot close longitudinal horn relief");
    const auto cutter = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()),
                                              gp_Vec(hi.X() - lo.X() + 2, 0, 0))
                            .Shape();
    const auto result = boolean<BRepAlgoAPI_Cut>(source, cutter, stop);
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(result, TopAbs_SOLID, solids);
    check(solids.Extent() == 1 && std::isfinite(volume(result)) && volume(result) > 1e-10, stop,
          "Horn relief does not leave one valid section");
    return result;
}
TopoDS_Shape hornTranslationSweep(const TopoDS_Shape &source, double lift, std::stop_token stop) {
    check(std::isfinite(lift) && lift > 0 && lift <= 100000, stop, "Invalid horn return distance");
    TopTools_ListOfShape args, tools;
    // Adjacent sections are separate solids. Fuse them as individual arguments:
    // treating their touching compound as one operand can retain internal faces
    // and give contradictory point/Boolean classifications of the return volume.
    for (TopExp_Explorer ex(source, TopAbs_SOLID); ex.More(); ex.Next()) {
        if (args.IsEmpty())
            args.Append(ex.Current());
        else
            tools.Append(ex.Current());
    }
    check(!args.IsEmpty(), stop, "No solid horn sections to sweep");
    bool swept = false;
    for (TopExp_Explorer ex(source, TopAbs_FACE); ex.More(); ex.Next()) {
        check(true, stop, "");
        const auto face = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface surface(face);
        TopoDS_Shape forward;
        const double orientation = face.Orientation() == TopAbs_REVERSED ? -1. : 1.;
        if (surface.GetType() == GeomAbs_Plane) {
            const double sign = orientation * surface.Plane().Axis().Direction().Y();
            if (sign <= 1e-12)
                continue;
            forward = face;
        } else if (surface.GetType() == GeomAbs_Cylinder) {
            const auto cylinder = surface.Cylinder();
            check(std::abs(cylinder.Axis().Direction().Y()) < 1e-12, stop,
                  "Horn return requires an upright plane/cylinder boundary");
            double u0, u1, v0, v1;
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            gp_Pnt sample;
            gp_Vec du, dv;
            surface.D1((u0 + u1) / 2, (v0 + v1) / 2, sample, du, dv);
            const gp_Vec axis(cylinder.Axis().Direction()), delta(cylinder.Location(), sample);
            const auto radial = delta - axis * delta.Dot(axis);
            const double sign = orientation * du.Crossed(dv).Dot(radial);
            check(std::isfinite(sign) && std::abs(sign) > 1e-12, stop,
                  "Unknown horn surface orientation");
            const auto box = bounds(face);
            const auto lo = box.CornerMin(), hi = box.CornerMax();
            const double centre = cylinder.Location().Y();
            if ((sign > 0 && hi.Y() <= centre) || (sign < 0 && lo.Y() >= centre))
                continue;
            if ((sign > 0 && lo.Y() >= centre) || (sign < 0 && hi.Y() <= centre))
                forward = face;
            else {
                const double lower = sign > 0 ? centre : lo.Y() - 1,
                             upper = sign > 0 ? hi.Y() + 1 : centre;
                forward = boolean<BRepAlgoAPI_Common>(
                    face,
                    BRepPrimAPI_MakeBox(gp_Pnt(lo.X() - 1, lower, lo.Z() - 1), hi.X() - lo.X() + 2,
                                        upper - lower, hi.Z() - lo.Z() + 2)
                        .Shape(),
                    stop);
            }
        } else {
            check(false, stop, "Unsupported surface in horn return envelope");
        }
        for (TopExp_Explorer f(forward, TopAbs_FACE); f.More(); f.Next()) {
            auto prism = BRepPrimAPI_MakePrism(f.Current(), gp_Vec(0, lift, 0), true).Shape();
            check(BRepCheck_Analyzer(prism).IsValid() && std::isfinite(volume(prism)) &&
                      volume(prism) > 0,
                  stop, "Cannot sweep a forward horn face");
            tools.Append(prism);
            swept = true;
        }
    }
    check(swept, stop, "No forward horn boundary found");
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(args);
    fuse.SetTools(tools);
    fuse.SetNonDestructive(true);
    fuse.SetRunParallel(false);
    fuse.SetFuzzyValue(1e-7);
    fuse.Build();
    check(fuse.IsDone() && !fuse.HasErrors() && BRepCheck_Analyzer(fuse.Shape()).IsValid(), stop,
          "Cannot validate full horn return envelope");
    return fuse.Shape();
}
} // namespace bending::cad
