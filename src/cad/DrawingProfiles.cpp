#include "DrawingProfiles.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <IntAna_QuadQuadGeo.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>

namespace bending::cad {
namespace {
using namespace application;
using Wall = std::array<std::size_t, 2>;
struct Unmeasured : std::runtime_error {
    Unmeasured() : std::runtime_error("No unambiguous open profile at this section") {}
};
void cancellation(std::stop_token stop) {
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
}
gp_Dir axisOf(const RecognizedBend &b) {
    return gp_Dir(gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                         gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z)));
}
Wall wallOf(const RecognizedBend &b, std::size_t side) {
    Wall wall{b.flangeFaces[side * 2], b.flangeFaces[side * 2 + 1]};
    std::sort(wall.begin(), wall.end());
    return wall;
}
std::size_t wallSide(const RecognizedBend &b, Wall wall) {
    if (wallOf(b, 0) == wall)
        return 0;
    if (wallOf(b, 1) == wall)
        return 1;
    throw Unmeasured{};
}
TopoDS_Face faceAt(const TopTools_IndexedMapOfShape &faces, std::size_t index) {
    if (!index || index > static_cast<std::size_t>(faces.Extent()))
        throw Unmeasured{};
    return TopoDS::Face(faces(static_cast<int>(index)));
}
gp_Pln planeAt(const TopTools_IndexedMapOfShape &faces, std::size_t index) {
    BRepAdaptor_Surface surface(faceAt(faces, index));
    if (surface.GetType() != GeomAbs_Plane)
        throw Unmeasured{};
    return surface.Plane();
}
gp_Pnt onPlane(gp_Pnt point, const gp_Pln &plane) {
    const gp_Vec normal(plane.Axis().Direction());
    return point.Translated(-normal * gp_Vec(plane.Location(), point).Dot(normal));
}
gp_Pnt bendCenter(const RecognizedBend &b, gp_Pnt station, gp_Dir normal) {
    const gp_Pnt origin(b.axisStart.x, b.axisStart.y, b.axisStart.z);
    const auto axis = axisOf(b);
    const double denominator = axis.Dot(normal);
    if (std::abs(denominator) < 1 - 1e-7)
        throw Unmeasured{};
    return origin.Translated(gp_Vec(axis) * gp_Vec(origin, station).Dot(gp_Vec(normal)) /
                             denominator);
}
DrawingPoint project(gp_Pnt p, const gp_Ax2 &frame) {
    const gp_Vec v(frame.Location(), p);
    return {v.Dot(gp_Vec(frame.XDirection())), v.Dot(gp_Vec(frame.YDirection()))};
}
PointMm pointDto(gp_Pnt p) {
    return {p.X(), p.Y(), p.Z()};
}

