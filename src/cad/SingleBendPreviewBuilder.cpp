#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bending::cad {
namespace {
using namespace application;
class CadPreview final : public SingleBendPreview {
  public:
    simulation::SingleBend geometry{};
    BoundsMm envelope{};
    std::vector<PreviewFrame> frames;
    const simulation::SingleBend &model() const noexcept override {
        return geometry;
    }
    std::size_t frameCount() const noexcept override {
        return frames.size();
    }
    BoundsMm bounds() const noexcept override {
        return envelope;
    }
};
gp_Pnt point(PointMm p) {
    return {p.x, p.y, p.z};
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return std::abs(properties.Mass());
}
double area(const TopoDS_Shape &shape) {
    GProp_GProps properties;
    BRepGProp::SurfaceProperties(shape, properties);
    return properties.Mass();
}
struct FrameBasis {
    gp_Pnt origin;
    gp_Vec x;
    gp_Vec y;
    gp_Vec z;
};
std::optional<gp_Vec> railRadial(const TopoDS_Face &cylinder, const TopoDS_Face &flange,
                                 const gp_Pnt &axisOrigin, const gp_Vec &axis, double tolerance) {
    TopTools_IndexedMapOfShape cylinderEdges, flangeEdges;
    TopExp::MapShapes(cylinder, TopAbs_EDGE, cylinderEdges);
    TopExp::MapShapes(flange, TopAbs_EDGE, flangeEdges);
    std::optional<gp_Vec> radial;
    for (int i = 1; i <= cylinderEdges.Extent(); ++i) {
        if (!flangeEdges.Contains(cylinderEdges(i))) {
            continue;
        }
        const BRepAdaptor_Curve curve(TopoDS::Edge(cylinderEdges(i)));
        if (curve.GetType() != GeomAbs_Line ||
            !curve.Line().Direction().IsParallel(gp_Dir(axis), 1.0e-5) || radial) {
            return {};
        }
        const gp_Vec offset(axisOrigin, curve.Value(curve.FirstParameter()));
        const auto value = offset - axis * offset.Dot(axis);
        if (value.Magnitude() <= tolerance) {
            return {};
        }
        radial = value.Normalized();
    }
    return radial;
}
TopoDS_Shape prism(const std::vector<gp_Pnt> &points, const gp_Vec &extrusion) {
    BRepBuilderAPI_MakeWire wire;
    for (std::size_t i = 0; i < points.size(); ++i) {
        wire.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % points.size()]).Edge());
    }
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), extrusion).Shape();
}
PreviewFrame makeFrame(const simulation::SingleBend &model, const FrameBasis &basis,
                       double progress) {
    const auto state = simulation::singleBendSection(model, progress);
    if (!state) {
        throw std::invalid_argument("Unsupported single-bend parameters");
    }
    const auto position = [&](simulation::Point2Mm p) {
        return basis.origin.Translated(basis.x * p.x + basis.y * p.y);
    };
    const auto extrusion = basis.z * model.widthMm;
    const auto flange = [&](const auto &corners) {
        std::vector<gp_Pnt> points;
        for (const auto p : corners) {
            points.push_back(position(p));
        }
        return prism(points, extrusion);
    };
    PreviewFrame frame;
    frame.regions[0] = flange(state->fixed);
    frame.regions[2] = flange(state->moving);
    if (progress == 0.0) {
        frame.regions[1] = prism({position(state->innerArc[0]), position(state->outerArc[0]),
                                  position(state->outerArc[2]), position(state->innerArc[2])},
                                 extrusion);
    } else {
        const auto arc = [&](const auto &points) {
            return BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(position(points[0]),
                                                              position(points[1]),
                                                              position(points[2]))
                                               .Value())
                .Edge();
        };
        BRepBuilderAPI_MakeWire wire;
        wire.Add(BRepBuilderAPI_MakeEdge(position(state->innerArc[0]), position(state->outerArc[0]))
                     .Edge());
        wire.Add(arc(state->outerArc));
        wire.Add(BRepBuilderAPI_MakeEdge(position(state->outerArc[2]), position(state->innerArc[2]))
                     .Edge());
        wire.Add(TopoDS::Edge(arc(state->innerArc).Reversed()));
        frame.regions[1] =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), extrusion).Shape();
    }
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto &region : frame.regions) {
        if (!BRepCheck_Analyzer(region).IsValid()) {
            throw std::runtime_error("Invalid preview region");
        }
        builder.Add(compound, region);
    }
    frame.shape = compound;
    return frame;
}
bool sameVolume(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance, double budget,
                std::stop_token stop) {
    for (const bool reverse : {false, true}) {
        if (stop.stop_requested()) {
            return false;
        }
        BRepAlgoAPI_Cut cut;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(reverse ? b : a);
        tools.Append(reverse ? a : b);
        cut.SetArguments(arguments);
        cut.SetTools(tools);
        cut.SetNonDestructive(true);
        cut.SetFuzzyValue(tolerance);
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors() || !BRepCheck_Analyzer(cut.Shape()).IsValid()) {
            return false;
        }
        const double residual = volume(cut.Shape());
        if (!std::isfinite(residual) || residual > budget) {
            return false;
        }
    }
    return true;
}
} // namespace
const PreviewFrame &OcctPreviewAccess::frame(const application::SingleBendPreview &preview,
                                             std::size_t index) {
    return dynamic_cast<const CadPreview &>(preview).frames.at(index);
}
application::BendPreviewResult SingleBendPreviewBuilder::build(DocumentPtr document,
                                                               std::size_t bendIndex,
                                                               core::BendTarget target,
                                                               bool fixedSecond,
                                                               std::stop_token stop) const {
    const auto unsupported = [](const char *detail) -> BendPreviewResult {
        return BendPreviewError{BendPreviewErrorCode::UnsupportedGeometry, detail};
    };
    const auto cancelled = []() -> BendPreviewResult {
        return BendPreviewError{BendPreviewErrorCode::Cancelled, "Preview cancelled"};
    };
    if (stop.stop_requested()) {
        return cancelled();
    }
    const auto native = std::dynamic_pointer_cast<const CadDocument>(document);
    if (!native) {
        return unsupported("Unsupported document type");
    }
    const auto &metadata = native->metadata();
    const auto &analysis = metadata.bendAnalysis;
    if (analysis.status != BendRecognitionStatus::Completed || analysis.bends.size() != 1 ||
        bendIndex != 0 || analysis.bends[0].trimmedEnds || metadata.faceCount != 10) {
        return unsupported("Requires one straight bend and two rectangular flanges, without "
                           "cutouts or trimmed ends");
    }
    const auto &bend = analysis.bends[0];
    if (!core::validBendTarget(target) ||
        std::abs(target.angleRadians - bend.angleRadians) > analysis.angleToleranceRadians ||
        std::abs(target.innerRadiusMm - bend.innerRadiusMm) > analysis.lengthToleranceMm) {
        return BendPreviewError{BendPreviewErrorCode::TargetMismatch,
                                "Target must match the imported final shape"};
    }
    try {
        const auto &source = OcctDocumentAccess::shape(*native);
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(source, TopAbs_FACE, faces);
        const auto &cylinder = TopoDS::Face(faces(static_cast<int>(bend.innerFace)));
        const auto &flangeA = TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[0])));
        const auto &flangeB = TopoDS::Face(faces(static_cast<int>(bend.flangeFaces[2])));
        auto axis = gp_Vec(point(bend.axisStart), point(bend.axisEnd)).Normalized();
        auto radialA =
            railRadial(cylinder, flangeA, point(bend.axisStart), axis, analysis.lengthToleranceMm);
        auto radialB =
            railRadial(cylinder, flangeB, point(bend.axisStart), axis, analysis.lengthToleranceMm);
        if (!radialA || !radialB) {
            return unsupported("Cannot determine tangency rails");
        }
        if (fixedSecond) {
            std::swap(radialA, radialB);
        }
        if (radialA->Crossed(*radialB).Dot(axis) < 0.0) {
            axis.Reverse();
        }
        const auto origin = axis.Dot(gp_Vec(point(bend.axisStart), point(bend.axisEnd))) > 0.0
                                ? point(bend.axisStart)
                                : point(bend.axisEnd);
        FrameBasis basis{origin.Translated(*radialA * bend.innerRadiusMm), axis.Crossed(*radialA),
                         -*radialA, axis};
        simulation::SingleBend model{bend.thicknessMm,
                                     bend.innerRadiusMm,
                                     bend.angleRadians,
                                     bend.lengthMm,
                                     area(fixedSecond ? flangeB : flangeA) / bend.lengthMm,
                                     area(fixedSecond ? flangeA : flangeB) / bend.lengthMm};
        if (!simulation::validSingleBend(model)) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                    "Supported sizes: thickness/radius 0.1–100000 mm, "
                                    "width/flanges 1–100000 mm, angle 5–175 degrees"};
        }
        auto finalFrame = makeFrame(model, basis, 1.0);
        const double expectedVolume =
            simulation::blankLengthMm(model) * model.thicknessMm * model.widthMm;
        const double budget =
            std::max(metadata.volumeMm3 * 1.0e-8, area(source) * analysis.lengthToleranceMm * 2.0);
        if (std::abs(expectedVolume - metadata.volumeMm3) > budget ||
            !sameVolume(source, finalFrame.shape, analysis.lengthToleranceMm, budget, stop)) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            return unsupported("Reconstructed strip does not match the entire imported solid");
        }
        auto preview = std::make_shared<CadPreview>();
        preview->geometry = model;
        Bnd_Box envelope;
        constexpr std::size_t intervals = 20;
        for (std::size_t i = 0; i <= intervals; ++i) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            auto frame = i == intervals
                             ? std::move(finalFrame)
                             : makeFrame(model, basis,
                                         static_cast<double>(i) / static_cast<double>(intervals));
            const double materialVolume = volume(frame.shape);
            if (!std::isfinite(materialVolume) ||
                std::abs(materialVolume - expectedVolume) > budget) {
                return BendPreviewError{BendPreviewErrorCode::Failed,
                                        "Intermediate state changed material volume"};
            }
            BRepMesh_IncrementalMesh mesh(frame.shape, 0.05, false, 0.35, false);
            if (!mesh.IsDone()) {
                return BendPreviewError{BendPreviewErrorCode::Failed, "Preview mesh failed"};
            }
            BRepBndLib::Add(frame.shape, envelope);
            preview->frames.push_back(std::move(frame));
        }
        if (stop.stop_requested()) {
            return cancelled();
        }
        envelope.Get(preview->envelope.minimum.x, preview->envelope.minimum.y,
                     preview->envelope.minimum.z, preview->envelope.maximum.x,
                     preview->envelope.maximum.y, preview->envelope.maximum.z);
        return BendPreviewPtr(std::move(preview));
    } catch (const Standard_Failure &error) {
        return BendPreviewError{BendPreviewErrorCode::Failed, error.GetMessageString()
                                                                  ? error.GetMessageString()
                                                                  : "OCCT preview failure"};
    } catch (const std::exception &error) {
        return BendPreviewError{BendPreviewErrorCode::Failed, error.what()};
    }
}
} // namespace bending::cad
