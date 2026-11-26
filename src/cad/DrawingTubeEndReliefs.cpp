#include "DrawingTubeProfile.h"

#include <BOPAlgo_PaveFiller.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
constexpr double angularTolerance = 1e-6;
using EdgeFaces = TopTools_IndexedDataMapOfShapeListOfShape;
using Bounds = std::array<double, 6>;

Bounds boundsIn(const TopoDS_Shape &shape, const gp_Trsf &frame) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape.Moved(TopLoc_Location(frame)), box, false, false);
    Bounds result;
    box.Get(result[0], result[1], result[2], result[3], result[4], result[5]);
    return result;
}

struct AxialFrame {
    gp_Dir axis;
    gp_Trsf toLocal;
    Bounds bounds;
};

std::optional<AxialFrame> rectangularStockFrame(const TopoDS_Shape &shape,
                                                const TopTools_IndexedMapOfShape &faces, double tol,
                                                std::stop_token stop) {
    struct Planes {
        gp_Dir normal;
        std::vector<double> offsets;
    };
    std::vector<Planes> families;
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i) {
        const BRepAdaptor_Surface surface(TopoDS::Face(faces(i)), false);
        if (surface.GetType() != GeomAbs_Plane)
            continue;
        const auto plane = surface.Plane();
        const auto normal = plane.Axis().Direction();
        const auto family = std::find_if(families.begin(), families.end(), [&](const auto &group) {
            return group.normal.IsParallel(normal, angularTolerance);
        });
        const gp_Vec location(plane.Location().XYZ());
        if (family == families.end())
            families.push_back({normal, {location.Dot(gp_Vec(normal))}});
        else
            family->offsets.push_back(location.Dot(gp_Vec(family->normal)));
    }
    for (auto &group : families)
        std::sort(group.offsets.begin(), group.offsets.end());
    std::erase_if(families, [tol](const auto &group) {
        const auto &offsets = group.offsets;
        if (offsets.size() < 4)
            return true;
        double previous = offsets.front();
        int planes = 1;
        for (double offset : offsets)
            if (offset - previous > tol) {
                previous = offset;
                if (++planes == 4)
                    return false;
            }
        return true;
    });
    std::optional<AxialFrame> result;
    for (std::size_t i = 0; i < families.size() && !stop.stop_requested(); ++i)
        for (std::size_t j = i + 1; j < families.size() && !stop.stop_requested(); ++j) {
            const auto &x = families[i].normal, &y = families[j].normal;
            if (!x.IsNormal(y, angularTolerance))
                continue;
            const gp_Dir axis = x.Crossed(y);
            if (result && result->axis.IsParallel(axis, angularTolerance))
                continue;
            gp_Trsf toLocal;
            toLocal.SetTransformation(gp_Ax3(gp_Pnt{}, axis, x));
            const auto b = boundsIn(shape, toLocal);
            const double margin = std::max(b[3] - b[0], b[4] - b[1]);
            if (!std::isfinite(margin) || margin <= tol || b[5] - b[2] <= 2 * margin + tol)
                continue;
            if (result)
                return {}; // Multiple plausible stock axes.
            result = AxialFrame{axis, toLocal, b};
        }
    return stop.stop_requested() ? std::nullopt : result;
}

std::optional<std::vector<int>> skinOwners(BRepAlgoAPI_Common &common,
                                           const TopTools_IndexedMapOfShape &stockFaces,
                                           const TopTools_IndexedMapOfShape &sourceFaces,
                                           const std::vector<int> &skin) {
    std::vector<int> owners(static_cast<std::size_t>(stockFaces.Extent()) + 1);
    for (int i = 1; i <= sourceFaces.Extent(); ++i) {
        const auto record = [&](const TopoDS_Shape &face) {
            const int index = stockFaces.FindIndex(face);
            if (!std::binary_search(skin.begin(), skin.end(), index))
                return true;
            auto &owner = owners[static_cast<std::size_t>(index)];
            if (owner && owner != i)
                return false;
            owner = i;
            return true;
        };
        if (!record(sourceFaces(i)))
            return {};
        for (const auto &face : common.Modified(sourceFaces(i)))
            if (!record(face))
                return {};
    }
    if (std::any_of(skin.begin(), skin.end(),
                    [&](int index) { return !owners[static_cast<std::size_t>(index)]; }))
        return {};
    return owners;
}

