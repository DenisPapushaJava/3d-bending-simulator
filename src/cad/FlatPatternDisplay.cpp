#include "FlatPatternDisplay.h"
#include "bending/application/FlatPattern.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
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
bool sameContour(const std::vector<gp_Pnt> &a, const std::vector<gp_Pnt> &b) {
    Bnd_Box first, second;
    for (const auto &p : a)
        first.Add(p);
    for (const auto &p : b)
        second.Add(p);
    if (first.CornerMin().Distance(second.CornerMin()) > .04 ||
        first.CornerMax().Distance(second.CornerMax()) > .04)
        return false;
    const auto covered = [](const auto &points, const auto &line) {
        return std::all_of(points.begin(), points.end(), [&](gp_Pnt p) {
            for (std::size_t i = 1; i < line.size(); ++i)
                if (segmentDistance(p, line[i - 1], line[i]) <= .04)
                    return true;
            return false;
        });
    };
    return covered(a, b) && covered(b, a);
}
} // namespace
FlatCutoutDisplay flatCutoutDisplay(const TopoDS_Shape &source,
                                    const std::vector<FlatDisplayRegion> &regions, gp_Vec shift,
                                    double tolerance, std::stop_token stop) {
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw application::FlatPatternError{application::FlatPatternErrorCode::Cancelled,
                                                "Cancelled"};
    };
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> classifiers;
    std::vector<Bnd_Box> boxes;
    for (const auto &region : regions) {
        classifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(region.solid));
        Bnd_Box box;
        BRepBndLib::AddOptimal(region.solid, box, false, false);
        box.Enlarge(tolerance * 10);
        boxes.push_back(box);
    }
    const auto map = [&](gp_Pnt p) {
        cancelled();
        std::optional<gp_Pnt> mapped;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            if (boxes[i].IsOut(p))
                continue;
            classifiers[i]->Perform(p, tolerance * 10);
            if (classifiers[i]->State() != TopAbs_IN && classifiers[i]->State() != TopAbs_ON)
                continue;
            const auto next = regions[i].map(p).Translated(shift);
            check(!mapped || mapped->Distance(next) <= .02,
                  "Ambiguous source surface mapping at an unfolded boundary");
            mapped = next;
        }
        check(mapped.has_value(), "A source cutout surface is outside the unfolded regions");
        return *mapped;
    };
    auto copy = BRepBuilderAPI_Copy(source, true, false).Shape();
    BRepMesh_IncrementalMesh mesher(copy, .02, false, .1, false);
    check(mesher.IsDone(), "Cannot mesh source cutouts for unfolding display");
    FlatCutoutDisplay result;
    TopoDS_Compound skin;
    BRep_Builder builder;
    builder.MakeCompound(skin);
    std::vector<std::vector<gp_Pnt>> outlines;
    std::size_t pointCount = 0;
    for (TopExp_Explorer f(copy, TopAbs_FACE); f.More(); f.Next()) {
        cancelled();
        const auto face = TopoDS::Face(f.Current());
        TopLoc_Location location;
        const auto original = BRep_Tool::Triangulation(face, location);
        check(!original.IsNull(), "Missing original surface triangulation");
        Handle(Poly_Triangulation) mesh =
            new Poly_Triangulation(original->NbNodes(), original->NbTriangles(), false);
        for (int n = 1; n <= original->NbNodes(); ++n)
            mesh->SetNode(n, map(original->Node(n).Transformed(location.Transformation())));
        for (int t = 1; t <= original->NbTriangles(); ++t)
            mesh->SetTriangle(t, original->Triangle(t));
        mesh->Deflection(.02);
        TopoDS_Face display;
        builder.MakeFace(display, mesh);
        display.Orientation(face.Orientation());
        builder.Add(skin, display);
        const auto outer = BRepTools::OuterWire(face);
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            if (w.Current().IsSame(outer))
                continue;
            std::vector<gp_Pnt> contour;
            for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
                if (BRep_Tool::Degenerated(e.Current()))
                    continue;
                BRepAdaptor_Curve curve(e.Current());
                auto first = curve.FirstParameter(), last = curve.LastParameter();
                if (e.Orientation() == TopAbs_REVERSED)
                    std::swap(first, last);
                check(std::isfinite(first) && std::isfinite(last), "Unbounded cutout edge");
                const auto at = [&](double u) {
                    auto p = map(curve.Value(u));
                    p.SetZ(0);
                    return p;
                };
                if (contour.empty())
                    contour.push_back(at(first));
                const auto sample = [&](auto &&self, double u0, double u1, gp_Pnt a, gp_Pnt b,
                                        int depth) -> void {
                    const double mid = (u0 + u1) / 2;
                    const auto m = at(mid);
                    const double error = std::max({segmentDistance(m, a, b),
                                                   segmentDistance(at((3 * u0 + u1) / 4), a, b),
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
                    sample(sample, u0, u1, at(u0), at(u1), 0);
                }
            }
            check(contour.size() >= 4 && contour.front().Distance(contour.back()) <= .04,
                  "Cutout contour is not closed after unfolding");
            contour.back() = contour.front();
            if (std::none_of(outlines.begin(), outlines.end(),
                             [&](const auto &other) { return sameContour(contour, other); }))
                outlines.push_back(std::move(contour));
        }
    }
    for (const auto &line : outlines) {
        std::vector<application::PointMm> points;
        for (const auto &p : line)
            points.push_back({p.X(), p.Y(), 0});
        result.contours.push_back(std::move(points));
    }
    result.surface = skin;
    return result;
}
} // namespace bending::cad
