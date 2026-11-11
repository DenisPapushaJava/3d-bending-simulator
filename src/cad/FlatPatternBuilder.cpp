#include "bending/cad/FlatPatternBuilder.h"
#include "BendSectorOwnership.h"
#include "FlatCutProfileBuilder.h"
#include "FlatPatternDisplay.h"
#include "FlatWallRegions.h"
#include "ModelMeasurements.h"
#include "RouteGeometry.h"
#include "ShapeVolumeDifference.h"
#include "SheetGeometry.h"
#include "SheetOrientation.h"
#include "TrimmedBend.h"
#include "bending/cad/CadDocument.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/simulation/SingleBend.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Lin.hxx>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

namespace bending::cad {
namespace {
using namespace application;
using Wall = std::array<std::size_t, 2>;
class CadFlatPattern final : public FlatPattern {
  public:
    DocumentPtr source;
    FlatPatternInfo data;
    TopoDS_Shape display;
    TopoDS_Shape geometry;
    std::vector<std::vector<PointMm>> contours;
    std::vector<std::array<TopoDS_Shape, 2>> lines;
    SheetLayout layout;
    std::map<std::size_t, std::size_t> sourceWallOwners;
    SheetOrientationVisual orientation;
    const FlatPatternInfo &info() const noexcept override {
        return data;
    }
};
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw FlatPatternError{FlatPatternErrorCode::Cancelled, "Cancelled"};
}
void require(bool condition, const char *message) {
    if (!condition)
        throw FlatPatternError{FlatPatternErrorCode::UnsupportedGeometry, message};
}
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
double area(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p);
    return std::abs(p.Mass());
}
TopoDS_Shape runBoolean(BRepAlgoAPI_BooleanOperation &op, const TopoDS_Shape &a,
                        const TopoDS_Shape &b, double tolerance, std::stop_token stop,
                        const char *context = "Boolean verification failed") {
    cancelled(stop);
    TopTools_ListOfShape args, tools;
    args.Append(a);
    tools.Append(b);
    op.SetArguments(args);
    op.SetTools(tools);
    op.SetNonDestructive(true);
    op.SetRunParallel(false);
    op.SetFuzzyValue(tolerance);
    op.Build();
    cancelled(stop);
    require(op.IsDone() && !op.HasErrors() && BRepCheck_Analyzer(op.Shape()).IsValid(), context);
    return op.Shape();
}
template <class Op>
TopoDS_Shape boolean(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                     std::stop_token stop, const char *context = "Boolean verification failed") {
    Op op;
    return runBoolean(op, a, b, tolerance, stop, context);
}
TopoDS_Face midBendCutter(const TopoDS_Shape &sector, double thickness, double tolerance) {
    TopoDS_Face inner;
    double radius = std::numeric_limits<double>::infinity();
    for (TopExp_Explorer e(sector, TopAbs_FACE); e.More(); e.Next()) {
        const auto face = TopoDS::Face(e.Current());
        const BRepAdaptor_Surface surface(face);
        if (surface.GetType() == GeomAbs_Cylinder && surface.Cylinder().Radius() < radius) {
            inner = face;
            radius = surface.Cylinder().Radius();
        }
    }
    require(!inner.IsNull(), "Missing source cylinder for the mid-thickness section");
    const auto cylinder = BRepAdaptor_Surface(inner).Cylinder();
    double u0, u1, v0, v1;
    BRepTools::UVBounds(inner, u0, u1, v0, v1);
    // The cut tool covers the complete folded sector, including caps
    // that extend past the axial bounds of the inner skin.
    Bnd_Box box;
    BRepBndLib::AddOptimal(sector, box, false, true);
    require(!box.IsVoid() && !box.IsOpen(), "Invalid folded sector bounds");
    const auto low = box.CornerMin(), high = box.CornerMax();
    const gp_Vec axis(cylinder.Axis().Direction());
    for (double x : {low.X(), high.X()})
        for (double y : {low.Y(), high.Y()})
            for (double z : {low.Z(), high.Z()}) {
                const double v = gp_Vec(cylinder.Location(), gp_Pnt(x, y, z)).Dot(axis);
                v0 = std::min(v0, v);
                v1 = std::max(v1, v);
            }
    const Handle(Geom_CylindricalSurface) surface =
        new Geom_CylindricalSurface(cylinder.Position(), radius + thickness / 2);
    // A complete cylinder with its seam outside the open bend angle.
    const double seam = u0 - std::numbers::pi / 2;
    return BRepBuilderAPI_MakeFace(surface, seam, seam + 2 * std::numbers::pi, v0 - thickness,
                                   v1 + thickness, tolerance)
        .Face();
}
TopoDS_Face midThicknessFace(const SheetLayout &layout, const gp_Trsf &origin, double thickness,
                             double tolerance, std::stop_token stop) {
    const double joinTolerance = std::min(tolerance, 1.e-4);
    const auto plane =
        BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, thickness / 2), gp_Dir(0, 0, 1))).Face();
    TopoDS_Shape united;
    const auto singleFace = [](const TopoDS_Shape &section) {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(section, TopAbs_FACE, faces);
        require(faces.Extent() == 1, "Invalid mid-thickness stock section");
        return TopoDS::Face(faces(1));
    };
    const auto addPatch = [&](TopoDS_Face face) {
        const BRepAdaptor_Surface surface(face);
        require(surface.GetType() == GeomAbs_Plane, "Stock section is not planar");
        const auto patchPlane = surface.Plane();
        const double normal = patchPlane.Axis().Direction().Z();
        require(std::abs(normal) >= 1 - 1e-7 &&
                    std::abs(patchPlane.Location().Z() - thickness / 2) <= joinTolerance,
                "Stock section is outside the mid-thickness plane");
        if ((normal < 0) != (face.Orientation() == TopAbs_REVERSED))
            face.Reverse();
        const double patchArea = area(face);
        require(std::isfinite(patchArea) && patchArea > 1.e-5, "Empty stock section");
        if (united.IsNull()) {
            united = face;
            return;
        }
        const auto overlap = boolean<BRepAlgoAPI_Common>(united, face, joinTolerance, stop,
                                                         "Cannot check stock sections for overlap");
        require(area(overlap) <= 1.e-5, "Mid-thickness stock sections overlap");
        united = boolean<BRepAlgoAPI_Fuse>(united, face, joinTolerance, stop,
                                           "Cannot join mid-thickness stock sections");
    };
    const auto addSection = [&](const TopoDS_Shape &solid) {
        addPatch(singleFace(boolean<BRepAlgoAPI_Common>(solid, plane, joinTolerance, stop,
                                                        "Cannot section the flat stock")));
    };
    for (const auto &wall : layout.walls) {
        cancelled(stop);
        addSection(BRepBuilderAPI_Transform(wall.solid, wall.toFlat, true).Shape());
    }
    for (const auto &bend : layout.bends) {
        cancelled(stop);
        if (!bend.flatSolid.IsNull()) {
            addSection(bend.flatSolid);
            continue;
        }
        // The verified folded sector still exists even if its complete flat
        // solid could not be built. Cut the real material at mid thickness.
        const auto cutter = midBendCutter(bend.solid, thickness, joinTolerance);
        const auto section = singleFace(boolean<BRepAlgoAPI_Common>(
            bend.solid, cutter, joinTolerance, stop, "Cannot section the folded bend"));
        addPatch(flattenBendCylinderFace(section, bend.toFlat, tolerance, stop));
    }
    require(!united.IsNull(), "No mid-thickness stock sections");
    ShapeUpgrade_UnifySameDomain unify(united, true, true, true);
    unify.SetLinearTolerance(joinTolerance);
    unify.SetAngularTolerance(1.e-7);
    unify.Build();
    cancelled(stop);
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(unify.Shape(), TopAbs_FACE, faces);
    require(faces.Extent() == 1 && BRepCheck_Analyzer(unify.Shape()).IsValid(),
            "Mid-thickness sections do not form one valid face");
    gp_Trsf lower;
    lower.SetTranslation(gp_Vec(0, 0, -thickness / 2));
    return TopoDS::Face(BRepBuilderAPI_Transform(faces(1), origin * lower, true).Shape());
}
TopoDS_Face face(const TopTools_IndexedMapOfShape &faces, std::size_t i) {
    require(i && i <= static_cast<std::size_t>(faces.Extent()), "Invalid face reference");
    return TopoDS::Face(faces(static_cast<int>(i)));
}
gp_Pln plane(const TopTools_IndexedMapOfShape &faces, std::size_t i) {
    BRepAdaptor_Surface s(face(faces, i));
    require(s.GetType() == GeomAbs_Plane, "A wall is not planar");
    return s.Plane();
}
gp_Pnt projected(gp_Pnt p, const gp_Pln &plane) {
    const gp_Vec n(plane.Axis().Direction());
    return p.Translated(-n * gp_Vec(plane.Location(), p).Dot(n));
}
Wall wall(const RecognizedBend &b, std::size_t side) {
    Wall w{b.flangeFaces[side * 2], b.flangeFaces[side * 2 + 1]};
    std::sort(w.begin(), w.end());
    return w;
}
// Two distinct cylindrical patches may join the same continuous pair of walls
// on opposite ends of a relief. They share a rigid wall placement, not material.
bool disjointCoaxialBends(const RecognizedBend &a, const RecognizedBend &b, double tolerance) {
    const auto start = point(a.axisStart), end = point(a.axisEnd);
    const auto otherStart = point(b.axisStart), otherEnd = point(b.axisEnd);
    const gp_Dir axis(gp_Vec(start, end));
    const gp_Lin line(start, axis);
    if (!axis.IsParallel(gp_Dir(gp_Vec(otherStart, otherEnd)), 1.e-7) ||
        line.Distance(otherStart) > tolerance || line.Distance(otherEnd) > tolerance ||
        std::abs(a.innerRadiusMm - b.innerRadiusMm) > tolerance ||
        std::abs(a.angleRadians - b.angleRadians) > 1.e-7)
        return false;
    const double s = gp_Vec(start, otherStart).Dot(gp_Vec(axis));
    const double e = gp_Vec(start, otherEnd).Dot(gp_Vec(axis));
    return std::max(s, e) <= 0 || std::min(s, e) >= start.Distance(end);
}

