#include "SheetOrientation.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <queue>

namespace bending::cad {
namespace {
gp_Vec normal(const TopoDS_Face &face) {
    auto n = gp_Vec(BRepAdaptor_Surface(face).Plane().Axis().Direction());
    return face.Orientation() == TopAbs_REVERSED ? -n : n;
}
bool on(const TopoDS_Face &face, gp_Pnt p, double tolerance) {
    BRepClass_FaceClassifier test(face, p, tolerance);
    return test.State() == TopAbs_IN || test.State() == TopAbs_ON;
}
bool sameSurface(const TopoDS_Face &a, const TopoDS_Face &b, double tolerance) {
    BRepAdaptor_Surface x(a), y(b);
    if (x.GetType() != y.GetType())
        return false;
    if (x.GetType() == GeomAbs_Plane)
        return normal(a).Dot(normal(b)) > 1 - 1e-7 &&
               x.Plane().Distance(y.Plane().Location()) <= tolerance;
    if (x.GetType() == GeomAbs_Cylinder)
        return a.Orientation() == b.Orientation() &&
               x.Cylinder().Axis().IsCoaxial(y.Cylinder().Axis(), 1e-7, tolerance) &&
               std::abs(x.Cylinder().Radius() - y.Cylinder().Radius()) <= tolerance;
    return false;
}
bool sharedInterior(const TopoDS_Face &target, const TopoDS_Face &source) {
    double u0, u1, v0, v1;
    BRepTools::UVBounds(target, u0, u1, v0, v1);
    if (!std::isfinite(u0) || !std::isfinite(u1) || !std::isfinite(v0) || !std::isfinite(v1))
        return false;
    BRepAdaptor_Surface surface(target);
    for (double u : {.5, .37, .63})
        for (double v : {.5, .37, .63}) {
            const auto p = surface.Value(u0 + (u1 - u0) * u, v0 + (v1 - v0) * v);
            BRepClass_FaceClassifier a(target, p, 1e-5), b(source, p, 1e-5);
            if (a.State() == TopAbs_IN && b.State() == TopAbs_IN)
                return true;
        }
    return false;
}
Bnd_Box faceBounds(const TopoDS_Shape &face) {
    Bnd_Box box;
    try {
        BRepBndLib::Add(face, box, false);
        if (!box.IsVoid())
            box.Enlarge(1e-5);
    } catch (const Standard_Failure &) {
        box.SetVoid(); // Unknown bounds cannot reject a possible correspondence.
    }
    return box;
}
} // namespace
SheetOrientationData classifySheetOrientation(const TopoDS_Shape &shape,
                                              const application::DocumentMetadata &metadata,
                                              std::stop_token stop,
                                              std::optional<SheetReference> reference) {
    SheetOrientationData result;
    try {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        const auto count = static_cast<std::size_t>(faces.Extent());
        result.faces.resize(count + 1, SheetSide::Neutral);
        std::vector<std::vector<std::pair<std::size_t, int>>> graph(count + 1);
        const auto link = [&](std::size_t a, std::size_t b, int different) {
            if (!a || !b || a > count || b > count)
                return;
            graph[a].push_back({b, different});
            graph[b].push_back({a, different});
        };
        for (const auto &bend : metadata.bendAnalysis.bends) {
            link(bend.innerFace, bend.outerFace, 1);
            for (std::size_t i : {0u, 2u}) {
                link(bend.innerFace, bend.flangeFaces[i], 0);
                link(bend.outerFace, bend.flangeFaces[i + 1], 0);
                link(bend.flangeFaces[i], bend.flangeFaces[i + 1], 1);
            }
        }
        TopTools_IndexedDataMapOfShapeListOfShape adjacent;
        TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, adjacent);
        for (int e = 1; e <= adjacent.Extent(); ++e) {
            if (stop.stop_requested())
                return {};
            const auto &around = adjacent.FindFromIndex(e);
            if (around.Extent() != 2)
                continue;
            const auto a = TopoDS::Face(around.First()), b = TopoDS::Face(around.Last());
            if (BRepAdaptor_Surface(a).GetType() == GeomAbs_Plane && sameSurface(a, b, 1e-5))
                link(faces.FindIndex(a), faces.FindIndex(b), 0);
        }
        std::size_t root = 0, fallbackRoot = 0;
        double largest = 0, fallbackArea = 0;
        const auto estimatedThickness = application::suggestThickness(metadata.thickness);
        for (std::size_t i = 1; i <= count; ++i) {
            const auto face = TopoDS::Face(faces(static_cast<int>(i)));
            if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane)
                continue;
            if (reference) {
                if (reference->normal.SquareMagnitude() > .5 &&
                    normal(face).Dot(reference->normal) > 1 - 1e-7 &&
                    BRepAdaptor_Surface(face).Plane().Distance(reference->point) < 1e-5 &&
                    on(face, reference->point, 1e-5)) {
                    root = i;
                    break;
                }
            } else {
                GProp_GProps area;
                BRepGProp::SurfaceProperties(face, area);
                const bool eligible = !graph[i].empty() || estimatedThickness.has_value();
                if (eligible && area.Mass() > fallbackArea) {
                    fallbackRoot = i;
                    fallbackArea = area.Mass();
                }
                bool freeEdge = false;
                if (area.Mass() > largest)
                    for (TopExp_Explorer it(BRepTools::OuterWire(face), TopAbs_EDGE);
                         it.More() && !freeEdge; it.Next()) {
                        bool rail = false;
                        const auto at = adjacent.FindIndex(it.Current());
                        if (at)
                            for (const auto &other : adjacent.FindFromIndex(at)) {
                                const auto index = faces.FindIndex(other);
                                rail |= index != static_cast<int>(i) && index > 0 &&
                                        !graph[index].empty();
                            }
                        freeEdge =
                            !rail &&
                            BRepAdaptor_Curve(TopoDS::Edge(it.Current())).GetType() == GeomAbs_Line;
                    }
                if (freeEdge && eligible && area.Mass() > largest) {
                    largest = area.Mass();
                    root = i;
                }
            }
        }
        if (!root && !reference)
            root = fallbackRoot;
        if (!root)
            return {};
        const auto base = TopoDS::Face(faces(static_cast<int>(root)));
        if (!reference) {
            SheetReference initial;
            initial.normal = normal(base);
            initial.thicknessMm =
                estimatedThickness.value_or(metadata.bendAnalysis.bends.empty()
                                                ? 0
                                                : metadata.bendAnalysis.bends.front().thicknessMm);
            std::vector<std::pair<double, SheetEdgeMarker>> candidates;
            const auto outer = BRepTools::OuterWire(base);
            for (TopExp_Explorer it(outer, TopAbs_EDGE); it.More(); it.Next()) {
                const auto edge = TopoDS::Edge(it.Current());
                const auto at = adjacent.FindIndex(edge);
                bool rail = false;
                if (at)
                    for (const auto &other : adjacent.FindFromIndex(at)) {
                        const auto index = faces.FindIndex(other);
                        rail |=
                            index != static_cast<int>(root) && index > 0 && !graph[index].empty();
                    }
                if (rail)
                    continue;
                BRepAdaptor_Curve curve(edge);
                if (curve.GetType() != GeomAbs_Line)
                    continue;
                auto a = curve.Value(curve.FirstParameter()),
                     b = curve.Value(curve.LastParameter());
                const double length = a.Distance(b);
                if (length <= 1e-5)
                    continue;
                const auto less = [](gp_Pnt x, gp_Pnt y) {
                    if (x.X() != y.X())
                        return x.X() < y.X();
                    if (x.Y() != y.Y())
                        return x.Y() < y.Y();
                    return x.Z() < y.Z();
                };
                if (less(b, a))
                    std::swap(a, b);
                const gp_Vec direction(a, b);
                const auto middle = a.Translated(direction * .5);
                const auto half = direction.Normalized() * std::min(length * .2, 20.);
                candidates.push_back(
                    {length,
                     {{middle.Translated(-half), middle.Translated(half)}, initial.normal}});
            }
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const auto &a, const auto &b) {
                                 if (a.first != b.first)
                                     return a.first > b.first;
                                 const auto x = a.second.points[0], y = b.second.points[0];
                                 if (x.X() != y.X())
                                     return x.X() < y.X();
                                 if (x.Y() != y.Y())
                                     return x.Y() < y.Y();
                                 return x.Z() < y.Z();
                             });
            for (std::size_t i = 0; i < std::min<std::size_t>(candidates.size(), 16); ++i)
                initial.edges.push_back(candidates[i].second);
            if (initial.edges.empty()) {
                bool found = false;
                for (TopExp_Explorer it(outer, TopAbs_EDGE); it.More() && !found; it.Next()) {
                    BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
                    const double first = curve.FirstParameter(), last = curve.LastParameter();
                    if (std::isfinite(first) && std::isfinite(last)) {
                        initial.point = curve.Value((first + last) * .5);
                        found = true;
                    }
                }
                if (!found)
                    return {};
            } else
                initial.point = initial.edges.front().points[0];
            reference = std::move(initial);
        }
        // The opposite skin of an entirely flat state has no remaining bend links.
        if (reference->thicknessMm > 0) {
            const auto opposite =
                reference->point.Translated(-reference->normal * reference->thicknessMm);
            for (std::size_t i = 1; i <= count; ++i) {
                const auto face = TopoDS::Face(faces(static_cast<int>(i)));
                if (i != root && BRepAdaptor_Surface(face).GetType() == GeomAbs_Plane &&
                    normal(face).Dot(reference->normal) < -1 + 1e-7 &&
                    BRepAdaptor_Surface(face).Plane().Distance(opposite) < 1e-5 &&
                    on(face, opposite, 1e-5))
                    link(root, i, 1);
            }
        }
        std::vector<int> labels(count + 1, -1);
        std::queue<std::size_t> pending;
        pending.push(root);
        labels[root] = 0;
        while (!pending.empty()) {
            if (stop.stop_requested())
                return {};
            const auto at = pending.front();
            pending.pop();
            for (const auto &[next, parity] : graph[at]) {
                const auto expected = labels[at] ^ parity;
                if (labels[next] != -1 && labels[next] != expected)
                    return {}; // A conflicting topology must never invent a side.
                if (labels[next] == -1) {
                    labels[next] = expected;
                    pending.push(next);
                }
            }
        }
        std::array<std::size_t, 2> mapped{};
        for (std::size_t i = 1; i <= count; ++i)
            if (labels[i] >= 0) {
                result.faces[i] = labels[i] ? SheetSide::B : SheetSide::A;
                ++mapped[labels[i]];
            }
        result.available = mapped[0] && mapped[1];
        result.reference = *reference;
        return result;
    } catch (const Standard_Failure &) {
        return {};
    }
}
SheetReference movedSheetReference(const SheetReference &source, const gp_Trsf &map) {
    auto result = source;
    result.point.Transform(map);
    result.normal.Transform(map);
    for (auto &edge : result.edges) {
        for (auto &p : edge.points)
            p.Transform(map);
        edge.normal.Transform(map);
    }
    return result;
}
void appendSheetSides(SheetOrientationVisual &visual, const TopoDS_Shape &shape,
                      const std::vector<SheetSide> &labels) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    if (labels.size() != static_cast<std::size_t>(faces.Extent()) + 1)
        return;
    for (int i = 1; i <= faces.Extent(); ++i)
        if (labels[i] != SheetSide::Neutral)
            visual.sides[labels[i] == SheetSide::A ? 0 : 1].push_back(faces(i));
}
SheetOrientationVisual sheetOrientationVisual(const TopoDS_Shape &shape,
                                              const SheetOrientationData &data) {
    SheetOrientationVisual result;
    if (data.available) {
        appendSheetSides(result, shape, data.faces);
        result.edges = data.reference.edges;
    }
    return result;
}
std::vector<SheetSide> matchSheetSides(const TopoDS_Shape &shape, const TopoDS_Shape &source,
                                       const SheetOrientationData &data) {
    TopTools_IndexedMapOfShape targets, originals;
    TopExp::MapShapes(shape, TopAbs_FACE, targets);
    TopExp::MapShapes(source, TopAbs_FACE, originals);
    std::vector<SheetSide> result(static_cast<std::size_t>(targets.Extent()) + 1,
                                  SheetSide::Neutral);
    if (!data.available || data.faces.size() != static_cast<std::size_t>(originals.Extent()) + 1)
        return result;
    std::vector<Bnd_Box> originalBounds(static_cast<std::size_t>(originals.Extent()) + 1);
    for (int j = 1; j <= originals.Extent(); ++j)
        if (data.faces[j] != SheetSide::Neutral)
            originalBounds[j] = faceBounds(originals(j));
    for (int i = 1; i <= targets.Extent(); ++i) {
        const auto original = originals.FindIndex(targets(i));
        if (original && targets(i).Orientation() == originals(original).Orientation()) {
            result[i] = data.faces[original];
            continue;
        }
        const auto targetBounds = faceBounds(targets(i));
        for (int j = 1; j <= originals.Extent(); ++j)
            if (data.faces[j] != SheetSide::Neutral) {
                if (!targetBounds.IsVoid() && !originalBounds[j].IsVoid() &&
                    targetBounds.IsOut(originalBounds[j]))
                    continue;
                bool matches = false;
                try {
                    matches =
                        sameSurface(TopoDS::Face(targets(i)), TopoDS::Face(originals(j)), 1e-5) &&
                        sharedInterior(TopoDS::Face(targets(i)), TopoDS::Face(originals(j)));
                } catch (const Standard_Failure &) {
                }
                if (!matches)
                    continue;
                if (result[i] != SheetSide::Neutral && result[i] != data.faces[j]) {
                    result[i] = SheetSide::Neutral;
                    break;
                }
                result[i] = data.faces[j];
            }
    }
    return result;
}
std::optional<std::size_t> sheetReferenceRegion(const SheetReference &reference,
                                                std::span<const TopoDS_Shape> regions,
                                                double tolerance) {
    for (std::size_t i = 0; i < regions.size(); ++i) {
        try {
            BRepClass3d_SolidClassifier test(regions[i], reference.point, tolerance * 10);
            if (test.State() == TopAbs_IN || test.State() == TopAbs_ON)
                return i;
        } catch (const Standard_Failure &) {
        }
    }
    return {};
}
} // namespace bending::cad
