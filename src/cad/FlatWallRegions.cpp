#include "FlatWallRegions.h"
#include "bending/application/Document.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
double area(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    return properties.Mass();
}
bool boundedWall(const TopoDS_Shape &solid, const std::array<TopoDS_Face, 2> &anchors,
                 const std::array<TopTools_IndexedMapOfShape, 2> &skins, double tolerance,
                 std::stop_token stop) {
    const auto plane = BRepAdaptor_Surface(anchors[0]).Plane();
    gp_Trsf local;
    local.SetTransformation(plane.Position());
    const double other = BRepAdaptor_Surface(anchors[1]).Plane().Location().Transformed(local).Z();
    const double lower = std::min(0., other), upper = std::max(0., other);
    Bnd_Box box;
    BRepBndLib::AddOptimal(solid.Moved(TopLoc_Location(local)), box, false, false);
    if (box.IsVoid() || box.IsOpen() || box.IsWhole())
        return false;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    if (!std::isfinite(z0) || !std::isfinite(z1) || z0 < lower - tolerance ||
        z1 > upper + tolerance)
        return false;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(solid, TopAbs_FACE, faces);
    std::vector<unsigned> tags(static_cast<std::size_t>(faces.Extent()) + 1, 0);
    for (int i = 1; i <= faces.Extent(); ++i) {
        for (std::size_t side = 0; side < 2; ++side)
            if (skins[side].Contains(faces(i)))
                tags[static_cast<std::size_t>(i)] |= 1U << side;
        if (tags[static_cast<std::size_t>(i)])
            continue;
        const BRepAdaptor_Surface surface(TopoDS::Face(faces(i)));
        // A third parallel face is a pocket floor, an extra skin patch, or a
        // step in thickness. None is an outer bevel between the proved skins.
        if (surface.GetType() == GeomAbs_Plane &&
            surface.Plane().Axis().Direction().IsParallel(plane.Axis().Direction(), 1e-7))
            return false;
    }
    TopTools_IndexedDataMapOfShapeListOfShape adjacent;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, adjacent);
    std::vector<bool> seen(tags.size(), false);
    for (int start = 1; start <= faces.Extent(); ++start) {
        if (tags[static_cast<std::size_t>(start)] || seen[static_cast<std::size_t>(start)])
            continue;
        std::vector<int> pending{start};
        seen[static_cast<std::size_t>(start)] = true;
        unsigned touched = 0;
        for (std::size_t at = 0; at < pending.size(); ++at) {
            if (stop.stop_requested())
                return false;
            for (TopExp_Explorer ex(faces(pending[at]), TopAbs_EDGE); ex.More(); ex.Next()) {
                if (!adjacent.Contains(ex.Current()))
                    return false;
                const auto &neighbours = adjacent.FindFromKey(ex.Current());
                if (neighbours.Extent() != 2)
                    return false;
                for (TopTools_ListIteratorOfListOfShape it(neighbours); it.More(); it.Next()) {
                    const int index = faces.FindIndex(it.Value());
                    if (!index)
                        return false;
                    touched |= tags[static_cast<std::size_t>(index)];
                    if (!tags[static_cast<std::size_t>(index)] &&
                        !seen[static_cast<std::size_t>(index)]) {
                        seen[static_cast<std::size_t>(index)] = true;
                        pending.push_back(index);
                    }
                }
            }
        }
        // A through-hole boundary meets both skins; a blind pocket/cavity
        // meets only one or neither. Do not turn it into sheet stock.
        if (touched != 3)
            return false;
    }
    return true;
}
} // namespace
std::map<std::size_t, std::size_t>
mapSourceWallOwners(const TopoDS_Shape &original, const TopoDS_Shape &calculation,
                    std::span<const SheetWallRegion> walls,
                    std::span<const application::FaceInfo> metadata, bool differentTopology,
                    double tolerance, std::stop_token stop) {
    TopTools_IndexedMapOfShape sourceFaces, targetFaces;
    TopExp::MapShapes(original, TopAbs_FACE, sourceFaces);
    TopExp::MapShapes(calculation, TopAbs_FACE, targetFaces);
    std::map<std::size_t, std::size_t> owners;
    std::size_t comparisons = 0;
    for (const auto &face : metadata) {
        if (stop.stop_requested())
            return {};
        if (face.surface != application::SurfaceKind::Plane || face.index == 0 ||
            face.index > static_cast<std::size_t>(sourceFaces.Extent()))
            continue;
        TopoDS_Face sourceFace;
        gp_Pln plane;
        Bnd_Box sourceBox;
        if (differentTopology) {
            sourceFace = TopoDS::Face(sourceFaces(static_cast<int>(face.index)));
            plane = BRepAdaptor_Surface(sourceFace).Plane();
            BRepBndLib::Add(sourceFace, sourceBox);
        }
        std::optional<std::size_t> owner;
        bool ambiguous = false;
        for (std::size_t w = 0; w < walls.size() && !ambiguous; ++w)
            for (auto index : walls[w].faces) {
                if (stop.stop_requested())
                    return {};
                if (!differentTopology) {
                    if (index == face.index) {
                        if (owner && *owner != w) {
                            ambiguous = true;
                            break;
                        }
                        owner = w;
                    }
                    continue;
                }
                const auto targetFace = TopoDS::Face(targetFaces(static_cast<int>(index)));
                BRepAdaptor_Surface surface(targetFace);
                if (surface.GetType() != GeomAbs_Plane ||
                    !plane.Axis().Direction().IsParallel(surface.Plane().Axis().Direction(),
                                                         1.e-7) ||
                    plane.Distance(surface.Plane().Location()) > tolerance * 4)
                    continue;
                Bnd_Box box;
                BRepBndLib::Add(targetFace, box);
                if (sourceBox.IsOut(box))
                    continue;
                if (comparisons >= 2048) {
                    ambiguous = true; // An untested candidate cannot prove unique ownership.
                    break;
                }
                ++comparisons;
                try {
                    // IDs of the filled solid are not IDs of the imported skin.
                    // Require complete area ownership, not a centroid or a few samples.
                    BRepAlgoAPI_Common common;
                    TopTools_ListOfShape arguments, tools;
                    arguments.Append(sourceFace);
                    tools.Append(targetFace);
                    common.SetArguments(arguments);
                    common.SetTools(tools);
                    common.SetNonDestructive(true);
                    common.Build();
                    if (!common.IsDone()) {
                        ambiguous = true;
                        break;
                    }
                    const auto overlapArea = area(common.Shape());
                    if (!std::isfinite(overlapArea) || !std::isfinite(face.areaMm2)) {
                        ambiguous = true;
                        break;
                    }
                    if (overlapArea <= 0 || face.areaMm2 < 1.e-8 ||
                        std::abs(overlapArea - face.areaMm2) >
                            std::max(1.e-9, std::min(face.areaMm2 * 1.e-7,
                                                     tolerance * std::sqrt(face.areaMm2) * 8)))
                        continue;
                    if (owner && *owner != w) {
                        ambiguous = true;
                        break;
                    }
                    owner = w;
                } catch (const Standard_Failure &) {
                    ambiguous = true;
                    break;
                }
            }
        if (owner && !ambiguous)
            owners[face.index] = *owner;
    }
    return owners;
}

