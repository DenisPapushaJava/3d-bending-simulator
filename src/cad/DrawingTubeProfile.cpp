#include "DrawingTubeProfile.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bending::cad {
namespace {
constexpr double angularTolerance = 1e-6;
struct End {
    int face;
    TopoDS_Wire outer;
    TopoDS_Wire bore;
};
using EdgeFaces = TopTools_IndexedDataMapOfShapeListOfShape;

std::vector<int> wallFaces(const TopoDS_Wire &wire, int end, const EdgeFaces &adjacent,
                           const TopTools_IndexedMapOfShape &faces) {
    std::vector<int> result;
    for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next()) {
        if (result.size() >= 8 || !adjacent.Contains(e.Current()))
            return {};
        const auto &owners = adjacent.FindFromKey(e.Current());
        if (owners.Extent() != 2)
            return {};
        const int first = faces.FindIndex(owners.First());
        const int last = faces.FindIndex(owners.Last());
        if (first != end && last != end)
            return {};
        const int wall = first == end ? last : first;
        if (wall == 0 || wall == end)
            return {};
        result.push_back(wall);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct Walls {
    std::vector<std::pair<int, gp_Pln>> planes;
    std::vector<gp_Cylinder> cylinders;
};
std::optional<Walls> supportSurfaces(const std::vector<int> &indices,
                                     const TopTools_IndexedMapOfShape &faces) {
    Walls result;
    for (int index : indices) {
        const BRepAdaptor_Surface surface(TopoDS::Face(faces(index)), false);
        switch (surface.GetType()) {
        case GeomAbs_Plane:
            result.planes.emplace_back(index, surface.Plane());
            break;
        case GeomAbs_Cylinder:
            result.cylinders.push_back(surface.Cylinder());
            break;
        default:
            return {};
        }
    }
    return result;
}

struct SidePair {
    double low;
    double high;
    int lowFace;
    int highFace;
};
std::optional<SidePair> sidePair(const Walls &walls, const gp_Dir &direction,
                                 const gp_Pnt &origin) {
    std::array<std::pair<double, int>, 2> positions;
    std::size_t count = 0;
    for (const auto &[face, plane] : walls.planes) {
        if (plane.Axis().Direction().IsParallel(direction, angularTolerance)) {
            if (count == positions.size())
                return {};
            positions[count++] = {gp_Vec(origin, plane.Location()).Dot(gp_Vec(direction)), face};
        } else if (std::abs(plane.Axis().Direction().Dot(direction)) > angularTolerance) {
            return {};
        }
    }
    if (count != positions.size())
        return {};
    if (positions[1] < positions[0])
        std::swap(positions[0], positions[1]);
    return SidePair{positions[0].first, positions[1].first, positions[0].second,
                    positions[1].second};
}

bool coaxial(const gp_Ax1 &a, const gp_Ax1 &b, double tolerance) {
    return a.IsParallel(b, angularTolerance) && gp_Lin(a).Distance(b.Location()) <= tolerance;
}

std::optional<DrawingTubeGeometry> section(const Walls &outer, const Walls &inner, double tol) {
    DrawingTubeGeometry result;
    if (outer.planes.empty() && inner.planes.empty() && outer.cylinders.size() == 1 &&
        inner.cylinders.size() == 1) {
        const auto &a = outer.cylinders.front(), &b = inner.cylinders.front();
        const double t = a.Radius() - b.Radius();
        if (!coaxial(a.Axis(), b.Axis(), tol) || t <= tol)
            return {};
        result.axis = a.Axis().Direction();
        result.profile.sizeMm = 2 * a.Radius();
        result.profile.wallMm = t;
        return result;
    }
    if (outer.planes.size() != 4 || inner.planes.size() != 4 ||
        outer.cylinders.size() != inner.cylinders.size() ||
        (!outer.cylinders.empty() && outer.cylinders.size() != 4))
        return {};
    const auto &reference = outer.planes.front().second;
    const auto x = reference.Axis().Direction();
    const auto y = std::find_if(outer.planes.begin(), outer.planes.end(), [&](const auto &wall) {
        return wall.second.Axis().Direction().IsNormal(x, angularTolerance);
    });
    if (y == outer.planes.end())
        return {};
    const auto other = y->second.Axis().Direction();
    const auto axis = x.Crossed(other);
    const auto corner = reference.Location();
    const auto ox = sidePair(outer, x, corner), ix = sidePair(inner, x, corner);
    const auto oy = sidePair(outer, other, corner), iy = sidePair(inner, other, corner);
    if (!ox || !ix || !oy || !iy || ix->high - ix->low <= tol || iy->high - iy->low <= tol)
        return {};
    const double thickness = ix->low - ox->low;
    if (thickness <= tol || std::abs(ox->high - ix->high - thickness) > tol ||
        std::abs(iy->low - oy->low - thickness) > tol ||
        std::abs(oy->high - iy->high - thickness) > tol)
        return {};
    for (const auto &out : outer.cylinders) {
        if (!out.Axis().Direction().IsParallel(axis, angularTolerance) ||
            std::count_if(inner.cylinders.begin(), inner.cylinders.end(), [&](const auto &in) {
                return coaxial(out.Axis(), in.Axis(), tol) &&
                       std::abs(out.Radius() - in.Radius() - thickness) <= tol;
            }) != 1)
            return {};
    }
    const double spanX = ox->high - ox->low, spanY = oy->high - oy->low;
    result.axis = axis;
    result.sideFace = static_cast<std::size_t>(spanX >= spanY ? oy->highFace : ox->highFace);
    result.planarWalls = std::array{
        DrawingTubeWall{ox->lowFace, ix->lowFace, x.Reversed()},
        DrawingTubeWall{ox->highFace, ix->highFace, x},
        DrawingTubeWall{oy->lowFace, iy->lowFace, other.Reversed()},
        DrawingTubeWall{oy->highFace, iy->highFace, other},
    };
    result.profile.sizeMm = std::max(spanX, spanY);
    result.profile.minorSizeMm = std::min(spanX, spanY);
    result.profile.wallMm = thickness;
    return result;
}

gp_Trsf axialProjection(const gp_Dir &axis) {
    gp_Trsf transform;
    transform.SetTransformation(gp_Ax3(gp_Pnt{}, axis));
    return transform;
}
std::array<double, 2> axialRange(const TopoDS_Shape &shape, const gp_Trsf &projection) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape.Moved(TopLoc_Location(projection)), box, false, false);
    return {box.CornerMin().Z(), box.CornerMax().Z()};
}
std::array<double, 2> axialRange(const TopoDS_Shape &shape, const gp_Dir &axis) {
    return axialRange(shape, axialProjection(axis));
}
struct EndRim {
    std::vector<int> faces;
    int datum = 0;
};