// Intersect the outer wire only: internal closed holes do not split a schematic wall.
// Multiple intervals from the exterior contour are retained, never bridged across a notch.
std::vector<std::array<double, 2>> wallIntervals(const TopoDS_Face &face, gp_Pnt station,
                                                 gp_Dir normal, gp_Dir along, double tolerance,
                                                 std::stop_token stop) {
    cancellation(stop);
    const auto outer = BRepTools::OuterWire(face);
    if (outer.IsNull())
        throw Unmeasured{};
    BRepAlgoAPI_Section section(outer, gp_Pln(station, normal), false);
    section.SetNonDestructive(true);
    section.SetRunParallel(false);
    section.Build();
    cancellation(stop);
    if (!section.IsDone() || TopExp_Explorer(section.Shape(), TopAbs_EDGE).More())
        throw Unmeasured{}; // A section coincident with an edge is not a unique end.
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(section.Shape(), TopAbs_VERTEX, vertices);
    std::vector<double> points;
    for (int i = 1; i <= vertices.Extent(); ++i)
        points.push_back(
            gp_Vec(station, BRep_Tool::Pnt(TopoDS::Vertex(vertices(i)))).Dot(gp_Vec(along)));
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end(),
                             [&](double a, double b) { return std::abs(a - b) <= tolerance; }),
                 points.end());
    if (points.empty() || points.size() % 2)
        throw Unmeasured{};
    std::vector<std::array<double, 2>> intervals;
    for (std::size_t i = 0; i < points.size(); i += 2)
        if (points[i + 1] - points[i] > tolerance)
            intervals.push_back({points[i], points[i + 1]});
    return intervals;
}
std::array<double, 2> occupiedInterval(const std::vector<std::array<double, 2>> &intervals,
                                       const std::vector<double> &tangencies, double tolerance) {
    std::optional<std::array<double, 2>> found;
    for (const auto &range : intervals) {
        if (!std::all_of(tangencies.begin(), tangencies.end(), [&](double t) {
                return t >= range[0] - tolerance && t <= range[1] + tolerance &&
                       std::min(std::abs(t - range[0]), std::abs(t - range[1])) <= tolerance;
            }))
            continue;
        if (found)
            throw Unmeasured{};
        found = range;
    }
    if (!found)
        throw Unmeasured{};
    return *found;
}
bool endsAtAnotherBend(gp_Pnt point, Wall wall, const std::vector<std::size_t> &members,
                       const std::vector<RecognizedBend> &bends,
                       const TopTools_IndexedMapOfShape &faces, double tolerance) {
    for (std::size_t i = 0; i < bends.size(); ++i) {
        if (std::find(members.begin(), members.end(), i) != members.end() ||
            (wallOf(bends[i], 0) != wall && wallOf(bends[i], 1) != wall))
            continue;
        for (auto index : {bends[i].innerFace, bends[i].outerFace}) {
            const auto face = faceAt(faces, index);
            const auto cylinder = BRepAdaptor_Surface(face).Cylinder();
            if (std::abs(gp_Lin(cylinder.Axis()).Distance(point) - cylinder.Radius()) > tolerance)
                continue;
            BRepClass_FaceClassifier at(face, point, tolerance);
            if (at.State() == TopAbs_ON || at.State() == TopAbs_IN)
                return true;
        }
    }
    return false;
}

bool supportedFreeCap(const std::array<gp_Pnt, 2> &end, Wall wall, gp_Dir axis,
                      const TopTools_IndexedMapOfShape &faces, double tolerance,
                      std::stop_token stop) {
    const std::array vertices{BRepBuilderAPI_MakeVertex(end[0]).Shape(),
                              BRepBuilderAPI_MakeVertex(end[1]).Shape()};
    for (int i = 1; i <= faces.Extent(); ++i) {
        cancellation(stop);
        if (static_cast<std::size_t>(i) == wall[0] || static_cast<std::size_t>(i) == wall[1])
            continue;
        const auto face = TopoDS::Face(faces(i));
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() == GeomAbs_Plane) {
            if (surface.Plane().Distance(end[0]) > tolerance ||
                surface.Plane().Distance(end[1]) > tolerance)
                continue;
        } else if (surface.GetType() == GeomAbs_Cylinder) {
            const auto cylinder = surface.Cylinder();
            if (cylinder.Axis().Direction().IsParallel(axis, 1e-7) ||
                std::abs(gp_Lin(cylinder.Axis()).Distance(end[0]) - cylinder.Radius()) >
                    tolerance ||
                std::abs(gp_Lin(cylinder.Axis()).Distance(end[1]) - cylinder.Radius()) > tolerance)
                continue;
        } else
            continue;
        bool onBoth = true;
        for (const auto &vertex : vertices) {
            BRepExtrema_DistShapeShape distance(vertex, face);
            onBoth = onBoth && distance.IsDone() && distance.Value() <= tolerance;
        }
        if (onBoth)
            return true;
    }
    return false; // Do not mistake the start of an unrecognized bend for a free edge.
}

