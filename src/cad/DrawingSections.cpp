#include "DrawingSections.h"
#include "DrawingProfiles.h"
#include "DrawingRepeats.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <limits>
#include <map>

namespace bending::cad {
namespace {
using namespace application;
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw DrawingError{"Cancelled", true};
}
DrawingPoint project(gp_Pnt point, const gp_Ax2 &frame) {
    const gp_Vec v(gp_Pnt(0, 0, 0), point);
    return {v.Dot(gp_Vec(frame.XDirection())), v.Dot(gp_Vec(frame.YDirection()))};
}
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
gp_Ax2 sectionFrame(const DrawingProfile &profile, const gp_Ax2 &reference) {
    const gp_Dir normal(profile.normal.x, profile.normal.y, profile.normal.z);
    return drawingSectionFrame(point(profile.station), normal, reference);
}
struct ActualSection {
    DrawingView view;
    TopoDS_Shape flat;
};
std::optional<ActualSection> intersect(const TopoDS_Shape &shape, const gp_Ax2 &frame,
                                       std::size_t &budget, std::stop_token stop) {
    cancelled(stop);
    BRepAlgoAPI_Section operation(shape, gp_Pln(frame), false);
    operation.SetNonDestructive(true);
    operation.SetRunParallel(false);
    operation.Build();
    cancelled(stop);
    if (!operation.IsDone())
        return {};
    gp_Trsf placement;
    placement.SetTransformation(gp_Ax3(frame));
    ActualSection result;
    result.flat = BRepBuilderAPI_Transform(operation.Shape(), placement, true).Shape();
    Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
    for (TopExp_Explorer e(result.flat, TopAbs_EDGE); e.More(); e.Next())
        edges->Append(e.Current());
    Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-5, false, wires);
    result.view.minimum = {1e100, 1e100};
    result.view.maximum = {-1e100, -1e100};
    for (int i = 1; i <= wires->Length(); ++i) {
        const auto wire = TopoDS::Wire(wires->Value(i));
        if (!BRep_Tool::IsClosed(wire))
            return {};
        std::vector<DrawingPoint> loop;
        for (BRepTools_WireExplorer e(wire); e.More(); e.Next()) {
            cancelled(stop);
            BRepAdaptor_Curve curve(e.Current());
            DrawingPath path;
            if (curve.GetType() == GeomAbs_Line) {
                const auto a = curve.Value(curve.FirstParameter()),
                           b = curve.Value(curve.LastParameter());
                path.points = {{a.X(), a.Y()}, {b.X(), b.Y()}};
            } else {
                GCPnts_QuasiUniformDeflection sample(curve, .02, GeomAbs_C0);
                if (!sample.IsDone() || sample.NbPoints() < 2 || sample.NbPoints() > 4096)
                    return {};
                for (int j = 1; j <= sample.NbPoints(); ++j) {
                    const auto p = sample.Value(j);
                    path.points.push_back({p.X(), p.Y()});
                }
            }
            budget += path.points.size();
            if (budget > 200000)
                throw DrawingError{"Drawing projection exceeds 200000 points"};
            if (e.Current().Orientation() == TopAbs_REVERSED)
                std::reverse(path.points.begin(), path.points.end());
            if (!loop.empty() && std::hypot(loop.back().x - path.points.front().x,
                                            loop.back().y - path.points.front().y) > 1e-5)
                return {};
            loop.insert(loop.end(), path.points.begin(), path.points.end());
            for (auto p : path.points) {
                result.view.minimum.x = std::min(result.view.minimum.x, p.x);
                result.view.minimum.y = std::min(result.view.minimum.y, p.y);
                result.view.maximum.x = std::max(result.view.maximum.x, p.x);
                result.view.maximum.y = std::max(result.view.maximum.y, p.y);
            }
            result.view.paths.push_back(std::move(path));
        }
        if (loop.empty() ||
            std::hypot(loop.front().x - loop.back().x, loop.front().y - loop.back().y) > 1e-5)
            return {};
        result.view.sectionLoops.push_back(std::move(loop));
    }
    if (result.view.sectionLoops.empty())
        return {};
    return result;
}
double length(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::LinearProperties(shape, properties, true);
    return properties.Mass();
}
bool equivalent(const ActualSection &a, const ActualSection &b, const DrawingProfile &pa,
                const DrawingProfile &pb, const std::vector<RecognizedBend> &bends,
                double tolerance, std::stop_token stop) {
    if (!gp_Dir(pa.normal.x, pa.normal.y, pa.normal.z)
             .IsEqual(gp_Dir(pb.normal.x, pb.normal.y, pb.normal.z), 1e-7) ||
        pa.bends.size() != pb.bends.size() || pa.segments.size() != pb.segments.size() ||
        pa.turnSigns != pb.turnSigns || a.view.sectionLoops.size() != b.view.sectionLoops.size())
        return false;
    for (std::size_t i = 0; i < pa.segments.size(); ++i)
        if (std::abs(pa.segments[i].dimension.valueMm - pb.segments[i].dimension.valueMm) >
            tolerance)
            return false;
    for (std::size_t i = 0; i < pa.bends.size(); ++i)
        if (std::abs(bends[pa.bends[i]].lengthMm - bends[pb.bends[i]].lengthMm) > tolerance ||
            std::abs(pa.view.angles[i].radians - pb.view.angles[i].radians) > 1e-7)
            return false;
    cancelled(stop);
    try {
        BRepAlgoAPI_Common common(a.flat, b.flat);
        cancelled(stop);
        if (!common.IsDone())
            return false;
        const double la = length(a.flat), lb = length(b.flat), lc = length(common.Shape());
        return la > tolerance && std::abs(la - lc) <= tolerance && std::abs(lb - lc) <= tolerance;
    } catch (const Standard_Failure &) {
        return false; // A failed equivalence check keeps two separate valid sections.
    }
}
std::optional<DrawingSectionLocation> location(const DrawingProfile &profile,
                                               const std::array<gp_Ax2, 3> &frames,
                                               const std::array<DrawingView, 3> &views) {
    const gp_Dir axis(profile.normal.x, profile.normal.y, profile.normal.z);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (std::abs(axis.Dot(frames[i].Direction())) > 1e-7)
            continue;
        const auto &v = views[i];
        const double nx = axis.Dot(frames[i].XDirection()), ny = axis.Dot(frames[i].YDirection());
        const double c = gp_Vec(gp_Pnt(0, 0, 0), point(profile.station)).Dot(gp_Vec(axis));
        std::vector<DrawingPoint> ends;
        const auto add = [&](double x, double y) {
            if (x >= v.minimum.x - 1e-5 && x <= v.maximum.x + 1e-5 && y >= v.minimum.y - 1e-5 &&
                y <= v.maximum.y + 1e-5 && std::none_of(ends.begin(), ends.end(), [&](auto p) {
                    return std::hypot(x - p.x, y - p.y) < 1e-5;
                }))
                ends.push_back({x, y});
        };
        if (std::abs(ny) > 1e-7)
            for (double x : {v.minimum.x, v.maximum.x})
                add(x, (c - nx * x) / ny);
        if (std::abs(nx) > 1e-7)
            for (double y : {v.minimum.y, v.maximum.y})
                add((c - ny * y) / nx, y);
        if (ends.size() == 2)
            return DrawingSectionLocation{
                i, {ends[0], ends[1], {-nx, -ny}, {}}, profile.station, profile.bends};
    }
    return {};
}
bool aligned(const gp_Ax2 &section, const gp_Ax2 &source, const DrawingSectionMarker &marker,
             double tolerance) {
    const gp_Vec n(section.Direction()), u(section.XDirection()), v(section.YDirection());
    const gp_Vec su(source.XDirection()), sv(source.YDirection()), sn(source.Direction());
    const auto trace = sn.Crossed(n);
    if (std::abs(sn.Dot(n)) > 1e-7 || std::abs(u.Crossed(v).Dot(n) - 1) > 1e-7 ||
        std::abs(trace.Dot(u) - trace.Dot(su)) > 1e-7 ||
        std::abs(trace.Dot(v) - trace.Dot(sv)) > 1e-7 ||
        std::hypot(marker.viewDirection.x + n.Dot(su), marker.viewDirection.y + n.Dot(sv)) > 1e-7 ||
        std::hypot(marker.second.x - marker.first.x, marker.second.y - marker.first.y) < 1e-7)
        return false;
    // Source projections use model-origin coordinates. Their arbitrary depth
    // along sn cannot affect a cut that is edge-on in this source view.
    for (const auto &p : {marker.first, marker.second}) {
        const auto world = gp_Pnt(0, 0, 0).Translated(su * p.x + sv * p.y);
        if (std::abs(gp_Vec(section.Location(), world).Dot(n)) > tolerance)
            return false;
    }
    return true;
}
void compactCoordinates(DrawingView &view) {
    using Center = std::pair<double, double>;
    const auto center = [](DrawingPoint p) { return Center{p.x, p.y}; };
    std::map<Center, std::optional<std::size_t>> openings;
    const auto addOpening = [&](DrawingPoint point, std::optional<std::size_t> index) {
        auto [it, added] = openings.try_emplace(center(point), index);
        if (!added)
            it->second.reset(); // Different contours share this projected center.
    };
    const auto circleCount = view.holeDiameters.size();
    for (std::size_t i = 0; i < circleCount; ++i)
        addOpening(view.holeDiameters[i].first, i);
    for (std::size_t i = 0; i < view.openingAxes.size(); ++i) {
        const auto &axes = view.openingAxes[i];
        addOpening(axes.center, axes.obround ? std::optional(circleCount + i) : std::nullopt);
    }
    const auto sameOpening = [&](std::size_t first, std::size_t second) {
        if (first < circleCount && second < circleCount)
            return std::abs(view.holeDiameters[first].second - view.holeDiameters[second].second) <
                   1e-5;
        if (first < circleCount || second < circleCount)
            return false;
        const auto &a = view.openingAxes[first - circleCount];
        const auto &b = view.openingAxes[second - circleCount];
        const double ax = a.first.x - a.center.x, ay = a.first.y - a.center.y;
        const double bx = b.first.x - b.center.x, by = b.first.y - b.center.y;
        const double al = std::hypot(ax, ay), bl = std::hypot(bx, by);
        return std::abs(al - bl) < 1e-5 && std::abs(ax * by - ay * bx) < 1e-7 * al * bl &&
               std::abs(std::hypot(a.second.x - a.center.x, a.second.y - a.center.y) -
                        std::hypot(b.second.x - b.center.x, b.second.y - b.center.y)) < 1e-5;
    };
    struct Row {
        std::size_t base;
        bool horizontal;
        std::vector<std::size_t> holes;
    };
    std::vector<Row> rows;
    std::vector<DrawingDimension> result;
    for (bool horizontal : {true, false}) {
        std::vector<DrawingDimension> group;
        for (const auto &d : view.holeDimensions)
            if ((std::abs(d.end.y - d.start.y) < 1e-7) == horizontal)
                group.push_back(d);
        for (std::size_t first = 0; first < group.size();) {
            std::size_t end = first + 1;
            const double step = end < group.size() ? group[end].valueMm - group[first].valueMm : 0;
            if (step > 1e-5)
                while (end < group.size() && std::abs(group[end].valueMm - group[first].valueMm -
                                                      step * (end - first)) < 1e-5)
                    ++end;
            // A row may have independent fastening holes before/after its regular
            // perforation. Keep every outlier; only replace a verified run of >=4 axes.
            if (end - first < 4) {
                result.push_back(group[first++]);
                continue;
            }
            Row row{result.size(), horizontal, {}};
            const auto across = [horizontal](DrawingPoint point) {
                return horizontal ? point.y : point.x;
            };
            for (std::size_t i = first; i < end; ++i) {
                const auto hole = openings.find(center(group[i].end));
                if (hole == openings.end() || !hole->second ||
                    std::abs(across(group[i].end) - across(group[first].end)) > 1e-5 ||
                    (!row.holes.empty() && !sameOpening(row.holes.front(), *hole->second)))
                    break;
                row.holes.push_back(*hole->second);
            }
            if (row.holes.size() == end - first)
                rows.push_back(std::move(row));
            result.push_back(group[first]);
            DrawingDimension pitch{group[first].end,
                                   group[end - 1].end,
                                   group[end - 1].valueMm - group[first].valueMm,
                                   {}};
            if (horizontal)
                pitch.end.y = pitch.start.y;
            else
                pitch.end.x = pitch.start.x;
            pitch.witnesses = std::array{group[first].end, group[end - 1].end};
            pitch.pitchIntervals = end - first - 1;
            pitch.pitchMm = step;
            result.push_back(std::move(pitch));
            first = end;
        }
    }
    std::vector<bool> used(circleCount + view.openingAxes.size());
    for (auto &row : rows) {
        const auto reference = result[row.base].end;
        const auto across = std::find_if(result.begin(), result.end(), [&](const auto &d) {
            const bool horizontal = std::abs(d.end.y - d.start.y) < 1e-7;
            return !d.pitchIntervals && horizontal != row.horizontal &&
                   std::abs((horizontal ? d.end.x - reference.x : d.end.y - reference.y)) < 1e-5;
        });
        if (across == result.end() ||
            std::any_of(row.holes.begin(), row.holes.end(), [&](auto i) { return used[i]; }))
            continue;
        for (auto i : row.holes)
            used[i] = true;
        across->openingPatternReference = true;
        result[row.base].openingPatternReference = true;
        result[row.base + 1].openingPatternReference = true;
        if (row.holes.front() < circleCount)
            view.circularOpeningRows.push_back(std::move(row.holes));
        else {
            for (auto &i : row.holes)
                i -= circleCount;
            view.obroundOpeningRows.push_back(std::move(row.holes));
        }
    }
    view.holeDimensions = std::move(result);
}
void axialDimensions(const std::vector<RecognizedBend> &bends, const gp_Ax2 &frame,
                     DrawingView &view) {
    for (bool horizontal : {true, false}) {
        const auto direction = horizontal ? frame.XDirection() : frame.YDirection();
        std::vector<double> coordinates;
        for (const auto &b : bends) {
            if (b.trimmedEnds)
                continue; // A common core is not the full boundary of an oblique cap.
            const auto a = point(b.axisStart), z = point(b.axisEnd);
            if (!gp_Dir(gp_Vec(a, z)).IsParallel(direction, 1e-7))
                continue;
            for (auto p : {a, z}) {
                const auto q = project(p, frame);
                coordinates.push_back(horizontal ? q.x : q.y);
            }
        }
        if (coordinates.empty() || coordinates.size() > 128)
            continue;
        coordinates.push_back(horizontal ? view.minimum.x : view.minimum.y);
        coordinates.push_back(horizontal ? view.maximum.x : view.maximum.y);
        std::sort(coordinates.begin(), coordinates.end());
        coordinates.erase(std::unique(coordinates.begin(), coordinates.end(),
                                      [](auto a, auto b) { return std::abs(a - b) < 1e-5; }),
                          coordinates.end());
        if (coordinates.size() < 3)
            continue; // Overall size and L already explain a single full-width zone.
        const auto at = [&](double coordinate) {
            return horizontal ? DrawingPoint{coordinate, view.minimum.y}
                              : DrawingPoint{view.minimum.x, coordinate};
        };
        const auto dimension = [&](std::size_t from, std::size_t to) {
            return DrawingDimension{at(coordinates[from]),
                                    at(coordinates[to]),
                                    coordinates[to] - coordinates[from],
                                    {}};
        };
        for (std::size_t i = 0; i + 1 < coordinates.size();) {
            // Keep one dimensioned pair, then give the rest as a pitch.
            // Verify every boundary against the same starting point so
            // a drift or a single different interval does not disappear.
            std::size_t end = i;
            double pitch = 0;
            if (coordinates.size() - i >= 9) {
                const double first = coordinates[i + 1] - coordinates[i];
                pitch = coordinates[i + 2] - coordinates[i];
                if (std::abs(first * 2 - pitch) > 1e-5)
                    while (end + 2 < coordinates.size()) {
                        const double offset = coordinates[i] + (end - i) / 2 * pitch;
                        if (std::abs(coordinates[end + 1] - (offset + first)) > 1e-5 ||
                            std::abs(coordinates[end + 2] - (offset + pitch)) > 1e-5)
                            break;
                        end += 2;
                    }
            }
            if (end - i < 8) {
                view.axialDimensions.push_back(dimension(i, i + 1));
                ++i;
                continue;
            }
            view.axialDimensions.push_back(dimension(i, i + 1));
            view.axialDimensions.push_back(dimension(i + 1, i + 2));
            auto run = dimension(i + 2, end);
            run.pitchIntervals = (end - i) / 2 - 1;
            run.pitchMm = pitch;
            view.axialDimensions.push_back(std::move(run));
            i = end;
        }
    }
}
} // namespace