bool containsFace(const std::vector<int> &indices, int face) {
    return std::binary_search(indices.begin(), indices.end(), face);
}

int otherFace(const TopoDS_Shape &edge, int face, const EdgeFaces &adjacent,
              const TopTools_IndexedMapOfShape &faces) {
    if (!adjacent.Contains(edge))
        return 0;
    const auto &owners = adjacent.FindFromKey(edge);
    if (owners.Extent() != 2)
        return 0;
    const int first = faces.FindIndex(owners.First()), last = faces.FindIndex(owners.Last());
    return first == face && last != face ? last : last == face && first != face ? first : 0;
}

// A local notch can connect the outer and inner contours into a single wire.
std::optional<EndRim> notchedEnd(const TopTools_IndexedMapOfShape &faces, const EdgeFaces &adjacent,
                                 const std::vector<int> &outer, const std::vector<int> &inner,
                                 const gp_Dir &axis, double bound, double span, double tol,
                                 std::stop_token stop) {
    const auto reachesEnd = [&](int face) {
        return axialRange(faces(face), axis)[1] >= bound - span;
    };
    if (!std::all_of(outer.begin(), outer.end(), reachesEnd) ||
        !std::all_of(inner.begin(), inner.end(), reachesEnd))
        return {};
    EndRim result;
    bool atEnd = false;
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i) {
        const auto face = TopoDS::Face(faces(i));
        const BRepAdaptor_Surface surface(face, false);
        if (surface.GetType() != GeomAbs_Plane ||
            std::abs(surface.Plane().Axis().Direction().Dot(axis)) <= angularTolerance)
            continue;
        TopExp_Explorer wires(face, TopAbs_WIRE);
        if (!wires.More())
            continue;
        const auto rim = TopoDS::Wire(wires.Current());
        wires.Next();
        if (wires.More() || !BRep_Tool::IsClosed(rim))
            continue;
        bool touchesOuter = false, touchesInner = false;
        for (TopExp_Explorer edge(rim, TopAbs_EDGE); edge.More(); edge.Next()) {
            const int neighbor = otherFace(edge.Current(), i, adjacent, faces);
            touchesOuter |= containsFace(outer, neighbor);
            touchesInner |= containsFace(inner, neighbor);
        }
        if (!touchesOuter || !touchesInner)
            continue;
        const auto range = axialRange(face, axis);
        if (range[1] > bound + tol || range[0] < bound - span)
            continue;
        result.faces.push_back(i);
        if (std::abs(range[1] - bound) <= tol) {
            atEnd = true;
            if (surface.Plane().Axis().Direction().IsParallel(axis, angularTolerance))
                result.datum = i;
        }
    }
    return atEnd && !stop.stop_requested() ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<gp_Pln> planeAt(const TopTools_IndexedMapOfShape &faces, int index) {
    const BRepAdaptor_Surface surface(TopoDS::Face(faces(index)), false);
    if (surface.GetType() != GeomAbs_Plane)
        return {};
    return surface.Plane();
}