DrawingProfile buildProfile(const std::vector<std::size_t> &members,
                            const std::vector<RecognizedBend> &bends,
                            const TopTools_IndexedMapOfShape &faces, const gp_Ax2 &reference,
                            double tolerance, std::stop_token stop, double fraction = .5) {
    std::map<Wall, std::vector<std::size_t>> graph;
    for (auto index : members)
        for (std::size_t side = 0; side < 2; ++side)
            graph[wallOf(bends[index], side)].push_back(index);
    std::vector<Wall> endWalls;
    for (const auto &[wall, neighbors] : graph) {
        if (neighbors.size() > 2)
            throw Unmeasured{};
        if (neighbors.size() == 1)
            endWalls.push_back(wall);
    }
    if (endWalls.size() != 2)
        throw Unmeasured{}; // Closed/branched graphs need their own representation.
    std::vector<Wall> walls{endWalls.front()};
    std::vector<std::size_t> ordered;
    while (ordered.size() < members.size()) {
        const auto &neighbors = graph.at(walls.back());
        const auto next = std::find_if(neighbors.begin(), neighbors.end(), [&](auto index) {
            return std::find(ordered.begin(), ordered.end(), index) == ordered.end();
        });
        if (next == neighbors.end())
            throw Unmeasured{};
        const auto &bend = bends[*next];
        const auto side = wallSide(bend, walls.back());
        ordered.push_back(*next);
        walls.push_back(wallOf(bend, 1 - side));
    }
    if (walls.back() != endWalls.back())
        throw Unmeasured{};
    auto axis = axisOf(bends[ordered.front()]);
    for (auto direction : {reference.Direction(), reference.YDirection(), reference.XDirection()})
        if (std::abs(axis.Dot(direction)) > 1e-5) {
            if (axis.Dot(direction) < 0)
                axis.Reverse();
            break;
        }
    double low = -1e100, high = 1e100;
    const double thickness = bends[ordered.front()].thicknessMm;
    for (auto index : ordered) {
        const auto &b = bends[index];
        if (!axis.IsParallel(axisOf(b), 1e-7) || std::abs(b.thicknessMm - thickness) > tolerance)
            throw Unmeasured{};
        const double a =
            gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z))
                .Dot(gp_Vec(axis));
        const double z = gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z))
                             .Dot(gp_Vec(axis));
        low = std::max(low, std::min(a, z));
        high = std::min(high, std::max(a, z));
    }
    if (high - low <= 20 * tolerance)
        throw Unmeasured{};
    const gp_Pnt station(gp_XYZ(axis.X(), axis.Y(), axis.Z()) * (low + (high - low) * fraction));
    const auto frame = drawingSectionFrame(station, axis, reference);
    std::vector<DrawingBendCorner> corners;
    for (auto index : ordered) {
        const auto corner = drawingBendCorner(bends[index], faces, station);
        if (!corner)
            throw Unmeasured{};
        corners.push_back(*corner);
    }
    DrawingProfile profile;
    profile.bends = ordered;
    profile.station = pointDto(station);
    profile.normal = {axis.X(), axis.Y(), axis.Z()};
    std::vector<std::array<gp_Pnt, 2>> middleEnds;
    std::vector<std::array<std::array<gp_Pnt, 2>, 2>> contourEnds;
    for (std::size_t w = 0; w < walls.size(); ++w) {
        cancellation(stop);
        const auto wall = walls[w];
        const auto firstPlane = planeAt(faces, wall[0]), secondPlane = planeAt(faces, wall[1]);
        if (!firstPlane.Axis().Direction().IsParallel(secondPlane.Axis().Direction(), 1e-7) ||
            std::abs(firstPlane.Distance(secondPlane.Location()) - thickness) > tolerance)
            throw Unmeasured{};
        const gp_Dir along(gp_Vec(axis).Crossed(gp_Vec(firstPlane.Axis().Direction())));
        const auto &neighbors = graph.at(wall);
        std::array<std::array<gp_Pnt, 2>, 2> wallEnds;
        for (std::size_t f = 0; f < 2; ++f) {
            const auto plane = planeAt(faces, wall[f]);
            std::vector<double> tangencies;
            for (auto neighbor : neighbors) {
                const auto tangent = onPlane(bendCenter(bends[neighbor], station, axis), plane);
                tangencies.push_back(gp_Vec(station, tangent).Dot(gp_Vec(along)));
            }
            const auto range = occupiedInterval(
                wallIntervals(faceAt(faces, wall[f]), station, axis, along, tolerance, stop),
                tangencies, tolerance * 20);
            const auto pointAt = [&](double coordinate) {
                return onPlane(station.Translated(gp_Vec(along) * coordinate), plane);
            };
            if (neighbors.size() == 2) {
                wallEnds[f] = {onPlane(bendCenter(bends[ordered[w - 1]], station, axis), plane),
                               onPlane(bendCenter(bends[ordered[w]], station, axis), plane)};
            } else {
                const double end =
                    std::abs(tangencies[0] - range[0]) < std::abs(tangencies[0] - range[1])
                        ? range[1]
                        : range[0];
                const auto free = pointAt(end);
                if (endsAtAnotherBend(free, wall, members, bends, faces, tolerance * 20))
                    throw Unmeasured{};
                const auto tangent = pointAt(tangencies[0]);
                wallEnds[f] = w == 0 ? std::array{free, tangent} : std::array{tangent, free};
            }
        }
        const auto middle = [&](std::size_t end) {
            return gp_Pnt((wallEnds[0][end].XYZ() + wallEnds[1][end].XYZ()) / 2);
        };
        if (neighbors.size() == 1) {
            const std::size_t end = w == 0 ? 0 : 1;
            if (!supportedFreeCap({wallEnds[0][end], wallEnds[1][end]}, wall, axis, faces,
                                  tolerance * 20, stop))
                throw Unmeasured{};
        }
        const std::array mid{middle(0), middle(1)};
        middleEnds.push_back(mid);
        contourEnds.push_back(wallEnds);
        for (const auto &ends : wallEnds)
            profile.view.paths.push_back(
                {{project(ends[0], frame), project(ends[1], frame)}, false});
        if (neighbors.size() == 1) {
            const std::size_t end = w == 0 ? 0 : 1;
            profile.view.paths.push_back(
                {{project(wallEnds[0][end], frame), project(wallEnds[1][end], frame)}, false});
        }
        const auto adjacentPosition = w == 0 ? 0 : w - 1;
        const auto adjacent = ordered[adjacentPosition];
        const auto side = wallSide(bends[adjacent], wall);
        const auto outsideFace = bends[adjacent].flangeFaces[side * 2 + 1];
        const auto outside = planeAt(faces, outsideFace);
        const auto facePosition = outsideFace == wall[0] ? 0u : 1u;
        const auto first = w == 0 ? wallEnds[facePosition][0] : corners[w - 1].point;
        const auto second = w == ordered.size() ? wallEnds[facePosition][1] : corners[w].point;
        // Reverse bends use opposite sides of the same physical wall. Project the two
        // outer virtual corners onto one wall plane: the dimension is along the wall.
        const auto a = onPlane(first, outside), b = onPlane(second, outside);
        const double length = std::abs(gp_Vec(a, b).Dot(gp_Vec(along)));
        if (length <= tolerance || std::abs(a.Distance(b) - length) > tolerance * 20)
            throw Unmeasured{};
        DrawingProfileSegment segment{
            {project(a, frame), project(b, frame), length, "P" + std::to_string(w + 1)},
            wall,
            w ? std::optional(ordered[w - 1]) : std::nullopt,
            w < ordered.size() ? std::optional(ordered[w]) : std::nullopt};
        profile.segments.push_back(std::move(segment));
    }
    profile.view.profileStart =
        std::pair{project(middleEnds.front()[0], frame), project(middleEnds.front()[1], frame)};
    for (std::size_t k = 0; k < ordered.size(); ++k) {
        cancellation(stop);
        const auto &bend = bends[ordered[k]];
        const auto &corner = corners[k];
        const auto bisector = gp_Vec(corner.directions[0]) + gp_Vec(corner.directions[1]);
        const auto innerCorner = corner.point.Translated(
            bisector.Normalized() * (bend.thicknessMm / std::sin(corner.interior / 2)));
        profile.view.angles.push_back(
            {project(innerCorner, frame),
             project(innerCorner.Translated(gp_Vec(corner.directions[0])), frame),
             project(innerCorner.Translated(gp_Vec(corner.directions[1])), frame), corner.interior,
             bend.innerRadiusMm / std::sin(corner.interior / 2) - bend.innerRadiusMm});
        const auto before = gp_Vec(middleEnds[k][0], middleEnds[k][1]).Normalized();
        const auto after = gp_Vec(middleEnds[k + 1][0], middleEnds[k + 1][1]).Normalized();
        const double turn = std::atan2(gp_Vec(axis).Dot(before.Crossed(after)), before.Dot(after));
        if (std::abs(std::abs(turn) - bend.angleRadians) > 1e-5)
            throw Unmeasured{};
        profile.turnSigns.push_back(turn > 0 ? 1 : -1);
        BRepAdaptor_Surface surface(faceAt(faces, bend.innerFace));
        double u0, u1, v0, v1;
        BRepTools::UVBounds(faceAt(faces, bend.innerFace), u0, u1, v0, v1);
        const auto samePoint = [tolerance](DrawingPoint a, DrawingPoint b) {
            return std::hypot(a.x - b.x, a.y - b.y) <= tolerance * 20;
        };
        DrawingPoint anchor;
        for (double offset : {0., bend.thicknessMm}) {
            DrawingPath arc;
            for (int j = 0; j <= 32; ++j) {
                auto point = surface.Value(u0 + (u1 - u0) * j / 32., (v0 + v1) / 2);
                const auto center = bendCenter(bend, point, axis);
                point.Translate(gp_Vec(center, point) * (offset / bend.innerRadiusMm));
                point.Translate(gp_Vec(axis) * gp_Vec(point, station).Dot(gp_Vec(axis)));
                arc.points.push_back(project(point, frame));
            }
            const auto matches = [&](DrawingPoint a, DrawingPoint b) {
                return std::any_of(contourEnds[k].begin(), contourEnds[k].end(),
                                   [&](const auto &ends) {
                                       return samePoint(a, project(ends[1], frame));
                                   }) &&
                       std::any_of(
                           contourEnds[k + 1].begin(), contourEnds[k + 1].end(),
                           [&](const auto &ends) { return samePoint(b, project(ends[0], frame)); });
            };
            if (!(matches(arc.points.front(), arc.points.back()) ||
                  matches(arc.points.back(), arc.points.front())))
                throw Unmeasured{};
            anchor.x += arc.points[16].x / 2;
            anchor.y += arc.points[16].y / 2;
            profile.view.paths.push_back(std::move(arc));
        }
        profile.view.bendNotes.push_back({anchor, ordered[k]});
        profile.view.dimensionedBends.push_back(ordered[k]);
    }
    profile.view.minimum = {1e100, 1e100};
    profile.view.maximum = {-1e100, -1e100};
    for (const auto &path : profile.view.paths)
        for (auto p : path.points) {
            profile.view.minimum.x = std::min(profile.view.minimum.x, p.x);
            profile.view.minimum.y = std::min(profile.view.minimum.y, p.y);
            profile.view.maximum.x = std::max(profile.view.maximum.x, p.x);
            profile.view.maximum.y = std::max(profile.view.maximum.y, p.y);
        }
    // Outside dimension witness points are also part of the required view envelope.
    for (const auto &segment : profile.segments)
        for (auto p : {segment.dimension.start, segment.dimension.end}) {
            profile.view.minimum.x = std::min(profile.view.minimum.x, p.x);
            profile.view.minimum.y = std::min(profile.view.minimum.y, p.y);
            profile.view.maximum.x = std::max(profile.view.maximum.x, p.x);
            profile.view.maximum.y = std::max(profile.view.maximum.y, p.y);
        }
    return profile;
}
} // namespace

