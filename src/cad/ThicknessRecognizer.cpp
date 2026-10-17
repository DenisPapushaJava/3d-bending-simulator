#include "ThicknessRecognizer.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>

namespace bending::cad {
namespace {
double volume(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props, true);
    return std::abs(props.Mass());
}
double area(const TopoDS_Face &face) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    return props.Mass();
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
                                                  std::stop_token stop) {
    using namespace application;
    ThicknessAnalysis result;
    result.toleranceMm = std::max(1.0e-6, toleranceMm * 10.0);
    try {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        if (faces.Extent() > 128) {
            result.status = ThicknessStatus::LimitReached;
            return result;
        }
        struct PlaneFace {
            int index;
            gp_Pln plane;
            gp_Dir outward;
            double area;
        };
        std::vector<PlaneFace> planes;
        for (int i = 1; i <= faces.Extent(); ++i) {
            BRepAdaptor_Surface surface(TopoDS::Face(faces(i)));
            if (surface.GetType() != GeomAbs_Plane) {
                continue;
            }
            const auto plane = surface.Plane();
            auto normal = plane.Axis().Direction();
            if (faces(i).Orientation() == TopAbs_REVERSED) {
                normal.Reverse();
            }
            const double faceArea = area(TopoDS::Face(faces(i)));
            if (!std::isfinite(faceArea) || faceArea <= 0.0) {
                result.status = ThicknessStatus::Failed;
                return result;
            }
            planes.push_back({i, plane, normal, faceArea});
        }
        const double originalVolume = volume(shape);
        std::vector<ThicknessEvidence> detected;
        int reconstructions = 0;
        for (std::size_t i = 0; i < planes.size(); ++i) {
            for (std::size_t j = i + 1; j < planes.size(); ++j) {
                if (stop.stop_requested()) {
                    return {};
                }
                const auto &a = planes[i];
                const auto &b = planes[j];
                if (!a.outward.IsOpposite(b.outward, 1.0e-7)) {
                    continue;
                }
                const gp_Vec direction(a.outward);
                const double signedGap =
                    gp_Vec(a.plane.Location(), b.plane.Location()).Dot(direction);
                // Opposing outer skins: B must be behind A's outward normal, not across an air gap.
                const double gap = -signedGap;
                if (!std::isfinite(gap) || gap <= result.toleranceMm * 100.0 ||
                    gap > std::sqrt(std::min(a.area, b.area)) / 5.0) {
                    continue;
                }
                const auto &faceA = TopoDS::Face(faces(a.index));
                const auto &faceB = TopoDS::Face(faces(b.index));
                BRepExtrema_DistShapeShape distance(faceA, faceB);
                if (!distance.IsDone() || std::abs(distance.Value() - gap) > result.toleranceMm) {
                    continue;
                }
                const ThicknessEvidence pair{static_cast<std::size_t>(a.index),
                                             static_cast<std::size_t>(b.index), gap};
                result.evidence.push_back(pair);
                const double volumeTolerance =
                    std::max(originalVolume * 1.0e-8, a.area * result.toleranceMm * 2.0);
                if (std::abs(a.area - b.area) * gap > volumeTolerance ||
                    std::abs(a.area * gap - originalVolume) > volumeTolerance) {
                    continue;
                }
                gp_Trsf local;
                local.SetTransformation(a.plane.Position());
                Bnd_Box bounds;
                BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(faceA, local, true).Shape(), bounds,
                                       false, false);
                if (bounds.IsVoid() || bounds.IsWhole() || bounds.IsOpen()) {
                    continue;
                }
                double x0, y0, z0, x1, y1, z1;
                bounds.Get(x0, y0, z0, x1, y1, z1);
                if (std::min(x1 - x0, y1 - y0) < gap * 5.0) {
                    continue;
                }
                if (++reconstructions > 8) {
                    result.status = ThicknessStatus::LimitReached;
                    return result;
                }
                const auto prism =
                    BRepPrimAPI_MakePrism(faceA, direction * signedGap, true).Shape();
                if (BRepCheck_Analyzer(prism).IsValid() &&
                    sameSolid(shape, prism, volumeTolerance, stop)) {
                    detected.push_back(pair);
                }
            }
        }
        if (!detected.empty()) {
            const double thickness = detected.front().distanceMm;
            const bool agrees =
                std::all_of(detected.begin(), detected.end(), [&](const auto &pair) {
                    return std::abs(pair.distanceMm - thickness) <= result.toleranceMm;
                });
            result.status = agrees ? ThicknessStatus::Detected : ThicknessStatus::Ambiguous;
            if (agrees) {
                result.thicknessMm = thickness;
                result.evidence = std::move(detected);
            }
        }
    } catch (const Standard_Failure &) {
        result.status = ThicknessStatus::Failed;
        result.thicknessMm.reset();
    }
    return result;
}
} // namespace bending::cad