std::vector<int> endBevel(const EndRim &rim, const TopTools_IndexedMapOfShape &faces,
                          const EdgeFaces &adjacent, const gp_Dir &axis, double tol) {
    std::optional<gp_Pln> bevel;
    std::vector<int> result;
    for (int index : rim.faces) {
        const auto plane = *planeAt(faces, index);
        if (plane.Axis().Direction().IsParallel(axis, angularTolerance))
            continue;
        bool touchesDatum = false;
        for (TopExp_Explorer e(faces(index), TopAbs_EDGE); e.More() && !touchesDatum; e.Next())
            touchesDatum = otherFace(e.Current(), index, adjacent, faces) == rim.datum;
        if (!touchesDatum)
            continue;
        if (bevel &&
            (!bevel->Axis().Direction().IsParallel(plane.Axis().Direction(), angularTolerance) ||
             bevel->Distance(plane.Location()) > tol))
            return {}; // Different local cuts cannot share one angle or axial dimension.
        bevel = plane;
        result.push_back(index);
    }
    return result;
}

std::optional<DrawingTubeNotch>
openEndSlot(int back, const EndRim &rim, const std::vector<int> &bevels,
            const TopTools_IndexedMapOfShape &faces, const EdgeFaces &adjacent,
            const std::vector<int> &outer, const std::vector<int> &inner, const gp_Dir &axis,
            double wall, double tol) {
    if (back == rim.datum ||
        !planeAt(faces, back)->Axis().Direction().IsParallel(axis, angularTolerance))
        return {};
    int outside = 0, inside = 0;
    std::array<int, 2> sides{};
    std::array<gp_Pnt, 2> lip;
    for (TopExp_Explorer e(faces(back), TopAbs_EDGE); e.More(); e.Next()) {
        const int other = otherFace(e.Current(), back, adjacent, faces);
        if (!other)
            return {};
        const BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
        if (curve.GetType() != GeomAbs_Line)
            return {};
        if (containsFace(outer, other) && !outside) {
            outside = other;
            lip = {curve.Value(curve.FirstParameter()), curve.Value(curve.LastParameter())};
        } else if (containsFace(inner, other) && !inside) {
            inside = other;
        } else if (!containsFace(outer, other) && !containsFace(inner, other) && !sides[1]) {
            sides[sides[0] ? 1 : 0] = other;
        } else {
            return {};
        }
    }
    if (!outside || !inside || !sides[1] || sides[0] == sides[1])
        return {};
    const auto exterior = planeAt(faces, outside), interior = planeAt(faces, inside);
    if (!exterior || !interior)
        return {};
    gp_Dir n = exterior->Axis().Direction();
    if (!n.IsNormal(axis, angularTolerance) ||
        !n.IsParallel(interior->Axis().Direction(), angularTolerance))
        return {};
    const double separation = gp_Vec(interior->Location(), exterior->Location()).Dot(gp_Vec(n));
    if (std::abs(std::abs(separation) - wall) > tol)
        return {};
    if (separation < 0)
        n.Reverse();
    const auto transverse = n.Crossed(axis);
    if (lip[0].Distance(lip[1]) <= tol ||
        !gp_Dir(gp_Vec(lip[0], lip[1])).IsParallel(transverse, angularTolerance))
        return {};
    const auto reachesSkin = [&](const TopoDS_Shape &edge, int other, int flat, const gp_Pln &plane,
                                 const std::vector<int> &skin) {
        if (other == flat)
            return true;
        if (!containsFace(skin, other))
            return false;
        const BRepAdaptor_Surface surface(TopoDS::Face(faces(other)), false);
        if (surface.GetType() != GeomAbs_Cylinder)
            return false;
        const auto cylinder = surface.Cylinder();
        const BRepAdaptor_Curve line(TopoDS::Edge(edge));
        if (!cylinder.Axis().Direction().IsParallel(axis, angularTolerance) ||
            line.GetType() != GeomAbs_Line ||
            !line.Line().Direction().IsParallel(axis, angularTolerance))
            return false;
        const auto at = line.Value(line.FirstParameter());
        const gp_Vec fromAxis(cylinder.Location(), at);
        const auto radial = fromAxis - gp_Vec(axis) * fromAxis.Dot(gp_Vec(axis));
        return plane.Distance(at) <= tol && radial.Magnitude() > tol &&
               gp_Dir(radial).IsParallel(plane.Axis().Direction(), angularTolerance);
    };
    for (int side : sides) {
        const auto plane = planeAt(faces, side);
        if (!plane || !plane->Axis().Direction().IsParallel(transverse, angularTolerance))
            return {};
        bool sharesMouth = false, sharesOutside = false, sharesInside = false;
        for (TopExp_Explorer e(faces(side), TopAbs_EDGE); e.More(); e.Next()) {
            const auto other = otherFace(e.Current(), side, adjacent, faces);
            sharesMouth |= other == rim.datum || containsFace(bevels, other);
            sharesOutside |= reachesSkin(e.Current(), other, outside, *exterior, outer);
            sharesInside |= reachesSkin(e.Current(), other, inside, *interior, inner);
        }
        if (!sharesMouth || !sharesOutside || !sharesInside)
            return {};
    }

    // Locate the slot from the closest parallel exterior stock plane and the
    // real square part of the end. Select witnesses on those actual faces.
    double distance = std::numeric_limits<double>::infinity();
    int sideDatum = 0;
    for (int index : outer) {
        const auto plane = planeAt(faces, index);
        if (!plane || !plane->Axis().Direction().IsParallel(transverse, angularTolerance))
            continue;
        const double d = std::min(plane->Distance(lip[0]), plane->Distance(lip[1]));
        if (d > tol && d < distance) {
            distance = d;
            sideDatum = index;
        }
    }
    if (!sideDatum)
        return {};
    const gp_Pnt middle((lip[0].XYZ() + lip[1].XYZ()) / 2);
    const auto witness = [&](int index, bool alongAxis) {
        std::optional<gp_Pnt> best;
        double nearest = std::numeric_limits<double>::infinity();
        const gp_Vec direction(alongAxis ? axis : transverse);
        for (TopExp_Explorer v(faces(index), TopAbs_VERTEX); v.More(); v.Next()) {
            const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
            const double d = std::abs(gp_Vec(middle, p).Dot(direction));
            if (d < nearest) {
                nearest = d;
                best = p;
            }
        }
        return best;
    };
    const auto endPoint = witness(rim.datum, false), edgePoint = witness(sideDatum, true);
    if (!endPoint || !edgePoint || std::abs(gp_Vec(*endPoint, middle).Dot(gp_Vec(axis))) <= tol)
        return {};
    return DrawingTubeNotch{n, lip, *endPoint, *edgePoint};
}

