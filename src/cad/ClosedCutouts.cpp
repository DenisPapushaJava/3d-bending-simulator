#include "ClosedCutouts.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace bending::cad {
namespace {
double volume(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}
double area(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}
} // namespace
FilledCutouts fillClosedCutouts(const TopoDS_Shape &source, double tolerance,
                                std::stop_token stop) {
    FilledCutouts result;
    using Status = application::ClosedCutoutStatus;
    try {
        TopTools_IndexedMapOfShape faces, edges;
        TopExp::MapShapes(source, TopAbs_FACE, faces);
        TopExp::MapShapes(source, TopAbs_EDGE, edges);
        if (faces.Extent() > 512 || edges.Extent() > 2048 || tolerance > .01) {
            result.report.status = Status::Unsupported;
            return result;
        }
        TopTools_IndexedDataMapOfShapeListOfShape adjacent;
        TopExp::MapShapesAndAncestors(source, TopAbs_EDGE, TopAbs_FACE, adjacent);
        std::set<int> hosts;
        std::vector<TopTools_IndexedMapOfShape> outerEdges(
            static_cast<std::size_t>(faces.Extent() + 1));
        std::vector<int> seeds;
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested())
                return {};
            const auto face = TopoDS::Face(faces(i));
            TopTools_IndexedMapOfShape wires;
            TopExp::MapShapes(face, TopAbs_WIRE, wires);
            if (wires.Extent() <= 1)
                continue;
            const auto outer = BRepTools::OuterWire(face);
            if (outer.IsNull() || !wires.Contains(outer)) {
                result.report.status = Status::Unsupported;
                return result;
            }
            hosts.insert(i);
            TopExp::MapShapes(outer, TopAbs_EDGE, outerEdges[static_cast<std::size_t>(i)]);
        }
        if (hosts.empty())
            return result;
        result.report.status = Status::Unsupported;
        for (const auto i : hosts) {
            TopTools_IndexedMapOfShape wires;
            const auto face = TopoDS::Face(faces(i));
            TopExp::MapShapes(face, TopAbs_WIRE, wires);
            const auto outer = BRepTools::OuterWire(face);
            for (int j = 1; j <= wires.Extent(); ++j) {
                if (wires(j).IsSame(outer))
                    continue;
                TopTools_IndexedMapOfShape innerEdges;
                TopExp::MapShapes(wires(j), TopAbs_EDGE, innerEdges);
                for (int k = 1; k <= innerEdges.Extent(); ++k) {
                    if (!adjacent.Contains(innerEdges(k)))
                        return result;
                    std::set<int> neighbors;
                    for (const auto &neighbor : adjacent.FindFromKey(innerEdges(k))) {
                        neighbors.insert(faces.FindIndex(neighbor));
                    }
                    if (neighbors.size() != 2 || !neighbors.contains(i))
                        return result;
                    for (const auto index : neighbors) {
                        if (index != i) {
                            seeds.push_back(index);
                            result.report.boundaryFaces.push_back(static_cast<std::size_t>(index));
                        }
                    }
                }
            }
        }
        // Remove the complete cutout-wall component, including countersinks/pocket floors.
        // A component reaching any retained outer wire is an open feature and is rejected.
        std::set<int> removed;
        for (std::size_t cursor = 0; cursor < seeds.size(); ++cursor) {
            if (stop.stop_requested())
                return {};
            const auto index = seeds[cursor];
            if (index <= 0 || hosts.contains(index))
                return result;
            if (!removed.insert(index).second)
                continue;
            TopTools_IndexedMapOfShape wallEdges;
            TopExp::MapShapes(faces(index), TopAbs_EDGE, wallEdges);
            for (int e = 1; e <= wallEdges.Extent(); ++e) {
                if (!adjacent.Contains(wallEdges(e)))
                    return result;
                for (const auto &neighbor : adjacent.FindFromKey(wallEdges(e))) {
                    const auto next = faces.FindIndex(neighbor);
                    if (hosts.contains(next)) {
                        if (outerEdges[static_cast<std::size_t>(next)].Contains(wallEdges(e)))
                            return result;
                    } else if (!removed.contains(next)) {
                        seeds.push_back(next);
                    }
                }
            }
        }
        result.report.boundaryFaces.clear();
        for (const auto i : removed)
            result.report.boundaryFaces.push_back(static_cast<std::size_t>(i));
        BRepBuilderAPI_Copy copy(source, true, false);
        BRepBuilderAPI_Sewing sew(tolerance);
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested())
                return {};
            if (removed.contains(i))
                continue;
            auto face = TopoDS::Face(copy.ModifiedShape(faces(i)));
            if (hosts.contains(i)) {
                const auto orientation = face.Orientation();
                BRepBuilderAPI_MakeFace patch(BRep_Tool::Surface(face), BRepTools::OuterWire(face),
                                              true);
                if (!patch.IsDone())
                    return result;
                face = patch.Face();
                face.Orientation(orientation);
                if (!BRepCheck_Analyzer(face).IsValid())
                    return result;
            }
            sew.Add(face);
        }
        sew.Perform();
        TopTools_IndexedMapOfShape shells;
        TopExp::MapShapes(sew.SewedShape(), TopAbs_SHELL, shells);
        if (shells.Extent() != 1 || sew.NbFreeEdges() != 0)
            return result;
        auto solid = BRepBuilderAPI_MakeSolid(TopoDS::Shell(shells(1))).Solid();
        if (!BRepLib::OrientClosedSolid(solid) || !BRepCheck_Analyzer(solid).IsValid())
            return result;
        const double originalVolume = volume(source), filledVolume = volume(solid);
        const double budget = std::max(originalVolume * 1e-8, area(source) * tolerance * 2);
        if (!std::isfinite(filledVolume) || filledVolume - originalVolume <= budget)
            return result;
        Bnd_Box originalBox, filledBox;
        BRepBndLib::AddOptimal(source, originalBox, false, false);
        BRepBndLib::AddOptimal(solid, filledBox, false, false);
        if (originalBox.CornerMin().Distance(filledBox.CornerMin()) > tolerance ||
            originalBox.CornerMax().Distance(filledBox.CornerMax()) > tolerance)
            return result;
        BRepAlgoAPI_Cut missing;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(source);
        tools.Append(solid);
        missing.SetArguments(arguments);
        missing.SetTools(tools);
        missing.SetNonDestructive(true);
        missing.SetFuzzyValue(tolerance);
        missing.Build();
        if (stop.stop_requested())
            return {};
        if (!missing.IsDone() || missing.HasErrors() ||
            !BRepCheck_Analyzer(missing.Shape()).IsValid() ||
            !std::isfinite(volume(missing.Shape())) || volume(missing.Shape()) > budget)
            return result;
        result.shape = solid;
        result.report.status = Status::Filled;
        result.report.addedVolumeMm3 = filledVolume - originalVolume;
        return result;
    } catch (const Standard_Failure &) {
        result.report.status = Status::Failed;
        result.shape.Nullify();
        return result;
    }
}
} // namespace bending::cad