std::optional<std::vector<TopoDS_Shape>>
isolateFlatWalls(const TopoDS_Shape &source, const std::vector<TopoDS_Shape> &sectors,
                 const std::vector<std::array<TopoDS_Face, 2>> &anchors, double tolerance,
                 std::stop_token stop) {
    if (stop.stop_requested() || sectors.empty() || anchors.empty())
        return {};
    BRepAlgoAPI_Cut cut;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(source);
    for (const auto &sector : sectors)
        tools.Append(sector);
    cut.SetArguments(arguments);
    cut.SetTools(tools);
    cut.SetNonDestructive(true);
    cut.SetRunParallel(false);
    cut.SetToFillHistory(true);
    cut.SetFuzzyValue(tolerance);
    cut.Build();
    if (stop.stop_requested() || !cut.IsDone() || cut.HasErrors() ||
        !BRepCheck_Analyzer(cut.Shape()).IsValid())
        return {};
    TopTools_IndexedMapOfShape solids, faces;
    TopExp::MapShapes(cut.Shape(), TopAbs_SOLID, solids);
    if (solids.Extent() != static_cast<int>(anchors.size()))
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
    std::vector<int> assigned(anchors.size(), 0);
    std::vector<std::array<TopTools_IndexedMapOfShape, 2>> skins(anchors.size());
    std::vector<bool> used(static_cast<std::size_t>(solids.Extent()) + 1, false);
    for (std::size_t wall = 0; wall < anchors.size(); ++wall) {
        for (std::size_t side = 0; side < 2; ++side) {
            const auto &anchor = anchors[wall][side];
            if (stop.stop_requested())
                return {};
            TopTools_IndexedMapOfShape descendants;
            for (TopTools_ListIteratorOfListOfShape it(cut.Modified(anchor)); it.More();
                 it.Next()) {
                if (it.Value().ShapeType() != TopAbs_FACE)
                    return {};
                descendants.Add(it.Value());
            }
            if (descendants.IsEmpty() && !cut.IsDeleted(anchor))
                descendants.Add(anchor);
            if (descendants.IsEmpty())
                return {};
            double covered = 0;
            int owner = 0;
            for (int i = 1; i <= descendants.Extent(); ++i) {
                const int index = faces.FindIndex(descendants(i));
                if (!index || !owners[static_cast<std::size_t>(index)])
                    return {};
                const int current = owners[static_cast<std::size_t>(index)];
                if (owner && owner != current)
                    return {};
                owner = current;
                covered += area(descendants(i));
            }
            GProp_GProps perimeter;
            BRepGProp::LinearProperties(anchor, perimeter);
            const double expected = area(anchor);
            const double budget = std::max(expected * 1e-7, perimeter.Mass() * tolerance * 2);
            if (!std::isfinite(covered) || !std::isfinite(expected) || expected <= 0 ||
                !std::isfinite(budget) || std::abs(covered - expected) > budget || !owner ||
                (assigned[wall] && assigned[wall] != owner))
                return {};
            assigned[wall] = owner;
            skins[wall][side] = descendants;
        }
        if (!assigned[wall] || used[static_cast<std::size_t>(assigned[wall])])
            return {};
        used[static_cast<std::size_t>(assigned[wall])] = true;
        if (!boundedWall(solids(assigned[wall]), anchors[wall], skins[wall], tolerance, stop))
            return {};
    }
    std::vector<TopoDS_Shape> result;
    for (const auto owner : assigned) {
        if (stop.stop_requested())
            return {};
        result.push_back(BRepBuilderAPI_Copy(solids(owner), true, false).Shape());
    }
    return result;
}
} // namespace bending::cad
