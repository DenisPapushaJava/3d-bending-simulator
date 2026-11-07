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
#include <BRep_Tool.hxx>
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

namespace bending::cad {
namespace {
double measurementTolerance(const TopoDS_Face &face) {
    const double tolerance = BRep_Tool::Tolerance(face);
    if (!std::isfinite(tolerance) || tolerance < 0 || !std::isfinite(tolerance * 10))
        throw Standard_Failure("Invalid thickness surface tolerance");
    return std::max(1.0e-6, tolerance * 10);
}
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
    constexpr int maximumFaces = 4096, maximumPlanes = 512, maximumRays = 1024;
    try {
        if (!std::isfinite(toleranceMm) || toleranceMm < 0 || !std::isfinite(toleranceMm * 10))
            throw Standard_Failure("Invalid thickness topology tolerance");
        // Edge/vertex uncertainty controls containment and reconstruction. It
        // does not make distances between precise analytic surfaces imprecise.
        const double topologyTolerance = std::max(1.0e-6, toleranceMm * 10);
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
            double tolerance;
        };
        std::map<std::pair<std::size_t, std::size_t>, double> measured;
        const auto remember = [&](int a, int b, double gap, ThicknessEvidenceKind kind,
                                  double uncertainty) {
            auto pair = std::pair{static_cast<std::size_t>(a), static_cast<std::size_t>(b)};
            if (pair.first > pair.second)
                std::swap(pair.first, pair.second);
            if (measured.emplace(pair, uncertainty).second) {
                result.evidence.push_back({pair.first, pair.second, gap, kind});
                result.toleranceMm = std::max(result.toleranceMm, uncertainty);
            }
        };
        const auto recordBendThickness = [&]() {
            if (bends && bends->status == BendRecognitionStatus::Completed) {
                for (const auto &bend : bends->bends) {
                    if (stop.stop_requested())
                        return false;
                    if (!bend.innerFace || !bend.outerFace || bend.innerFace == bend.outerFace ||
                        bend.innerFace > static_cast<std::size_t>(faces.Extent()) ||
                        bend.outerFace > static_cast<std::size_t>(faces.Extent()))
                        throw Standard_Failure("Invalid bend thickness evidence");
                    BRepAdaptor_Surface inner(
                        TopoDS::Face(faces(static_cast<int>(bend.innerFace))));
                    BRepAdaptor_Surface outer(
                        TopoDS::Face(faces(static_cast<int>(bend.outerFace))));
                    if (inner.GetType() != GeomAbs_Cylinder ||
                        outer.GetType() != GeomAbs_Cylinder || !std::isfinite(bend.thicknessMm) ||
                        bend.thicknessMm <= 0)
                        throw Standard_Failure("Inconsistent bend thickness evidence");
                    const double uncertainty = std::max(
                        measurementTolerance(TopoDS::Face(faces(static_cast<int>(bend.innerFace)))),
                        measurementTolerance(
                            TopoDS::Face(faces(static_cast<int>(bend.outerFace)))));
                    if (std::abs(outer.Cylinder().Radius() - inner.Cylinder().Radius() -
                                 bend.thicknessMm) > uncertainty)
                        throw Standard_Failure("Inconsistent bend thickness evidence");
                    // Insufficient local resolution is not contradictory geometry.
                    if (bend.thicknessMm <= uncertainty * 100)
                        continue;
                    remember(static_cast<int>(bend.innerFace), static_cast<int>(bend.outerFace),
                             bend.thicknessMm, ThicknessEvidenceKind::BendRadii, uncertainty);
                }
            }
            return true;
        };
        const auto uncertaintyOf = [&](const ThicknessEvidence &pair) {
            return measured.at({pair.firstFace, pair.secondFace});
        };
        const auto agrees = [&](const std::vector<ThicknessEvidence> &evidence) {
            return std::all_of(evidence.begin(), evidence.end(), [&](const auto &pair) {
                return std::abs(pair.distanceMm - evidence.front().distanceMm) <=
                       std::max(uncertaintyOf(pair), uncertaintyOf(evidence.front()));
            });
        };
        const auto sortEvidence = [&] {
            std::stable_sort(
                result.evidence.begin(), result.evidence.end(),
                [&](const auto &a, const auto &b) { return uncertaintyOf(a) < uncertaintyOf(b); });
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
            planes.emplace(i, PlaneFace{plane, normal, props.Mass(), props.CentreOfMass(),
                                        measurementTolerance(TopoDS::Face(faces(i)))});
            if (planes.size() > maximumPlanes) {
                // Dense perforation can exhaust the planar-wall budget. The
                // recognized bend surfaces can still give local evidence.
                if (!recordBendThickness())
                    return {};
                if (result.evidence.empty())
                    result.status = ThicknessStatus::LimitReached;
                else {
                    sortEvidence();
                    if (!agrees(result.evidence))
                        result.status = ThicknessStatus::Ambiguous;
                }
                return result;
            }
        }
        IntCurvesFace_ShapeIntersector ray;
        ray.Load(shape, topologyTolerance);
        BRepClass3d_SolidClassifier material(shape);
        int rays = 0;
        for (const auto &[index, a] : planes) {
            const auto &face = TopoDS::Face(faces(index));
            std::vector<gp_Pnt> samples;
            const auto add = [&](double u, double v) {
                if (samples.size() >= 3 || !std::isfinite(u) || !std::isfinite(v))
                    return;
                if (BRepClass_FaceClassifier(face, gp_Pnt2d(u, v), topologyTolerance).State() !=
                    TopAbs_IN)
                    return;
                const auto point = ElSLib::Value(u, v, a.plane);
                if (std::none_of(samples.begin(), samples.end(), [&](const auto &p) {
                        return p.Distance(point) <= topologyTolerance;
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
            const double minimum = topologyTolerance * 10;
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
                material.Perform(sample.Translated(gp_Vec(inward) * minimum), topologyTolerance);
                if (material.State() != TopAbs_IN)
                    continue;
                // Search before the global interior probe as well: with an
                // imprecise remote edge that probe can lie beyond a thin wall,
                // in another wall behind air. Never skip the first exit.
                ray.Perform(gp_Lin(sample, inward), a.tolerance * 10, maximum);
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
                if (ray.WParameter(first) <= minimum)
                    continue;
                // Never skip an earlier non-planar exit to measure a different wall beyond air.
                const int other = faces.FindIndex(ray.Face(first));
                const auto found = planes.find(other);
                if (other == index || found == planes.end() || ray.State(first) != TopAbs_IN)
                    continue;
                const auto &b = found->second;
                const double uncertainty = std::max(a.tolerance, b.tolerance);
                const double gap =
                    -gp_Vec(a.plane.Location(), b.plane.Location()).Dot(gp_Vec(a.outward));
                if (!a.outward.IsOpposite(b.outward, 1.0e-7) || !std::isfinite(gap) ||
                    gap <= uncertainty * 100 || gap > std::sqrt(std::min(a.area, b.area)) / 5 ||
                    std::abs(ray.WParameter(first) - gap) > uncertainty)
                    continue;
                remember(index, other, gap, ThicknessEvidenceKind::PlanarWalls, uncertainty);
            }
        }
        if (!recordBendThickness())
            return {};
        sortEvidence();
        // A loose topology budget must not hide a measured change of thickness
        // even if one prism would fit within that larger occupied-volume budget.
        if (!result.evidence.empty() && !agrees(result.evidence)) {
            result.status = ThicknessStatus::Ambiguous;
            return result;
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
            // Preserve the former resolution bound for a full-body certificate.
            // Newly resolved local distances may only supply a local proposal.
            if (gap <= topologyTolerance * 100)
                continue;
            const auto &faceA = TopoDS::Face(faces(static_cast<int>(pair.firstFace)));
            const double volumeTolerance =
                std::max(originalVolume * 1.0e-8, a.area * topologyTolerance * 2.0);
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
        if (!detected.empty() && agrees(detected)) {
            result.status = ThicknessStatus::Detected;
            result.thicknessMm = detected.front().distanceMm;
            result.evidence = std::move(detected);
        } else if (!detected.empty() && !agrees(detected)) {
            result.status = ThicknessStatus::Ambiguous;
        }
    } catch (const Standard_Failure &) {
        result.status = ThicknessStatus::Failed;
        result.thicknessMm.reset();
    }
    return result;
}
} // namespace bending::cad
