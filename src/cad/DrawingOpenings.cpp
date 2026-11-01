#include "DrawingOpenings.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ElCLib.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <numbers>

namespace bending::cad {
namespace {
using namespace application;
constexpr double tolerance = 1e-5;
DrawingPoint project(gp_Pnt p, const gp_Ax2 &frame) {
    const gp_Vec v(gp_Pnt(0, 0, 0), p);
    return {v.Dot(gp_Vec(frame.XDirection())), v.Dot(gp_Vec(frame.YDirection()))};
}
double length(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(shape, properties, true);
    return properties.Mass();
}
bool covered(const TopoDS_Shape &wire, const TopoDS_Shape &other) {
    try {
        TopTools_ListOfShape arguments, tools;
        arguments.Append(wire);
        tools.Append(other);
        BRepAlgoAPI_Common common;
        common.SetArguments(arguments);
        common.SetTools(tools);
        common.SetFuzzyValue(1e-6);
        common.Build();
        const double expected = length(wire);
        if (!common.IsDone())
            return false;
        const double actual = length(common.Shape());
        return expected > tolerance &&
               std::abs(actual - expected) < std::max(tolerance, expected * 1e-7);
    } catch (const Standard_Failure &) {
        return false;
    }
}

using Intervals = std::vector<std::array<double, 2>>;
double wrap(double value) {
    value = std::fmod(value, 2 * std::numbers::pi);
    return value < 0 ? value + 2 * std::numbers::pi : value;
}
bool intervalsCover(Intervals visible, const Intervals &required, double epsilon) {
    std::sort(visible.begin(), visible.end());
    for (const auto interval : required) {
        double end = interval[0];
        for (const auto segment : visible) {
            if (segment[1] < end - epsilon)
                continue;
            if (segment[0] > end + epsilon)
                break;
            end = std::max(end, segment[1]);
            if (end >= interval[1] - epsilon)
                break;
        }
        if (end < interval[1] - epsilon)
            return false;
    }
    return true;
}
bool visibleCoverage(const TopoDS_Shape &wire, const TopoDS_Shape &visible) {
    for (TopExp_Explorer source(wire, TopAbs_EDGE); source.More(); source.Next()) {
        BRepAdaptor_Curve reference(TopoDS::Edge(source.Current()));
        const auto type = reference.GetType();
        const bool periodic = type == GeomAbs_Circle || type == GeomAbs_Ellipse;
        const auto parameter = [&](gp_Pnt point) {
            point.SetZ(0);
            if (type == GeomAbs_Line)
                return ElCLib::Parameter(reference.Line(), point);
            if (type == GeomAbs_Circle)
                return ElCLib::Parameter(reference.Circle(), point);
            return ElCLib::Parameter(reference.Ellipse(), point);
        };
        const auto at = [&](double p) {
            if (type == GeomAbs_Line)
                return ElCLib::Value(p, reference.Line());
            if (type == GeomAbs_Circle)
                return ElCLib::Value(p, reference.Circle());
            return ElCLib::Value(p, reference.Ellipse());
        };
        const auto intervals = [&](const BRepAdaptor_Curve &curve) {
            const double a = parameter(curve.Value(curve.FirstParameter()));
            const double b = parameter(curve.Value(curve.LastParameter()));
            if (!periodic)
                return Intervals{{std::min(a, b), std::max(a, b)}};
            const double middle =
                parameter(curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2));
            double start = a, span = wrap(b - a);
            if (wrap(middle - a) > span + 1e-9) {
                start = b;
                span = 2 * std::numbers::pi - span;
            }
            if (span > 2 * std::numbers::pi - 1e-7)
                return Intervals{{0, 2 * std::numbers::pi}};
            start = wrap(start);
            Intervals ranges{{start, std::min(2 * std::numbers::pi, start + span)}};
            if (start + span > 2 * std::numbers::pi)
                ranges.push_back({0, start + span - 2 * std::numbers::pi});
            return ranges;
        };
        Intervals fragments;
        for (TopExp_Explorer ei(visible, TopAbs_EDGE); ei.More(); ei.Next()) {
            BRepAdaptor_Curve curve(TopoDS::Edge(ei.Current()));
            if (curve.GetType() != type) {
                if (!periodic ||
                    (curve.GetType() != GeomAbs_Circle && curve.GetType() != GeomAbs_Ellipse))
                    continue;
                const auto ellipse =
                    type == GeomAbs_Ellipse ? reference.Ellipse() : curve.Ellipse();
                if (std::abs(ellipse.MajorRadius() - ellipse.MinorRadius()) > tolerance)
                    continue; // HLR can express a projected circle as a near-circular ellipse.
            }
            bool same = true;
            for (int sample = 0; sample <= 4; ++sample) {
                auto point =
                    curve.Value(curve.FirstParameter() +
                                (curve.LastParameter() - curve.FirstParameter()) * sample / 4.);
                point.SetZ(0);
                if (point.Distance(at(parameter(point))) > tolerance) {
                    same = false;
                    break;
                }
            }
            if (!same)
                continue;
            auto ranges = intervals(curve);
            fragments.insert(fragments.end(), ranges.begin(), ranges.end());
        }
        const double parameterTolerance =
            type == GeomAbs_Circle    ? tolerance / reference.Circle().Radius()
            : type == GeomAbs_Ellipse ? tolerance / reference.Ellipse().MajorRadius()
                                      : tolerance;
        if (!intervalsCover(std::move(fragments), intervals(reference), parameterTolerance))
            return false;
    }
    return true;
}
std::array<double, 4> bounds(const TopoDS_Shape &shape, const gp_Ax2 &frame) {
    gp_Trsf transform;
    transform.SetTransformation(gp_Ax3(frame));
    Bnd_Box box;
    BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(shape, transform, true).Shape(), box, false,
                           false);
    return {box.CornerMin().X(), box.CornerMin().Y(), box.CornerMax().X(), box.CornerMax().Y()};
}
} // namespace
std::vector<DrawingOpening> drawingOpenings(const TopoDS_Shape &shape,
                                            const TopTools_IndexedMapOfShape &faces,
                                            std::stop_token stop) {
    TopTools_IndexedDataMapOfShapeListOfShape neighbors;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, neighbors);
    std::vector<DrawingOpening> result;
    for (int id = 1; id <= faces.Extent(); ++id) {
        if (stop.stop_requested())
            throw DrawingError{"Cancelled", true};
        const auto face = TopoDS::Face(faces(id));
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane)
            continue;
        const auto plane = surface.Plane();
        const auto outer = BRepTools::OuterWire(face);
        for (TopExp_Explorer wi(face, TopAbs_WIRE); wi.More(); wi.Next()) {
            const auto wire = TopoDS::Wire(wi.Current());
            if (wire.IsSame(outer) || !BRep_Tool::IsClosed(wire))
                continue;
            std::vector<gp_Pnt> circleCenters;
            gp_Dir major = plane.XAxis().Direction();
            bool allowed = true, curved = false, circular = true;
            double radius = 0, span = 0;
            double longestLine = 0;
            std::vector<gp_Dir> lineDirections;
            int edges = 0;
            for (BRepTools_WireExplorer ei(wire, face); ei.More(); ei.Next()) {
                BRepAdaptor_Curve curve(ei.Current());
                if (++edges > 64) {
                    allowed = false;
                    break;
                }
                if (curve.GetType() == GeomAbs_Circle) {
                    const auto circle = curve.Circle();
                    curved = true;
                    if (!circleCenters.empty() &&
                        (circle.Location().Distance(circleCenters.front()) > tolerance ||
                         std::abs(radius - circle.Radius()) > tolerance))
                        circular = false;
                    circleCenters.push_back(circle.Location());
                    radius = circle.Radius();
                    span += curve.LastParameter() - curve.FirstParameter();
                } else if (curve.GetType() == GeomAbs_Ellipse &&
                           curve.LastParameter() - curve.FirstParameter() >
                               2 * std::numbers::pi - 1e-7) {
                    major = curve.Ellipse().XAxis().Direction();
                    curved = true;
                    circular = false;
                } else if (curve.GetType() == GeomAbs_Line) {
                    circular = false;
                    lineDirections.push_back(curve.Line().Direction());
                    const auto size = curve.Value(curve.FirstParameter())
                                          .Distance(curve.Value(curve.LastParameter()));
                    if (size > longestLine) {
                        longestLine = size;
                        major = curve.Line().Direction();
                    }
                } else {
                    allowed = false;
                    break;
                }
            }
            const bool rectangle =
                !curved && edges == 4 && lineDirections.size() == 4 &&
                std::all_of(lineDirections.begin(), lineDirections.end(), [&](auto direction) {
                    return direction.IsParallel(major, 1e-7) || direction.IsNormal(major, 1e-7);
                });
            if (!allowed || (!curved && !rectangle))
                continue;
            const auto box = bounds(wire, plane.Position().Ax2());
            const auto center = plane.Location().Translated(
                gp_Vec(plane.XAxis().Direction()) * ((box[0] + box[2]) / 2) +
                gp_Vec(plane.YAxis().Direction()) * ((box[1] + box[3]) / 2));
            circular = circular && std::abs(span - 2 * std::numbers::pi) < 1e-6;
            if (!circular) {
                gp_Trsf rotate;
                rotate.SetRotation(gp_Ax1(center, plane.Axis().Direction()), std::numbers::pi);
                if (!covered(wire, BRepBuilderAPI_Transform(wire, rotate, true).Shape()))
                    continue; // A centroid or bounding-box midpoint is not an axis of symmetry.
            }
            double farthest = 0;
            for (auto a : circleCenters)
                for (auto b : circleCenters)
                    if (a.Distance(b) > std::max(tolerance, farthest)) {
                        farthest = a.Distance(b);
                        major = gp_Dir(gp_Vec(a, b));
                    }
            const auto dimensions = bounds(wire, gp_Ax2(center, plane.Axis().Direction(), major));
            DrawingOpening opening{wire,
                                   center,
                                   plane.Axis().Direction(),
                                   gp_Vec(major) *
                                       std::max(std::abs(dimensions[0]), std::abs(dimensions[2])),
                                   gp_Vec(plane.Axis().Direction()).Crossed(gp_Vec(major)) *
                                       std::max(std::abs(dimensions[1]), std::abs(dimensions[3])),
                                   circular,
                                   {}};
            for (TopExp_Explorer ei(wire, TopAbs_EDGE); ei.More(); ei.Next())
                for (const auto &neighbor : neighbors.FindFromKey(ei.Current())) {
                    BRepAdaptor_Surface wall(TopoDS::Face(neighbor));
                    if (wall.GetType() == GeomAbs_Cylinder) {
                        const auto index = static_cast<std::size_t>(faces.FindIndex(neighbor));
                        if (std::find(opening.wallFaces.begin(), opening.wallFaces.end(), index) ==
                            opening.wallFaces.end())
                            opening.wallFaces.push_back(index);
                    }
                }
            result.push_back(std::move(opening));
        }
    }
    return result;
}
void addDrawingOpenings(const std::vector<DrawingOpening> &openings, const gp_Ax2 &projection,
                        const TopoDS_Shape &visible, application::DrawingView &view,
                        std::stop_token stop) {
    std::vector<DrawingPoint> centers;
    for (const auto &opening : openings) {
        if (stop.stop_requested())
            throw DrawingError{"Cancelled", true};
        if (!opening.normal.IsParallel(projection.Direction(), 1e-7))
            continue;
        const auto c = project(opening.center, projection);
        const bool duplicate = std::any_of(centers.begin(), centers.end(), [&](auto old) {
            return std::hypot(old.x - c.x, old.y - c.y) < tolerance;
        });
        if (duplicate &&
            (!opening.circular ||
             std::any_of(
                 view.holeDiameters.begin(), view.holeDiameters.end(), [&](const auto &old) {
                     return std::hypot(old.first.x - c.x, old.first.y - c.y) < tolerance &&
                            std::abs(old.second - 2 * opening.major.Magnitude()) < tolerance;
                 })))
            continue;
        const double depth =
            gp_Vec(gp_Pnt(0, 0, 0), opening.center).Dot(gp_Vec(projection.Direction()));
        gp_Trsf flatten;
        flatten.SetTransformation(
            gp_Ax3(gp_Pnt(0, 0, 0).Translated(gp_Vec(projection.Direction()) * depth),
                   projection.Direction(), projection.XDirection()));
        const bool knownCircle =
            opening.circular
                ? std::any_of(view.circleCenters.begin(), view.circleCenters.end(),
                              [&](const auto &circle) {
                                  return std::hypot(circle.first.x - c.x, circle.first.y - c.y) <
                                             tolerance &&
                                         std::abs(circle.second - opening.major.Magnitude()) <
                                             tolerance;
                              })
                : false;
        const bool fullyVisible =
            knownCircle ||
            visibleCoverage(BRepBuilderAPI_Transform(opening.wire, flatten, true).Shape(), visible);
        if (!fullyVisible)
            continue;
        if (opening.circular)
            view.holeDiameters.push_back({c, 2 * opening.major.Magnitude()});
        if (duplicate)
            continue;
        centers.push_back(c);
        ++view.visibleOpeningCount;
        if (opening.circular && !knownCircle)
            view.circleCenters.push_back({c, opening.major.Magnitude()});
        if (!opening.circular)
            view.openingAxes.push_back(
                {c, project(opening.center.Translated(opening.major), projection),
                 project(opening.center.Translated(opening.minor), projection)});
    }
    for (bool horizontal : {true, false}) {
        auto sorted = centers;
        std::sort(sorted.begin(), sorted.end(), [horizontal](auto a, auto b) {
            return horizontal ? (a.x == b.x ? a.y < b.y : a.x < b.x)
                              : (a.y == b.y ? a.x < b.x : a.y < b.y);
        });
        std::optional<double> previous;
        for (const auto c : sorted) {
            const double coordinate = horizontal ? c.x : c.y;
            const double value = coordinate - (horizontal ? view.minimum.x : view.minimum.y);
            if (value <= tolerance || (previous && coordinate - *previous < tolerance))
                continue;
            previous = coordinate;
            view.holeDimensions.push_back(
                {horizontal ? DrawingPoint{view.minimum.x, c.y} : DrawingPoint{c.x, view.minimum.y},
                 c, value, ""});
        }
    }
}
std::vector<std::size_t> drawingContourRoundings(const TopoDS_Shape &shape,
                                                 const TopTools_IndexedMapOfShape &faces,
                                                 const std::vector<RecognizedBend> &bends,
                                                 double linearTolerance, std::stop_token stop) {
    TopTools_IndexedDataMapOfShapeListOfShape neighbors;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, neighbors);
    std::vector<std::size_t> result;
    linearTolerance = std::max(1e-5, linearTolerance);
    for (int i = 1; i <= faces.Extent(); ++i) {
        if (stop.stop_requested())
            throw DrawingError{"Cancelled", true};
        const auto face = TopoDS::Face(faces(i));
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Cylinder)
            continue;
        std::vector<std::size_t> walls;
        for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
            const auto edge = TopoDS::Edge(e.Current());
            if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Circle)
                continue;
            for (const auto &neighbor : neighbors.FindFromKey(edge)) {
                const auto flat = TopoDS::Face(neighbor);
                BRepAdaptor_Surface plane(flat);
                if (plane.GetType() != GeomAbs_Plane ||
                    !plane.Plane().Axis().Direction().IsParallel(
                        surface.Cylinder().Axis().Direction(), 1e-7))
                    continue;
                bool outer = false;
                for (TopExp_Explorer oe(BRepTools::OuterWire(flat), TopAbs_EDGE); oe.More();
                     oe.Next())
                    outer = outer || oe.Current().IsSame(edge);
                const auto index = static_cast<std::size_t>(faces.FindIndex(flat));
                if (outer && std::find(walls.begin(), walls.end(), index) == walls.end())
                    walls.push_back(index);
            }
        }
        if (walls.size() != 2)
            continue;
        std::sort(walls.begin(), walls.end());
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        for (const auto &b : bends)
            for (std::size_t side = 0; side < 2; ++side) {
                std::array pair{b.flangeFaces[side * 2], b.flangeFaces[side * 2 + 1]};
                std::sort(pair.begin(), pair.end());
                if (pair[0] == walls[0] && pair[1] == walls[1] &&
                    std::abs(v1 - v0 - b.thicknessMm) <= linearTolerance &&
                    std::find(result.begin(), result.end(), static_cast<std::size_t>(i)) ==
                        result.end())
                    result.push_back(static_cast<std::size_t>(i));
            }
    }
    return result;
}
} // namespace bending::cad
