#include "FlatPatternDisplay.h"
#include "bending/application/FlatPattern.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
struct RetryWithFaceOwner {};
void check(bool ok, const char *message) {
    if (!ok)
        throw application::FlatPatternError{application::FlatPatternErrorCode::UnsupportedGeometry,
                                            message};
}
double segmentDistance(gp_Pnt p, gp_Pnt a, gp_Pnt b) {
    const gp_Vec ab(a, b);
    const double f = ab.SquareMagnitude() > 1e-20
                         ? std::clamp(gp_Vec(a, p).Dot(ab) / ab.SquareMagnitude(), 0., 1.)
                         : 0;
    return p.Distance(a.Translated(ab * f));
}
struct Outline {
    std::vector<gp_Pnt> points;
    gp_Pnt minimum, maximum;
};
bool sameContour(const Outline &a, const Outline &b) {
    if (a.minimum.Distance(b.minimum) > .04 || a.maximum.Distance(b.maximum) > .04)
        return false;
    const auto covered = [](const auto &points, const auto &line) {
        return std::all_of(points.begin(), points.end(), [&](gp_Pnt p) {
            for (std::size_t i = 1; i < line.size(); ++i)
                if (segmentDistance(p, line[i - 1], line[i]) <= .04)
                    return true;
            return false;
        });
    };
    return covered(a.points, b.points) && covered(b.points, a.points);
}
} // namespace
std::optional<std::size_t> sourceFaceRegionOwner(const TopoDS_Face &face,
                                                 std::span<const TopoDS_Shape> regions,
                                                 std::span<const Bnd_Box> boxes, double tolerance,
                                                 std::size_t &queries, std::stop_token stop) {
    const auto area = [](const TopoDS_Shape &shape) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(shape, props);
        return props.Mass();
    };
    try {
        Bnd_Box faceBox;
        BRepBndLib::AddOptimal(face, faceBox, false, false);
        const auto total = area(face);
        if (!std::isfinite(total) || total <= 1.e-8)
            return {};
        const double areaTolerance = std::max(1.e-8, total * 1.e-7);
        std::optional<std::size_t> owner;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            if (stop.stop_requested())
                return {};
            if (boxes[i].IsOut(faceBox))
                continue;
            if (queries >= sourceFaceRegionQueryLimit)
                return {};
            ++queries;
            TopTools_ListOfShape arguments, tools;
            arguments.Append(face);
            tools.Append(regions[i]);
            BRepAlgoAPI_Common common;
            common.SetArguments(arguments);
            common.SetTools(tools);
            common.SetNonDestructive(true);
            common.SetFuzzyValue(tolerance);
            common.Build();
            if (stop.stop_requested() || !common.IsDone() || common.HasErrors() ||
                common.Shape().IsNull())
                return {};
            const auto covered = area(common.Shape());
            if (!std::isfinite(covered) || covered < 0)
                return {};
            if (covered <= areaTolerance)
                continue;
            if (owner || std::abs(covered - total) > areaTolerance ||
                !BRepCheck_Analyzer(common.Shape()).IsValid())
                return {};
            owner = i;
        }
        return owner;
    } catch (const Standard_Failure &) {
        return {};
    }
}
FlatCutoutDisplay flatCutoutDisplay(const TopoDS_Shape &source,
                                    const std::vector<FlatDisplayRegion> &regions, gp_Vec shift,
                                    double tolerance, std::stop_token stop,
                                    const SheetOrientationData *orientation) {
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw application::FlatPatternError{application::FlatPatternErrorCode::Cancelled,
                                                "Cancelled"};
    };
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> classifiers;
    std::vector<TopoDS_Shape> regionSolids;
    std::vector<Bnd_Box> boxes;
    for (const auto &region : regions) {
        classifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(region.solid));
        regionSolids.push_back(region.solid);
        Bnd_Box box;
        BRepBndLib::AddOptimal(region.solid, box, false, false);
        box.Enlarge(tolerance * 10);
        boxes.push_back(box);
    }
    std::size_t ownerQueries = 0;
    const auto map = [&](gp_Pnt p, const TopoDS_Face &face, std::optional<std::size_t> &owner) {
        cancelled();
        if (owner)
            return regions[*owner].map(p).Translated(shift);
        std::optional<gp_Pnt> mapped;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            if (boxes[i].IsOut(p))
                continue;
            classifiers[i]->Perform(p, tolerance * 10);
            if (classifiers[i]->State() != TopAbs_IN && classifiers[i]->State() != TopAbs_ON)
                continue;
            const auto next = regions[i].map(p).Translated(shift);
            if (mapped && mapped->Distance(next) > .02) {
                owner =
                    sourceFaceRegionOwner(face, regionSolids, boxes, tolerance, ownerQueries, stop);
                cancelled();
                check(owner.has_value(),
                      "Ambiguous source surface mapping at an unfolded boundary");
                throw RetryWithFaceOwner{};
            }
            mapped = next;
        }
        check(mapped.has_value(), "A source cutout surface is outside the unfolded regions");
        return *mapped;
    };
    BRepBuilderAPI_Copy copier(source, true, false);
    auto copy = copier.Shape();
    TopTools_IndexedMapOfShape sourceFaces, copiedFaces;
    TopExp::MapShapes(source, TopAbs_FACE, sourceFaces);
    TopExp::MapShapes(copy, TopAbs_FACE, copiedFaces);
    std::vector<SheetSide> sides(static_cast<std::size_t>(copiedFaces.Extent()) + 1,
                                 SheetSide::Neutral);
    if (orientation && orientation->available &&
        orientation->faces.size() == static_cast<std::size_t>(sourceFaces.Extent()) + 1)
        for (int i = 1; i <= sourceFaces.Extent(); ++i) {
            const auto index = copiedFaces.FindIndex(copier.ModifiedShape(sourceFaces(i)));
            if (index)
                sides[index] = orientation->faces[i];
        }
    BRepMesh_IncrementalMesh mesher(copy, .02, false, .1, false);
    check(mesher.IsDone(), "Cannot mesh source cutouts for unfolding display");
    FlatCutoutDisplay result;
    result.faceSides.push_back(SheetSide::Neutral);
    TopoDS_Compound skin;
    BRep_Builder builder;
    builder.MakeCompound(skin);
    std::vector<Outline> outlines;
    std::size_t pointCount = 0;
    for (TopExp_Explorer f(copy, TopAbs_FACE); f.More(); f.Next()) {
        cancelled();
        const auto face = TopoDS::Face(f.Current());
        TopLoc_Location location;
        const auto original = BRep_Tool::Triangulation(face, location);
        check(!original.IsNull(), "Missing original surface triangulation");
        Handle(Poly_Triangulation) mesh =
            new Poly_Triangulation(original->NbNodes(), original->NbTriangles(), false);
        std::optional<std::size_t> owner;
        std::vector<std::vector<gp_Pnt>> faceOutlines;
        const auto mapFace = [&] {
            for (int n = 1; n <= original->NbNodes(); ++n)
                mesh->SetNode(
                    n, map(original->Node(n).Transformed(location.Transformation()), face, owner));
            const auto outer = BRepTools::OuterWire(face);
            for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
                if (w.Current().IsSame(outer))
                    continue;
                std::vector<gp_Pnt> contour;
                for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More();
                     e.Next()) {
                    if (BRep_Tool::Degenerated(e.Current()))
                        continue;
                    BRepAdaptor_Curve curve(e.Current());
                    auto first = curve.FirstParameter(), last = curve.LastParameter();
                    if (e.Orientation() == TopAbs_REVERSED)
                        std::swap(first, last);
                    check(std::isfinite(first) && std::isfinite(last), "Unbounded cutout edge");
                    const auto at = [&](double u) {
                        auto p = map(curve.Value(u), face, owner);
                        p.SetZ(0);
                        return p;
                    };
                    auto previous = at(first);
                    if (contour.empty())
                        contour.push_back(previous);
                    const auto sample = [&](auto &&self, double u0, double u1, gp_Pnt a, gp_Pnt b,
                                            int depth) -> void {
                        const double mid = (u0 + u1) / 2;
                        const auto m = at(mid);
                        const double error = std::max(
                            {segmentDistance(m, a, b), segmentDistance(at((3 * u0 + u1) / 4), a, b),
                             segmentDistance(at((u0 + 3 * u1) / 4), a, b)});
                        if (error > .02) {
                            check(depth < 16, "Cutout display curve exceeds approximation limit");
                            self(self, u0, mid, a, m, depth + 1);
                            self(self, mid, u1, m, b, depth + 1);
                        } else {
                            check(++pointCount <= 100000, "Cutout display exceeds point budget");
                            contour.push_back(b);
                        }
                    };
                    for (int step = 0; step < 8; ++step) {
                        const double u0 = first + (last - first) * step / 8;
                        const double u1 = first + (last - first) * (step + 1) / 8;
                        const auto next = at(u1);
                        sample(sample, u0, u1, previous, next, 0);
                        previous = next;
                    }
                }
                check(contour.size() >= 4 && contour.front().Distance(contour.back()) <= .04,
                      "Cutout contour is not closed after unfolding");
                contour.back() = contour.front();
                faceOutlines.push_back(std::move(contour));
            }
        };
        const auto previousPointCount = pointCount;
        try {
            mapFace();
        } catch (const RetryWithFaceOwner &) {
            // Commit the complete face and its wires using a single proven transform.
            faceOutlines.clear();
            pointCount = previousPointCount;
            mapFace();
        }
        for (int t = 1; t <= original->NbTriangles(); ++t)
            mesh->SetTriangle(t, original->Triangle(t));
        mesh->Deflection(.02);
        TopoDS_Face display;
        builder.MakeFace(display, mesh);
        display.Orientation(face.Orientation());
        builder.Add(skin, display);
        result.faceSides.push_back(sides[copiedFaces.FindIndex(face)]);
        for (auto &contour : faceOutlines) {
            Bnd_Box bounds;
            for (const auto &point : contour)
                bounds.Add(point);
            Outline outline{std::move(contour), bounds.CornerMin(), bounds.CornerMax()};
            if (std::none_of(outlines.begin(), outlines.end(),
                             [&](const auto &other) { return sameContour(outline, other); }))
                outlines.push_back(std::move(outline));
        }
    }
    for (const auto &line : outlines) {
        std::vector<application::PointMm> points;
        points.reserve(line.points.size());
        for (const auto &p : line.points)
            points.push_back({p.X(), p.Y(), 0});
        result.contours.push_back(std::move(points));
    }
    result.surface = skin;
    return result;
}
} // namespace bending::cad