using Span = std::array<double, 2>;
struct FaceSpan {
    Span axial;
    bool longitudinal;
    unsigned outerWalls;
};

std::vector<Span> stockSpans(const std::vector<FaceSpan> &spans, double tol) {
    std::vector<double> stations;
    for (const auto &face : spans) {
        stations.push_back(face.axial[0]);
        stations.push_back(face.axial[1]);
    }
    std::sort(stations.begin(), stations.end());
    std::vector<Span> candidates;
    for (std::size_t i = 1; i < stations.size(); ++i) {
        const double first = stations[i - 1], last = stations[i];
        if (last - first <= 8 * tol)
            continue;
        unsigned outerWalls = 0;
        bool cut = false;
        for (const auto &face : spans) {
            const bool crosses = face.axial[0] < last - tol && face.axial[1] > first + tol;
            if (!face.longitudinal && crosses) {
                cut = true;
                break;
            }
            if (face.axial[0] <= first + tol && face.axial[1] >= last - tol)
                outerWalls |= face.outerWalls;
        }
        if (!cut && outerWalls == 0b1111)
            candidates.push_back({first + 2 * tol, last - 2 * tol});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &a, const auto &b) { return a[1] - a[0] > b[1] - b[0]; });
    constexpr std::size_t maximumProbes = 3;
    if (candidates.size() > maximumProbes)
        candidates.resize(maximumProbes);
    return candidates;
}

TopoDS_Shape slab(const AxialFrame &frame, Span span) {
    const auto &b = frame.bounds;
    return BRepPrimAPI_MakeBox(gp_Pnt(b[0] - 1, b[1] - 1, span[0]),
                               gp_Pnt(b[3] + 1, b[4] + 1, span[1]))
        .Shape()
        .Moved(TopLoc_Location(frame.toLocal.Inverted()));
}

void clippedBy(BRepAlgoAPI_Common &common, const TopoDS_Shape &shape, const TopoDS_Shape &tool) {
    TopTools_ListOfShape arguments, tools;
    arguments.Append(shape);
    tools.Append(tool);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.Build();
}

struct CutComponents {
    std::array<std::vector<int>, 2> rims;
    std::array<double, 2> depth{};
    bool independentCuts = false;
};

