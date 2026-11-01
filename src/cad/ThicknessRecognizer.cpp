#include "ThicknessRecognizer.h"
#include "bending/application/Document.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <ElSLib.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace bending::cad {
namespace {
double volume(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props, true);
    return std::abs(props.Mass());
}
bool sameSolid(const TopoDS_Shape &original, const TopoDS_Shape &prism, double volumeTolerance,
               std::stop_token stop) {
    for (int direction = 0; direction < 2; ++direction) {
        if (stop.stop_requested()) {
            return false;
        }
        BRepAlgoAPI_Cut difference;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(direction == 0 ? original : prism);
        tools.Append(direction == 0 ? prism : original);
        difference.SetArguments(arguments);
        difference.SetTools(tools);
        difference.SetNonDestructive(true);
        difference.SetRunParallel(false);
        difference.Build();
        if (!difference.IsDone() || difference.HasErrors() || difference.Shape().IsNull() ||
            !BRepCheck_Analyzer(difference.Shape()).IsValid()) {
            return false;
        }
        const double remaining = volume(difference.Shape());
        if (!std::isfinite(remaining) || remaining > volumeTolerance) {
            return false;
        }
    }
    return true;
}
} // namespace
application::ThicknessAnalysis recognizeThickness(const TopoDS_Shape &shape, double toleranceMm,
                                                  std::stop_token stop,
                                                  const application::BendAnalysis *bends) {
    using namespace application;
    ThicknessAnalysis result;
    result.toleranceMm = std::max(1.0e-6, toleranceMm * 10.0);
    constexpr int maximumFaces = 4096, maximumPlanes = 512, maximumRays = 1024;
    try {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        if (faces.Extent() > maximumFaces) {
            result.status = ThicknessStatus::LimitReached;
            return result;
        }
        struct PlaneFace {
            gp_Pln plane;
            gp_Dir outward;
            double area;
            gp_Pnt centre;
        };
        std::map<int, PlaneFace> planes;
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested())
                return {};
            BRepAdaptor_Surface surface(TopoDS::Face(faces(i)));
            if (surface.GetType() != GeomAbs_Plane)
                continue;
            const auto plane = surface.Plane();
            auto normal = plane.Axis().Direction();
            if (faces(i).Orientation() == TopAbs_REVERSED)
                normal.Reverse();
            GProp_GProps props;
            BRepGProp::SurfaceProperties(faces(i), props);
            if (!std::isfinite(props.Mass()) || props.Mass() <= 0.0)
                throw Standard_Failure("Invalid planar face area");
            planes.emplace(i, PlaneFace{plane, normal, props.Mass(), props.CentreOfMass()});
            if (planes.size() > maximumPlanes) {
                result.status = ThicknessStatus::LimitReached;
                return result;
            }
        }
        std::set<std::pair<std::size_t, std::size_t>> measured;
        const auto remember = [&](int a, int b, double gap, ThicknessEvidenceKind kind) {
            auto pair = std::pair{static_cast<std::size_t>(a), static_cast<std::size_t>(b)};
            if (pair.first > pair.second)
                std::swap(pair.first, pair.second);
            if (measured.emplace(pair.first, pair.second).second)
                result.evidence.push_back({pair.first, pair.second, gap, kind});
        };
        IntCurvesFace_ShapeIntersector ray;
        ray.Load(shape, result.toleranceMm);
        BRepClass3d_SolidClassifier material(shape);
        int rays = 0;
        for (const auto &[index, a] : planes) {
            const auto &face = TopoDS::Face(faces(index));
            std::vector<gp_Pnt> samples;
            const auto add = [&](double u, double v) {
                if (samples.size() >= 3 || !std::isfinite(u) || !std::isfinite(v))
                    return;
                if (BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), result.toleranceMm).State() !=
                    TopAbs_IN)
                    return;
                const auto point = ElSLib::Value(u, v, a.plane);
                if (std::none_of(samples.begin(), samples.end(), [&](const auto &p) {
                        return p.Distance(point) <= result.toleranceMm;
                    }))
                    samples.push_back(point);
            };
            double u, v, u0, u1, v0, v1;
            ElSLib::Parameters(a.plane, a.centre, u, v);
            add(u, v);
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            constexpr std::array fractions{.5, .25, .75, .125, .875};
            for (double fu : fractions)
                for (double fv : fractions)
                    add(u0 + fu * (u1 - u0), v0 + fv * (v1 - v0));
            const auto inward = a.outward.Reversed();
            const double minimum = result.toleranceMm * 10;
            const double maximum = std::sqrt(a.area) / 5;
            if (maximum <= minimum)
                continue;
            for (const auto &sample : samples) {
                if (stop.stop_requested())
                    return {};
                if (++rays > maximumRays) {
                    result.status = ThicknessStatus::LimitReached;
                    return result;
                }
                material.Perform(sample.Translated(gp_Vec(inward) * minimum), result.toleranceMm);
                if (material.State() != TopAbs_IN)
                    continue;
                ray.Perform(gp_Lin(sample, inward), minimum, maximum);
                if (!ray.IsDone())
                    throw Standard_Failure("Thickness ray failed");
                if (!ray.NbPnt())
                    continue;
                int first = 1;
                for (int hit = 1; hit <= ray.NbPnt(); ++hit) {
                    if (!std::isfinite(ray.WParameter(hit)))
                        throw Standard_Failure("Non-finite thickness intersection");
                    if (ray.WParameter(hit) < ray.WParameter(first))
                        first = hit;
                }
                // Never skip an earlier non-planar exit to measure a different wall beyond air.
                const int other = faces.FindIndex(ray.Face(first));
                const auto found = planes.find(other);
                if (other == index || found == planes.end() || ray.State(first) != TopAbs_IN)
                    continue;
                const auto &b = found->second;
                const double gap =
                    -gp_Vec(a.plane.Location(), b.plane.Location()).Dot(gp_Vec(a.outward));
                if (!a.outward.IsOpposite(b.outward, 1.0e-7) || !std::isfinite(gap) ||
                    gap <= result.toleranceMm * 100 ||
                    gap > std::sqrt(std::min(a.area, b.area)) / 5 ||
                    std::abs(ray.WParameter(first) - gap) > result.toleranceMm)
                    continue;
                remember(index, other, gap, ThicknessEvidenceKind::PlanarWalls);
            }
        }
        if (bends && bends->status == BendRecognitionStatus::Completed) {
            for (const auto &bend : bends->bends) {
                if (stop.stop_requested())
                    return {};
                if (!bend.innerFace || !bend.outerFace || bend.innerFace == bend.outerFace ||
                    bend.innerFace > static_cast<std::size_t>(faces.Extent()) ||
                    bend.outerFace > static_cast<std::size_t>(faces.Extent()))
                    throw Standard_Failure("Invalid bend thickness evidence");
                BRepAdaptor_Surface inner(TopoDS::Face(faces(static_cast<int>(bend.innerFace))));
                BRepAdaptor_Surface outer(TopoDS::Face(faces(static_cast<int>(bend.outerFace))));
                if (inner.GetType() != GeomAbs_Cylinder || outer.GetType() != GeomAbs_Cylinder ||
                    !std::isfinite(bend.thicknessMm) ||
                    bend.thicknessMm <= result.toleranceMm * 100 ||
                    std::abs(outer.Cylinder().Radius() - inner.Cylinder().Radius() -
                             bend.thicknessMm) > result.toleranceMm)
                    throw Standard_Failure("Inconsistent bend thickness evidence");
                remember(static_cast<int>(bend.innerFace), static_cast<int>(bend.outerFace),
                         bend.thicknessMm, ThicknessEvidenceKind::BendRadii);
            }
        }
        // A local consensus remains a proposal. Only the complete prism proof below may
        // certify constant thickness, including all cutouts and the original outer contour.
        const double originalVolume = volume(shape);
        std::vector<ThicknessEvidence> detected;
        int reconstructions = 0;
        for (const auto &pair : result.evidence) {
            if (stop.stop_requested())
                return {};
            if (pair.kind != ThicknessEvidenceKind::PlanarWalls)
                continue;
            const auto &a = planes.at(static_cast<int>(pair.firstFace));
            const auto &b = planes.at(static_cast<int>(pair.secondFace));
            const double gap = pair.distanceMm;
            const auto &faceA = TopoDS::Face(faces(static_cast<int>(pair.firstFace)));
            const double volumeTolerance =
                std::max(originalVolume * 1.0e-8, a.area * result.toleranceMm * 2.0);
            if (std::abs(a.area - b.area) * gap > volumeTolerance ||
                std::abs(a.area * gap - originalVolume) > volumeTolerance)
                continue;
            gp_Trsf local;
            local.SetTransformation(a.plane.Position());
            Bnd_Box bounds;
            BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(faceA, local, true).Shape(), bounds,
                                   false, false);
            if (bounds.IsVoid() || bounds.IsWhole() || bounds.IsOpen())
                continue;
            double x0, y0, z0, x1, y1, z1;
            bounds.Get(x0, y0, z0, x1, y1, z1);
            const double diagonal = std::hypot(x1 - x0, y1 - y0);
            if (std::min(x1 - x0, y1 - y0) < gap * 5 || !std::isfinite(diagonal) || diagonal <= 0 ||
                a.area / diagonal < gap * 5)
                continue;
            if (++reconstructions > 8) {
                result.status = ThicknessStatus::LimitReached;
                return result;
            }
            const auto prism = BRepPrimAPI_MakePrism(faceA, gp_Vec(a.outward) * -gap, true).Shape();
            if (BRepCheck_Analyzer(prism).IsValid() &&
                sameSolid(shape, prism, volumeTolerance, stop))
                detected.push_back(pair);
        }
        const auto agrees = [&](const std::vector<ThicknessEvidence> &evidence) {
            return std::all_of(evidence.begin(), evidence.end(), [&](const auto &pair) {
                return std::abs(pair.distanceMm - evidence.front().distanceMm) <=
                       result.toleranceMm;
            });
        };
        if (!detected.empty() && agrees(detected)) {
            result.status = ThicknessStatus::Detected;
            result.thicknessMm = detected.front().distanceMm;
            result.evidence = std::move(detected);
        } else if ((!detected.empty() && !agrees(detected)) ||
                   (!result.evidence.empty() && !agrees(result.evidence))) {
            result.status = ThicknessStatus::Ambiguous;
        }
    } catch (const Standard_Failure &) {
        result.status = ThicknessStatus::Failed;
        result.thicknessMm.reset();
    }
    return result;
}
} // namespace bending::cad