struct WallGraph {
    std::map<Wall, std::vector<std::size_t>> neighbours;
    std::size_t linkCount;

    bool connected(const std::vector<RecognizedBend> &bends) const {
        std::vector<Wall> pending{neighbours.begin()->first};
        std::set<Wall> visited{pending.front()};
        for (std::size_t i = 0; i < pending.size(); ++i)
            for (const auto index : neighbours.at(pending[i])) {
                const auto first = wall(bends[index], 0), second = wall(bends[index], 1);
                const auto other = first == pending[i] ? second : first;
                if (visited.insert(other).second)
                    pending.push_back(other);
            }
        return visited.size() == neighbours.size();
    }
};

WallGraph makeWallGraph(const std::vector<RecognizedBend> &bends, double thickness,
                        double tolerance, bool geometryOnly, std::stop_token stop) {
    WallGraph graph{};
    std::map<std::array<Wall, 2>, std::vector<std::size_t>> links;
    for (std::size_t i = 0; i < bends.size(); ++i) {
        cancelled(stop);
        const auto &b = bends[i];
        require(std::abs(b.thicknessMm - thickness) <= tolerance,
                "Requires constant thickness and straight bends");
        auto first = wall(b, 0), second = wall(b, 1);
        require(first != second, "A bend joins a wall to itself");
        if (second < first)
            std::swap(first, second);
        auto &segments = links[{first, second}];
        for (const auto previous : segments)
            require(!geometryOnly && disjointCoaxialBends(bends[previous], b, tolerance),
                    "Bend zones sharing walls do not form a supported split link");
        segments.push_back(i);
        graph.neighbours[first].push_back(i);
        graph.neighbours[second].push_back(i);
    }
    graph.linkCount = links.size();
    return graph;
}

bool joinCoplanarWallFaces(const TopoDS_Shape &original, TopoDS_Shape &source,
                           TopTools_IndexedMapOfShape &faces, std::vector<RecognizedBend> &bends,
                           std::stop_token stop) {
    constexpr double linearTolerance = 1.e-7;
    constexpr double angularTolerance = 1.e-7;
    std::vector<gp_Pln> wallPlanes;
    for (const auto &bend : bends)
        for (const auto id : bend.flangeFaces)
            wallPlanes.push_back(plane(faces, id));

    TopTools_IndexedDataMapOfShapeListOfShape adjacency;
    TopExp::MapShapesAndAncestors(source, TopAbs_EDGE, TopAbs_FACE, adjacency);
    const auto coplanar = [](const gp_Pln &a, const gp_Pln &b) {
        return a.Axis().Direction().IsParallel(b.Axis().Direction(), angularTolerance) &&
               a.Distance(b.Location()) <= linearTolerance &&
               b.Distance(a.Location()) <= linearTolerance;
    };
    const auto canRemoveSeam = [&](const TopTools_ListOfShape &owners) {
        if (owners.Extent() != 2 || owners.First().IsSame(owners.Last()))
            return false;
        const BRepAdaptor_Surface first(TopoDS::Face(owners.First()));
        const BRepAdaptor_Surface second(TopoDS::Face(owners.Last()));
        if (first.GetType() != GeomAbs_Plane || second.GetType() != GeomAbs_Plane)
            return false;
        const auto a = first.Plane(), b = second.Plane();
        const bool sameOrientation = owners.First().Orientation() == owners.Last().Orientation();
        return coplanar(a, b) &&
               (a.Axis().Direction().Dot(b.Axis().Direction()) > 0) == sameOrientation &&
               std::any_of(wallPlanes.begin(), wallPlanes.end(),
                           [&](const gp_Pln &anchor) { return coplanar(a, anchor); });
    };
    TopTools_MapOfShape protectedEdges;
    for (int i = 1; i <= adjacency.Extent(); ++i) {
        cancelled(stop);
        if (!canRemoveSeam(adjacency.FindFromIndex(i)))
            protectedEdges.Add(adjacency.FindKey(i));
    }
    if (protectedEdges.Extent() == adjacency.Extent())
        return false;
    ShapeUpgrade_UnifySameDomain unify(source, false, true, false);
    unify.KeepShapes(protectedEdges);
    unify.SetLinearTolerance(linearTolerance);
    unify.SetAngularTolerance(angularTolerance);
    unify.Build();
    cancelled(stop);
    const auto &joined = unify.Shape();
    TopTools_IndexedMapOfShape joinedFaces, joinedEdges;
    TopExp::MapShapes(joined, TopAbs_FACE, joinedFaces);
    TopExp::MapShapes(joined, TopAbs_EDGE, joinedEdges);
    if (joinedFaces.Extent() >= faces.Extent() || !BRepCheck_Analyzer(joined).IsValid())
        return false;
    for (TopTools_MapOfShape::Iterator it(protectedEdges); it.More(); it.Next())
        if (!joinedEdges.Contains(it.Value()))
            return false;

    auto mapped = bends;
    const auto remap = [&](std::size_t &id, bool wallSkin) {
        const auto old = face(faces, id);
        int index = joinedFaces.FindIndex(old);
        if (!index && wallSkin) {
            const auto &modified = unify.History()->Modified(old);
            if (modified.Extent() == 1)
                index = joinedFaces.FindIndex(modified.First());
        }
        if (!index)
            return false;
        id = static_cast<std::size_t>(index);
        return true;
    };
    for (auto &bend : mapped) {
        if (!remap(bend.innerFace, false) || !remap(bend.outerFace, false))
            return false;
        for (auto &id : bend.flangeFaces)
            if (!remap(id, true))
                return false;
    }
    // The input to unification can share unchanged topology with the result.
    // Compare against the document's independent source instead.
    if (!sameVolumeWithin(original, joined, linearTolerance, 1.e-8, stop))
        return false;
    source = joined;
    faces = std::move(joinedFaces);
    bends = std::move(mapped);
    return true;
}