std::optional<CutComponents> cutComponents(const TopoDS_Shape &shape,
                                           const TopTools_IndexedMapOfShape &faces,
                                           const std::vector<FaceSpan> &spans,
                                           const DrawingTubeGeometry &tube, const Bounds &bounds,
                                           double tol, std::stop_token stop) {
    EdgeFaces adjacent;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, adjacent);
    const auto isSkin = [&](int face) {
        return std::binary_search(tube.longitudinalFaces.begin(), tube.longitudinalFaces.end(),
                                  face);
    };
    std::vector<bool> visited(spans.size());
    CutComponents result;
    for (int seed = 1; seed <= faces.Extent() && !stop.stop_requested(); ++seed) {
        if (isSkin(seed) || visited[static_cast<std::size_t>(seed - 1)])
            continue;
        std::vector<int> component{seed};
        visited[static_cast<std::size_t>(seed - 1)] = true;
        bool outer = false, inner = false;
        Span range = spans[static_cast<std::size_t>(seed - 1)].axial;
        for (std::size_t i = 0; i < component.size() && !stop.stop_requested(); ++i) {
            const int current = component[i];
            const auto &span = spans[static_cast<std::size_t>(current - 1)];
            range[0] = std::min(range[0], span.axial[0]);
            range[1] = std::max(range[1], span.axial[1]);
            for (TopExp_Explorer edge(faces(current), TopAbs_EDGE); edge.More(); edge.Next()) {
                if (BRep_Tool::Degenerated(TopoDS::Edge(edge.Current())))
                    continue;
                if (!adjacent.Contains(edge.Current()))
                    return {};
                const auto &owners = adjacent.FindFromKey(edge.Current());
                if (owners.Extent() != 2)
                    return {};
                const int first = faces.FindIndex(owners.First());
                const int last = faces.FindIndex(owners.Last());
                const int neighbor = first == current ? last : first;
                if ((first != current && last != current) || neighbor <= 0)
                    return {};
                if (neighbor == current) // A seam on a cylindrical opening.
                    continue;
                for (const auto &wall : *tube.planarWalls) {
                    outer |= neighbor == wall.outerFace;
                    inner |= neighbor == wall.innerFace;
                }
                const auto flag = static_cast<std::size_t>(neighbor - 1);
                if (!isSkin(neighbor) && !visited[flag]) {
                    visited[flag] = true;
                    component.push_back(neighbor);
                }
            }
        }
        const bool atStart = std::abs(range[0] - bounds[2]) <= tol;
        const bool atEnd = std::abs(range[1] - bounds[5]) <= tol;
        // Wall openings have their own projection and dimension path.
        if (!atStart && !atEnd) {
            result.independentCuts = true;
            continue;
        }
        if (atStart == atEnd || !outer || !inner)
            return {};
        const std::size_t side = atEnd ? 1 : 0;
        if (!result.rims[side].empty())
            return {};
        result.depth[side] = range[1] - range[0];
        result.rims[side] = std::move(component);
    }
    if (stop.stop_requested() || result.depth[0] <= tol || result.depth[1] <= tol)
        return {};
    return result;
}

bool emptyDifference(BRepAlgoAPI_Cut &cut, const TopoDS_Shape &source, const TopoDS_Shape &tool,
                     std::stop_token stop) {
    if (stop.stop_requested())
        return false;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(source);
    tools.Append(tool);
    cut.SetArguments(arguments);
    cut.SetTools(tools);
    cut.SetNonDestructive(true);
    cut.Build();
    return !stop.stop_requested() && cut.IsDone() && !cut.HasErrors() && !cut.Shape().IsNull() &&
           !TopExp_Explorer(cut.Shape(), TopAbs_FACE).More();
}

bool cutFromStock(const TopoDS_Shape &shape, const TopoDS_Shape &section, const AxialFrame &frame,
                  double tol, std::stop_token stop) {
    // Sweep the proven annular end of the middle slab. The real part must fit
    // inside this material, including its open core and rounded corners.
    const double station = boundsIn(section, frame.toLocal)[2];
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(frame.axis) * (frame.bounds[2] - station - tol));
    const auto start = section.Moved(TopLoc_Location(shift));
    const auto length = frame.bounds[5] - frame.bounds[2] + 2 * tol;
    const auto stock = BRepPrimAPI_MakePrism(start, gp_Vec(frame.axis) * length).Shape();
    BRepAlgoAPI_Cut remainder;
    return emptyDifference(remainder, shape, stock, stop);
}

