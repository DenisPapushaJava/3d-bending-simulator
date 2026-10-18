#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
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
#include <gp_Ax3.hxx>
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
    std::optional<simulation::FlatToolSetup> setup;
    std::optional<simulation::PressMotion> motion;
    std::vector<simulation::PressState> states;
    std::optional<PunchRetractionCheck> retraction;
    TopoDS_Shape sweep;
    const PunchRetractionCheck *punchRetraction() const noexcept override {
        return retraction ? &*retraction : nullptr;
    }
    const simulation::PressMotion *pressMotion() const noexcept override {
        return motion ? &*motion : nullptr;
    }
    const simulation::PressState *pressState(std::size_t i) const noexcept override {
        return i < states.size() ? &states[i] : nullptr;
    }
    const simulation::FlatToolSetup *toolSetup() const noexcept override {
        return setup ? &*setup : nullptr;
    }
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
gp_Pnt sectionPoint(const FrameBasis &basis, simulation::Point2Mm p) {
    return basis.origin.Translated(basis.x * p.x + basis.y * p.y);
}
gp_Ax3 flangeAxes(const FrameBasis &basis, const simulation::BendSection &section, bool second) {
    const auto root = second ? section.moving[0] : section.fixed[0];
    const auto end = second ? section.moving[3] : section.fixed[1];
    return {sectionPoint(basis, root), gp_Dir(basis.z),
            gp_Dir(gp_Vec(sectionPoint(basis, root), sectionPoint(basis, end)))};
}
double flangeExtent(const TopoDS_Shape &inner, const TopoDS_Shape &outer, const gp_Ax3 &axes) {
    gp_Trsf local;
    local.SetTransformation(axes);
    Bnd_Box bounds;
    for (const auto &face : {inner, outer}) {
        const auto transformed = BRepBuilderAPI_Transform(face, local, true).Shape();
        BRepBndLib::AddOptimal(transformed, bounds, false, false);
    }
    return bounds.CornerMax().X();
}
void assemble(PreviewFrame &frame) {
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
}
// Keep the exact flange solids, including holes, countersinks and the free contour.
// Every frame owns fresh geometry: meshing may not mutate the imported document.
void placeFlanges(PreviewFrame &frame, const std::array<TopoDS_Shape, 2> &flanges,
                  const FrameBasis &sourceBasis, const simulation::BendSection &sourceSection,
                  const FrameBasis &targetBasis, const simulation::BendSection &targetSection) {
    for (std::size_t i = 0; i < flanges.size(); ++i) {
        gp_Trsf placement;
        placement.SetDisplacement(flangeAxes(sourceBasis, sourceSection, i == 1),
                                  flangeAxes(targetBasis, targetSection, i == 1));
        frame.regions[i * 2] = BRepBuilderAPI_Transform(flanges[i], placement, true).Shape();
    }
    assemble(frame);
}
std::optional<TopoDS_Shape> extractFlange(const TopoDS_Shape &source, const TopoDS_Shape &envelope,
                                          double tolerance) {
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(source);
    tools.Append(envelope);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    if (!common.IsDone() || common.HasErrors() || !BRepCheck_Analyzer(common.Shape()).IsValid()) {
        return {};
    }
    TopTools_IndexedMapOfShape solids;
    TopExp::MapShapes(common.Shape(), TopAbs_SOLID, solids);
    if (solids.Extent() != 1 || !std::isfinite(volume(solids(1))) || volume(solids(1)) <= 0) {
        return {};
    }
    return BRepBuilderAPI_Copy(solids(1), true, false).Shape();
}
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
PreviewFrame makeSectionFrame(const simulation::SingleBend &model, const FrameBasis &basis,
                              const simulation::BendSection &state) {
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
    frame.regions[0] = flange(state.fixed);
    frame.regions[2] = flange(state.moving);
    if (state.angleRadians == 0.0) {
        frame.regions[1] = prism({position(state.innerArc[0]), position(state.outerArc[0]),
                                  position(state.outerArc[2]), position(state.innerArc[2])},
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
        wire.Add(BRepBuilderAPI_MakeEdge(position(state.innerArc[0]), position(state.outerArc[0]))
                     .Edge());
        wire.Add(arc(state.outerArc));
        wire.Add(BRepBuilderAPI_MakeEdge(position(state.outerArc[2]), position(state.innerArc[2]))
                     .Edge());
        wire.Add(TopoDS::Edge(arc(state.innerArc).Reversed()));
        frame.regions[1] =
            BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), extrusion).Shape();
    }
    assemble(frame);
    return frame;
}
PreviewFrame makeFrame(const simulation::SingleBend &model, const FrameBasis &basis,
                       double progress) {
    const auto state = simulation::singleBendSection(model, progress);
    if (!state) {
        throw std::invalid_argument("Unsupported single-bend parameters");
    }
    return makeSectionFrame(model, basis, *state);
}
// The profile width is nondecreasing with Y. Its exact upward translational
// sweep is the original profile plus the vertical strip above its full-width top.
TopoDS_Shape makePunch(const core::PressTools &t, double tip, double extension = 0.0) {
    const double halfAngle = t.punchAngleRadians / 2.0;
    const double tangentX = t.noseRadiusMm * std::cos(halfAngle);
    const double tangentY = tip + t.noseRadiusMm * (1.0 - std::sin(halfAngle));
    const double topY = tip + t.openingMm;
    const double topX =
        (t.openingMm - t.noseRadiusMm) * std::tan(halfAngle) + t.noseRadiusMm / std::cos(halfAngle);
    const gp_Pnt leftTop(-topX, topY, 0), rightTop(topX, topY, 0);
    const gp_Pnt left(-tangentX, tangentY, 0), right(tangentX, tangentY, 0);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(leftTop, left).Edge());
    wire.Add(
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(left, gp_Pnt(0, tip, 0), right).Value()).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(right, rightTop).Edge());
    if (extension > 0) {
        const gp_Vec lift(0, extension, 0);
        wire.Add(BRepBuilderAPI_MakeEdge(rightTop, rightTop.Translated(lift)).Edge());
        wire.Add(
            BRepBuilderAPI_MakeEdge(rightTop.Translated(lift), leftTop.Translated(lift)).Edge());
        wire.Add(BRepBuilderAPI_MakeEdge(leftTop.Translated(lift), leftTop).Edge());
    } else {
        wire.Add(BRepBuilderAPI_MakeEdge(rightTop, leftTop).Edge());
    }
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(),
                                 gp_Vec(0, 0, t.lengthMm))
        .Shape();
}
std::array<TopoDS_Shape, 2> makeTools(const simulation::FlatToolSetup &setup, double thickness) {
    const auto &t = setup.tools;
    const double v = t.openingMm;
    const double bottom = -setup.grooveDepthMm - v / 2.0;
    const gp_Vec extrusion(0, 0, t.lengthMm);
    auto die = prism({{-v, 0, 0},
                      {-v / 2.0, 0, 0},
                      {0, -setup.grooveDepthMm, 0},
                      {v / 2.0, 0, 0},
                      {v, 0, 0},
                      {v, bottom, 0},
                      {-v, bottom, 0}},
                     extrusion);
    const auto punch = makePunch(t, thickness + t.clearanceMm);
    for (const auto &shape : {die, punch}) {
        if (!BRepCheck_Analyzer(shape).IsValid() || !std::isfinite(volume(shape)) ||
            volume(shape) <= 0.0) {
            throw std::runtime_error("Invalid tool solid");
        }
    }
    return {die, punch};
}
bool disjointVolumes(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                     double budget) {
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape arguments, tools;
    arguments.Append(a);
    tools.Append(b);
    common.SetArguments(arguments);
    common.SetTools(tools);
    common.SetNonDestructive(true);
    common.SetFuzzyValue(tolerance);
    common.Build();
    return common.IsDone() && !common.HasErrors() && BRepCheck_Analyzer(common.Shape()).IsValid() &&
           std::isfinite(volume(common.Shape())) && volume(common.Shape()) <= budget;
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
const TopoDS_Shape &
OcctPreviewAccess::retractionSweep(const application::SingleBendPreview &preview) {
    return dynamic_cast<const CadPreview &>(preview).sweep;
}
static application::BendPreviewResult buildPreview(DocumentPtr document, std::size_t bendIndex,
                                                   core::BendTarget target, bool fixedSecond,
                                                   std::stop_token stop,
                                                   std::optional<core::PressTools> toolParameters,
                                                   bool withMotion = false) {
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
    if (!analysis.interiorContourDiagnostics.empty()) {
        return BendPreviewError{BendPreviewErrorCode::BendZoneInteriorContours,
                                "Enclosed opening boundaries interrupt a cylindrical bend zone"};
    }
    if (analysis.status != BendRecognitionStatus::Completed || analysis.bends.size() != 1 ||
        bendIndex != 0 || analysis.bends[0].trimmedEnds) {
        return unsupported("Requires one complete straight bend and two planar flanges");
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
        const FrameBasis sourceBasis = basis;
        simulation::SingleBend model{
            bend.thicknessMm, bend.innerRadiusMm, bend.angleRadians, bend.lengthMm, 1, 1};
        const auto seed = simulation::singleBendSection(model, 1.0);
        if (!seed) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                    "Unsupported bend dimensions/angle"};
        }
        const std::array<std::size_t, 2> innerIndices{fixedSecond ? 2U : 0U, fixedSecond ? 0U : 2U};
        std::array<double, 2> extents{};
        for (std::size_t i = 0; i < extents.size(); ++i) {
            const auto index = innerIndices[i];
            extents[i] = flangeExtent(faces(static_cast<int>(bend.flangeFaces[index])),
                                      faces(static_cast<int>(bend.flangeFaces[index + 1])),
                                      flangeAxes(basis, *seed, i == 1));
        }
        model.fixedFlangeMm = extents[0];
        model.movingFlangeMm = extents[1];
        if (!simulation::validSingleBend(model)) {
            return BendPreviewError{BendPreviewErrorCode::UnsupportedParameters,
                                    "Supported sizes: thickness/radius 0.1–100000 mm, "
                                    "width/flanges 1–100000 mm, angle 5–175 degrees"};
        }
        const auto sourceSection = *simulation::singleBendSection(model, 1.0);
        auto finalFrame = makeFrame(model, basis, 1.0);
        std::array<TopoDS_Shape, 2> flanges;
        for (std::size_t i = 0; i < flanges.size(); ++i) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            const auto extracted =
                extractFlange(source, finalFrame.regions[i * 2], analysis.lengthToleranceMm);
            if (!extracted) {
                return unsupported("Cannot isolate one connected solid for each planar flange");
            }
            flanges[i] = *extracted;
            finalFrame.regions[i * 2] = *extracted;
        }
        assemble(finalFrame);
        const double expectedVolume = metadata.volumeMm3;
        const double budget =
            std::max(metadata.volumeMm3 * 1.0e-8, area(source) * analysis.lengthToleranceMm * 2.0);
        if (std::abs(volume(finalFrame.shape) - expectedVolume) > budget ||
            !sameVolume(source, finalFrame.shape, analysis.lengthToleranceMm, budget, stop)) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            return unsupported("Isolated flanges and complete bend do not match the entire STEP");
        }
        auto preview = std::make_shared<CadPreview>();
        preview->geometry = model;
        if (toolParameters) {
            const auto placement =
                simulation::flatToolSetup(model, *toolParameters, analysis.lengthToleranceMm);
            if (const auto *issue = std::get_if<simulation::ToolSetupIssue>(&placement)) {
                const auto code = *issue == simulation::ToolSetupIssue::ToolTooShort
                                      ? BendPreviewErrorCode::ToolTooShort
                                  : *issue == simulation::ToolSetupIssue::MissingLeftSupport
                                      ? BendPreviewErrorCode::MissingLeftSupport
                                  : *issue == simulation::ToolSetupIssue::MissingRightSupport
                                      ? BendPreviewErrorCode::MissingRightSupport
                                      : BendPreviewErrorCode::UnsupportedParameters;
                return BendPreviewError{code, "Initial tool setup parameters/support rejected"};
            }
            preview->setup = std::get<simulation::FlatToolSetup>(placement);
            if (withMotion) {
                const auto prepared = simulation::preparePressMotion(model, *toolParameters,
                                                                     analysis.lengthToleranceMm);
                if (const auto *issue = std::get_if<simulation::PressMotionIssue>(&prepared)) {
                    const auto code = *issue == simulation::PressMotionIssue::ToolAngleConflict
                                          ? BendPreviewErrorCode::ToolAngleConflict
                                      : *issue == simulation::PressMotionIssue::NoseTooLarge
                                          ? BendPreviewErrorCode::NoseTooLarge
                                      : *issue == simulation::PressMotionIssue::SupportNotProven
                                          ? BendPreviewErrorCode::SupportNotProven
                                          : BendPreviewErrorCode::UnsupportedParameters;
                    return BendPreviewError{code, "Contact motion parameters rejected"};
                }
                preview->motion = std::get<simulation::PressMotion>(prepared);
            }
            // The section model uses flange envelopes. Certify actual material under every
            // possible shoulder position, rather than inferring support from a bounding box.
            const double reach =
                preview->motion
                    ? preview->motion->maximumSupportDistanceMm
                    : (toolParameters->openingMm - simulation::bendAllowanceMm(model)) / 2.0;
            if (reach > analysis.lengthToleranceMm) {
                auto supportSection = sourceSection;
                const double length = reach + analysis.lengthToleranceMm / 2.0;
                const auto shorten = [length](simulation::Point2Mm root, simulation::Point2Mm end,
                                              double extent) {
                    return simulation::Point2Mm{root.x + (end.x - root.x) * length / extent,
                                                root.y + (end.y - root.y) * length / extent};
                };
                supportSection.fixed[1] =
                    shorten(sourceSection.fixed[0], sourceSection.fixed[1], model.fixedFlangeMm);
                supportSection.fixed[2] =
                    shorten(sourceSection.fixed[3], sourceSection.fixed[2], model.fixedFlangeMm);
                supportSection.moving[3] =
                    shorten(sourceSection.moving[0], sourceSection.moving[3], model.movingFlangeMm);
                supportSection.moving[2] =
                    shorten(sourceSection.moving[1], sourceSection.moving[2], model.movingFlangeMm);
                const auto support = makeSectionFrame(model, sourceBasis, supportSection);
                for (std::size_t i = 0; i < flanges.size(); ++i) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    BRepAlgoAPI_Cut missing;
                    TopTools_ListOfShape arguments, tools;
                    arguments.Append(support.regions[i * 2]);
                    tools.Append(flanges[i]);
                    missing.SetArguments(arguments);
                    missing.SetTools(tools);
                    missing.SetNonDestructive(true);
                    missing.SetFuzzyValue(analysis.lengthToleranceMm);
                    missing.Build();
                    const double supportBudget =
                        area(support.regions[i * 2]) * analysis.lengthToleranceMm * 2.0;
                    if (!missing.IsDone() || missing.HasErrors() ||
                        !BRepCheck_Analyzer(missing.Shape()).IsValid() ||
                        !std::isfinite(volume(missing.Shape())) ||
                        volume(missing.Shape()) > supportBudget) {
                        return BendPreviewError{BendPreviewErrorCode::SupportNotProven,
                                                "A hole or contour interrupts the conservative "
                                                "full-width support strip"};
                    }
                }
            }
            basis = {{-simulation::bendAllowanceMm(model) / 2.0, model.thicknessMm,
                      (toolParameters->lengthMm - model.widthMm) / 2.0},
                     {1, 0, 0},
                     {0, 1, 0},
                     {0, 0, 1}};
        }
        if (withMotion) {
            basis.origin = gp_Pnt(0, 0, (toolParameters->lengthMm - model.widthMm) / 2.0);
        }
        std::array<TopoDS_Shape, 2> initialTools;
        if (preview->setup) {
            initialTools = makeTools(*preview->setup, model.thicknessMm);
        }
        Bnd_Box envelope;
        constexpr std::size_t formingIntervals = 40;
        const std::size_t intervals = withMotion ? formingIntervals + 20 : toolParameters ? 0 : 20;
        for (std::size_t i = 0; i <= intervals; ++i) {
            if (stop.stop_requested()) {
                return cancelled();
            }
            const double progress =
                intervals ? static_cast<double>(i) / static_cast<double>(intervals) : 0.0;
            if (withMotion) {
                const auto state = simulation::pressMotionState(model, *preview->motion, progress);
                if (!state) {
                    return BendPreviewError{BendPreviewErrorCode::Failed,
                                            "Cannot compute press state", i};
                }
                preview->states.push_back(*state);
            }
            const auto section = withMotion ? preview->states.back().section
                                            : *simulation::singleBendSection(model, progress);
            auto frame = makeSectionFrame(model, basis, section);
            placeFlanges(frame, flanges, sourceBasis, sourceSection, basis, section);
            const double materialVolume = volume(frame.shape);
            if (!std::isfinite(materialVolume) ||
                std::abs(materialVolume - expectedVolume) > budget) {
                return BendPreviewError{BendPreviewErrorCode::Failed,
                                        "Intermediate state changed material volume"};
            }
            if (withMotion && i == formingIntervals) {
                // Independently place the already verified CAD reconstruction into the machine
                // frame. The contact section must be congruent with it, not merely have the same
                // volume.
                const auto &left = preview->states.back().section.innerArc[0];
                const double h = model.angleRadians / 2.0;
                const FrameBasis endpoint{gp_Pnt(left.x, left.y, basis.origin.Z()),
                                          gp_Vec(std::cos(h), -std::sin(h), 0),
                                          gp_Vec(std::sin(h), std::cos(h), 0), gp_Vec(0, 0, 1)};
                gp_Trsf placement;
                placement.SetDisplacement(
                    gp_Ax3(sourceBasis.origin, gp_Dir(sourceBasis.z), gp_Dir(sourceBasis.x)),
                    gp_Ax3(endpoint.origin, gp_Dir(endpoint.z), gp_Dir(endpoint.x)));
                const auto expected = BRepBuilderAPI_Transform(source, placement, true).Shape();
                if (!sameVolume(expected, frame.shape, analysis.lengthToleranceMm, budget, stop)) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    return BendPreviewError{
                        BendPreviewErrorCode::Failed,
                        "Contact endpoint differs from the verified STEP reconstruction", i};
                }
            }
            if (withMotion && i == formingIntervals) {
                const double tip = preview->states.back().punchTipYmm;
                const double lift = model.thicknessMm + toolParameters->clearanceMm - tip;
                if (!std::isfinite(lift) || lift <= analysis.lengthToleranceMm) {
                    return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                            "Invalid vertical punch return distance", i};
                }
                preview->sweep = makePunch(*toolParameters, tip, lift);
                if (!BRepCheck_Analyzer(preview->sweep).IsValid() ||
                    !std::isfinite(volume(preview->sweep)) || volume(preview->sweep) <= 0.0) {
                    return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                            "Invalid vertical punch return envelope", i};
                }
                for (const auto &obstacle : {frame.shape, initialTools[0]}) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    if (!disjointVolumes(preview->sweep, obstacle, analysis.lengthToleranceMm,
                                         budget)) {
                        return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                                "Punch return swept volume intersects sheet/die "
                                                "or its Boolean check failed",
                                                i};
                    }
                }
            }
            if (preview->setup) {
                frame.tools = initialTools;
                if (withMotion) {
                    gp_Trsf translation;
                    translation.SetTranslation(gp_Vec(0,
                                                      preview->states.back().punchTipYmm -
                                                          model.thicknessMm -
                                                          toolParameters->clearanceMm,
                                                      0));
                    frame.tools[1] = initialTools[1].Moved(TopLoc_Location(translation));
                }
                for (const auto &[a, b] : {std::pair{frame.shape, frame.tools[0]},
                                           std::pair{frame.shape, frame.tools[1]},
                                           std::pair{frame.tools[0], frame.tools[1]}}) {
                    if (stop.stop_requested()) {
                        return cancelled();
                    }
                    if (!disjointVolumes(a, b, analysis.lengthToleranceMm, budget)) {
                        return BendPreviewError{
                            BendPreviewErrorCode::ToolIntersection,
                            "Tool/material intersection or failed Boolean check", i};
                    }
                }
                if (withMotion) {
                    const auto &state = preview->states.back();
                    for (std::size_t flange = 0; flange < 2; ++flange) {
                        if (stop.stop_requested()) {
                            return cancelled();
                        }
                        const double x = (flange == 0 ? -0.5 : 0.5) * toolParameters->openingMm;
                        const auto shoulder =
                            BRepBuilderAPI_MakeEdge(gp_Pnt(x, 0, 0),
                                                    gp_Pnt(x, 0, toolParameters->lengthMm))
                                .Edge();
                        BRepExtrema_DistShapeShape contact(frame.regions[flange * 2], shoulder);
                        if (!contact.IsDone() || contact.Value() > analysis.lengthToleranceMm) {
                            return BendPreviewError{BendPreviewErrorCode::ContactLost,
                                                    "Die shoulder contact missing", i};
                        }
                    }
                    BRepExtrema_DistShapeShape punchContact(frame.regions[1], frame.tools[1]);
                    const double expectedGap = state.phase == simulation::PressPhase::Approach
                                                   ? state.punchTipYmm - model.thicknessMm
                                                   : 0.0;
                    if (!punchContact.IsDone() || !std::isfinite(punchContact.Value()) ||
                        (state.phase != simulation::PressPhase::Retracting &&
                         std::abs(punchContact.Value() - expectedGap) >
                             analysis.lengthToleranceMm)) {
                        return BendPreviewError{state.phase == simulation::PressPhase::Retracting
                                                    ? BendPreviewErrorCode::RetractionNotProven
                                                    : BendPreviewErrorCode::ContactLost,
                                                "Punch contact/clearance mismatch", i};
                    }
                }
                if (withMotion && i == intervals) {
                    BRepExtrema_DistShapeShape distance(frame.shape, frame.tools[1]);
                    if (!distance.IsDone() || !std::isfinite(distance.Value()) ||
                        distance.Value() <= analysis.lengthToleranceMm) {
                        return BendPreviewError{BendPreviewErrorCode::RetractionNotProven,
                                                "Final punch separation not confirmed", i};
                    }
                    const double home = model.thicknessMm + toolParameters->clearanceMm;
                    preview->retraction = PunchRetractionCheck{
                        home - preview->states[formingIntervals].punchTipYmm, distance.Value()};
                }
                BRep_Builder builder;
                TopoDS_Compound assembly;
                builder.MakeCompound(assembly);
                builder.Add(assembly, frame.shape);
                for (const auto &tool : frame.tools) {
                    builder.Add(assembly, tool);
                }
                frame.shape = assembly;
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
application::BendPreviewResult SingleBendPreviewBuilder::build(DocumentPtr document,
                                                               std::size_t bendIndex,
                                                               core::BendTarget target,
                                                               bool fixedSecond,
                                                               std::stop_token stop) const {
    return buildPreview(std::move(document), bendIndex, target, fixedSecond, stop, {});
}
application::BendPreviewResult
SingleBendPreviewBuilder::buildToolSetup(DocumentPtr document, std::size_t bendIndex,
                                         core::BendTarget target, core::PressTools tools,
                                         std::stop_token stop) const {
    return buildPreview(std::move(document), bendIndex, target, false, stop, tools);
}
application::BendPreviewResult
SingleBendPreviewBuilder::buildToolMotion(DocumentPtr document, std::size_t bendIndex,
                                          core::BendTarget target, core::PressTools tools,
                                          std::stop_token stop) const {
    return buildPreview(std::move(document), bendIndex, target, false, stop, tools, true);
}
} // namespace bending::cad
