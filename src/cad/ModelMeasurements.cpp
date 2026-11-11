#include "ModelMeasurements.h"
#include "DrawingOpenings.h"
#include "DrawingProfiles.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
using namespace application;
PointMm dto(gp_Pnt p) {
    return {p.X(), p.Y(), p.Z()};
}
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
PointMm vectorDto(gp_Vec v) {
    return {v.X(), v.Y(), v.Z()};
}
bool has(const std::vector<std::size_t> &ids, std::size_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
std::optional<MeasuredSpan> location(const MeasuredEdge &edge, gp_Pnt center) {
    if (!edge.straight)
        return {};
    const auto a = point(edge.first), b = point(edge.second);
    const gp_Vec ab(a, b);
    const double q = ab.SquareMagnitude();
    if (q <= 1.e-12)
        return {};
    const double t = gp_Vec(a, center).Dot(ab) / q;
    if (t < -1.e-8 || t > 1 + 1.e-8)
        return {}; // Never extend a base beyond its real finite edge.
    const auto foot = a.Translated(ab * t);
    return MeasuredSpan{dto(foot), dto(center), foot.Distance(center), edge.faces};
}
} // namespace

std::shared_ptr<const application::ModelMeasurements>
prepareModelMeasurements(const TopoDS_Shape &original,
                         const application::DocumentMetadata &metadata, std::stop_token stop,
                         std::span<const application::FaceInfo> preparedFaces) {
    using namespace application;
    auto result = std::make_shared<ModelMeasurements>();
    if (stop.stop_requested())
        return result;
    // The shared drawing helpers may build CAD sections/Commons. Keep their
    // temporary geometry independent from the imported part and its mesh.
    BRepBuilderAPI_Copy copy(original, true, false);
    const auto shape = copy.Shape();
    TopTools_IndexedMapOfShape faces, edges, originalFaces, originalEdges;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(original, TopAbs_FACE, originalFaces);
    TopExp::MapShapes(original, TopAbs_EDGE, originalEdges);
    if (faces.Extent() != originalFaces.Extent() || edges.Extent() != originalEdges.Extent())
        return result;
    for (int i = 1; i <= faces.Extent(); ++i)
        if (stop.stop_requested() || !faces(i).IsSame(copy.ModifiedShape(originalFaces(i))))
            return result;
    for (int i = 1; i <= edges.Extent(); ++i)
        if (stop.stop_requested() || !edges(i).IsSame(copy.ModifiedShape(originalEdges(i))))
            return result;
    std::vector<std::size_t> edgeIndex(static_cast<std::size_t>(edges.Extent()) + 1, 10000);
    for (int i = 1; i <= std::min(edges.Extent(), 10000) && !stop.stop_requested(); ++i) {
        try {
            const auto edge = TopoDS::Edge(edges(i));
            BRepAdaptor_Curve curve(edge);
            GProp_GProps p;
            BRepGProp::LinearProperties(edge, p);
            if (!std::isfinite(p.Mass()) || p.Mass() <= 0)
                continue;
            MeasuredEdge measured;
            measured.id = static_cast<std::size_t>(i);
            measured.first = dto(curve.Value(curve.FirstParameter()));
            measured.second = dto(curve.Value(curve.LastParameter()));
            measured.lengthMm = p.Mass();
            const int samples = curve.GetType() == GeomAbs_Line ? 1 : 24;
            for (int k = 0; k <= samples; ++k)
                measured.outline.push_back(dto(
                    curve.Value(curve.FirstParameter() +
                                (curve.LastParameter() - curve.FirstParameter()) * k / samples)));
            measured.straight = curve.GetType() == GeomAbs_Line;
            measured.center =
                dto(curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2));
            if (curve.GetType() == GeomAbs_Circle) {
                measured.radiusMm = curve.Circle().Radius();
                measured.center = dto(curve.Circle().Location());
            }
            edgeIndex[static_cast<std::size_t>(i)] = result->edges.size();
            result->edges.push_back(std::move(measured));
        } catch (const Standard_Failure &) { /* Optional presentation measurement. */
        }
    }
    for (int i = 1; i <= faces.Extent() && !stop.stop_requested(); ++i) {
        try {
            const auto face = TopoDS::Face(faces(i));
            BRepAdaptor_Surface surface(face);
            MeasuredFace measured;
            measured.id = static_cast<std::size_t>(i);
            if (preparedFaces.size() >= measured.id &&
                preparedFaces[measured.id - 1].index == measured.id)
                measured.areaMm2 = preparedFaces[measured.id - 1].areaMm2;
            else {
                GProp_GProps p;
                BRepGProp::SurfaceProperties(face, p);
                measured.areaMm2 = p.Mass();
            }
            double u0, u1, v0, v1;
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            bool anchored = false;
            for (double u : {.5, .3, .7}) {
                for (double v : {.5, .3, .7}) {
                    const auto candidate = surface.Value(u0 + (u1 - u0) * u, v0 + (v1 - v0) * v);
                    BRepClass_FaceClassifier onFace(face, candidate, 1.e-6);
                    if (onFace.State() == TopAbs_IN || onFace.State() == TopAbs_ON) {
                        measured.anchor = dto(candidate);
                        anchored = true;
                        break;
                    }
                }
                if (anchored)
                    break;
            }
            if (!anchored) {
                TopExp_Explorer vertex(face, TopAbs_VERTEX);
                if (!vertex.More())
                    continue;
                measured.anchor = dto(BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current())));
            }

            measured.planar = surface.GetType() == GeomAbs_Plane;
            if (measured.planar)
                measured.normal = vectorDto(gp_Vec(surface.Plane().Axis().Direction()));
            const auto outer = BRepTools::OuterWire(face);
            std::vector<MeasuredEdge *> boundary;
            for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
                const auto id = static_cast<std::size_t>(edges.FindIndex(e.Current()));
                const auto at = edgeIndex[id];
                const auto found =
                    at < result->edges.size() ? result->edges.begin() + at : result->edges.end();
                if (found != result->edges.end() && !has(found->faces, measured.id))
                    found->faces.push_back(measured.id);
            }
            for (BRepTools_WireExplorer e(outer, face); e.More(); e.Next()) {
                const auto id = static_cast<std::size_t>(edges.FindIndex(e.Current()));
                measured.outerEdges.push_back(id);
                BRepAdaptor_Curve outerCurve(e.Current());
                const int samples = outerCurve.GetType() == GeomAbs_Line ? 1 : 24;
                const bool reverse = e.Current().Orientation() == TopAbs_REVERSED;
                for (int k = 0; k < samples; ++k) {
                    const double fraction = reverse ? 1. - static_cast<double>(k) / samples
                                                    : static_cast<double>(k) / samples;
                    measured.outline.push_back(dto(outerCurve.Value(
                        outerCurve.FirstParameter() +
                        (outerCurve.LastParameter() - outerCurve.FirstParameter()) * fraction)));
                }
                const auto at = edgeIndex[id];
                const auto found =
                    at < result->edges.size() ? result->edges.begin() + at : result->edges.end();
                if (found != result->edges.end())
                    boundary.push_back(&*found);
            }
            // A rectangular outer wire is verified, not inferred from a face bounding box.
            bool rectangle = measured.planar && boundary.size() == 4 &&
                             std::all_of(boundary.begin(), boundary.end(),
                                         [](const auto *e) { return e->straight; });
            for (std::size_t k = 0; rectangle && k < 4; ++k) {
                const gp_Vec a(point(boundary[k]->first), point(boundary[k]->second));
                const gp_Vec b(point(boundary[(k + 1) % 4]->first),
                               point(boundary[(k + 1) % 4]->second));
                rectangle = std::abs(a.Normalized().Dot(b.Normalized())) < 1.e-7;
            }
            if (rectangle)
                for (std::size_t k = 0; k < 2; ++k)
                    measured.sizes.push_back({boundary[k]->first,
                                              boundary[k]->second,
                                              boundary[k]->lengthMm,
                                              {measured.id}});
            result->faces.push_back(std::move(measured));
        } catch (
            const Standard_Failure &) { /* Keep import usable when a measurement is unavailable. */
        }
    }
    for (auto &edge : result->edges)
        for (const auto &bend : metadata.bendAnalysis.bends)
            if (has(edge.faces, bend.innerFace) || has(edge.faces, bend.outerFace))
                edge.bendBoundary = true;
    if (stop.stop_requested())
        return result;
    try {
        for (const auto &opening : drawingOpenings(shape, faces, stop, 256)) {
            if (result->openings.size() >= 256 || stop.stop_requested())
                break;
            MeasuredOpening measured;
            measured.id = result->openings.size();
            measured.center = dto(opening.center);
            measured.normal = vectorDto(gp_Vec(opening.normal));
            measured.major = vectorDto(opening.major);
            measured.minor = vectorDto(opening.minor);
            measured.lengthMm = opening.major.Magnitude() * 2;
            measured.widthMm = opening.minor.Magnitude() * 2;
            measured.circular = opening.circular;
            measured.faces = opening.wallFaces;
            for (BRepTools_WireExplorer e(opening.wire); e.More(); e.Next()) {
                const auto id = static_cast<std::size_t>(edges.FindIndex(e.Current()));
                measured.edges.push_back(id);
                BRepAdaptor_Curve curve(e.Current());
                const int count = curve.GetType() == GeomAbs_Line ? 1 : 24;
                for (int k = 0; k < count; ++k) {
                    const double fraction = e.Current().Orientation() == TopAbs_REVERSED
                                                ? 1. - static_cast<double>(k) / count
                                                : static_cast<double>(k) / count;
                    measured.outline.push_back(dto(
                        curve.Value(curve.FirstParameter() +
                                    (curve.LastParameter() - curve.FirstParameter()) * fraction)));
                }
            }
            // Find the actual planar host of this inner wire, then choose two
            // perpendicular finite base edges with valid feet on those edges.
            for (int id = 1; !measured.hostFace && id <= faces.Extent(); ++id) {
                const auto face = TopoDS::Face(faces(id));
                BRepAdaptor_Surface surface(face);
                if (surface.GetType() != GeomAbs_Plane)
                    continue;
                for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next())
                    if (w.Current().IsSame(opening.wire))
                        measured.hostFace = static_cast<std::size_t>(id);
            }
            const auto host =
                std::find_if(result->faces.begin(), result->faces.end(),
                             [&](const auto &f) { return f.id == measured.hostFace; });
            std::vector<const MeasuredEdge *> bases;
            if (host != result->faces.end())
                for (auto id : host->outerEdges) {
                    const auto edge = std::find_if(
                        result->edges.begin(), result->edges.end(),
                        [&](const auto &e) { return e.id == id && e.straight && !e.bendBoundary; });
                    if (edge != result->edges.end() && location(*edge, opening.center))
                        bases.push_back(&*edge);
                }
            std::stable_sort(bases.begin(), bases.end(), [&](const auto *a, const auto *b) {
                return location(*a, opening.center)->valueMm <
                       location(*b, opening.center)->valueMm;
            });
            for (const auto *edge : bases) {
                if (measured.locations.size() == 2)
                    break;
                if (!measured.locations.empty()) {
                    const gp_Vec first(point(measured.locations.front().first), opening.center);
                    const gp_Vec next(point(location(*edge, opening.center)->first),
                                      opening.center);
                    if (first.Magnitude() <= 1.e-8 || next.Magnitude() <= 1.e-8 ||
                        std::abs(first.Normalized().Dot(next.Normalized())) > 1.e-7)
                        continue;
                }
                measured.locations.push_back(*location(*edge, opening.center));
            }
            // Any actual side wall of the closed contour can select the whole
            // feature, including planar slot walls and non-cylindrical sides.
            for (auto id : measured.edges) {
                const auto at = id < edgeIndex.size() ? edgeIndex[id] : result->edges.size();
                if (at >= result->edges.size())
                    continue;
                for (auto face : result->edges[at].faces)
                    if (face != measured.hostFace && !has(measured.faces, face))
                        measured.faces.push_back(face);
            }
            measured.hostFaces = {measured.hostFace};
            // Merge opposite skins only with a shared actual wall, never merely
            // because two holes happen to be coaxial in different flanges.
            const auto duplicate = std::find_if(
                result->openings.begin(), result->openings.end(), [&](const auto &old) {
                    const gp_Vec delta(point(old.center), opening.center);
                    return std::abs(old.lengthMm - measured.lengthMm) < 1.e-5 &&
                           std::abs(old.widthMm - measured.widthMm) < 1.e-5 &&
                           delta.Crossed(gp_Vec(opening.normal)).Magnitude() < 1.e-5 &&
                           std::any_of(measured.faces.begin(), measured.faces.end(),
                                       [&](auto id) { return has(old.faces, id); });
                });
            if (duplicate == result->openings.end())
                result->openings.push_back(std::move(measured));
            else {
                duplicate->hostFaces.push_back(measured.hostFace);
                for (auto id : measured.edges)
                    if (!has(duplicate->edges, id))
                        duplicate->edges.push_back(id);
                for (auto id : measured.faces)
                    if (!has(duplicate->faces, id))
                        duplicate->faces.push_back(id);
            }
        }
    } catch (const Standard_Failure &) {
    } catch (const application::DrawingError &) {
    }
    // Reuse the tested section measurement without HLR, layout or PDF generation.
    try {
        const gp_Ax2 reference(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
        for (const auto &profile : drawingProfiles(metadata.bendAnalysis.bends, faces, reference,
                                                   metadata.bendAnalysis.lengthToleranceMm, stop)) {
            const auto station = point(profile.station);
            const auto frame = drawingSectionFrame(
                station, gp_Dir(profile.normal.x, profile.normal.y, profile.normal.z), reference);
            const auto unproject = [&](DrawingPoint p) {
                return dto(station.Translated(gp_Vec(frame.XDirection()) * p.x +
                                              gp_Vec(frame.YDirection()) * p.y));
            };
            for (const auto &segment : profile.segments) {
                MeasuredSpan span{unproject(segment.dimension.start),
                                  unproject(segment.dimension.end),
                                  segment.dimension.valueMm,
                                  {segment.wallFaces[0], segment.wallFaces[1]}};
                if (std::none_of(result->flanges.begin(), result->flanges.end(),
                                 [&](const auto &old) {
                                     return old.faces == span.faces &&
                                            point(old.first).Distance(point(span.first)) < 1.e-5 &&
                                            point(old.second).Distance(point(span.second)) < 1.e-5;
                                 }))
                    result->flanges.push_back(std::move(span));
            }
        }
    } catch (const Standard_Failure &) {
    } catch (const application::DrawingError &) {
    }
    return result;
}