gp_Ax2 drawingSectionFrame(gp_Pnt origin, gp_Dir normal, const gp_Ax2 &reference) {
    const auto from = reference.Direction();
    const double cosine = std::clamp(from.Dot(normal), -1., 1.);
    const gp_Vec trace = gp_Vec(from).Crossed(gp_Vec(normal));
    const double sine = trace.Magnitude();
    gp_Vec right(reference.XDirection());
    if (sine > 1e-12) {
        const gp_Vec axis = trace / sine;
        // Rodrigues' shortest rotation maps the source normal onto the cut
        // normal while leaving the common trace fixed, including oblique cuts.
        right =
            right * cosine + axis.Crossed(right) * sine + axis * (axis.Dot(right) * (1 - cosine));
    } else if (cosine < 0) {
        // Parallel planes have no unique trace. Schematics keep source-up;
        // a located section still requires an edge-on source view in location().
        right.Reverse();
    }
    return {origin, normal, gp_Dir(right)};
}

std::optional<DrawingBendCorner> drawingBendCorner(const application::RecognizedBend &bend,
                                                   const TopTools_IndexedMapOfShape &faces,
                                                   gp_Pnt station) {
    try {
        const auto axis = axisOf(bend);
        const auto first = planeAt(faces, bend.flangeFaces[1]);
        const auto second = planeAt(faces, bend.flangeFaces[3]);
        IntAna_QuadQuadGeo intersection(first, second, 1e-8, 1e-6);
        if (!intersection.IsDone() || intersection.TypeInter() != IntAna_Line ||
            intersection.NbSolutions() != 1)
            return {};
        const auto line = intersection.Line(1);
        const double denominator = line.Direction().Dot(axis);
        if (std::abs(denominator) < 1 - 1e-7)
            return {};
        const auto corner = line.Location().Translated(
            gp_Vec(line.Direction()) * gp_Vec(line.Location(), station).Dot(gp_Vec(axis)) /
            denominator);
        const auto center = bendCenter(bend, station, axis);
        std::array<gp_Dir, 2> directions{gp_Dir(gp_Vec(corner, onPlane(center, first))),
                                         gp_Dir(gp_Vec(corner, onPlane(center, second)))};
        const double interior = std::acos(std::clamp(directions[0].Dot(directions[1]), -1., 1.));
        if (interior < 1e-5 || interior > std::numbers::pi - 1e-5 ||
            std::abs(interior + bend.angleRadians - std::numbers::pi) > 1e-5)
            return {};
        return DrawingBendCorner{corner, directions, interior};
    } catch (const Unmeasured &) {
        return {};
    } catch (const Standard_Failure &) {
        return {};
    }
}