DrawingTubeCompoundEnd measureCompoundEnd(std::size_t side, const EndRim &rim,
                                          const TopTools_IndexedMapOfShape &faces,
                                          const EdgeFaces &adjacent, const std::vector<int> &outer,
                                          const std::vector<int> &inner, const gp_Dir &axis,
                                          double wall, double tol) {
    DrawingTubeCompoundEnd result{side, {}, {}};
    if (!rim.datum)
        return result;
    result.bevelFaces = endBevel(rim, faces, adjacent, axis, tol);
    for (int index : rim.faces)
        if (auto next = openEndSlot(index, rim, result.bevelFaces, faces, adjacent, outer, inner,
                                    axis, wall, tol)) {
            if (result.notch) {
                result.notch.reset();
                break;
            }
            result.notch = next;
        }
    return result;
}
double cutAngle(const TopoDS_Shape &face, const gp_Dir &axis) {
    const auto normal = BRepAdaptor_Surface(TopoDS::Face(face), false).Plane().Axis().Direction();
    return std::atan2(gp_Vec(axis).CrossMagnitude(gp_Vec(normal)), std::abs(axis.Dot(normal))) *
           180 / std::numbers::pi;
}
} // namespace

std::optional<DrawingTubeGeometry> drawingTubeProfile(const TopoDS_Shape &shape,
                                                      const TopTools_IndexedMapOfShape &faces,
                                                      double tolerance, std::stop_token stop) {
    std::vector<End> ends;
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i) {
        const auto face = TopoDS::Face(faces(i));
        std::array<TopoDS_Wire, 2> wires;
        std::size_t count = 0;
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More() && count <= wires.size(); w.Next()) {
            if (count < wires.size())
                wires[count] = TopoDS::Wire(w.Current());
            ++count;
        }
        if (count != wires.size() || !BRep_Tool::IsClosed(wires[0]) ||
            !BRep_Tool::IsClosed(wires[1]) ||
            BRepAdaptor_Surface(face, false).GetType() != GeomAbs_Plane)
            continue;
        const auto outer = BRepTools::OuterWire(face);
        if (outer.IsNull())
            continue;
        ends.push_back({i, outer, wires[outer.IsSame(wires[0]) ? 1 : 0]});
        if (ends.size() > 32)
            return {};
    }
    if (ends.empty() || stop.stop_requested())
        return {};
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    if (solids.Extent() != 1)
        return {};
    EdgeFaces adjacent;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, adjacent);
    const double tol = std::clamp(tolerance, 1e-5, .01);
    std::optional<DrawingTubeGeometry> match;
    std::optional<DrawingTubeGeometry> notched;
    bool ambiguousNotched = false;
    for (std::size_t i = 0; i < ends.size() && !stop.stop_requested(); ++i) {
        const auto &a = ends[i];
        const auto outer = wallFaces(a.outer, a.face, adjacent, faces);
        const auto inner = wallFaces(a.bore, a.face, adjacent, faces);
        if (outer.empty() || inner.empty() ||
            std::any_of(inner.begin(), inner.end(), [&](int face) {
                return std::binary_search(outer.begin(), outer.end(), face);
            }))
            continue;
        const auto outerWalls = supportSurfaces(outer, faces);
        const auto innerWalls = supportSurfaces(inner, faces);
        if (!outerWalls || !innerWalls)
            continue;
        auto candidate = section(*outerWalls, *innerWalls, tol);
        if (!candidate)
            continue;
        candidate->longitudinalFaces = outer;
        candidate->longitudinalFaces.insert(candidate->longitudinalFaces.end(), inner.begin(),
                                            inner.end());
        std::sort(candidate->longitudinalFaces.begin(), candidate->longitudinalFaces.end());
        for (std::size_t j = i + 1; j < ends.size() && !stop.stop_requested(); ++j) {
            const auto &b = ends[j];
            if (outer != wallFaces(b.outer, b.face, adjacent, faces) ||
                inner != wallFaces(b.bore, b.face, adjacent, faces))
                continue;
            // The same inner and outer skins reach both ends. Require room between
            // the cut planes and reject openings away from the axial extrema.
            const auto &axis = candidate->axis;
            const auto aRange = axialRange(faces(a.face), axis);
            const auto bRange = axialRange(faces(b.face), axis);
            const bool ascending = aRange[0] < bRange[0];
            const auto &low = ascending ? aRange : bRange;
            const auto &high = ascending ? bRange : aRange;
            if (high[0] - low[1] <= 2 * candidate->profile.wallMm)
                continue;
            const auto whole = axialRange(shape, axis);
            if (std::abs(whole[0] - low[0]) > tol || std::abs(whole[1] - high[1]) > tol)
                continue;
            if (match)
                return {}; // More than one confirmed through-profile.
            const auto &first = ascending ? a : b;
            const auto &last = ascending ? b : a;
            candidate->bores = {first.bore, last.bore};
            candidate->endFaces = {first.face, last.face};
            candidate->cutsDegrees = {cutAngle(faces(first.face), axis),
                                      cutAngle(faces(last.face), axis)};
            match = candidate;
        }
        if (match || ambiguousNotched || !candidate->profile.minorSizeMm)
            continue;
        const auto fullRange = axialRange(shape, candidate->axis);
        const auto knownEnd = axialRange(faces(a.face), candidate->axis);
        const bool startsAtLow = std::abs(fullRange[0] - knownEnd[0]) <= tol;
        const bool endsAtHigh = std::abs(fullRange[1] - knownEnd[1]) <= tol;
        const double span = *candidate->profile.minorSizeMm;
        const double freeLength =
            startsAtLow ? fullRange[1] - knownEnd[1] : knownEnd[0] - fullRange[0];
        if (startsAtLow == endsAtHigh || freeLength <= 2 * span)
            continue;
        const auto rim = notchedEnd(faces, adjacent, outer, inner,
                                    startsAtLow ? candidate->axis : candidate->axis.Reversed(),
                                    startsAtLow ? fullRange[1] : -fullRange[0], span, tol, stop);
        if (!rim)
            continue;
        const std::size_t end = startsAtLow ? 0 : 1;
        try {
            candidate->compoundEnd =
                measureCompoundEnd(1 - end, *rim, faces, adjacent, outer, inner, candidate->axis,
                                   candidate->profile.wallMm, tol);
        } catch (const Standard_Failure &) {
            // Keep the confirmed profile if the optional local dimensions are unavailable.
        }
        candidate->bores[end] = a.bore;
        candidate->endFaces[end] = a.face;
        candidate->cutsDegrees[end] = cutAngle(faces(a.face), candidate->axis);
        ambiguousNotched = notched.has_value();
        notched = candidate;
    }
    return match ? match : (ambiguousNotched ? std::nullopt : notched);
}