bool mirrored(const TopoDS_Shape &first, const TopoDS_Shape &last, const AxialFrame &frame,
              double station, std::stop_token stop) {
    gp_Trsf reflection;
    reflection.SetMirror(gp_Ax2(gp_Pnt(frame.axis.XYZ() * station), frame.axis));
    const auto reflected = BRepBuilderAPI_Transform(last, reflection, true).Shape();
    TopTools_ListOfShape operands;
    operands.Append(first);
    operands.Append(reflected);
    BOPAlgo_PaveFiller filler;
    filler.SetNonDestructive(true);
    filler.SetArguments(operands);
    filler.Perform();
    if (stop.stop_requested() || filler.HasErrors())
        return false;
    for (bool reverse : {false, true}) {
        BRepAlgoAPI_Cut cut(filler);
        if (!emptyDifference(cut, reverse ? reflected : first, reverse ? first : reflected, stop))
            return false;
    }
    return true;
}
bool mirroredEnds(const TopoDS_Shape &shape, const AxialFrame &frame, const CutComponents &cuts,
                  double station, double wall, double tol, std::stop_token stop) {
    const auto &b = frame.bounds;
    // Keep all geometry that an enlarged end view can show beyond the cut.
    const double width = std::hypot(b[3] - b[0], b[4] - b[1]);
    const double length =
        std::max(cuts.depth[0], cuts.depth[1]) + width / 2 + std::max(6., 3 * wall) + 4 * tol;
    if (stop.stop_requested())
        return false;
    if (!cuts.independentCuts || b[2] + length >= station)
        return mirrored(shape, shape, frame, station, stop);
    // Use both source end slabs in one boolean. Local machining near either end
    // remains part of the symmetry proof; a hole between the slabs does not.
    BRep_Builder builder;
    TopoDS_Compound endSlabs;
    builder.MakeCompound(endSlabs);
    builder.Add(endSlabs, slab(frame, {b[2] - tol, b[2] + length}));
    builder.Add(endSlabs, slab(frame, {b[5] - length, b[5] + tol}));
    BRepAlgoAPI_Common common;
    clippedBy(common, shape, endSlabs);
    if (stop.stop_requested() || !common.IsDone() || common.HasErrors())
        return false;
    std::array<TopoDS_Shape, 2> local;
    for (TopExp_Explorer solids(common.Shape(), TopAbs_SOLID); solids.More(); solids.Next()) {
        const auto &part = solids.Current();
        const auto extent = boundsIn(part, frame.toLocal);
        const std::size_t side = extent[2] < station ? 0 : 1;
        if (!local[side].IsNull() || (side == 0 && extent[5] > station))
            return false;
        local[side] = part;
    }
    return !local[0].IsNull() && !local[1].IsNull() &&
           mirrored(local[0], local[1], frame, station, stop);
}