std::vector<application::DrawingProfile>
drawingProfiles(const std::vector<application::RecognizedBend> &bends,
                const TopTools_IndexedMapOfShape &faces, const gp_Ax2 &reference, double tolerance,
                std::stop_token stop) {
    std::vector<DrawingProfile> candidates;
    if (bends.empty() || bends.size() > 128)
        return candidates;
    tolerance = std::max(tolerance, 1e-6);
    std::vector<std::vector<std::size_t>> sets;
    for (std::size_t seed = 0; seed < bends.size(); ++seed) {
        cancellation(stop);
        const auto axis = axisOf(bends[seed]);
        const auto spanOf = [&](const auto &b) {
            const double a =
                gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z))
                    .Dot(gp_Vec(axis));
            const double z = gp_Vec(gp_Pnt(0, 0, 0), gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z))
                                 .Dot(gp_Vec(axis));
            return std::array{std::min(a, z), std::max(a, z)};
        };
        auto span = spanOf(bends[seed]);
        std::vector<std::size_t> members{seed};
        for (std::size_t k = 0; k < members.size(); ++k)
            for (std::size_t j = 0; j < bends.size(); ++j) {
                if (std::find(members.begin(), members.end(), j) != members.end() ||
                    !axis.IsParallel(axisOf(bends[j]), 1e-7))
                    continue;
                bool linked = false;
                for (std::size_t a = 0; a < 2; ++a)
                    for (std::size_t b = 0; b < 2; ++b)
                        linked = linked || wallOf(bends[members[k]], a) == wallOf(bends[j], b);
                const auto next = spanOf(bends[j]);
                if (!linked ||
                    std::min(span[1], next[1]) - std::max(span[0], next[0]) <= 20 * tolerance)
                    continue;
                members.push_back(j);
                span = {std::max(span[0], next[0]), std::min(span[1], next[1])};
            }
        std::sort(members.begin(), members.end());
        if (std::find(sets.begin(), sets.end(), members) != sets.end())
            continue;
        sets.push_back(members);
        try {
            candidates.push_back(buildProfile(members, bends, faces, reference, tolerance, stop));
        } catch (const Unmeasured &) {
            // Ordinary views and local dimensions remain available; coverage reports gaps.
        } catch (const Standard_Failure &) {
            // An optional schematic must not hide the source views.
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &a, const auto &b) { return a.bends.size() > b.bends.size(); });
    std::vector<DrawingProfile> result;
    std::vector<bool> used(bends.size());
    for (auto &candidate : candidates) {
        if (std::any_of(candidate.bends.begin(), candidate.bends.end(),
                        [&](auto i) { return used[i]; }))
            continue;
        for (auto i : candidate.bends)
            used[i] = true;
        candidate.designation = "П" + std::to_string(result.size() + 1);
        result.push_back(std::move(candidate));
    }
    return result;
}
std::optional<application::DrawingProfile>
drawingProfileAt(const std::vector<std::size_t> &members,
                 const std::vector<application::RecognizedBend> &bends,
                 const TopTools_IndexedMapOfShape &faces, const gp_Ax2 &reference, double tolerance,
                 double fraction, std::stop_token stop) {
    try {
        return buildProfile(members, bends, faces, reference, std::max(1e-6, tolerance), stop,
                            fraction);
    } catch (const Unmeasured &) {
        return {};
    } catch (const Standard_Failure &) {
        return {};
    }
}
} // namespace bending::cad