std::optional<std::array<double, 2>> drawingTubeBreak(const TopTools_IndexedMapOfShape &faces,
                                                      const TopTools_IndexedMapOfShape &edges,
                                                      const DrawingTubeGeometry &tube,
                                                      const gp_Dir &right, double low, double high,
                                                      std::stop_token stop) {
    if (!right.IsParallel(tube.axis, angularTolerance))
        return {};
    const auto transform = axialProjection(right);
    std::vector<std::array<double, 2>> blocked;
    const double margin = std::max(8., tube.profile.sizeMm / 4);
    const double keepEnd = std::max(40., 1.25 * tube.profile.sizeMm);
    const auto block = [&](std::array<double, 2> span) {
        blocked.push_back({span[0] - margin, span[1] + margin});
    };
    // Non-profile faces include all caps, cutouts and their walls. Even if a
    // projection hides a feature, it blocks the same range in every view.
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i)
        if (!std::binary_search(tube.longitudinalFaces.begin(), tube.longitudinalFaces.end(), i))
            block(axialRange(faces(i), transform));
    for (int i = 1; i <= edges.Extent() && !stop.stop_requested(); ++i) {
        const auto edge = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(edge))
            continue;
        const BRepAdaptor_Curve curve(edge);
        if (curve.GetType() == GeomAbs_Line &&
            curve.Line().Direction().IsParallel(right, angularTolerance)) {
            // Along the analytic skin nothing changes between these stations.
            for (double u : {curve.FirstParameter(), curve.LastParameter()}) {
                const double station = curve.Value(u).Transformed(transform).Z();
                block({station, station});
            }
        } else {
            block(axialRange(edge, transform));
        }
    }
    if (stop.stop_requested())
        return {};
    std::sort(blocked.begin(), blocked.end());
    std::optional<std::array<double, 2>> best;
    double length = std::max(50., (high - low) / 5);
    double start = low + keepEnd;
    const double limit = high - keepEnd;
    const auto consider = [&](double end) {
        if (end - start > length) {
            length = end - start;
            best = {start, end};
        }
    };
    for (const auto &span : blocked) {
        consider(std::min(span[0], limit));
        start = std::max(start, span[1]);
        if (start >= limit)
            break;
    }
    consider(limit);
    return best;
}

