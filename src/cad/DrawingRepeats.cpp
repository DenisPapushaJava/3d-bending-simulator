#include "DrawingRepeats.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>

namespace bending::cad {
namespace {
using namespace application;
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
}
double length(const TopoDS_Shape &shape) {
    GProp_GProps p;
    BRepGProp::LinearProperties(shape, p, true);
    return p.Mass();
}
TopoDS_Shape slice(const TopoDS_Shape &shape, double low, double high, double bottom, double top,
                   double tolerance, std::stop_token stop) {
    cancelled(stop);
    const auto face =
        BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), low, high, bottom, top)
            .Face();
    BRepAlgoAPI_Common operation(shape, face);
    cancelled(stop);
    if (!operation.IsDone())
        throw Standard_Failure("Cannot clip repeated section");
    TopoDS_Compound result;
    BRep_Builder builder;
    builder.MakeCompound(result);
    for (TopExp_Explorer it(operation.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        const auto a = curve.Value(curve.FirstParameter()), b = curve.Value(curve.LastParameter());
        // A free end cap is not repeated at an interior clipping boundary.
        if (curve.GetType() == GeomAbs_Line &&
            ((std::abs(a.X() - low) < tolerance && std::abs(b.X() - low) < tolerance) ||
             (std::abs(a.X() - high) < tolerance && std::abs(b.X() - high) < tolerance)))
            continue;
        builder.Add(result, it.Current());
    }
    return result;
}
std::vector<DrawingPoint> clipped(std::vector<DrawingPoint> loop, double low, double high) {
    for (bool left : {true, false}) {
        std::vector<DrawingPoint> result;
        const double boundary = left ? low : high;
        const auto inside = [&](DrawingPoint p) { return left ? p.x >= low : p.x <= high; };
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const auto a = loop[i], b = loop[(i + 1) % loop.size()];
            if (inside(a))
                result.push_back(a);
            if (inside(a) != inside(b)) {
                const double t = (boundary - a.x) / (b.x - a.x);
                result.push_back({boundary, a.y + t * (b.y - a.y)});
            }
        }
        loop = std::move(result);
    }
    if (!loop.empty())
        loop.push_back(loop.front());
    return loop;
}
void bounds(DrawingView &view) {
    view.minimum = {1e100, 1e100};
    view.maximum = {-1e100, -1e100};
    for (const auto &path : view.paths)
        for (auto p : path.points) {
            view.minimum.x = std::min(view.minimum.x, p.x);
            view.minimum.y = std::min(view.minimum.y, p.y);
            view.maximum.x = std::max(view.maximum.x, p.x);
            view.maximum.y = std::max(view.maximum.y, p.y);
        }
}
} // namespace
std::optional<RepeatedDrawingSection>
repeatedDrawingSection(const TopoDS_Shape &flat, const DrawingView &actual,
                       const DrawingProfile &profile, const std::vector<RecognizedBend> &bends,
                       double tolerance, std::stop_token stop) {
    const auto n = profile.bends.size();
    if (n < 4 || profile.view.angles.size() != n || profile.segments.size() != n + 1 ||
        profile.turnSigns.size() != n || actual.omittedX || actual.sectionLoops.size() != 1)
        return {};
    const auto &first = bends[profile.bends.front()];
    for (auto i : profile.bends)
        if (std::abs(bends[i].innerRadiusMm - first.innerRadiusMm) > tolerance ||
            std::abs(bends[i].thicknessMm - first.thicknessMm) > tolerance ||
            std::abs(bends[i].lengthMm - first.lengthMm) > tolerance)
            return {}; // Common R/t/L remain explicit in the sheet header.
    const auto samePoint = [&](DrawingPoint a, DrawingPoint b) {
        return std::hypot(a.x - b.x, a.y - b.y) <= tolerance;
    };
    for (std::size_t period = 2; period <= n / 2; ++period) {
        cancelled(stop);
        if (n % period)
            continue;
        const auto origin = profile.view.angles[0].vertex;
        const auto next = profile.view.angles[period].vertex;
        const DrawingPoint shift{next.x - origin.x, next.y - origin.y};
        const double pitch = std::hypot(shift.x, shift.y);
        if (pitch <= tolerance)
            continue;
        bool same = true;
        for (std::size_t i = period; i < n && same; ++i) {
            const auto phase = i % period, repeat = i / period;
            const auto translated = [&](DrawingPoint p) {
                return DrawingPoint{p.x + repeat * shift.x, p.y + repeat * shift.y};
            };
            const auto &a = profile.view.angles[phase], &b = profile.view.angles[i];
            same = profile.turnSigns[i] == profile.turnSigns[phase] &&
                   std::abs(a.radians - b.radians) < 1e-7 &&
                   samePoint(translated(a.vertex), b.vertex) &&
                   samePoint(translated(a.first), b.first) &&
                   samePoint(translated(a.second), b.second);
        }
        for (std::size_t i = 1; i < n && same; ++i) {
            const auto reference = i % period ? i % period : period;
            same = std::abs(profile.segments[i].dimension.valueMm -
                            profile.segments[reference].dimension.valueMm) <= tolerance;
        }
        if (!same || std::abs(profile.segments.front().dimension.valueMm +
                              profile.segments.back().dimension.valueMm -
                              profile.segments[period].dimension.valueMm) > tolerance)
            continue;
        const double c = shift.x / pitch, s = -shift.y / pitch;
        const auto rotate = [&](DrawingPoint p) {
            return DrawingPoint{c * p.x - s * p.y, s * p.x + c * p.y};
        };
        const auto dimension = [&](DrawingDimension d) {
            d.start = rotate(d.start);
            d.end = rotate(d.end);
            d.label.clear();
            if (d.witnesses)
                for (auto &p : *d.witnesses)
                    p = rotate(p);
            return d;
        };
        const auto start = rotate(profile.segments.front().dimension.start);
        const auto end = rotate(profile.segments.back().dimension.end);
        const auto count = n / period;
        for (const auto &segment : profile.segments) {
            const auto d = dimension(segment.dimension);
            same = same && d.end.x >= d.start.x - tolerance;
        }
        if (!same || std::abs(end.x - start.x - count * pitch) > tolerance ||
            std::abs(end.y - start.y) > tolerance)
            continue;
        RepeatedDrawingSection result{{}, {}, count};
        result.overview.paths = actual.paths;
        result.overview.sectionLoops = actual.sectionLoops;
        for (auto &path : result.overview.paths)
            for (auto &p : path.points)
                p = rotate(p);
        for (auto &loop : result.overview.sectionLoops)
            for (auto &p : loop)
                p = rotate(p);
        bounds(result.overview);
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), std::atan2(s, c));
        const auto normalized = BRepBuilderAPI_Transform(flat, rotation, true).Shape();
        try {
            const auto unit =
                slice(normalized, start.x, start.x + pitch, result.overview.minimum.y - 10,
                      result.overview.maximum.y + 10, tolerance, stop);
            const double referenceLength = length(unit);
            if (referenceLength <= tolerance)
                continue;
            for (std::size_t j = 1; j < count && same; ++j) {
                auto copy = slice(normalized, start.x + pitch * j, start.x + pitch * (j + 1),
                                  result.overview.minimum.y - 10, result.overview.maximum.y + 10,
                                  tolerance, stop);
                gp_Trsf translation;
                translation.SetTranslation(gp_Vec(-pitch * j, 0, 0));
                copy = BRepBuilderAPI_Transform(copy, translation, true).Shape();
                BRepAlgoAPI_Common common(unit, copy);
                cancelled(stop);
                same = common.IsDone() && std::abs(length(copy) - referenceLength) <= tolerance &&
                       std::abs(length(common.Shape()) - referenceLength) <= tolerance;
            }
        } catch (const Standard_Failure &) {
            same = false;
        }
        if (!same)
            continue; // A notch, unequal cap or SDK failure keeps the original section.
        for (const auto &loop : result.overview.sectionLoops) {
            auto fragment = clipped(loop, start.x, start.x + pitch);
            if (fragment.size() < 4)
                continue;
            result.unit.paths.push_back({fragment, false});
            result.unit.sectionLoops.push_back(std::move(fragment));
        }
        bounds(result.unit);
        if (result.unit.paths.empty())
            continue;
        for (std::size_t i = 0; i <= period; ++i) {
            auto d = dimension(profile.segments[i].dimension);
            const auto originalStart = d.start, originalEnd = d.end;
            if (std::abs(d.end.x - d.start.x) > tolerance) {
                const auto at = [&](double x) {
                    const double t = (x - originalStart.x) / (originalEnd.x - originalStart.x);
                    return DrawingPoint{x, originalStart.y + t * (originalEnd.y - originalStart.y)};
                };
                d.start = at(std::clamp(d.start.x, start.x, start.x + pitch));
                d.end = at(std::clamp(d.end.x, start.x, start.x + pitch));
            }
            d.valueMm = std::hypot(d.end.x - d.start.x, d.end.y - d.start.y);
            result.unit.dimensions.push_back(d);
        }
        for (std::size_t i = 0; i < period; ++i) {
            auto angle = profile.view.angles[i];
            angle.vertex = rotate(angle.vertex);
            angle.first = rotate(angle.first);
            angle.second = rotate(angle.second);
            result.unit.angles.push_back(angle);
        }
        result.unit.dimensionedBends = profile.bends;
        result.overview.dimensions = {dimension(profile.segments.front().dimension),
                                      dimension(profile.segments[period].dimension),
                                      dimension(profile.segments.back().dimension)};
        DrawingDimension extent{{start.x, result.overview.minimum.y},
                                {end.x, result.overview.minimum.y},
                                count * pitch,
                                {}};
        extent.pitchIntervals = count;
        extent.pitchMm = pitch;
        result.overview.dimensions.push_back(extent);
        return result;
    }
    return {};
}
void compactRepeatedFlat(DrawingFlatPattern &flat, double tolerance) {
    const auto &bends = flat.info.bends;
    if (bends.size() < 4)
        return;
    const auto first = bends.front().centreLine();
    const bool horizontal = std::abs(first[0].y - first[1].y) < tolerance;
    if (!horizontal && std::abs(first[0].x - first[1].x) >= tolerance)
        return;
    struct Line {
        const FlatBend *bend;
        double coordinate, low, high;
    };
    std::vector<Line> lines;
    for (const auto &bend : bends) {
        const auto line = bend.centreLine();
        if (std::abs(horizontal ? line[0].y - line[1].y : line[0].x - line[1].x) >= tolerance)
            return;
        const double a = horizontal ? line[0].x : line[0].y, b = horizontal ? line[1].x : line[1].y;
        lines.push_back(
            {&bend, horizontal ? line[0].y : line[0].x, std::min(a, b), std::max(a, b)});
    }
    std::sort(lines.begin(), lines.end(),
              [](auto a, auto b) { return a.coordinate < b.coordinate; });
    const double span = horizontal ? flat.view.maximum.y : flat.view.maximum.x;
    for (std::size_t period = 1; period <= lines.size() / 2; ++period) {
        if (lines.size() % period)
            continue;
        const auto count = lines.size() / period;
        const double pitch = lines[period].coordinate - lines[0].coordinate;
        if (pitch <= tolerance || std::abs(span - count * pitch) > tolerance)
            continue;
        bool same = true;
        for (std::size_t i = 0; i < lines.size() && same; ++i) {
            const auto &reference = lines[i % period], &candidate = lines[i];
            const auto &a = *reference.bend, &b = *candidate.bend;
            same = std::abs(candidate.coordinate - reference.coordinate - i / period * pitch) <=
                       tolerance &&
                   std::abs(candidate.low - reference.low) <= tolerance &&
                   std::abs(candidate.high - reference.high) <= tolerance &&
                   a.direction == b.direction && std::abs(a.turnRadians - b.turnRadians) < 1e-7 &&
                   std::abs(a.radiusMm - b.radiusMm) <= tolerance && std::abs(a.k - b.k) < 1e-7;
        }
        if (!same)
            continue;
        flat.repeatedBendGroups.resize(period);
        for (std::size_t i = 0; i < lines.size(); ++i)
            flat.repeatedBendGroups[i % period].push_back(lines[i].bend->bendIndex);
        auto &dimensions = horizontal ? flat.view.sectionDimensions : flat.view.axialDimensions;
        dimensions.clear();
        for (std::size_t i = 0; i < period; ++i)
            dimensions.push_back(horizontal
                                     ? DrawingDimension{{flat.view.maximum.x, 0},
                                                        {flat.view.maximum.x, lines[i].coordinate},
                                                        lines[i].coordinate,
                                                        {}}
                                     : DrawingDimension{{0, flat.view.maximum.y},
                                                        {lines[i].coordinate, flat.view.maximum.y},
                                                        lines[i].coordinate,
                                                        {}});
        for (auto &d : flat.view.dimensions)
            if (std::abs(d.valueMm - span) <= tolerance &&
                (std::abs(d.end.x - d.start.x) < tolerance) == horizontal) {
                d.pitchIntervals = count;
                d.pitchMm = pitch;
            }
        return;
    }
}
} // namespace bending::cad
