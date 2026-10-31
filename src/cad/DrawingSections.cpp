#include "DrawingSections.h"
#include "DrawingProfiles.h"
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
    auto right =
        gp_Vec(reference.XDirection()) - gp_Vec(normal) * normal.Dot(reference.XDirection());
    if (right.Magnitude() < 1e-7)
        right = gp_Vec(reference.Direction()) - gp_Vec(normal) * normal.Dot(reference.Direction());
    return {point(profile.station), normal, gp_Dir(right)};
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
void compactCoordinates(DrawingView &view) {
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
        for (std::size_t i = 1; i < coordinates.size(); ++i) {
            const auto a = horizontal ? DrawingPoint{coordinates[i - 1], view.minimum.y}
                                      : DrawingPoint{view.minimum.x, coordinates[i - 1]};
            const auto b = horizontal ? DrawingPoint{coordinates[i], view.minimum.y}
                                      : DrawingPoint{view.minimum.x, coordinates[i]};
            view.axialDimensions.push_back({a, b, coordinates[i] - coordinates[i - 1], {}});
        }
    }
}
} // namespace

void prepareClassicalDrawing(const TopoDS_Shape &shape, const TopTools_IndexedMapOfShape &faces,
                             const std::vector<RecognizedBend> &bends,
                             const std::array<gp_Ax2, 3> &frames, BendingDrawing &drawing,
                             double tolerance, std::size_t &budget, std::stop_token stop) {
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
    for (const auto &original : drawing.profiles) {
        cancelled(stop);
        try {
            std::optional<ActualSection> best;
            DrawingProfile measured = original;
            for (double fraction : {.5, .382, .618, .25, .75}) {
                auto candidate = drawingProfileAt(original.bends, bends, faces, frames[0],
                                                  tolerance, fraction, stop);
                if (!candidate || candidate->segments.size() != original.segments.size())
                    continue;
                bool same = true;
                for (std::size_t i = 0; i < original.segments.size(); ++i)
                    same = same && std::abs(candidate->segments[i].dimension.valueMm -
                                            original.segments[i].dimension.valueMm) <= tolerance;
                if (!same)
                    continue; // Moving a section must not silently shorten a rounded flange.
                std::optional<ActualSection> actual;
                try {
                    actual = intersect(shape, sectionFrame(*candidate, frames[0]), budget, stop);
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
                continue;
            auto at = location(measured, frames, *drawing.classicalViews);
            if (!at)
                continue;
            std::size_t index = 0;
            for (; index < geometries.size(); ++index)
                if (equivalent(geometries[index], *best, representatives[index], measured, bends,
                               tolerance, stop))
                    break;
            if (index == geometries.size()) {
                DrawingSection section;
                static const std::array names{"А", "Б", "В", "Г", "Д", "Е", "Ж", "З",
                                              "И", "К", "Л", "М", "Н", "П", "Р", "С"};
                if (index >= names.size())
                    continue; // Local detail fallback retains the remaining zones.
                section.designation = names[index];
                section.normal = measured.normal;
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
            section.view.dimensionedBends.insert(section.view.dimensionedBends.end(),
                                                 measured.bends.begin(), measured.bends.end());
            section.locations.push_back(std::move(*at));
        } catch (const Standard_Failure &) {
            // Optional real sections cannot discard the source views or simulation analysis.
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
        for (const auto &label : drawing.views[i].labels) {
            if (drawing.views[i].reverseProjection)
                continue;
            const auto found =
                std::find_if(drawing.details.begin(), drawing.details.end(),
                             [&](const auto &d) { return d.designation == label.second; });
            if (found != drawing.details.end() && !covered(found->bendIndex)) {
                v.labels.push_back(label);
                for (const auto &circle : drawing.views[i].detailCircles)
                    if (std::hypot(circle.first.x - label.first.x, circle.first.y - label.first.y) <
                        tolerance)
                        v.detailCircles.push_back(circle);
            }
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