namespace {
double distanceToSegmentSquared(application::DrawingPoint p, application::DrawingPoint a,
                                application::DrawingPoint b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length = dx * dx + dy * dy;
    const double t =
        length > 1e-14 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0., 1.) : 0.;
    const double x = p.x - a.x - t * dx, y = p.y - a.y - t * dy;
    return x * x + y * y;
}
std::optional<application::DrawingPoint> visibleCutAnchor(const std::vector<gp_Pnt> &samples,
                                                          const gp_Ax2 &frame,
                                                          const application::DrawingView &view,
                                                          bool horizontal, std::stop_token stop) {
    const double middle =
        horizontal ? (view.minimum.y + view.maximum.y) / 2 : (view.minimum.x + view.maximum.x) / 2;
    std::optional<application::DrawingPoint> best;
    double score = std::numeric_limits<double>::infinity();
    for (const auto &sample : samples) {
        if (stop.stop_requested())
            return {};
        const gp_Vec offset(frame.Location(), sample);
        const application::DrawingPoint at{offset.Dot(gp_Vec(frame.XDirection())),
                                           offset.Dot(gp_Vec(frame.YDirection()))};
        const double dist = std::abs((horizontal ? at.y : at.x) - middle);
        if (dist >= score)
            continue;
        if (drawingTubeVisibleEdge(at, view, stop)) {
            best = at;
            score = dist;
        }
    }
    return best;
}
} // namespace