std::shared_ptr<const application::ModelMeasurements>
flatModelMeasurements(const application::ModelMeasurements &data, const TopoDS_Shape &stock,
                      std::span<const SheetWallRegion> walls, const gp_Trsf &origin,
                      const std::map<std::size_t, std::size_t> &owners, std::stop_token stop) {
    using namespace application;
    auto result = std::make_shared<ModelMeasurements>();
    const auto ownerOf = [&](const auto &ids) -> std::optional<std::size_t> {
        std::optional<std::size_t> owner;
        for (auto id : ids)
            if (const auto it = owners.find(id); it != owners.end()) {
                if (owner && *owner != it->second)
                    return {};
                owner = it->second;
            }
        return owner;
    };
    const auto transformAt = [&](std::size_t wall) {
        auto t = walls[wall].toFlat;
        t.PreMultiply(origin);
        return t;
    };
    const auto mapPoint = [](PointMm p, const gp_Trsf &t) { return dto(point(p).Transformed(t)); };
    for (const auto &opening : data.openings)
        if (const auto owner = ownerOf(opening.hostFaces))
            result->openings.push_back(transformOpening(opening, transformAt(*owner)));
    for (const auto &face : data.faces)
        if (const auto found = owners.find(face.id); found != owners.end()) {
            const auto transform = transformAt(found->second);
            auto mapped = face;
            mapped.anchor = mapPoint(face.anchor, transform);
            mapped.normal = vectorDto(
                gp_Vec(face.normal.x, face.normal.y, face.normal.z).Transformed(transform));
            for (auto &span : mapped.sizes) {
                span.first = mapPoint(span.first, transform);
                span.second = mapPoint(span.second, transform);
            }
            for (auto &p : mapped.outline)
                p = mapPoint(p, transform);
            result->faces.push_back(std::move(mapped));
        }
    for (const auto &edge : data.edges)
        if (const auto owner = ownerOf(edge.faces)) {
            const auto transform = transformAt(*owner);
            auto mapped = edge;
            mapped.first = mapPoint(edge.first, transform);
            mapped.second = mapPoint(edge.second, transform);
            mapped.center = mapPoint(edge.center, transform);
            for (auto &p : mapped.outline)
                p = mapPoint(p, transform);
            result->edges.push_back(std::move(mapped));
        }
    // Flat locations use the real external stock boundary, not a source-wall
    // tangency edge that became an internal seam during unfolding.
    std::size_t nextId = 1;
    for (const auto &edge : data.edges)
        nextId = std::max(nextId, edge.id + 1);
    const auto stockFace = TopoDS::Face(TopExp_Explorer(stock, TopAbs_FACE).Current());
    for (BRepTools_WireExplorer wire(BRepTools::OuterWire(stockFace), stockFace); wire.More();
         wire.Next()) {
        if (stop.stop_requested())
            return result;
        BRepAdaptor_Curve curve(wire.Current());
        if (curve.GetType() != GeomAbs_Line)
            continue;
        MeasuredEdge edge;
        edge.id = nextId++;
        edge.straight = true;
        edge.first = dto(curve.Value(curve.FirstParameter()));
        edge.second = dto(curve.Value(curve.LastParameter()));
        edge.lengthMm = point(edge.first).Distance(point(edge.second));
        if (edge.lengthMm <= 1.e-6)
            continue;
        edge.center = dto(gp_Pnt((point(edge.first).XYZ() + point(edge.second).XYZ()) / 2));
        edge.outline = {edge.first, edge.second};
        result->datumEdges.push_back(edge.id);
        result->edges.push_back(std::move(edge));
    }
    for (auto &opening : result->openings) {
        opening.locations.clear();
        auto center = point(opening.center);
        center.SetZ(0);
        std::vector<const MeasuredEdge *> candidates;
        for (auto id : result->datumEdges) {
            const auto found = std::find_if(result->edges.begin(), result->edges.end(),
                                            [&](const auto &e) { return e.id == id; });
            if (found != result->edges.end() && location(*found, center))
                candidates.push_back(&*found);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](const auto *a, const auto *b) {
            return location(*a, center)->valueMm < location(*b, center)->valueMm;
        });
        for (const auto *edge : candidates) {
            const auto measured = *location(*edge, center);
            if (!opening.locations.empty()) {
                const gp_Vec a(point(opening.locations.front().first), center),
                    b(point(measured.first), center);
                if (a.Magnitude() < 1.e-8 || b.Magnitude() < 1.e-8 ||
                    std::abs(a.Normalized().Dot(b.Normalized())) > 1.e-7)
                    continue;
            }
            opening.locations.push_back(measured);
            if (opening.locations.size() == 2)
                break;
        }
    }
    return result;
}

application::MeasuredOpening transformOpening(const application::MeasuredOpening &source,
                                              const gp_Trsf &transform) {
    auto result = source;
    const auto p = [&](PointMm v) { return dto(point(v).Transformed(transform)); };
    const auto v = [&](PointMm a) {
        return vectorDto(gp_Vec(a.x, a.y, a.z).Transformed(transform));
    };
    result.center = p(result.center);
    result.normal = v(result.normal);
    result.major = v(result.major);
    result.minor = v(result.minor);
    for (auto &span : result.locations) {
        span.first = p(span.first);
        span.second = p(span.second);
    }
    for (auto &vertex : result.outline)
        vertex = p(vertex);
    return result;
}
} // namespace bending::cad