gp_Trsf placementMap(gp_Pnt source, gp_Dir axis, gp_Dir across, gp_Pnt target, gp_Dir flatAxis,
                     gp_Dir flatAcross) {
    const gp_Vec normal = gp_Vec(axis).Crossed(gp_Vec(across));
    const gp_Vec flatNormal = gp_Vec(flatAxis).Crossed(gp_Vec(flatAcross));
    double m[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = flatAxis.Coord(i + 1) * axis.Coord(j + 1) +
                      flatAcross.Coord(i + 1) * across.Coord(j + 1) +
                      flatNormal.Coord(i + 1) * normal.Coord(j + 1);
    const auto shift = [&](int i) {
        return target.Coord(i + 1) - m[i][0] * source.X() - m[i][1] * source.Y() -
               m[i][2] * source.Z();
    };
    gp_Trsf result;
    result.SetValues(m[0][0], m[0][1], m[0][2], shift(0), m[1][0], m[1][1], m[1][2], shift(1),
                     m[2][0], m[2][1], m[2][2], shift(2));
    return result;
}
BoundsMm bounds(const TopoDS_Shape &s) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, false, false);
    require(!b.IsVoid() && !b.IsOpen(), "Invalid bounds");
    BoundsMm result{};
    b.Get(result.minimum.x, result.minimum.y, result.minimum.z, result.maximum.x, result.maximum.y,
          result.maximum.z);
    return result;
}
TopoDS_Shape extruded(const TopoDS_Face &f, gp_Vec v) {
    auto result = BRepPrimAPI_MakePrism(f, v, true).Shape();
    GProp_GProps p;
    BRepGProp::VolumeProperties(result, p);
    if (p.Mass() < 0)
        result.Reverse();
    require(BRepCheck_Analyzer(result).IsValid(), "Invalid wall prism");
    return result;
}
TopoDS_Shape strip(gp_Pnt start, gp_Pnt end, gp_Vec offset) {
    BRepBuilderAPI_MakePolygon wire;
    wire.Add(start);
    wire.Add(end);
    wire.Add(end.Translated(offset));
    wire.Add(start.Translated(offset));
    wire.Close();
    require(wire.IsDone(), "Invalid bend strip");
    return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
}
TopoDS_Shape bendSolid(const RecognizedBend &b, const TopTools_IndexedMapOfShape &faces,
                       double tolerance, bool railAngle, double padding = 0) {
    const auto f = face(faces, b.innerFace);
    BRepAdaptor_Surface s(f);
    require(s.GetType() == GeomAbs_Cylinder, "Only cylindrical bends are supported");
    double u0, u1, v0, v1;
    BRepTools::UVBounds(f, u0, u1, v0, v1);
    const auto cylinder = s.Cylinder();
    if (railAngle) {
        // Use the validated rail angle for the partition envelope. The
        // actual skins and cut edges are recovered from the source below.
        const auto railU = [&](std::size_t id, double referenceAngle) {
            gp_Vec normal(plane(faces, id).Axis().Direction());
            const auto radial = gp_Vec(cylinder.Location(), s.Value(referenceAngle, 0));
            if (normal.Dot(radial) < 0)
                normal.Reverse();
            const double angle = std::atan2(normal.Dot(gp_Vec(cylinder.YAxis().Direction())),
                                            normal.Dot(gp_Vec(cylinder.XAxis().Direction())));
            return referenceAngle + std::remainder(angle - referenceAngle, 2 * std::numbers::pi);
        };
        auto first = railU(b.flangeFaces[0], u0), last = railU(b.flangeFaces[2], u1);
        if (std::abs(first - u0) * cylinder.Radius() > tolerance)
            std::tie(first, last) =
                std::pair{railU(b.flangeFaces[2], u0), railU(b.flangeFaces[0], u1)};
        require(std::abs(first - u0) * cylinder.Radius() <= tolerance &&
                    std::abs(last - u1) * cylinder.Radius() <= tolerance,
                "The compound end does not agree with its tangent rails");
        u0 = first;
        u1 = last;
    }
    if (b.trimmedEnds) {
        const gp_Vec axis(cylinder.Axis().Direction());
        const auto at = [&](gp_Pnt p) { return gp_Vec(cylinder.Location(), p).Dot(axis); };
        const double first = at(point(b.axisStart)), last = at(point(b.axisEnd));
        const double minimum = std::min(first, last), maximum = std::max(first, last);
        BRepAdaptor_Surface outer(face(faces, b.outerFace));
        double a0, a1, z0, z1;
        BRepTools::UVBounds(face(faces, b.outerFace), a0, a1, z0, z1);
        const double outerFirst = at(outer.Value(a0, z0)), outerLast = at(outer.Value(a0, z1));
        require(v0 >= minimum - tolerance * 20 && v1 <= maximum + tolerance * 20 &&
                    std::min(outerFirst, outerLast) >= minimum - tolerance * 20 &&
                    std::max(outerFirst, outerLast) <= maximum + tolerance * 20,
                "A trimmed cylinder is outside its complete axial span");
        v0 = minimum;
        v1 = maximum;
    }
    require(std::abs((u1 - u0) - b.angleRadians) < 1e-6 &&
                std::abs((v1 - v0) - b.lengthMm) < tolerance * 20 &&
                (b.trimmedEnds || std::abs(area(f) - b.innerRadiusMm * (u1 - u0) * (v1 - v0)) <
                                      std::max(1e-5, area(f) * 1e-7)),
            "A bend has openings, cut edges or an incomplete cylindrical surface");
    // The envelope bounds the cap intersection; the cylindrical faces are
    // copied from the source. Keep the cutting tool clear of their boundaries.
    v0 -= 10 * padding;
    v1 += 10 * padding;
    const auto p = [&](double u, double offset) {
        auto inner = s.Value(u, v0);
        // Projection onto the axial line, not onto its normal plane.
        const auto axial = cylinder.Location().Translated(
            gp_Vec(cylinder.Axis().Direction()) *
            gp_Vec(cylinder.Location(), inner).Dot(gp_Vec(cylinder.Axis().Direction())));
        return inner.Translated(gp_Vec(axial, inner) * (offset / b.innerRadiusMm));
    };
    const auto arc = [&](double offset) {
        return BRepBuilderAPI_MakeEdge(
                   GC_MakeArcOfCircle(p(u0, offset), p((u0 + u1) / 2, offset), p(u1, offset))
                       .Value())
            .Edge();
    };
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(p(u0, -padding), p(u0, b.thicknessMm + padding)).Edge());
    wire.Add(arc(b.thicknessMm + padding));
    wire.Add(BRepBuilderAPI_MakeEdge(p(u1, b.thicknessMm + padding), p(u1, -padding)).Edge());
    wire.Add(TopoDS::Edge(arc(-padding).Reversed()));
    return extruded(BRepBuilderAPI_MakeFace(wire.Wire()).Face(),
                    gp_Vec(cylinder.Axis().Direction()) * (v1 - v0));
}
// A full outer tangency rail must belong to this wall. A local point on one
// side alone cannot prove attachment along the complete axial span.
int materialSide(const TopoDS_Face &wallFace, const gp_Pln &wallPlane, const RecognizedBend &bend,
                 gp_Dir along, double tolerance, std::stop_token stop) {
    const auto first = projected(point(bend.axisStart), wallPlane);
    const auto second = projected(point(bend.axisEnd), wallPlane);
    const auto rail = BRepBuilderAPI_MakeEdge(first, second).Shape();
    const auto common = boolean<BRepAlgoAPI_Common>(rail, BRepTools::OuterWire(wallFace), tolerance,
                                                    stop, "Tangency rail intersection failed");
    GProp_GProps properties;
    BRepGProp::LinearProperties(common, properties, true);
    const double minimumRail = bend.trimmedEnds ? bend.minimumStraightLengthMm : bend.lengthMm;
    require(properties.Mass() + std::max(tolerance * 20, bend.lengthMm * 1e-7) >= minimumRail,
            "A complete tangency rail is not on the outer boundary of its wall");
    const double inset =
        std::max(tolerance * 50, std::min(bend.thicknessMm, bend.innerRadiusMm) * .05);
    int side = 0;
    for (double fraction : {.25, .5, .75}) {
        cancelled(stop);
        const auto center = first.Translated(gp_Vec(first, second) * fraction);
        const auto classify = [&](double sign) {
            return BRepClass_FaceClassifier(
                       wallFace, center.Translated(gp_Vec(along) * (inset * sign)), tolerance)
                .State();
        };
        const auto positive = classify(1), negative = classify(-1);
        const int current = positive == TopAbs_IN && negative == TopAbs_OUT   ? 1
                            : negative == TopAbs_IN && positive == TopAbs_OUT ? -1
                                                                              : 0;
        require(current && (!side || side == current),
                "The material side of a tangency rail is ambiguous");
        side = current;
    }
    return side;
}
} // namespace
bool OcctFlatPatternAccess::belongsTo(const FlatPattern &pattern, const Document &document) {
    const auto *native = dynamic_cast<const CadFlatPattern *>(&pattern);
    return native && native->source.get() == &document;
}
const TopoDS_Shape &OcctFlatPatternAccess::shape(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).display;
}
const TopoDS_Shape &OcctFlatPatternAccess::geometry(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).geometry;
}
const std::vector<std::vector<PointMm>> &
OcctFlatPatternAccess::openingContours(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).contours;
}
const std::vector<std::array<TopoDS_Shape, 2>> &
OcctFlatPatternAccess::bendLines(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).lines;
}
const SheetLayout &OcctFlatPatternAccess::layout(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).layout;
}
const gp_Trsf *OcctFlatPatternAccess::sourceWallToFlat(const FlatPattern &p, std::size_t faceId) {
    const auto &flat = dynamic_cast<const CadFlatPattern &>(p);
    const auto found = flat.sourceWallOwners.find(faceId);
    return found == flat.sourceWallOwners.end() ? nullptr
                                                : &flat.layout.walls[found->second].toFlat;
}
const SheetOrientationVisual &OcctFlatPatternAccess::orientation(const FlatPattern &p) {
    return dynamic_cast<const CadFlatPattern &>(p).orientation;
}
static FlatPatternPtr buildPlanarCutStock(const std::shared_ptr<const CadDocument> &document,
                                          const FlatPatternRequest &request, std::stop_token stop) {
    const auto &metadata = document->metadata();
    const double tolerance = std::clamp(metadata.bendAnalysis.lengthToleranceMm, 1.e-6, 1.e-4);
    const auto source =
        BRepBuilderAPI_Copy(OcctDocumentAccess::shape(*document), true, false).Shape();
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(source, TopAbs_SOLID, solids);
    require(solids.Extent() == 1, "Cutting requires one planar solid");

    TopoDS_Face skin;
    double skinArea = 0;
    for (TopExp_Explorer e(source, TopAbs_FACE); e.More(); e.Next()) {
        cancelled(stop);
        const auto candidate = TopoDS::Face(e.Current());
        if (BRepAdaptor_Surface(candidate, false).GetType() != GeomAbs_Plane)
            continue;
        const double candidateArea = area(candidate);
        if (candidateArea > skinArea) {
            skinArea = candidateArea;
            skin = candidate;
        }
    }
    require(!skin.IsNull(), "No planar cutting face");
    const auto surface = BRepAdaptor_Surface(skin, false).Plane();
    for (TopExp_Explorer e(source, TopAbs_FACE); e.More(); e.Next()) {
        cancelled(stop);
        const auto candidate = TopoDS::Face(e.Current());
        const BRepAdaptor_Surface candidateSurface(candidate, false);
        if (candidateSurface.GetType() == GeomAbs_Plane &&
            !surface.Axis().Direction().IsParallel(candidateSurface.Plane().Axis().Direction(),
                                                   1.e-7))
            require(skinArea - area(candidate) > 1.e-7 * skinArea,
                    "The cutting plane is ambiguous");
    }
    auto inward = surface.Axis().Direction();
    if (skin.Orientation() == TopAbs_FORWARD)
        inward.Reverse();
    gp_Trsf toPlane;
    toPlane.SetTransformation(gp_Ax3(surface.Location(), inward, surface.XAxis().Direction()));
    const auto placed = BRepBuilderAPI_Transform(source, toPlane, false).Shape();
    const auto box = bounds(placed);
    const double thickness = box.maximum.z;
    require(std::abs(box.minimum.z) <= tolerance && thickness > tolerance,
            "The cutting face must bound a solid of positive thickness");
    if (request.manualThicknessMm && std::abs(*request.manualThicknessMm - thickness) > tolerance)
        throw FlatPatternError{FlatPatternErrorCode::ThicknessConflict,
                               "Thickness differs from CAD"};
    const auto prism = extruded(skin, gp_Vec(inward) * thickness);
    constexpr double volumeBudget = 1.e-9;
    auto profile = skin;
    auto profileToPlane = toPlane;
    if (!sameVolumeWithin(source, prism, tolerance, volumeBudget, stop)) {
        // STEP may split a planar skin into several faces. A section proposes
        // the complete perimeter; it is accepted only if it reconstructs the stock.
        constexpr auto unsupported =
            "The solid does not have one through-cut profile across its thickness";
        const auto middle =
            BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, thickness / 2), gp_Dir(0, 0, 1))).Face();
        const auto section =
            boolean<BRepAlgoAPI_Common>(placed, middle, tolerance, stop, unsupported);
        TopTools_IndexedMapOfShape sectionFaces;
        TopExp::MapShapes(section, TopAbs_FACE, sectionFaces);
        require(sectionFaces.Extent() == 1, unsupported);
        profile = TopoDS::Face(sectionFaces(1));
        profileToPlane.SetTranslation(gp_Vec(0, 0, -thickness / 2));
        const auto bottom =
            TopoDS::Face(BRepBuilderAPI_Transform(profile, profileToPlane, true).Shape());
        const auto fullPrism = extruded(bottom, gp_Vec(0, 0, thickness));
        const bool complete = sameVolumeWithin(placed, fullPrism, tolerance, volumeBudget, stop);
        cancelled(stop);
        require(complete, unsupported);
    }

    gp_Trsf origin;
    origin.SetTranslation(gp_Vec(-box.minimum.x, -box.minimum.y, 0));
    const auto cutFace =
        TopoDS::Face(BRepBuilderAPI_Transform(profile, origin * profileToPlane, true).Shape());
    auto result = std::make_shared<CadFlatPattern>();
    result->source = document;
    result->data.settings = request.settings;
    result->data.thicknessMm = thickness;
    result->data.bounds = {
        {0, 0, 0}, {box.maximum.x - box.minimum.x, box.maximum.y - box.minimum.y, thickness}};
    result->data.cutProfile =
        buildFlatCutProfile(cutFace, source, {}, {}, {}, origin, thickness, tolerance, stop);
    cancelled(stop);
    require(bool(result->data.cutProfile), "The planar cutting contour is not supported");
    result->geometry = BRepBuilderAPI_Transform(placed, origin, false).Shape();
    result->display = result->geometry;
    return result;
}
static FlatPatternResult buildSheet(DocumentPtr document, FlatPatternRequest request,
                                    std::stop_token stop, bool geometryOnly) {
    try {
        cancelled(stop);
        const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
        require(native != nullptr, "Unsupported document type");
        if (!geometryOnly && isPlanarCutCandidate(native->metadata().bendAnalysis))
            return buildPlanarCutStock(native, request, stop);
        if (!core::validUnfoldSettings(request.settings) ||
            (!request.settings.manualK &&
             request.settings.material == core::SheetMaterial::Unknown))
            return FlatPatternError{FlatPatternErrorCode::MissingMaterial,
                                    "Choose a material or set K"};
        const bool cutoutsIgnored =
            native->metadata().closedCutouts.status == ClosedCutoutStatus::Filled;
        const auto &analysis = OcctDocumentAccess::simulationBends(*native);
        require(analysis.interiorContourDiagnostics.empty(),
                "Openings in a bend zone are not yet supported for unfolding");
        require(analysis.status == BendRecognitionStatus::Completed && !analysis.bends.empty(),
                "Requires recognized straight bends in a connected open sheet");
        const auto segmentLimit = geometryOnly ? maximumSheetBends : maximumFlatBendSegments;
        require(analysis.bends.size() <= segmentLimit, "Sheet bend segment limit exceeded");
        auto bends = analysis.bends;
        const double tolerance = std::max(1e-6, analysis.lengthToleranceMm);
        const double t = bends.front().thicknessMm;
        if (request.manualThicknessMm && (!std::isfinite(*request.manualThicknessMm) ||
                                          std::abs(*request.manualThicknessMm - t) > tolerance))
            return FlatPatternError{FlatPatternErrorCode::ThicknessConflict,
                                    "Thickness differs from CAD"};
        const auto &simulationShape = OcctDocumentAccess::simulationShape(*native);
        auto source = BRepBuilderAPI_Copy(simulationShape, true, false).Shape();
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(source, TopAbs_FACE, faces);
        auto axis = gp_Dir(gp_Vec(point(bends.front().axisStart), point(bends.front().axisEnd)));
        auto wallGraph = makeWallGraph(bends, t, tolerance, geometryOnly, stop);
        // A closed path around a relief can unfold without a seam if it
        // independently maps every shared wall to the same flat placement.
        // Geometry used for independent-motion simulation still needs a tree.
        constexpr auto disconnected = "Closed or disconnected sheet graph";
        bool joinedWallFaces = false;
        if (geometryOnly) {
            require(wallGraph.neighbours.size() == wallGraph.linkCount + 1, disconnected);
        } else if (!wallGraph.connected(bends)) {
            joinedWallFaces = joinCoplanarWallFaces(simulationShape, source, faces, bends, stop);
            cancelled(stop);
            require(joinedWallFaces, disconnected);
            wallGraph = makeWallGraph(bends, t, tolerance, false, stop);
            require(wallGraph.connected(bends), disconnected);
        }
        require(wallGraph.linkCount <= maximumSheetBends, "Sheet bend link limit exceeded");
        const double sourceVolume = volume(source);
        const double budget = std::max(1e-4, sourceVolume * 1e-7);
        const auto &graph = wallGraph.neighbours;
        auto root = graph.begin()->first;
        double rootArea = 0;
        for (const auto &[w, neighbours] : graph) {
            (void)neighbours;
            const double candidate = area(face(faces, w[0]));
            if (candidate > rootArea) {
                root = w;
                rootArea = candidate;
            }
        }
        const auto rootPlane = plane(faces, root[0]), rootOther = plane(faces, root[1]);
        const gp_Vec rootNormal(rootPlane.Axis().Direction());
        const auto toOther =
            rootNormal * gp_Vec(rootPlane.Location(), rootOther.Location()).Dot(rootNormal);
        require(toOther.Magnitude() > tolerance, "Degenerate reference wall");
        std::map<Wall, gp_Trsf> placements; // Reference surface of each wall maps to Z=0.
        if (std::abs(rootNormal.Dot(gp_Vec(axis))) > 1.e-7) {
            const auto &reference = bends[graph.at(root).front()];
            axis = gp_Dir(gp_Vec(point(reference.axisStart), point(reference.axisEnd)));
        }
        require(std::abs(rootNormal.Dot(gp_Vec(axis))) < 1.e-7,
                "Reference bend axis is outside its wall plane");
        placements.emplace(root, placementMap(projected(gp_Pnt(0, 0, 0), rootPlane), axis,
                                              gp_Dir(toOther.Crossed(gp_Vec(axis))),
                                              gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)));
        std::vector<Wall> pending{root};
        std::vector<bool> used(bends.size());
        auto result = std::make_shared<CadFlatPattern>();
        result->layout.usesJoinedWallFaces = joinedWallFaces;
        result->source = document;
        result->data.settings = request.settings;
        result->data.thicknessMm = t;
        result->data.cutoutsIgnored = cutoutsIgnored;
        // Reserve wall slots in traversal order so links remain stable as children
        // are discovered. All solid/mapping fields are filled before publication.
        std::map<Wall, std::size_t> wallIndices{{root, 0}};
        TopoDS_Compound original;
        BRep_Builder builder;
        builder.MakeCompound(original);
        std::vector<TopoDS_Shape> patches;
        std::vector<FlatDisplayRegion> regions;
        std::vector<std::size_t> wallRegions;
        bool nonRadialEnds = false;
        bool partitionedSectors = false;
        bool sourceCaps = false;
        TopTools_IndexedDataMapOfShapeListOfShape sourceAdjacency;
        for (std::size_t wi = 0; wi < pending.size(); ++wi) {
            cancelled(stop);
            const auto w = pending[wi];
            const auto &placement = placements.at(w);
            const auto pa = plane(faces, w[0]), pb = plane(faces, w[1]);
            require(pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-7) &&
                        std::abs(pa.Distance(pb.Location()) - t) <= tolerance,
                    "Inconsistent wall thickness");
            const gp_Vec n(pa.Axis().Direction());
            const auto wallSolid =
                extruded(face(faces, w[0]), n * gp_Vec(pa.Location(), pb.Location()).Dot(n));
            builder.Add(original, wallSolid);
            const auto mappedThickness =
                gp_Vec(pa.Location(), pb.Location()).Transformed(placement);
            const double zShift = mappedThickness.Z() < 0 ? t : 0;
            const auto wallMap = [placement, zShift](gp_Pnt p) {
                return p.Transformed(placement).Translated(gp_Vec(0, 0, zShift));
            };
            wallRegions.push_back(regions.size());
            regions.push_back({wallSolid, wallMap});
            gp_Trsf wallToFlat = placement;
            gp_Trsf thicknessShift;
            thicknessShift.SetTranslation(gp_Vec(0, 0, zShift));
            wallToFlat = thicknessShift * wallToFlat;
            if (result->layout.walls.size() <= wi)
                result->layout.walls.resize(wi + 1);
            result->layout.walls[wi] = {w, wallSolid, wallToFlat};
            patches.push_back(BRepBuilderAPI_Transform(face(faces, w[0]), placement, true).Shape());
            const auto flatBounds = bounds(patches.back());
            require(std::max(std::abs(flatBounds.minimum.z), std::abs(flatBounds.maximum.z)) <=
                        tolerance,
                    "A wall placement is outside the common flat plane");
            for (auto index : graph.at(w)) {
                if (used[index])
                    continue;
                const auto &b = bends[index];
                const auto child = wall(b, 0) == w ? wall(b, 1) : wall(b, 0);
                const gp_Dir bendAxis(gp_Vec(point(b.axisStart), point(b.axisEnd)));
                require(std::abs(pa.Axis().Direction().Dot(bendAxis)) < 1.e-7,
                        "A bend axis is outside its parent wall plane");
                gp_Dir outward(gp_Vec(bendAxis).Crossed(gp_Vec(pa.Axis().Direction())));
                if (materialSide(face(faces, w[0]), pa, b, outward, tolerance, stop) > 0)
                    outward.Reverse();
                const auto childPlane = plane(faces, child[0]);
                require(std::abs(childPlane.Axis().Direction().Dot(bendAxis)) < 1.e-7,
                        "A bend axis is outside its child wall plane");
                gp_Dir childAlong(gp_Vec(bendAxis).Crossed(gp_Vec(childPlane.Axis().Direction())));
                if (materialSide(face(faces, child[0]), childPlane, b, childAlong, tolerance,
                                 stop) < 0)
                    childAlong.Reverse();
                require(std::abs(std::acos(std::clamp(outward.Dot(childAlong), -1., 1.)) -
                                 b.angleRadians) < 1.e-6,
                        "Wall directions do not match the measured bend");
                const gp_Dir flatAxis(gp_Vec(bendAxis).Transformed(placement));
                const gp_Dir flatAcross(gp_Vec(outward).Transformed(placement));
                require(std::abs(flatAxis.Z()) < 1.e-7 && std::abs(flatAcross.Z()) < 1.e-7,
                        "A bend does not map into the flat plane");
                auto start = projected(point(b.axisStart), pa).Transformed(placement);
                auto end = projected(point(b.axisEnd), pa).Transformed(placement);
                start.SetZ(0);
                end.SetZ(0);
                const double k = *core::unfoldK(request.settings, b.innerRadiusMm, t);
                const auto ba = core::bendAllowance(b.innerRadiusMm, t, b.angleRadians, k);
                require(ba.has_value(), "Invalid bend parameters");
                const gp_Vec offset = gp_Vec(flatAcross) * *ba;
                const auto childStart = projected(point(b.axisStart), childPlane);
                const auto target = start.Translated(offset);
                if (const auto found = placements.find(child); found == placements.end()) {
                    placements.emplace(child, placementMap(childStart, bendAxis, childAlong, target,
                                                           flatAxis, flatAcross));
                    pending.push_back(child);
                    wallIndices.emplace(child, pending.size() - 1);
                } else {
                    const auto &shared = found->second;
                    const auto childEnd = projected(point(b.axisEnd), childPlane);
                    require(
                        childStart.Transformed(shared).Distance(target) <= tolerance &&
                            childEnd.Transformed(shared).Distance(end.Translated(offset)) <=
                                tolerance &&
                            (gp_Vec(bendAxis).Transformed(shared) - gp_Vec(flatAxis)).Magnitude() <=
                                1.e-7 &&
                            (gp_Vec(childAlong).Transformed(shared) - gp_Vec(flatAcross))
                                    .Magnitude() <= 1.e-7,
                        "Multiple bend paths disagree on the wall placement");
                }
                used[index] = true;
                patches.push_back(strip(start, end, offset));
                const double turn =
                    gp_Vec(bendAxis).Dot(gp_Vec(outward).Crossed(gp_Vec(childAlong)));
                const double side = gp_Vec(flatAxis).Crossed(gp_Vec(flatAcross)).Z();
                result->data.bends.push_back({index,
                                              {start.X(), start.Y(), 0},
                                              {end.X(), end.Y(), 0},
                                              {offset.X(), offset.Y(), 0},
                                              b.angleRadians,
                                              turn * side >= 0 ? 1 : -1,
                                              b.innerRadiusMm,
                                              k,
                                              *ba});
                bool skinOnly = false;
                if (b.trimmedEnds && !geometryOnly) {
                    double u0, u1, v0, v1;
                    BRepTools::UVBounds(face(faces, b.innerFace), u0, u1, v0, v1);
                    skinOnly =
                        std::abs(u1 - u0 - b.angleRadians) >= 1e-6 &&
                        std::any_of(graph.at(w).begin(), graph.at(w).end(), [&](std::size_t other) {
                            return other != index && (wall(bends[other], 0) == child ||
                                                      wall(bends[other], 1) == child);
                        });
                }
                TopoDS_Shape sector;
                const auto useSourceCaps = [&] {
                    if (geometryOnly || !b.trimmedEnds || stop.stop_requested())
                        return false;
                    if (sourceAdjacency.IsEmpty())
                        TopExp::MapShapesAndAncestors(source, TopAbs_EDGE, TopAbs_FACE,
                                                      sourceAdjacency);
                    const auto padded = bendSolid(b, faces, tolerance, skinOnly,
                                                  std::min(.05, b.innerRadiusMm / 10));
                    auto candidate = sectorFromSourceCaps(
                        sourceAdjacency, padded,
                        {face(faces, b.innerFace), face(faces, b.outerFace)}, tolerance, stop);
                    if (candidate.IsNull())
                        return false;
                    sector = std::move(candidate);
                    skinOnly = true;
                    sourceCaps = partitionedSectors = nonRadialEnds = true;
                    result->layout.hasPresentationOnlyBends = true;
                    return true;
                };
                try {
                    const auto fullSector = bendSolid(b, faces, tolerance, skinOnly);
                    BRepAlgoAPI_Common intersection;
                    intersection.SetToFillHistory(true);
                    sector = b.trimmedEnds && !skinOnly
                                 ? runBoolean(intersection, fullSector, source, tolerance, stop)
                                 : fullSector;
                    if (skinOnly) {
                        const auto owned = ownedBendSector(
                            source, fullSector,
                            {face(faces, b.innerFace), face(faces, b.outerFace)}, tolerance, stop,
                            BendSectorSelection::PreserveSourceSkins);
                        cancelled(stop);
                        require(owned.has_value(),
                                "Cannot retain both complete compound-end skins");
                        sector = *owned;
                        partitionedSectors = true;
                        nonRadialEnds = true;
                        result->layout.hasPresentationOnlyBends = true;
                    } else if (b.trimmedEnds) {
                        TopTools_IndexedMapOfShape components;
                        TopExp::MapShapes(sector, TopAbs_SOLID, components);
                        if (components.Extent() > 1) {
                            const auto owned = selectOwnedBendSector(
                                intersection, {face(faces, b.innerFace), face(faces, b.outerFace)},
                                tolerance, stop);
                            cancelled(stop);
                            require(owned.has_value(),
                                    "Cannot assign both complete bend skins to one sector");
                            sector = *owned;
                            partitionedSectors = true;
                        } else if (components.Extent() == 1) {
                            int cylinders = 0;
                            for (TopExp_Explorer ex(sector, TopAbs_FACE); ex.More(); ex.Next())
                                cylinders +=
                                    BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType() ==
                                    GeomAbs_Cylinder;
                            if (cylinders == 1) {
                                // Coincident envelope geometry can mask an analytic source skin.
                                const auto owned = ownedBendSector(
                                    source, fullSector,
                                    {face(faces, b.innerFace), face(faces, b.outerFace)}, tolerance,
                                    stop, BendSectorSelection::PreserveSourceSkins);
                                cancelled(stop);
                                if (owned)
                                    sector = *owned;
                            }
                        }
                        bool nonRadial = false;
                        const auto radial =
                            geometryOnly ? mapTrimmedBend(
                                               sector, [](gp_Pnt p) { return p; }, 1, tolerance,
                                               nullptr, stop, &nonRadial)
                                         : rebuildTrimmedBendForUnfoldCheck(sector, tolerance, stop,
                                                                            nonRadial);
                        nonRadialEnds |= nonRadial;
                        double missing = 0, excess = 0;
                        const bool checked = checkDirectedVolumeDifferences(
                            sector, radial, tolerance, stop, [&](bool reverse, double residual) {
                                (reverse ? excess : missing) = residual;
                                return true;
                            });
                        cancelled(stop);
                        require(checked, "Boolean verification failed");
                        if (missing > budget || excess > budget)
                            throw FlatPatternError{
                                FlatPatternErrorCode::UnsupportedGeometry,
                                "The trimmed end is not a supported radial ruled "
                                "surface: missing=" +
                                    std::to_string(missing) + " excess=" + std::to_string(excess)};
                    }
                } catch (const FlatPatternError &error) {
                    if (error.code != FlatPatternErrorCode::UnsupportedGeometry || !useSourceCaps())
                        throw;
                } catch (const TrimmedBendError &) {
                    if (!useSourceCaps())
                        throw;
                }
                builder.Add(original, sector);
                const auto center = point(b.axisStart);
                const gp_Dir radial(gp_Vec(center, projected(center, pa)));
                const gp_Dir otherRadial(gp_Vec(center, projected(center, childPlane)));
                const double sign =
                    gp_Vec(bendAxis).Dot(gp_Vec(radial).Crossed(gp_Vec(otherRadial))) >= 0 ? 1 : -1;
                const double zInner =
                    wallMap(center.Translated(gp_Vec(radial) * b.innerRadiusMm)).Z();
                const double zSign = gp_Vec(radial).Transformed(placement).Z();
                regions.push_back(
                    {sector, [center, bendAxis, radial, sign, start, flatAxis, offset, zInner,
                              zSign, b, skinOnly](gp_Pnt p) {
                         auto r = gp_Vec(center, p);
                         const double axial = r.Dot(gp_Vec(bendAxis));
                         r -= gp_Vec(bendAxis) * axial;
                         const double measuredAngle =
                             std::atan2(sign * gp_Vec(bendAxis).Dot(gp_Vec(radial).Crossed(r)),
                                        gp_Vec(radial).Dot(r));
                         const double angle = skinOnly
                                                  ? measuredAngle
                                                  : std::clamp(measuredAngle, 0., b.angleRadians);
                         auto mapped = start.Translated(gp_Vec(flatAxis) * axial +
                                                        offset * (angle / b.angleRadians));
                         mapped.SetZ(zInner + zSign * (r.Magnitude() - b.innerRadiusMm));
                         return mapped;
                     }});
                TopoDS_Shape flatSector;
                if (b.trimmedEnds) {
                    TopoDS_Face patch;
                    if (skinOnly)
                        patch = flattenTrimmedBendSkin(sector, regions.back().map, tolerance, stop);
                    else
                        flatSector =
                            mapTrimmedBend(sector, regions.back().map, 0, tolerance, &patch, stop);
                    gp_Trsf lower;
                    lower.SetTranslation(gp_Vec(0, 0, -zInner));
                    patches.back() = BRepBuilderAPI_Transform(patch, lower, true).Shape();
                } else
                    flatSector = extruded(TopoDS::Face(patches.back()), gp_Vec(0, 0, t));
                const auto tangent = center.Translated(gp_Vec(radial) * b.innerRadiusMm);
                const gp_Vec bx = gp_Vec(bendAxis).Crossed(gp_Vec(radial)) * sign;
                const gp_Vec by = -gp_Vec(radial), bz(bendAxis);
                const simulation::SingleBend model{
                    t, b.innerRadiusMm, b.angleRadians, b.lengthMm, 1, 1};
                const auto atProgress = [tangent, bx, by, bz, model](gp_Pnt p, double progress) {
                    const gp_Vec v(tangent, p);
                    const auto section = simulation::singleBendSection(model, progress);
                    if (!section)
                        throw std::runtime_error("Invalid draft bend progress");
                    const auto mapped =
                        simulation::mapBendPoint(model, {v.Dot(bx), v.Dot(by)}, *section);
                    if (!mapped)
                        throw std::runtime_error("Cannot map draft bend surface");
                    return tangent.Translated(bx * mapped->x + by * mapped->y + bz * v.Dot(bz));
                };
                const auto childTangent = center.Translated(gp_Vec(otherRadial) * b.innerRadiusMm);
                const auto childAtProgress = [atProgress, childTangent, center, bendAxis, sign,
                                              angle = b.angleRadians](double progress) {
                    gp_Trsf rotation, translation;
                    rotation.SetRotation(gp_Ax1(center, bendAxis), sign * (progress - 1) * angle);
                    translation.SetTranslation(gp_Vec(childTangent.Transformed(rotation),
                                                      atProgress(childTangent, progress)));
                    return translation * rotation;
                };
                const gp_Ax3 sectionAxes(sign > 0 ? tangent : tangent.Translated(bz * b.lengthMm),
                                         gp_Dir(bx.Crossed(by)), gp_Dir(bx));
                const auto solidAtProgress = [model, sectionAxes, sector, atProgress, b,
                                              tolerance](double progress) {
                    if (b.trimmedEnds)
                        return mapTrimmedBend(
                            sector,
                            [atProgress, progress](gp_Pnt p) { return atProgress(p, progress); },
                            progress, tolerance);
                    return makeRouteSector(model, sectionAxes, progress);
                };
                result->layout.bends.push_back({index, wallIndices.at(w), wallIndices.at(child),
                                                sector, flatSector, regions.back().map, atProgress,
                                                childAtProgress, solidAtProgress, sectionAxes});
            }
        }
        require(placements.size() == graph.size() &&
                    std::all_of(used.begin(), used.end(), [](bool b) { return b; }),
                "Incomplete or disconnected sheet graph");
        std::sort(result->data.bends.begin(), result->data.bends.end(),
                  [](const auto &a, const auto &b) { return a.bendIndex < b.bendIndex; });
        if (partitionedSectors) {
            // An envelope can also contain a neighbour's material. Every
            // chosen sector owns full real skins; sectors may not own the
            // same volume. Walls are proved separately below.
            std::vector<Bnd_Box> sectorBoxes(result->layout.bends.size());
            for (std::size_t i = 0; i < sectorBoxes.size(); ++i) {
                BRepBndLib::AddOptimal(result->layout.bends[i].solid, sectorBoxes[i], false, false);
                require(!sectorBoxes[i].IsVoid() && !sectorBoxes[i].IsOpen(),
                        "Invalid bend sector bounds");
                sectorBoxes[i].Enlarge(tolerance);
            }
            for (std::size_t i = 0; i < result->layout.bends.size(); ++i)
                for (std::size_t j = 0; j < i; ++j) {
                    cancelled(stop);
                    if (sectorBoxes[i].IsOut(sectorBoxes[j]))
                        continue;
                    const auto overlap = boolean<BRepAlgoAPI_Common>(
                        result->layout.bends[i].solid, result->layout.bends[j].solid, tolerance,
                        stop, "Bend sector ownership intersection failed");
                    require(volume(overlap) <= budget, "Bend sectors share material");
                }
        }
        TopoDS_Shape reconstruction = original;
        const double partitionTolerance = sourceCaps ? std::min(tolerance, .001) : tolerance;
        const auto reconstructs = [&](bool allowEmptyDifference = false) {
            const double actualVolume = volume(reconstruction);
            if (!std::isfinite(actualVolume) || !std::isfinite(sourceVolume) || actualVolume <= 0 ||
                sourceVolume <= 0)
                return false;
            const bool equalVolume = std::abs(actualVolume - sourceVolume) <= budget;
            if (!equalVolume && !allowEmptyDifference)
                return false;
            const auto missing = boolean<BRepAlgoAPI_Cut>(
                source, reconstruction, partitionTolerance, stop, "Source coverage cut failed");
            if (equalVolume && volume(missing) > budget)
                return false;
            const auto excess =
                boolean<BRepAlgoAPI_Cut>(reconstruction, source, partitionTolerance, stop,
                                         "Reverse source coverage cut failed");
            if (equalVolume)
                return volume(excess) <= budget;
            // A tolerance-bearing BRep can integrate differently after an
            // exact partition. Only EMPTY topology in BOTH directions may
            // resolve that numerical mismatch; no larger volume budget.
            const auto empty = [](const TopoDS_Shape &shape) {
                return shape.ShapeType() == TopAbs_COMPOUND && !TopoDS_Iterator(shape).More();
            };
            return empty(missing) && empty(excess);
        };
        // A partition already requires the actual source walls; do not check
        // the temporary prism reconstruction that will be replaced below.
        bool complete = !partitionedSectors && reconstructs();
        std::vector<TopoDS_Shape> wallPrisms;
        if ((!complete && nonRadialEnds) || partitionedSectors) {
            // An oblique cut may also bevel a rigid wall through its thickness.
            // Replace failed prism approximations by complete, owned source
            // components. Both original-solid differences still follow.
            std::vector<TopoDS_Shape> sectors;
            for (const auto &region : result->layout.bends)
                sectors.push_back(region.solid);
            std::vector<std::array<TopoDS_Face, 2>> anchors;
            for (const auto &region : result->layout.walls)
                anchors.push_back({face(faces, region.faces[0]), face(faces, region.faces[1])});
            const auto actual =
                isolateFlatWalls(source, sectors, anchors, partitionTolerance, stop);
            cancelled(stop);
            require(actual && actual->size() == result->layout.walls.size(),
                    "Cannot isolate all complete planar walls after removing bend sectors");
            TopoDS_Compound remainder;
            builder.MakeCompound(remainder);
            for (std::size_t i = 0; i < actual->size(); ++i) {
                if (!geometryOnly && !nonRadialEnds)
                    wallPrisms.push_back(result->layout.walls[i].solid);
                result->layout.walls[i].solid = (*actual)[i];
                regions[wallRegions[i]].solid = (*actual)[i];
                if (i > 0)
                    builder.Add(remainder, (*actual)[i]);
            }
            for (const auto &sector : sectors)
                builder.Add(remainder, sector);
            // Source tolerances can leave coincident boundary fragments in
            // independent pieces. Compare their real union with the source;
            // never add the same material twice via compound volume sums.
            reconstruction =
                boolean<BRepAlgoAPI_Fuse>((*actual)[0], remainder, partitionTolerance, stop,
                                          "Wall/bend reconstruction union failed");
            complete = reconstructs(true);
        }
        require(complete,
                "The recognized walls and bends do not reconstruct the complete original solid");
        if (geometryOnly) {
            cancelled(stop);
            return FlatPatternPtr(std::move(result));
        }
        for (auto &p : patches) {
            auto f = TopoDS::Face(p);
            const double normal = BRepAdaptor_Surface(f).Plane().Axis().Direction().Z() *
                                  (f.Orientation() == TopAbs_REVERSED ? -1 : 1);
            if (normal < 0)
                f.Reverse();
            p = f;
        }
        TopoDS_Shape flat = patches.front();
        for (std::size_t i = 1; i < patches.size(); ++i) {
            require(area(boolean<BRepAlgoAPI_Common>(flat, patches[i], tolerance, stop,
                                                     "Flat overlap check failed")) <= 1e-5,
                    "Flat wall patches overlap");
            flat = boolean<BRepAlgoAPI_Fuse>(flat, patches[i], tolerance, stop,
                                             "Flat patch union failed");
        }
        ShapeUpgrade_UnifySameDomain unify(flat, true, true, true);
        unify.SetLinearTolerance(tolerance);
        unify.SetAngularTolerance(1.e-7);
        unify.Build();
        flat = unify.Shape();
        TopTools_IndexedMapOfShape flatFaces;
        TopExp::MapShapes(flat, TopAbs_FACE, flatFaces);
        require(flatFaces.Extent() == 1 && BRepCheck_Analyzer(flat).IsValid(),
                "Flat contour is disconnected or invalid");
        auto box = bounds(flat);
        gp_Trsf origin;
        origin.SetTranslation(gp_Vec(-box.minimum.x, -box.minimum.y, 0));
        flat = BRepBuilderAPI_Transform(flat, origin, true).Shape();
        for (auto &b : result->data.bends) {
            b.start.x -= box.minimum.x;
            b.end.x -= box.minimum.x;
            b.start.y -= box.minimum.y;
            b.end.y -= box.minimum.y;
        }
        result->display =
            extruded(TopoDS::Face(TopExp_Explorer(flat, TopAbs_FACE).Current()), gp_Vec(0, 0, t));
        result->geometry = result->display;
        result->data.bounds = bounds(result->display);
        const auto &orientation = OcctDocumentAccess::sheetOrientation(*native);
        std::optional<SheetReference> flatReference;
        if (orientation.available) {
            std::vector<TopoDS_Shape> walls;
            for (const auto &wall : result->layout.walls)
                walls.push_back(wall.solid);
            if (const auto at = sheetReferenceRegion(orientation.reference, walls, tolerance)) {
                flatReference = movedSheetReference(orientation.reference,
                                                    origin * result->layout.walls[*at].toFlat);
                flatReference->thicknessMm = t;
                result->orientation.edges = flatReference->edges;
                if (std::abs(flatReference->normal.Z()) > 1 - 1.e-7)
                    result->data.positiveZIsSideA = flatReference->normal.Z() > 0;
            }
        }
        TopoDS_Face verifiedStock;
        const auto measures = OcctDocumentAccess::measurements(*native);
        if (measures || cutoutsIgnored) {
            try {
                result->sourceWallOwners = mapSourceWallOwners(
                    OcctDocumentAccess::shape(*native), source, result->layout.walls,
                    native->faces(), cutoutsIgnored || joinedWallFaces, tolerance, stop);
            } catch (const Standard_Failure &) {
                // Unknown ownership cannot be used to restore cutouts or measurements.
            }
            cancelled(stop);
        }
        if (measures) {
            try {
                result->data.measurements = flatModelMeasurements(
                    *measures, flat, result->layout.walls, origin, result->sourceWallOwners, stop);
            } catch (const Standard_Failure &) {
                // Optional annotations must not block unfolding or export.
            } catch (const std::exception &) {
            }
        }
        // Separating neighbouring bend sectors can require isolating the real
        // walls even when they have constant sections. Prove each wall still
        // matches the prism that generated its cutting patch.
        if (!stop.stop_requested()) {
            try {
                bool cutStock = !nonRadialEnds && reconstruction.IsSame(original);
                if (!nonRadialEnds && !cutStock &&
                    wallPrisms.size() == result->layout.walls.size()) {
                    cutStock = true;
                    for (std::size_t i = 0; i < wallPrisms.size() && cutStock; ++i)
                        cutStock = sameVolumeWithin(wallPrisms[i], result->layout.walls[i].solid,
                                                    tolerance, 1.e-9, stop);
                }
                if (cutStock || nonRadialEnds) {
                    const auto cutFace =
                        nonRadialEnds ? midThicknessFace(result->layout, origin, t, tolerance, stop)
                                      : TopoDS::Face(TopExp_Explorer(flat, TopAbs_FACE).Current());
                    result->data.cutProfile = buildFlatCutProfile(
                        cutFace, OcctDocumentAccess::shape(*native),
                        native->metadata().closedCutouts, result->layout, result->sourceWallOwners,
                        origin, t, tolerance, stop, nonRadialEnds,
                        sourceCaps ? CutJoinPolicy::SplineLine : CutJoinPolicy::LinearOnly,
                        sourceCaps ? &verifiedStock : nullptr);
                }
            } catch (const FlatPatternError &) {
                // A failed section only affects the optional DXF.
            } catch (const TrimmedBendError &) {
                // A failed section mapping only affects the optional DXF.
            } catch (const Standard_Failure &) {
                // Optional cutting data must not invalidate display or PDF.
            }
        }
        bool stockDisplay = !cutoutsIgnored;
        if (cutoutsIgnored) {
            try {
                auto cutouts = flatCutoutDisplay(OcctDocumentAccess::shape(*native), regions,
                                                 gp_Vec(-box.minimum.x, -box.minimum.y, 0),
                                                 tolerance, stop, &orientation);
                result->display = std::move(cutouts.surface);
                result->contours = std::move(cutouts.contours);
                appendSheetSides(result->orientation, result->display, cutouts.faceSides);
            } catch (const FlatPatternError &error) {
                if (error.code != FlatPatternErrorCode::UnsupportedGeometry ||
                    verifiedStock.IsNull())
                    throw;
                // Use the same verified stock and through openings as the DXF.
                // The original folded shape remains available in the model view.
                result->display = extruded(verifiedStock, gp_Vec(0, 0, t));
                result->geometry = result->display;
                result->data.bounds = bounds(result->display);
                stockDisplay = true;
            }
        }
        if (stockDisplay) {
            BRepMesh_IncrementalMesh mesh(result->display, .08, false, .25, false);
            require(mesh.IsDone(), "Flat preview meshing failed");
            if (flatReference) {
                auto flatMetadata = native->metadata();
                flatMetadata.bendAnalysis = {};
                const auto tags =
                    classifySheetOrientation(result->display, flatMetadata, stop, flatReference);
                result->orientation = sheetOrientationVisual(result->display, tags);
            }
        }
        TopoDS_Compound display;
        builder.MakeCompound(display);
        builder.Add(display, result->display);
        if (!stockDisplay) {
            gp_Trsf top;
            top.SetTranslation(gp_Vec(0, 0, t + .005));
            builder.Add(display,
                        BRepBuilderAPI_Transform(BRepTools::OuterWire(TopoDS::Face(
                                                     TopExp_Explorer(flat, TopAbs_FACE).Current())),
                                                 top, true)
                            .Shape());
            for (const auto &contour : result->contours) {
                BRepBuilderAPI_MakePolygon wire;
                for (std::size_t i = 0; i + 1 < contour.size(); ++i)
                    wire.Add(gp_Pnt(contour[i].x, contour[i].y, t + .005));
                wire.Close();
                require(wire.IsDone(), "Cannot build cutout display outline");
                builder.Add(display, wire.Wire());
            }
        }
        for (const auto &b : result->data.bends) {
            const auto ends = b.centreLine();
            const auto lineAt = [&](double z) {
                return BRepBuilderAPI_MakeEdge(gp_Pnt(ends[0].x, ends[0].y, z),
                                               gp_Pnt(ends[1].x, ends[1].y, z))
                    .Shape();
            };
            // Put one presentation edge just outside each sheet surface.
            constexpr double clearance = .005;
            auto &lines = result->lines.emplace_back(
                std::array<TopoDS_Shape, 2>{lineAt(t + clearance), lineAt(-clearance)});
            for (const auto &line : lines)
                builder.Add(display, line);
        }
        result->display = display;
        cancelled(stop);
        return FlatPatternPtr(std::move(result));
    } catch (const TrimmedBendError &e) {
        return FlatPatternError{stop.stop_requested() ? FlatPatternErrorCode::Cancelled
                                                      : FlatPatternErrorCode::UnsupportedGeometry,
                                e.what()};
    } catch (const FlatPatternError &e) {
        return e;
    } catch (const Standard_Failure &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed,
                                e.GetMessageString() ? e.GetMessageString() : "CAD failure"};
    } catch (const std::exception &e) {
        return FlatPatternError{FlatPatternErrorCode::Failed, e.what()};
    }
}
FlatPatternResult FlatPatternBuilder::build(DocumentPtr document, FlatPatternRequest request,
                                            std::stop_token stop) const {
    return buildSheet(std::move(document), request, stop, false);
}
SheetGeometryResult prepareSheetGeometry(DocumentPtr document, std::stop_token stop) {
    const auto result =
        buildSheet(document, {{core::SheetMaterial::Unknown, .5, 1}, {}}, stop, true);
    if (const auto *error = std::get_if<FlatPatternError>(&result))
        return *error;
    const auto &owner = std::get<FlatPatternPtr>(result);
    const auto &layout = dynamic_cast<const CadFlatPattern &>(*owner).layout;
    return VerifiedSheetGeometry{std::move(document),
                                 std::shared_ptr<const SheetLayout>(owner, &layout)};
}
} // namespace bending::cad