bool drawingTubeVisibleEdge(application::DrawingPoint at, const application::DrawingView &view,
                            std::stop_token stop) {
    // HLR curves are sampled with 0.02 mm deflection.
    for (const auto &path : view.paths) {
        if (stop.stop_requested())
            return false;
        if (!path.hidden &&
            std::adjacent_find(path.points.begin(), path.points.end(), [&](auto a, auto b) {
                return distanceToSegmentSquared(at, a, b) <= .03 * .03;
            }) != path.points.end())
            return true;
    }
    return false;
}

DrawingTubeCutLocations addDrawingTubeCuts(const DrawingTubeGeometry &tube,
                                           const TopTools_IndexedMapOfShape &faces,
                                           const std::array<gp_Ax2, 3> &frames,
                                           const std::array<application::DrawingView *, 3> &views,
                                           std::stop_token stop) {
    DrawingTubeCutLocations locations;
    for (std::size_t i = 0; i < tube.endFaces.size() && !stop.stop_requested(); ++i) {
        if (!tube.endFaces[i] || !tube.cutsDegrees[i])
            continue;
        const auto face = TopoDS::Face(faces(tube.endFaces[i]));
        const auto normal = BRepAdaptor_Surface(face, false).Plane().Axis().Direction();
        std::vector<gp_Pnt> samples;
        const auto rim = BRepTools::OuterWire(face);
        for (TopExp_Explorer edge(rim, TopAbs_EDGE); edge.More(); edge.Next()) {
            const BRepAdaptor_Curve curve(TopoDS::Edge(edge.Current()));
            for (const double f : {.5, .25, .75, 0., 1.})
                samples.push_back(
                    curve.Value(std::lerp(curve.FirstParameter(), curve.LastParameter(), f)));
        }
        std::array<int, 3> order{0, 1, 2};
        // Prefer a longitudinal view in which the cutting plane is edge-on.
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const auto cost = [&](int view) {
                return 2 * std::abs(tube.axis.Dot(frames[view].Direction())) +
                       std::abs(normal.Dot(frames[view].Direction()));
            };
            return cost(a) < cost(b);
        });
        for (int index : order) {
            const auto &frame = frames[index];
            if (std::abs(tube.axis.Dot(frame.Direction())) > .6)
                continue;
            const bool horizontal = std::abs(tube.axis.Dot(frame.XDirection())) >=
                                    std::abs(tube.axis.Dot(frame.YDirection()));
            if (auto anchor = visibleCutAnchor(samples, frame, *views[index], horizontal, stop)) {
                views[index]->tubeCuts.push_back({*anchor, *tube.cutsDegrees[i]});
                locations[i] = DrawingTubeCutLocation{static_cast<std::size_t>(index), *anchor};
                break;
            }
        }
    }
    return locations;
}
} // namespace bending::cad
