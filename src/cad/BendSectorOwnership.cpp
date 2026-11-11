#include "BendSectorOwnership.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeAnalysis_ShapeTolerance.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <vector>

namespace bending::cad {
namespace {
double area(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    return properties.Mass();
}

bool validSkins(const std::array<TopoDS_Face, 2> &skins, double tolerance) {
    return std::isfinite(tolerance) && tolerance > 0 && !skins[0].IsNull() && !skins[1].IsNull() &&
           !skins[0].IsSame(skins[1]) &&
           std::all_of(skins.begin(), skins.end(), [](const auto &skin) {
               return BRepAdaptor_Surface(skin).GetType() == GeomAbs_Cylinder;
           });
}

std::optional<TopoDS_Shape> selectSector(BRepAlgoAPI_Common &common,
                                         const std::array<TopoDS_Face, 2> &skins, double tolerance,
                                         std::stop_token stop, bool preserveSkins) {
    TopTools_IndexedMapOfShape solids, faces;
    TopExp::MapShapes(common.Shape(), TopAbs_SOLID, solids);
    if (preserveSkins ? solids.Extent() != 1 : solids.Extent() < 2)
        return {};
    std::vector<int> owners{0};
    for (int i = 1; i <= solids.Extent(); ++i) {
        TopTools_IndexedMapOfShape local;
        TopExp::MapShapes(solids(i), TopAbs_FACE, local);
        for (int j = 1; j <= local.Extent(); ++j) {
            const int index = faces.Add(local(j));
            if (index == static_cast<int>(owners.size()))
                owners.push_back(i);
            else if (owners[static_cast<std::size_t>(index)] != i)
                owners[static_cast<std::size_t>(index)] = 0;
        }
    }
    int selected = 0;
    for (const auto &skin : skins) {
        if (stop.stop_requested())
            return {};
        TopTools_IndexedMapOfShape descendants;
        for (TopTools_ListIteratorOfListOfShape it(common.Modified(skin)); it.More(); it.Next()) {
            if (it.Value().ShapeType() != TopAbs_FACE)
                return {};
            descendants.Add(it.Value());
        }
        if (descendants.IsEmpty() && !common.IsDeleted(skin))
            descendants.Add(skin);
        if (descendants.IsEmpty() || (preserveSkins && descendants.Extent() != 1))
            return {};
        double covered = 0;
        for (int i = 1; i <= descendants.Extent(); ++i) {
            const int index = faces.FindIndex(descendants(i));
            if (!index || !owners[static_cast<std::size_t>(index)])
                return {};
            if (preserveSkins &&
                BRepAdaptor_Surface(TopoDS::Face(descendants(i))).GetType() != GeomAbs_Cylinder)
                return {};
            const int owner = owners[static_cast<std::size_t>(index)];
            if (selected && selected != owner)
                return {};
            selected = owner;
            covered += area(descendants(i));
        }
        GProp_GProps perimeter;
        BRepGProp::LinearProperties(skin, perimeter);
        const double expected = area(skin);
        const double budget = std::max(expected * 1e-7, perimeter.Mass() * tolerance * 2);
        if (!std::isfinite(expected) || expected <= 0 || !std::isfinite(covered) ||
            !std::isfinite(budget) || std::abs(expected - covered) > budget)
            return {};
    }
    if (!selected)
        return {};
    return solids(selected);
}
} // namespace

std::optional<TopoDS_Shape> selectOwnedBendSector(BRepAlgoAPI_Common &common,
                                                  const std::array<TopoDS_Face, 2> &skins,
                                                  double tolerance, std::stop_token stop) {
    if (stop.stop_requested() || !common.IsDone() || common.HasErrors() ||
        !validSkins(skins, tolerance))
        return {};
    return selectSector(common, skins, tolerance, stop, false);
}

std::optional<TopoDS_Shape> ownedBendSector(const TopoDS_Shape &source,
                                            const TopoDS_Shape &envelope,
                                            const std::array<TopoDS_Face, 2> &skins,
                                            double tolerance, std::stop_token stop,
                                            BendSectorSelection selection) {
    if (stop.stop_requested() || source.IsNull() || envelope.IsNull() ||
        !validSkins(skins, tolerance))
        return {};
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    const bool preserveSkins = selection == BendSectorSelection::PreserveSourceSkins;
    arguments.Append(preserveSkins ? source : envelope);
    tools.Append(preserveSkins ? envelope : source);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetRunParallel(false);
    common.SetToFillHistory(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    if (stop.stop_requested() || !common.IsDone() || common.HasErrors() ||
        !BRepCheck_Analyzer(common.Shape()).IsValid())
        return {};
    return selectSector(common, skins, tolerance, stop, preserveSkins);
}

TopoDS_Shape sectorFromSourceCaps(const TopTools_IndexedDataMapOfShapeListOfShape &adjacent,
                                  const TopoDS_Shape &envelope,
                                  const std::array<TopoDS_Face, 2> &skins, double tolerance,
                                  std::stop_token stop) {
    const auto neighbours = [&](const TopoDS_Face &skin) {
        TopTools_IndexedMapOfShape faces;
        for (TopExp_Explorer e(skin, TopAbs_EDGE); e.More(); e.Next())
            for (const auto &other : adjacent.FindFromKey(e.Current()))
                if (!skin.IsSame(other))
                    faces.Add(other);
        return faces;
    };
    const auto inner = neighbours(skins[0]), outer = neighbours(skins[1]);
    std::vector<TopoDS_Face> caps;
    for (int i = 1; i <= inner.Extent(); ++i)
        if (outer.Contains(inner(i)))
            caps.push_back(TopoDS::Face(inner(i)));
    if (caps.size() != 2 || std::none_of(caps.begin(), caps.end(), [](const auto &cap) {
            return BRepAdaptor_Surface(cap, false).GetType() == GeomAbs_BSplineSurface;
        }))
        return {};

    BRepBuilderAPI_Sewing skin(tolerance);
    for (const auto &face : skins)
        skin.Add(face);
    for (const auto &cap : caps) {
        if (stop.stop_requested())
            return {};
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(cap);
        tools.Append(envelope);
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.SetNonDestructive(true);
        common.Build();
        if (stop.stop_requested() || !common.IsDone() || common.HasErrors() ||
            !BRepCheck_Analyzer(common.Shape()).IsValid())
            return {};
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(common.Shape(), TopAbs_FACE, faces);
        if (faces.Extent() != 1)
            return {};
        skin.Add(faces(1));
    }
    skin.Perform();
    const auto open = skin.SewedShape();
    if (open.IsNull() || open.ShapeType() != TopAbs_SHELL)
        return {};
    TopTools_IndexedDataMapOfShapeListOfShape boundary;
    TopExp::MapShapesAndAncestors(open, TopAbs_EDGE, TopAbs_FACE, boundary);
    Handle(TopTools_HSequenceOfShape) freeEdges = new TopTools_HSequenceOfShape;
    for (int i = 1; i <= boundary.Extent(); ++i) {
        const auto owners = boundary.FindFromIndex(i).Extent();
        if (owners == 1)
            freeEdges->Append(boundary.FindKey(i));
        else if (owners != 2)
            return {};
    }
    if (freeEdges->Length() != 8 || stop.stop_requested())
        return {};
    Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(freeEdges, tolerance, true, wires);
    if (wires->Length() != 2)
        return {};
    BRepBuilderAPI_Sewing closed(tolerance);
    closed.Add(open);
    for (int i = 1; i <= wires->Length(); ++i) {
        const auto wire = TopoDS::Wire(wires->Value(i));
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(wire, TopAbs_EDGE, edges);
        if (!wire.Closed() || edges.Extent() != 4)
            return {};
        BRepBuilderAPI_MakeFace patch(wire, true);
        if (!patch.IsDone())
            return {};
        closed.Add(patch.Face());
    }
    closed.Perform();
    const auto shell = closed.SewedShape();
    if (stop.stop_requested() || shell.IsNull() || shell.ShapeType() != TopAbs_SHELL ||
        !shell.Closed())
        return {};
    auto solid = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shell)).Solid();
    if (!BRepLib::OrientClosedSolid(solid) || !BRepCheck_Analyzer(solid).IsValid())
        return {};
    const double uncertainty = ShapeAnalysis_ShapeTolerance{}.Tolerance(solid, 1);
    if (!std::isfinite(uncertainty) || uncertainty > tolerance)
        return {};
    for (const auto &face : skins) {
        TopLoc_Location location;
        const auto surface = BRep_Tool::Surface(face, location);
        const double expected = area(face);
        GProp_GProps perimeter;
        BRepGProp::LinearProperties(face, perimeter);
        const double budget = std::max(expected * 1.e-7, perimeter.Mass() * uncertainty * 2);
        if (!std::isfinite(expected) || expected <= 0 || !std::isfinite(budget))
            return {};
        int complete = 0;
        for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
            const auto candidate = TopoDS::Face(it.Current());
            TopLoc_Location actualLocation;
            if (BRep_Tool::Surface(candidate, actualLocation) == surface &&
                actualLocation.IsEqual(location) && std::abs(area(candidate) - expected) <= budget)
                ++complete;
        }
        if (complete != 1)
            return {};
    }
    return solid;
}
} // namespace bending::cad