void prepareClassicalDrawing(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                             const std::vector<RecognizedBend> &bends,
                             const std::array<gp_Ax2, 3> &frames, BendingDrawing &drawing,
                             double tolerance, std::size_t &budget, std::stop_token stop,
                             const std::vector<DrawingSectionEdit> &edits) {
    if (!drawing.classicalViews)
        return;
    tolerance = std::max(1e-5, tolerance);
    for (auto &v : *drawing.classicalViews)
        compactCoordinates(v);
    for (auto &v : drawing.auxiliaryViews)
        compactCoordinates(v.view);
    axialDimensions(bends, frames[0], (*drawing.classicalViews)[0]);
    std::vector<ActualSection> geometries;
    std::vector<DrawingProfile> representatives;
    const auto key = [](const auto &zones) {
        auto result = zones;
        std::sort(result.begin(), result.end());
        return result;
    };
    std::vector<std::size_t> obliqueBends;
    constexpr std::size_t obliqueSectionCandidateLimit = 8;
    if (bends.size() <= 128)
        for (std::size_t i = 0;
             i < bends.size() && obliqueBends.size() < obliqueSectionCandidateLimit; ++i) {
            cancelled(stop);
            if (std::none_of(drawing.profiles.begin(), drawing.profiles.end(),
                             [&](const auto &p) {
                                 return std::find(p.bends.begin(), p.bends.end(), i) !=
                                        p.bends.end();
                             }) &&
                hasObliqueSectionNeighbour(i, bends))
                obliqueBends.push_back(i);
        }
    std::vector<bool> applied(edits.size());
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto &edit = edits[i];
        if (edit.bends.empty() || edit.bends != key(edit.bends) ||
            std::adjacent_find(edit.bends.begin(), edit.bends.end()) != edit.bends.end() ||
            (edit.fraction &&
             (!std::isfinite(*edit.fraction) || *edit.fraction < 0 || *edit.fraction > 1)) ||
            std::any_of(edits.begin(), edits.begin() + i,
                        [&](const auto &e) { return e.bends == edit.bends; }) ||
            (std::none_of(drawing.profiles.begin(), drawing.profiles.end(),
                          [&](const auto &p) { return key(p.bends) == edit.bends; }) &&
             !(edit.bends.size() == 1 && std::find(obliqueBends.begin(), obliqueBends.end(),
                                                   edit.bends.front()) != obliqueBends.end())))
            throw DrawingError{"Не удалось сопоставить настройки с исходным сечением"};
    }
    const auto prepare = [&](const DrawingProfile &original, bool oblique) {
        cancelled(stop);
        try {
            const auto initialLocation = location(original, frames, *drawing.classicalViews);
            if (!initialLocation)
                return;
            const auto &reference = frames[initialLocation->sourceView];
            const auto edit = std::find_if(edits.begin(), edits.end(), [&](const auto &e) {
                return e.bends == key(original.bends);
            });
            const bool manual = edit != edits.end() && edit->fraction.has_value();
            const bool reversed = edit != edits.end() && edit->reversed;
            const gp_Dir positionAxis(original.normal.x, original.normal.y, original.normal.z);
            std::optional<ActualSection> best;
            DrawingProfile measured = original;
            const auto fractions = manual ? std::vector<double>{*edit->fraction}
                                   : oblique
                                       ? std::vector<double>{.5, .04, .96, .025, .975, .25, .75}
                                       : std::vector<double>{.5, .382, .618, .25, .75};
            for (double fraction : fractions) {
                const DrawingProfilePose pose{
                    edit != edits.end() || oblique ? std::optional(positionAxis) : std::nullopt,
                    reversed};
                auto candidate =
                    oblique ? obliqueSectionProfileAt(original.bends.front(), bends, faces,
                                                      reference, tolerance, fraction, stop, pose)
                            : drawingProfileAt(original.bends, bends, faces, reference, tolerance,
                                               fraction, stop, pose);
                if (!candidate)
                    continue;
                if (!oblique) {
                    if (candidate->segments.size() != original.segments.size())
                        continue;
                    bool same = true;
                    for (std::size_t i = 0; i < original.segments.size(); ++i)
                        same =
                            same && std::abs(candidate->segments[i].dimension.valueMm -
                                             original.segments[i].dimension.valueMm) <= tolerance;
                    if (!same && !manual)
                        continue; // Moving a section must not silently shorten a rounded flange.
                }
                std::optional<ActualSection> actual;
                try {
                    actual = intersect(shape, sectionFrame(*candidate, reference), budget, stop);
                } catch (const Standard_Failure &) {
                    continue; // Preserve any earlier valid section at this station group.
                }
                if (actual &&
                    (!best || actual->view.sectionLoops.size() < best->view.sectionLoops.size())) {
                    best = std::move(actual);
                    measured = std::move(*candidate);
                }
                if (best && best->view.sectionLoops.size() == 1)
                    break;
            }
            if (!best)
                return;
            auto at = location(measured, frames, *drawing.classicalViews);
            if (!at || at->sourceView != initialLocation->sourceView)
                return;
            const auto basis = sectionFrame(measured, reference);
            if (!aligned(basis, reference, at->marker, tolerance))
                return; // Never publish a section with an unproved orientation.
            double low = -1e100, high = 1e100;
            for (auto index : original.bends) {
                const auto &bend = bends[index];
                const double a = gp_Vec(point(bend.axisStart).XYZ()).Dot(gp_Vec(positionAxis));
                const double b = gp_Vec(point(bend.axisEnd).XYZ()).Dot(gp_Vec(positionAxis));
                low = std::max(low, std::min(a, b));
                high = std::min(high, std::max(a, b));
            }
            at->fraction =
                manual
                    ? *edit->fraction
                    : std::clamp(
                          (gp_Vec(point(measured.station).XYZ()).Dot(gp_Vec(positionAxis)) - low) /
                              (high - low),
                          0., 1.);
            at->reversed = reversed;
            std::size_t index = oblique ? geometries.size() : 0;
            for (; index < geometries.size(); ++index)
                if (drawing.sections[index].basis &&
                    drawing.sections[index].basis->sourceView == at->sourceView &&
                    equivalent(geometries[index], *best, representatives[index], measured, bends,
                               tolerance, stop))
                    break;
            if (index == geometries.size()) {
                DrawingSection section;
                static const std::array names{"А", "Б", "В", "Г", "Д", "Е", "Ж", "З",
                                              "И", "К", "Л", "М", "Н", "П", "Р", "С"};
                if (index >= names.size()) {
                    if (edit != edits.end())
                        throw DrawingError{"Настройки сечения не применены: поддерживается не "
                                           "более 16 различных сечений. Верните «Авто» или "
                                           "измените настройки."};
                    return; // Local detail fallback retains the remaining zones.
                }
                section.designation = names[index];
                section.normal = measured.normal;
                section.basis = DrawingSectionBasis{
                    measured.station,
                    {basis.XDirection().X(), basis.XDirection().Y(), basis.XDirection().Z()},
                    {basis.YDirection().X(), basis.YDirection().Y(), basis.YDirection().Z()},
                    at->sourceView};
                section.view = best->view;
                section.view.angles = measured.view.angles;
                for (const auto &segment : measured.segments) {
                    auto dimension = segment.dimension;
                    dimension.label.clear();
                    section.view.dimensions.push_back(std::move(dimension));
                }
                for (const auto &[anchor, bend] : measured.view.bendNotes)
                    section.view.groupedBendNotes.push_back({anchor, {bend}});
                drawing.sections.push_back(std::move(section));
                geometries.push_back(std::move(*best));
                representatives.push_back(measured);
            } else {
                for (std::size_t i = 0; i < measured.bends.size(); ++i)
                    drawing.sections[index].view.groupedBendNotes[i].bends.push_back(
                        measured.bends[i]);
            }
            auto &section = drawing.sections[index];
            at->marker.designation = section.designation;
            if (!oblique)
                section.view.dimensionedBends.insert(section.view.dimensionedBends.end(),
                                                     measured.bends.begin(), measured.bends.end());
            section.locations.push_back(std::move(*at));
            if (edit != edits.end())
                applied[static_cast<std::size_t>(edit - edits.begin())] = true;
        } catch (const Standard_Failure &) {
            // Optional real sections cannot discard the source views or simulation analysis.
        }
    };
    for (const auto &profile : drawing.profiles)
        prepare(profile, false);
    for (auto index : obliqueBends) {
        cancelled(stop);
        if (geometries.size() >= 16)
            break;
        const auto &b = bends[index];
        const auto start = point(b.axisStart), end = point(b.axisEnd);
        auto axis = gp_Dir(gp_Vec(start, end));
        for (const auto direction :
             {frames[0].Direction(), frames[0].YDirection(), frames[0].XDirection()})
            if (std::abs(axis.Dot(direction)) > 1e-5) {
                if (axis.Dot(direction) < 0)
                    axis.Reverse();
                break;
            }
        const auto mid = start.Translated(gp_Vec(start, end) * .5);
        DrawingProfile seed;
        seed.bends = {index};
        seed.station = {mid.X(), mid.Y(), mid.Z()};
        seed.normal = {axis.X(), axis.Y(), axis.Z()};
        prepare(seed, true);
    }
    if (std::find(applied.begin(), applied.end(), false) != applied.end())
        throw DrawingError{"В выбранном положении сечение построить не удалось. Измените положение "
                           "или верните «Авто»."};
    if (drawing.profiles.size() == 1 && drawing.sections.size() == 1 && geometries.size() == 1 &&
        !drawing.sections.front().view.dimensionedBends.empty()) {
        std::optional<RepeatedDrawingSection> repeat;
        try {
            repeat = repeatedDrawingSection(geometries[0].flat, geometries[0].view,
                                            representatives[0], bends, tolerance, stop);
        } catch (const Standard_Failure &) {
            // Optional abbreviation must not discard the full valid section on SDK failure.
        }
        if (repeat) {
            auto &overview = drawing.sections.front();
            overview.view = std::move(repeat->overview);
            overview.rotated = false;
            DrawingSection unit;
            unit.view = std::move(repeat->unit);
            unit.normal = overview.normal;
            unit.designation = overview.designation;
            unit.repeatCount = repeat->count;
            unit.basis = overview.basis;
            drawing.sections.push_back(std::move(unit));
        }
    }
    for (const auto &section : drawing.sections)
        for (const auto &at : section.locations)
            (*drawing.classicalViews)[at.sourceView].sectionMarkers.push_back(at.marker);
    const auto covered = [&](std::size_t index) {
        return std::any_of(drawing.sections.begin(), drawing.sections.end(), [&](const auto &s) {
            return std::find(s.view.dimensionedBends.begin(), s.view.dimensionedBends.end(),
                             index) != s.view.dimensionedBends.end();
        });
    };
    for (std::size_t i = 0; i < 3; ++i) {
        auto &v = (*drawing.classicalViews)[i];
        if (!drawing.views[i].reverseProjection)
            for (const auto &note : drawing.views[i].bendNotes)
                if (!covered(note.second))
                    v.bendNotes.push_back(note);
        if (drawing.views[i].reverseProjection)
            for (auto &auxiliary : drawing.auxiliaryViews)
                if (auxiliary.projection == i && auxiliary.view.reverseProjection)
                    for (const auto &note : drawing.views[i].bendNotes)
                        if (!covered(note.second))
                            auxiliary.view.bendNotes.push_back(note);
        if (!drawing.views[i].reverseProjection)
            for (const auto &locator : drawing.views[i].detailLocators) {
                const auto found = std::find_if(
                    drawing.details.begin(), drawing.details.end(),
                    [&](const auto &d) { return d.designation == locator.designation; });
                if (found != drawing.details.end() && !covered(found->bendIndex))
                    v.detailLocators.push_back(locator);
            }
    }
    bool uncovered = false;
    for (std::size_t i = 0; i < bends.size(); ++i)
        uncovered = uncovered || !covered(i);
    if (!drawing.sections.empty() &&
        (!(*drawing.classicalViews)[2].holeDimensions.empty() ||
         !(*drawing.classicalViews)[2].bendNotes.empty() || uncovered)) {
        drawing.auxiliaryViews.push_back({(*drawing.classicalViews)[2], "Х", 2});
        const auto &v = (*drawing.classicalViews)[0];
        (*drawing.classicalViews)[0].viewArrows.push_back(
            {{v.minimum.x, (v.minimum.y + v.maximum.y) / 2}, {1, 0}, "Х"});
    }
}
} // namespace bending::cad