std::optional<DrawingTubeGeometry> proveTube(const TopoDS_Shape &shape,
                                             const TopTools_IndexedMapOfShape &faces,
                                             const AxialFrame &frame,
                                             const std::vector<FaceSpan> &spans, Span span,
                                             double tol, std::stop_token stop) {
    BRepAlgoAPI_Common common;
    clippedBy(common, shape, slab(frame, span));
    if (stop.stop_requested() || !common.IsDone() || common.HasErrors())
        return {};
    TopTools_IndexedMapOfShape middleFaces;
    TopExp::MapShapes(common.Shape(), TopAbs_FACE, middleFaces);
    auto tube = drawingTubeProfile(common.Shape(), middleFaces, tol, stop);
    if (!tube || !tube->planarWalls || !tube->endFaces[0] || !tube->endFaces[1] ||
        !tube->cutsDegrees[0] || !tube->cutsDegrees[1] || *tube->cutsDegrees[0] > 1e-4 ||
        *tube->cutsDegrees[1] > 1e-4 || !tube->axis.IsParallel(frame.axis, angularTolerance) ||
        tube->longitudinalFaces.size() + 2 != static_cast<std::size_t>(middleFaces.Extent()))
        return {};
    const auto middle = boundsIn(common.Shape(), frame.toLocal);
    if (std::abs(middle[2] - span[0]) > tol || std::abs(middle[5] - span[1]) > tol)
        return {};
    const auto owners = skinOwners(common, middleFaces, faces, tube->longitudinalFaces);
    if (!owners)
        return {};
    const auto sourceId = [&](int face) { return (*owners)[static_cast<std::size_t>(face)]; };
    tube->axis = frame.axis;
    tube->sideFace = static_cast<std::size_t>(sourceId(static_cast<int>(tube->sideFace)));
    for (auto &wall : *tube->planarWalls) {
        wall.outerFace = sourceId(wall.outerFace);
        wall.innerFace = sourceId(wall.innerFace);
    }
    for (auto &face : tube->longitudinalFaces)
        face = sourceId(face);
    std::sort(tube->longitudinalFaces.begin(), tube->longitudinalFaces.end());
    if (std::adjacent_find(tube->longitudinalFaces.begin(), tube->longitudinalFaces.end()) !=
        tube->longitudinalFaces.end())
        return {};
    const auto &b = frame.bounds;
    auto components = cutComponents(shape, faces, spans, *tube, b, tol, stop);
    if (!components || !cutFromStock(shape, middleFaces(tube->endFaces[0]), frame, tol, stop))
        return {};
    DrawingTubeEndReliefs reliefs{std::move(components->rims), {}};
    if (reliefs.rimFaces[0].size() == reliefs.rimFaces[1].size() &&
        std::abs(components->depth[0] - components->depth[1]) <= tol && !stop.stop_requested()) {
        try {
            const double station = (b[2] + b[5]) / 2;
            if (mirroredEnds(shape, frame, *components, station, tube->profile.wallMm, tol, stop))
                reliefs.mirrorStation = station;
        } catch (const Standard_Failure &) {
            // A failed symmetry proof leaves two independent end views.
        }
    }
    if (stop.stop_requested())
        return {};
    tube->bores = {};
    tube->endFaces = {};
    tube->cutsDegrees = {};
    tube->endReliefs = std::move(reliefs);
    return tube;
}
} // namespace

std::optional<DrawingTubeGeometry>
drawingTubeProfileWithEndReliefs(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                                 const TopTools_IndexedMapOfShape &edges, double tolerance,
                                 std::stop_token stop) {
    // The optional proof is limited to simple solids with local machining at both ends.
    if (faces.Extent() < 10 || faces.Extent() > 128 || edges.Extent() > 512 ||
        stop.stop_requested())
        return {};
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    if (solids.Extent() != 1)
        return {};
    const double tol = std::clamp(tolerance, 1e-5, .01);
    const auto frame = rectangularStockFrame(shape, faces, tol, stop);
    if (!frame)
        return {};
    const auto &b = frame->bounds;
    std::vector<FaceSpan> spans;
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i) {
        const auto box = boundsIn(faces(i), frame->toLocal);
        const BRepAdaptor_Surface surface(TopoDS::Face(faces(i)), false);
        const bool planar = surface.GetType() == GeomAbs_Plane;
        const bool longitudinal =
            planar ? surface.Plane().Axis().Direction().IsNormal(frame->axis, angularTolerance)
                   : surface.GetType() == GeomAbs_Cylinder &&
                         surface.Cylinder().Axis().Direction().IsParallel(frame->axis,
                                                                          angularTolerance);
        unsigned outerWalls = 0;
        if (planar && longitudinal)
            for (std::size_t axis = 0; axis < 2; ++axis) {
                if (std::abs(box[axis + 3] - box[axis]) > tol)
                    continue;
                if (std::abs(box[axis] - b[axis]) <= tol)
                    outerWalls |= 1u << (2 * axis);
                if (std::abs(box[axis + 3] - b[axis + 3]) <= tol)
                    outerWalls |= 1u << (2 * axis + 1);
            }
        spans.push_back({{box[2], box[5]}, longitudinal, outerWalls});
    }
    for (const auto &span : stockSpans(spans, tol)) {
        if (stop.stop_requested())
            break;
        if (auto tube = proveTube(shape, faces, *frame, spans, span, tol, stop))
            return tube;
    }
    return {};
}
} // namespace bending::cad
